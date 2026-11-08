// module.cpp - module loader: import resolution, compilation, bytecode
// cache (.tpc files in a temp folder, like Python's .pyc), hot reload,
// and the bytecode disassembler.
#include "loader.h"
#include "parser.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <sstream>

namespace fs = std::filesystem;

namespace tp {

static const uint32_t kCacheMagic = 0x31435054; // "TPC1"
static const uint32_t kCacheVersion = 5;

std::unique_ptr<Loader> makeLoader(VM& vm) { return std::make_unique<Loader>(vm); }

VM::~VM() {
    loader.reset();
    for (Obj* list : {youngList, oldList}) {
        Obj* o = list;
        while (o) {
            Obj* next = o->next;
            delete o;
            o = next;
        }
    }
    youngList = oldList = nullptr;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string Loader::overlayKey(const std::string& path) {
    std::error_code ec;
    std::string k = fs::absolute(fs::path(path), ec).lexically_normal().string();
#ifdef _WIN32
    for (auto& c : k) {
        c = (char)std::tolower((unsigned char)c);
        if (c == '/') c = '\\';
    }
#endif
    return k;
}

bool Loader::readSource(const std::string& path, std::string& out) {
    if (!overlays.empty()) {
        auto it = overlays.find(overlayKey(path));
        if (it != overlays.end()) {
            out = it->second;
            return true;
        }
    }
    // the host's reader first (taipan_set_file_reader), then the disk
    if (vm.fileReader && vm.fileReader(path, out)) return true;
    return readFile(path, out);
}

// Import resolution asks this instead of fs::exists, so a module that only exists
// behind the host's file reader still resolves.
bool Loader::sourceExists(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return true;
    if (!vm.fileReader) return false;
    std::string probe;
    return vm.fileReader(fs::absolute(path, ec).lexically_normal().string(), probe);
}

struct GcPauseGuard {
    VM& vm;
    explicit GcPauseGuard(VM& v) : vm(v) { vm.gcPause++; }
    ~GcPauseGuard() { vm.gcPause--; }
};

static Diagnostic plainError(const std::string& title, const std::string& label, int fileId = -1, Span sp = {}) {
    Diagnostic d;
    d.sev = Diagnostic::Error;
    d.title = title;
    d.label = label;
    d.fileId = fileId;
    d.span = sp;
    return d;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
std::string Loader::displayName(const std::string& path) {
    if (path.empty()) return "<string>";
    std::error_code ec;
    if (!root.empty()) {
        fs::path rel = fs::relative(fs::path(path), fs::path(root), ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.') return rel.generic_string();
    }
    return fs::path(path).filename().generic_string();
}

std::string Loader::keyForPath(const std::string& path) {
    std::error_code ec;
    fs::path p(path);
    if (!root.empty()) {
        fs::path rel = fs::relative(p, fs::path(root), ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.') {
            rel.replace_extension();
            std::string k = rel.generic_string();
            for (auto& c : k) {
                if (c == '/') c = '.';
            }
            return k;
        }
    }
    return p.stem().string();
}

bool Loader::resolvePath(const std::string& dotted, std::string& path, std::string& key, std::string& err) {
    std::string rel = dotted;
    for (auto& c : rel) {
        if (c == '.') c = '/';
    }
    std::error_code ec;
    if (dotted.find('.') != std::string::npos) {
        fs::path cand = fs::path(root) / (rel + ".tp");
        if (!sourceExists(cand)) {
            err = "cannot find module '" + dotted + "' (looked for " + cand.generic_string() + ")";
            return false;
        }
        path = fs::absolute(cand, ec).lexically_normal().string();
        key = dotted;
        return true;
    }
    // A bare name resolves only if exactly one such file is reachable from the root.
    std::vector<fs::path> found;
    std::string want = dotted + ".tp";
    fs::path direct = fs::path(root) / want;
    if (sourceExists(direct)) found.push_back(direct);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 8) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().filename() == want && it->path().parent_path() != fs::path(root)) {
            found.push_back(it->path());
        }
    }
    if (found.empty()) {
        err = "cannot find module '" + dotted + "' (no " + want + " under " + fs::path(root).generic_string() + ")";
        return false;
    }
    if (found.size() > 1) {
        err = "ambiguous import '" + dotted + "':";
        for (auto& f : found) err += " " + fs::relative(f, root, ec).generic_string();
        err += " - use the dotted path";
        return false;
    }
    path = fs::absolute(found[0], ec).lexically_normal().string();
    key = keyForPath(path);
    return true;
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------
ModuleSymbols* Loader::resolveImport(const std::string& dotted, int fromFile, Span span, std::vector<Diagnostic>& diags) {
    std::string path, key, err;
    if (!resolvePath(dotted, path, key, err)) {
        diags.push_back(plainError("module not found", err, fromFile, span));
        return nullptr;
    }
    for (size_t i = 0; i < compiling.size(); i++) {
        if (compiling[i] != key) continue;
        std::string next = i + 1 < compiling.size() ? compiling[i + 1] : compiling.back();
        Diagnostic d = plainError("circular import detected",
                                  "'" + key + "' imports '" + next + "', creating a cycle", fromFile, span);
        std::string chain;
        for (size_t j = i; j < compiling.size(); j++) chain += compiling[j] + " -> ";
        d.notes.push_back("import chain: " + chain + key);
        diags.push_back(d);
        return nullptr;
    }
    auto it = entries.find(key);
    Entry* e = nullptr;
    if (it != entries.end() && it->second->syms) {
        e = it->second.get();
    } else {
        e = compileFile(path, key, diags, CompileOptions());
        if (!e) return nullptr;
    }
    if (!depStack.empty()) depStack.back().push_back(key);
    return e->syms.get();
}

Loader::Entry* Loader::compileSource(const std::string& path, const std::string& display, const std::string& key,
                                     std::string text, std::vector<Diagnostic>& diags, const CompileOptions& opt) {
    GcPauseGuard pause(vm);
    uint64_t hash = fnv1a(text.data(), text.size());
    int fileId = vm.addSource(path, display, std::move(text));
    auto syms = std::make_unique<ModuleSymbols>();
    syms->key = key;
    syms->path = path;
    syms->fileId = fileId;
    size_t before = diags.size();
    syms->program = parseProgram(vm.sources[fileId], fileId, diags);
    bool parseFailed = false;
    for (size_t i = before; i < diags.size(); i++) {
        if (diags[i].sev == Diagnostic::Error) parseFailed = true;
    }
    // Normally a syntax error stops here. The language server still checks
    // the statements that did parse, so hover and completion keep working.
    if (parseFailed && !opt.sink) return nullptr;
    if (opt.sink) opt.sink->mainSyms = syms.get();
    compiling.push_back(key);
    depStack.emplace_back();
    size_t beforeCompile = diags.size();
    Proto* main = compileModule(vm, *syms, *this, diags, opt);
    compiling.pop_back();
    std::vector<std::string> direct = std::move(depStack.back());
    depStack.pop_back();
    if (parseFailed) diags.resize(beforeCompile); // only report the syntax errors
    if (!main || parseFailed) {
        if (opt.sink) opt.sink->keep.push_back(std::move(syms));
        return nullptr;
    }

    auto e = std::make_unique<Entry>();
    e->key = key;
    e->path = path;
    e->fileId = fileId;
    e->hash = hash;
    e->main = main;
    e->globals = syms->slotNames;
    std::unordered_set<std::string> seen;
    for (auto& k : direct) {
        auto it = entries.find(k);
        if (it == entries.end()) continue;
        Entry* d = it->second.get();
        if (seen.insert(d->path).second) e->deps.emplace_back(d->path, d->hash);
        for (auto& dd : d->deps) {
            if (seen.insert(dd.first).second) e->deps.push_back(dd);
        }
    }
    e->syms = std::move(syms);
    if (vm.dumpBytecode) {
        std::string listing = "== bytecode: " + display + " ==\n" + disassemble(vm, main);
        std::fputs(listing.c_str(), stdout);
    }
    Entry* raw = e.get();
    auto old = entries.find(key);
    if (old != entries.end()) graveyard.push_back(std::move(old->second));
    entries[key] = std::move(e);
    if (!path.empty() && vm.cacheEnabled) writeCache(raw);
    return raw;
}

Loader::Entry* Loader::compileFile(const std::string& path, const std::string& key, std::vector<Diagnostic>& diags,
                                   const CompileOptions& opt) {
    std::string text;
    if (!readSource(path, text)) {
        diags.push_back(plainError("cannot read file", "could not open '" + path + "'"));
        return nullptr;
    }
    return compileSource(path, displayName(path), key, std::move(text), diags, opt);
}

Loader::Entry* Loader::loadMain(const std::string& givenPath, std::vector<Diagnostic>& diags) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(givenPath), ec).lexically_normal();
    if (root.empty()) {
        root = !vm.scriptRoot.empty() ? fs::absolute(vm.scriptRoot, ec).lexically_normal().string()
                                      : abs.parent_path().string();
    }
    std::string path = abs.string();
    std::string text;
    if (!readSource(path, text)) {
        diags.push_back(plainError("cannot read file", "could not open '" + givenPath + "'"));
        return nullptr;
    }
    std::string key = keyForPath(path);
    auto it = entries.find(key);
    if (it != entries.end() && it->second->path == path && it->second->hash == fnv1a(text.data(), text.size())) {
        return it->second.get();
    }
    if (vm.cacheEnabled) {
        if (Entry* e = fromCache(path, key, text)) return e;
    }
    return compileSource(path, displayName(path), key, std::move(text), diags, CompileOptions());
}

