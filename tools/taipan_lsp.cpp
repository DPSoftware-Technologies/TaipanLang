// taipan_lsp.cpp - the Taipan language server (LSP over stdin/stdout).
//
// Features: diagnostics, hover, go to definition, completion, document
// outline. Editors start it as `taipan-lsp`. Scripts are only parsed and
// type-checked, never run.
//
// Settings (LSP initializationOptions):
//   scriptRoot  folder imports resolve from (default: each file's folder)
//   bindings    extra .tpi binding files ("*.tpi" in the workspace are found automatically)
//   standalone  include standalone-only built-ins like input() (default true)
#include "analysis.h"
#include "taipan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal JSON
// ---------------------------------------------------------------------------
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    Json(bool v) : type(Bool), b(v) {}
    Json(int v) : type(Num), n(v) {}
    Json(double v) : type(Num), n(v) {}
    Json(const char* v) : type(Str), s(v) {}
    Json(std::string v) : type(Str), s(std::move(v)) {}

    static Json object() {
        Json j;
        j.type = Obj;
        return j;
    }
    static Json array() {
        Json j;
        j.type = Arr;
        return j;
    }

    const Json& operator[](const std::string& key) const {
        static const Json none;
        if (type != Obj) return none;
        for (auto& kv : obj) {
            if (kv.first == key) return kv.second;
        }
        return none;
    }
    Json& set(const std::string& key, Json v) {
        type = Obj;
        for (auto& kv : obj) {
            if (kv.first == key) {
                kv.second = std::move(v);
                return *this;
            }
        }
        obj.emplace_back(key, std::move(v));
        return *this;
    }
    Json& push(Json v) {
        type = Arr;
        arr.push_back(std::move(v));
        return *this;
    }
    bool isNull() const { return type == Null; }
    int asInt() const { return (int)n; }
    const std::string& str() const { return s; }
};

static void writeJson(const Json& j, std::string& out) {
    switch (j.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
        char buf[32];
        if (j.n == (double)(long long)j.n) std::snprintf(buf, sizeof buf, "%lld", (long long)j.n);
        else std::snprintf(buf, sizeof buf, "%.17g", j.n);
        out += buf;
        break;
    }
    case Json::Str:
        out += '"';
        for (unsigned char c : j.s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
        out += '"';
        break;
    case Json::Arr:
        out += '[';
        for (size_t i = 0; i < j.arr.size(); i++) {
            if (i) out += ',';
            writeJson(j.arr[i], out);
        }
        out += ']';
        break;
    case Json::Obj:
        out += '{';
        for (size_t i = 0; i < j.obj.size(); i++) {
            if (i) out += ',';
            writeJson(Json(j.obj[i].first), out);
            out += ':';
            writeJson(j.obj[i].second, out);
        }
        out += '}';
        break;
    }
}