Loader::Entry* Loader::loadString(const std::string& source, const std::string& chunk, std::vector<Diagnostic>& diags) {
    std::error_code ec;
    if (root.empty()) {
        root = !vm.scriptRoot.empty() ? fs::absolute(vm.scriptRoot, ec).string() : fs::current_path(ec).string();
    }
    return compileSource("", chunk, chunk, source, diags, CompileOptions());
}

ObjModule* Loader::instantiate(Entry* e) {
    auto existing = vm.modules.find(e->key);
    if (existing != vm.modules.end()) return existing->second;
    GcPauseGuard pause(vm);
    ObjModule* m = vm.newModule(e->key, e->path, e->fileId);
    vm.modules[e->key] = m;
    m->globals.resize(e->globals.size());
    m->defined.assign(e->globals.size(), 0);
    for (size_t i = 0; i < e->globals.size(); i++) m->names[vm.str(e->globals[i])] = (int)i;
    ObjClosure* c = vm.alloc<ObjClosure>();
    c->proto = e->main;
    c->module = m;
    m->mainFn = Value::object(c);
    vm.touch(m);
    return m;
}

ObjModule* Loader::newInstance(Entry* e) {
    GcPauseGuard pause(vm);
    ObjModule* m = vm.newModule(e->key, e->path, e->fileId);
    vm.instances.push_back(m);
    m->globals.resize(e->globals.size());
    m->defined.assign(e->globals.size(), 0);
    for (size_t i = 0; i < e->globals.size(); i++) m->names[vm.str(e->globals[i])] = (int)i;
    ObjClosure* c = vm.alloc<ObjClosure>();
    c->proto = e->main;
    c->module = m;
    m->mainFn = Value::object(c);
    vm.touch(m);
    return m;
}

ObjModule* Loader::instantiateKey(const std::string& key, std::vector<Diagnostic>& diags) {
    auto it = entries.find(key);
    if (it != entries.end()) return instantiate(it->second.get());
    std::string path, k, err;
    if (!resolvePath(key, path, k, err)) {
        diags.push_back(plainError("module not found", err));
        return nullptr;
    }
    std::string text;
    if (!readSource(path, text)) {
        diags.push_back(plainError("cannot read file", "could not open '" + path + "'"));
        return nullptr;
    }
    Entry* e = vm.cacheEnabled ? fromCache(path, key, text) : nullptr;
    if (!e) e = compileSource(path, displayName(path), key, std::move(text), diags, CompileOptions());
    return e ? instantiate(e) : nullptr;
}

bool Loader::reload(const std::string& givenPath, std::vector<Diagnostic>& diags, std::vector<ObjModule*>& updated) {
    std::error_code ec;
    std::string path = fs::absolute(fs::path(givenPath), ec).lexically_normal().string();
    // every environment running this file: the shared module and all script instances
    std::vector<ObjModule*> targets;
    for (auto& kv : vm.modules) {
        if (kv.second->path == path) targets.push_back(kv.second);
    }
    for (auto* inst : vm.instances) {
        if (inst->path == path) targets.push_back(inst);
    }
    ObjModule* m = targets.empty() ? nullptr : targets[0];
    if (!m) {
        diags.push_back(plainError("cannot reload", "'" + givenPath + "' has not been loaded"));
        return false;
    }
    CompileOptions opt;
    opt.reload = true;
    auto it = entries.find(m->name->s);
    if (it != entries.end()) {
        opt.existingGlobals = it->second->globals;
    } else {
        opt.existingGlobals.resize(m->globals.size());
        for (auto& kv : m->names) opt.existingGlobals[kv.second] = kv.first->s;
    }
    std::string text;
    if (!readSource(path, text)) {
        diags.push_back(plainError("cannot read file", "could not open '" + givenPath + "'"));
        return false;
    }
    Entry* e = compileSource(path, displayName(path), m->name->s, std::move(text), diags, opt);
    if (!e) return false;
    GcPauseGuard pause(vm);
    for (ObjModule* t : targets) {
        t->globals.resize(e->globals.size());
        t->defined.resize(e->globals.size(), 0);
        for (size_t i = 0; i < e->globals.size(); i++) t->names[vm.str(e->globals[i])] = (int)i;
        ObjClosure* c = vm.alloc<ObjClosure>();
        c->proto = e->main;
        c->module = t;
        t->mainFn = Value::object(c);
        t->fileId = e->fileId;
        vm.touch(t);
    }
    updated = targets;
    return true;
}