struct JsonReader {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    void ws() {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        std::string out;
        i++; // "
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && s.compare(i, 2, "\\u") == 0) {
                    unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: out += e;
            }
        }
        i++;
        return out;
    }

    Json value() {
        ws();
        if (i >= s.size()) {
            ok = false;
            return Json();
        }
        char c = s[i];
        if (c == '{') {
            i++;
            Json o = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') {
                i++;
                return o;
            }
            while (ok) {
                ws();
                if (i >= s.size() || s[i] != '"') {
                    ok = false;
                    break;
                }
                std::string k = string();
                ws();
                if (i >= s.size() || s[i] != ':') {
                    ok = false;
                    break;
                }
                i++;
                o.obj.emplace_back(k, value());
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') i++;
                else ok = false;
                break;
            }
            return o;
        }
        if (c == '[') {
            i++;
            Json a = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') {
                i++;
                return a;
            }
            while (ok) {
                a.arr.push_back(value());
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') i++;
                else ok = false;
                break;
            }
            return a;
        }
        if (c == '"') return Json(string());
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            return Json(true);
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return Json(false);
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return Json();
        }
        size_t start = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E')) {
            i++;
        }
        if (start == i) {
            ok = false;
            return Json();
        }
        return Json(std::strtod(s.substr(start, i - start).c_str(), nullptr));
    }
};

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
static bool readMessage(std::string& body) {
    size_t length = 0;
    std::string line;
    while (true) {
        if (!std::getline(std::cin, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const char* key = "Content-Length:";
        if (line.compare(0, std::strlen(key), key) == 0) length = (size_t)std::strtoul(line.c_str() + std::strlen(key), nullptr, 10);
    }
    body.assign(length, '\0');
    std::cin.read(&body[0], (std::streamsize)length);
    return (size_t)std::cin.gcount() == length;
}

static void sendMessage(const Json& msg) {
    std::string body;
    writeJson(msg, body);
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::fwrite(header.data(), 1, header.size(), stdout);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

static void respond(const Json& id, Json result) {
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
    sendMessage(m);
}

static void respondError(const Json& id, int code, const std::string& message) {
    Json err = Json::object();
    err.set("code", code).set("message", message);
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("error", err);
    sendMessage(m);
}

static void notify(const std::string& method, Json params) {
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
    sendMessage(m);
}

// ---------------------------------------------------------------------------
// URIs, paths and positions (LSP columns count UTF-16 code units)
// ---------------------------------------------------------------------------
static std::string uriToPath(const std::string& uri) {
    std::string s = uri.compare(0, 7, "file://") == 0 ? uri.substr(7) : uri;
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
#ifdef _WIN32
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out = out.substr(1);
#endif
    return fs::path(out).lexically_normal().string();
}

static std::string pathToUri(const std::string& path) {
    std::string p = fs::path(path).generic_string();
    std::string out = "file://";
#ifdef _WIN32
    if (p.size() > 1 && p[1] == ':') {
        p[0] = (char)std::tolower((unsigned char)p[0]);
        out += "/";
    }
#endif
    for (unsigned char c : p) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

struct LineIndex {
    std::vector<int> starts;
    explicit LineIndex(const std::string& text) {
        starts.push_back(0);
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\n') starts.push_back((int)i + 1);
        }
    }
};

static int utf8Len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static Json position(const std::string& text, const LineIndex& idx, int offset) {
    offset = std::max(0, std::min(offset, (int)text.size()));
    int line = (int)(std::upper_bound(idx.starts.begin(), idx.starts.end(), offset) - idx.starts.begin()) - 1;
    int units = 0;
    for (int i = idx.starts[line]; i < offset;) {
        int n = utf8Len((unsigned char)text[i]);
        units += n == 4 ? 2 : 1;
        i += n;
    }
    Json p = Json::object();
    p.set("line", line).set("character", units);
    return p;
}

static int offsetOf(const std::string& text, const LineIndex& idx, const Json& pos) {
    int line = pos["line"].asInt();
    int ch = pos["character"].asInt();
    if (line < 0) return 0;
    if (line >= (int)idx.starts.size()) return (int)text.size();
    int i = idx.starts[line];
    int units = 0;
    while (i < (int)text.size() && text[i] != '\n' && units < ch) {
        int n = utf8Len((unsigned char)text[i]);
        units += n == 4 ? 2 : 1;
        i += n;
    }
    return std::min(i, (int)text.size());
}

static Json range(const std::string& text, const LineIndex& idx, int start, int end) {
    Json r = Json::object();
    r.set("start", position(text, idx, start)).set("end", position(text, idx, std::max(start, end)));
    return r;
}

// ---------------------------------------------------------------------------
// Server state
// ---------------------------------------------------------------------------
struct Document {
    std::string uri, path, text;
    tp::AnalysisResult result;
};

struct Server {
    std::map<std::string, Document> docs; // by uri
    std::string workspaceRoot;
    std::string scriptRoot;
    std::vector<std::string> extraBindings;
    bool standalone = true;
    bool shutdown = false;
    std::vector<std::string> bindingCache;
    std::chrono::steady_clock::time_point bindingScan;
    bool scanned = false;

    std::vector<std::string> bindingFiles() {
        auto now = std::chrono::steady_clock::now();
        if (!scanned || now - bindingScan > std::chrono::seconds(2)) {
            bindingCache.clear();
            if (!workspaceRoot.empty()) {
                std::error_code ec;
                for (auto it = fs::recursive_directory_iterator(workspaceRoot, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    std::string name = it->path().filename().string();
                    if (it->is_directory(ec)) {
                        if (it.depth() > 6 || name == ".git" || name == "node_modules" || name == "build" || name[0] == '.') {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    if (it->path().extension() == ".tpi") bindingCache.push_back(it->path().string());
                }
            }
            for (auto& b : extraBindings) {
                fs::path p(b);
                if (p.is_relative() && !workspaceRoot.empty()) p = fs::path(workspaceRoot) / p;
                bindingCache.push_back(p.lexically_normal().string());
            }
            bindingScan = now;
            scanned = true;
        }
        return bindingCache;
    }

    tp::AnalysisRequest request(const Document& d) {
        tp::AnalysisRequest req;
        req.path = d.path;
        req.text = d.text;
        req.root = scriptRoot;
        req.standalone = standalone;
        req.bindingFiles = bindingFiles();
        for (auto& kv : docs) {
            if (kv.first != d.uri) req.overlays[kv.second.path] = kv.second.text;
        }
        return req;
    }

    void analyzeAndPublish(Document& d) {
        d.result = tp::analyze(request(d));
        LineIndex idx(d.text);
        Json diags = Json::array();
        for (auto& ad : d.result.diags) {
            Json j = Json::object();
            j.set("range", range(d.text, idx, ad.start, ad.end))
                .set("severity", ad.error ? 1 : 2)
                .set("source", "taipan")
                .set("message", ad.message);
            diags.push(j);
        }
        Json params = Json::object();
        params.set("uri", d.uri).set("diagnostics", diags);
        notify("textDocument/publishDiagnostics", params);
    }

    Document* doc(const Json& params) {
        auto it = docs.find(params["textDocument"]["uri"].str());
        return it == docs.end() ? nullptr : &it->second;
    }

    const tp::AnalysisUse* useAt(const Document& d, int offset) {
        const tp::AnalysisUse* best = nullptr;
        for (auto& u : d.result.uses) {
            if (offset >= u.start && offset <= u.end && (!best || u.end - u.start < best->end - best->start)) best = &u;
        }
        return best;
    }

    std::string fileText(const std::string& path) {
        for (auto& kv : docs) {
            if (kv.second.path == path) return kv.second.text;
        }
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    Json initialize(const Json& params) {
        std::string root = params["rootUri"].isNull() ? "" : params["rootUri"].str();
        if (!root.empty()) workspaceRoot = uriToPath(root);
        else if (!params["rootPath"].isNull()) workspaceRoot = params["rootPath"].str();
        const Json& opts = params["initializationOptions"];
        if (!opts["scriptRoot"].isNull() && !opts["scriptRoot"].str().empty()) {
            fs::path p(opts["scriptRoot"].str());
            if (p.is_relative() && !workspaceRoot.empty()) p = fs::path(workspaceRoot) / p;
            scriptRoot = p.lexically_normal().string();
        }
        for (auto& b : opts["bindings"].arr) extraBindings.push_back(b.str());
        if (opts["standalone"].type == Json::Bool) standalone = opts["standalone"].b;

        Json sync = Json::object();
        sync.set("openClose", true).set("change", 1);
        Json save = Json::object();
        save.set("includeText", false);
        sync.set("save", save);
        Json completion = Json::object();
        completion.set("triggerCharacters", Json::array().push("."));
        Json caps = Json::object();
        caps.set("textDocumentSync", sync)
            .set("hoverProvider", true)
            .set("definitionProvider", true)
            .set("documentSymbolProvider", true)
            .set("completionProvider", completion);
        Json info = Json::object();
        info.set("name", "taipan-lsp").set("version", TAIPAN_VERSION);
        Json result = Json::object();
        result.set("capabilities", caps).set("serverInfo", info);
        return result;
    }

    Json hover(const Json& params) {
        Document* d = doc(params);
        if (!d) return Json();
        LineIndex idx(d->text);
        const tp::AnalysisUse* u = useAt(*d, offsetOf(d->text, idx, params["position"]));
        if (!u || u->detail.empty()) return Json();
        Json contents = Json::object();
        contents.set("kind", "markdown").set("value", "```taipan\n" + u->detail + "\n```");
        Json h = Json::object();
        h.set("contents", contents).set("range", range(d->text, idx, u->start, u->end));
        return h;
    }

    Json definition(const Json& params) {
        Document* d = doc(params);
        if (!d) return Json();
        LineIndex idx(d->text);
        const tp::AnalysisUse* u = useAt(*d, offsetOf(d->text, idx, params["position"]));
        if (!u || u->declPath.empty()) return Json();
        std::string text = u->declPath == d->path ? d->text : fileText(u->declPath);
        LineIndex tidx(text);
        Json loc = Json::object();
        loc.set("uri", u->declPath == d->path ? d->uri : pathToUri(u->declPath))
            .set("range", range(text, tidx, u->declStart, u->declEnd));
        return loc;
    }

    Json completion(const Json& params) {
        Document* d = doc(params);
        Json list = Json::object();
        list.set("isIncomplete", false);
        Json items = Json::array();
        if (d) {
            LineIndex idx(d->text);
            tp::AnalysisRequest req = request(*d);
            req.completionOffset = offsetOf(d->text, idx, params["position"]);
            tp::AnalysisResult r = tp::analyze(req);
            for (auto& c : r.completions) {
                Json item = Json::object();
                item.set("label", c.label).set("kind", c.kind).set("detail", c.detail);
                items.push(item);
            }
        }
        list.set("items", items);
        return list;
    }

    Json symbol(const Document& d, const LineIndex& idx, const tp::AnalysisSymbol& s) {
        Json j = Json::object();
        j.set("name", s.name).set("kind", s.kind)
            .set("range", range(d.text, idx, s.start, std::max(s.end, s.selEnd)))
            .set("selectionRange", range(d.text, idx, s.selStart, s.selEnd));
        if (!s.detail.empty()) j.set("detail", s.detail);
        if (!s.children.empty()) {
            Json kids = Json::array();
            for (auto& c : s.children) kids.push(symbol(d, idx, c));
            j.set("children", kids);
        }
        return j;
    }

    Json documentSymbol(const Json& params) {
        Document* d = doc(params);
        Json arr = Json::array();
        if (!d) return arr;
        LineIndex idx(d->text);
        for (auto& s : d->result.symbols) arr.push(symbol(*d, idx, s));
        return arr;
    }

    // Returns false when the server should exit.
    bool handle(const Json& msg) {
        const std::string& method = msg["method"].str();
        const Json& id = msg["id"];
        const Json& params = msg["params"];
        bool isRequest = !id.isNull();
        if (method == "initialize") {
            respond(id, initialize(params));
        } else if (method == "shutdown") {
            shutdown = true;
            respond(id, Json());
        } else if (method == "exit") {
            return false;
        } else if (method == "textDocument/didOpen") {
            Document d;
            d.uri = params["textDocument"]["uri"].str();
            d.path = uriToPath(d.uri);
            d.text = params["textDocument"]["text"].str();
            docs[d.uri] = d;
            analyzeAndPublish(docs[d.uri]);
        } else if (method == "textDocument/didChange") {
            if (Document* d = doc(params)) {
                const Json& changes = params["contentChanges"];
                if (!changes.arr.empty()) d->text = changes.arr.back()["text"].str(); // full sync
                analyzeAndPublish(*d);
            }
        } else if (method == "textDocument/didSave") {
            // another file may import the saved one: refresh everything that is open
            for (auto& kv : docs) analyzeAndPublish(kv.second);
        } else if (method == "textDocument/didClose") {
            std::string uri = params["textDocument"]["uri"].str();
            docs.erase(uri);
            Json p = Json::object();
            p.set("uri", uri).set("diagnostics", Json::array());
            notify("textDocument/publishDiagnostics", p);
        } else if (method == "textDocument/hover") {
            respond(id, hover(params));
        } else if (method == "textDocument/definition") {
            respond(id, definition(params));
        } else if (method == "textDocument/completion") {
            respond(id, completion(params));
        } else if (method == "textDocument/documentSymbol") {
            respond(id, documentSymbol(params));
        } else if (isRequest) {
            respondError(id, -32601, "method not supported: " + method);
        }
        return true;
    }
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version") {
            std::printf("taipan-lsp %s\n", TAIPAN_VERSION);
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::printf("taipan-lsp: Taipan language server. Editors start it and talk LSP over stdin/stdout.\n");
            return 0;
        }
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    Server server;
    std::string body;
    while (readMessage(body)) {
        JsonReader r {body};
        Json msg = r.value();
        if (!r.ok) continue;
        try {
            if (!server.handle(msg)) return server.shutdown ? 0 : 1;
        } catch (const std::exception& e) {
            if (!msg["id"].isNull()) respondError(msg["id"], -32603, std::string("internal error: ") + e.what());
        }
    }
    return server.shutdown ? 0 : 1;
}