// ---------------------------------------------------------------------------
// Bytecode cache
// ---------------------------------------------------------------------------
namespace {

struct Writer {
    std::string buf;
    void u8(uint8_t v) { buf.push_back((char)v); }
    void u32(uint32_t v) { buf.append(reinterpret_cast<const char*>(&v), 4); }
    void u64(uint64_t v) { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void str(const std::string& s) {
        u32((uint32_t)s.size());
        buf += s;
    }
};

struct Reader {
    const std::string& buf;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::string& b) : buf(b) {}
    bool need(size_t n) {
        if (pos + n > buf.size()) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? (uint8_t)buf[pos++] : 0; }
    uint32_t u32() {
        uint32_t v = 0;
        if (need(4)) std::memcpy(&v, buf.data() + pos, 4), pos += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        if (need(8)) std::memcpy(&v, buf.data() + pos, 8), pos += 8;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!need(n)) return "";
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }
};

void writeProto(Writer& w, const Proto* p) {
    w.str(p->name);
    w.u32((uint32_t)p->numParams);
    w.u32((uint32_t)p->minParams);
    w.u8((uint8_t)((p->variadic ? 1 : 0) | (p->isCoroutine ? 2 : 0) | (p->isMethod ? 4 : 0)));
    w.u32((uint32_t)p->numUpvals);
    w.u32((uint32_t)p->maxStack);
    w.u32((uint32_t)p->caches.size());
    w.u32((uint32_t)p->code.size());
    for (int32_t c : p->code) w.u32((uint32_t)c);
    for (const Span& s : p->spans) {
        w.u32((uint32_t)s.start);
        w.u32((uint32_t)s.end);
    }
    w.u32((uint32_t)p->consts.size());
    for (const Value& v : p->consts) {
        switch (v.t) {
        case VT::Null: w.u8(0); break;
        case VT::Bool: w.u8(1); w.u8(v.b ? 1 : 0); break;
        case VT::Int: w.u8(2); w.u64((uint64_t)v.i); break;
        case VT::Float: {
            uint64_t bits;
            std::memcpy(&bits, &v.f, 8);
            w.u8(3);
            w.u64(bits);
            break;
        }
        case VT::Obj:
            w.u8(4);
            w.str(static_cast<ObjString*>(v.o)->s);
            break;
        }
    }
    w.u32((uint32_t)p->protos.size());
    for (const Proto* c : p->protos) writeProto(w, c);
}

Proto* readProto(Reader& r, VM& vm, int fileId, int depth) {
    if (depth > 256 || !r.ok) return nullptr;
    Proto* p = vm.newProto();
    p->fileId = fileId;
    p->name = r.str();
    p->numParams = (int)r.u32();
    p->minParams = (int)r.u32();
    uint8_t flags = r.u8();
    p->variadic = flags & 1;
    p->isCoroutine = flags & 2;
    p->isMethod = flags & 4;
    p->numUpvals = (int)r.u32();
    p->maxStack = (int)r.u32();
    p->caches.resize(r.u32());
    uint32_t n = r.u32();
    if (!r.need((size_t)n * 12)) return nullptr;
    p->code.resize(n);
    for (uint32_t i = 0; i < n; i++) p->code[i] = (int32_t)r.u32();
    p->spans.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        p->spans[i].start = (int)r.u32();
        p->spans[i].end = (int)r.u32();
    }
    uint32_t nk = r.u32();
    for (uint32_t i = 0; i < nk && r.ok; i++) {
        uint8_t tag = r.u8();
        switch (tag) {
        case 0: p->consts.push_back(Value()); break;
        case 1: p->consts.push_back(Value::boolean(r.u8() != 0)); break;
        case 2: p->consts.push_back(Value::integer((int64_t)r.u64())); break;
        case 3: {
            uint64_t bits = r.u64();
            double d;
            std::memcpy(&d, &bits, 8);
            p->consts.push_back(Value::number(d));
            break;
        }
        case 4: p->consts.push_back(vm.strv(r.str())); break;
        default: r.ok = false;
        }
    }
    uint32_t np = r.u32();
    for (uint32_t i = 0; i < np && r.ok; i++) {
        Proto* c = readProto(r, vm, fileId, depth + 1);
        if (!c) return nullptr;
        p->protos.push_back(c);
    }
    return r.ok ? p : nullptr;
}

} // namespace

std::string Loader::cachePath(const std::string& path) {
    std::error_code ec;
    fs::path dir = vm.cacheDir.empty() ? fs::temp_directory_path(ec) / "taipan_cache" : fs::path(vm.cacheDir);
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.tpc", (unsigned long long)fnv1a(path.data(), path.size()));
    return (dir / name).string();
}

void Loader::writeCache(Entry* e) {
    Writer w;
    w.u32(kCacheMagic);
    w.u32(kCacheVersion);
    w.u64(e->hash);
    w.u32((uint32_t)e->deps.size());
    for (auto& d : e->deps) {
        w.str(d.first);
        w.u64(d.second);
    }
    w.u32((uint32_t)e->globals.size());
    for (auto& g : e->globals) w.str(g);
    writeProto(w, e->main);
    std::error_code ec;
    std::string cp = cachePath(e->path);
    fs::create_directories(fs::path(cp).parent_path(), ec);
    std::ofstream out(cp, std::ios::binary | std::ios::trunc);
    if (out) out.write(w.buf.data(), (std::streamsize)w.buf.size());
}

Loader::Entry* Loader::fromCache(const std::string& path, const std::string& key, const std::string& text) {
    std::string data;
    if (!readFile(cachePath(path), data)) return nullptr;
    Reader r(data);
    if (r.u32() != kCacheMagic || r.u32() != kCacheVersion) return nullptr;
    uint64_t hash = fnv1a(text.data(), text.size());
    if (r.u64() != hash) return nullptr; // source changed: stale cache
    auto e = std::make_unique<Entry>();
    uint32_t nd = r.u32();
    for (uint32_t i = 0; i < nd && r.ok; i++) {
        std::string dp = r.str();
        uint64_t dh = r.u64();
        std::string dt;
        if (!readFile(dp, dt) || fnv1a(dt.data(), dt.size()) != dh) return nullptr; // an import changed
        e->deps.emplace_back(dp, dh);
    }
    uint32_t ng = r.u32();
    for (uint32_t i = 0; i < ng && r.ok; i++) e->globals.push_back(r.str());
    if (!r.ok) return nullptr;
    GcPauseGuard pause(vm);
    int fileId = vm.addSource(path, displayName(path), text);
    Proto* main = readProto(r, vm, fileId, 0);
    if (!main) return nullptr;
    e->key = key;
    e->path = path;
    e->fileId = fileId;
    e->hash = hash;
    e->main = main;
    Entry* raw = e.get();
    auto old = entries.find(key);
    if (old != entries.end()) graveyard.push_back(std::move(old->second));
    entries[key] = std::move(e);
    return raw;
}

// ---------------------------------------------------------------------------
// Disassembler
// ---------------------------------------------------------------------------
static void disasm(VM& vm, Proto* p, std::string& out, int depth) {
    std::string indent(depth * 2, ' ');
    out += indent + "function " + p->name + " (params " + std::to_string(p->numParams) + ", upvalues " +
           std::to_string(p->numUpvals) + ")\n";
    int lastLine = -1;
    for (int ip = 0; ip < (int)p->code.size();) {
        int op = p->code[ip];
        int n = opOperands(p->code.data(), ip);
        int line = 0, col = 0;
        if (p->fileId >= 0 && p->fileId < (int)vm.sources.size()) vm.sources[p->fileId].lineCol(p->spans[ip].start, line, col);
        char head[64];
        if (line != lastLine) std::snprintf(head, sizeof head, "%s%5d %4d  ", indent.c_str(), ip, line);
        else std::snprintf(head, sizeof head, "%s%5d    |  ", indent.c_str(), ip);
        lastLine = line;
        out += head;
        out += opName(op);
        for (int i = 1; i <= n; i++) out += " " + std::to_string(p->code[ip + i]);
        if ((op == OP_CONST || op == OP_GET_BUILTIN || op == OP_GET_FIELD || op == OP_SET_FIELD || op == OP_INVOKE ||
             op == OP_CHECK_TYPE || op == OP_IMPORT || op == OP_METHOD || op == OP_CLASS) &&
            p->code[ip + 1] < (int)p->consts.size()) {
            out += "    ; " + vm.toStr(p->consts[p->code[ip + 1]]);
        }
        out += "\n";
        ip += 1 + n;
    }
    for (Proto* c : p->protos) disasm(vm, c, out, depth + 1);
}

std::string disassemble(VM& vm, Proto* p) {
    std::string out;
    disasm(vm, p, out, 0);
    return out;
}

} // namespace tp
