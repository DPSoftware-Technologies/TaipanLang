// analysis.cpp - editor analysis for the language server.
//
// Each request uses a fresh, throwaway VM: the file is parsed and checked
// (never run) with an AnalysisSink attached to the compiler, and the results
// are copied out as plain strings and offsets before the VM is destroyed.
#include "analysis.h"
#include "loader.h"
#include "parser.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace tp {

static const char* kPlaceholder = "__tp_complete__";

static std::string sigLine(const std::string& name, const SigInfo& s) {
    std::string r = name + "(";
    for (size_t i = 0; i < s.params.size(); i++) {
        if (i) r += ", ";
        bool variadic = s.variadic && i + 1 == s.params.size();
        r += T::name(s.params[i]) + (variadic ? "..." : "");
        if (i < s.names.size() && s.names[i].rfind("arg", 0) != 0) r += " " + s.names[i];
        if ((int)i >= s.required && !variadic) r += " = ...";
    }
    r += ")";
    if (s.ret && s.ret->k != TK::Void) r += " -> " + T::name(s.ret);
    return r;
}

namespace {

struct Builder {
    VM& vm;
    Loader& loader;
    AnalysisSink& sink;
    const AnalysisRequest& req;
    int mainFile;
    AnalysisResult& out;
    std::set<std::string> seen;

    void add(const std::string& label, int kind, const std::string& detail) {
        if (label.empty() || label[0] == '@' || label[0] == '$' || label == kPlaceholder) return;
        if (!seen.insert(label).second) return;
        out.completions.push_back(AnalysisCompletion {label, kind, detail});
    }

    std::string nativeDetail(const std::string& key, const std::string& label) {
        auto it = vm.nativeSigs.find(key);
        if (it == vm.nativeSigs.end() || it->second.empty()) return label;
        if (it->second[0] == '(') return label + it->second;
        return it->second + " " + label;
    }

    void namespaceMembers(const std::string& nsName) {
        ObjNamespace* ns = vm.namespaceFor(nsName, false);
        if (!ns) return;
        for (auto& kv : ns->members) {
            const std::string& n = kv.first->s;
            Value v = kv.second;
            if (v.isObj() && v.o->type == OT::Namespace) add(n, CkModule, "namespace " + nsName + "." + n);
            else if (v.isObj() && v.o->type == OT::Native) add(n, CkFunction, nativeDetail(nsName + "." + n, n));
            else add(n, CkConstant, nativeDetail(nsName + "." + n, n));
        }
    }

    void globalItem(const std::string& name, const ModuleSymbols::Global& g) {
        switch (g.kind) {
        case ModuleSymbols::Global::Func:
            add(name, CkFunction, g.type && g.type->sig ? sigLine(name, *g.type->sig) : name);
            break;
        case ModuleSymbols::Global::Class: add(name, CkClass, "class " + name); break;
        case ModuleSymbols::Global::Interface: add(name, CkInterface, "interface " + name); break;
        case ModuleSymbols::Global::Enum: add(name, CkEnum, "enum " + name); break;
        case ModuleSymbols::Global::Import: break;
        default:
            add(name, g.isConst ? CkConstant : CkVariable, T::name(g.type ? g.type : T::any()) + " " + name);
        }
    }

    void classMembers(ClassInfo* ci, bool includePrivate) {
        for (auto& f : ci->fields) {
            if (f.isPrivate && !includePrivate) continue;
            add(f.name, CkField, T::name(f.type) + " " + ci->name + "." + f.name);
        }
        for (auto& kv : ci->methods) {
            if (kv.first == "init" || (kv.second.isPrivate && !includePrivate)) continue;
            add(kv.first, CkMethod, sigLine(kv.first, *kv.second.sig));
        }
    }

    bool insideClass(ClassInfo* ci, int offset) {
        for (auto& c : sink.classes) {
            if (c.cls == ci && c.fileId == mainFile && offset >= c.start && offset <= c.end) return true;
        }
        return false;
    }

    void builtinMembers(const TypeP& t) {
        auto tn = [] (const TypeP& x) { return T::name(x); };
        add("length", CkProperty, "int length");
        if (t->k == TK::List) {
            std::string E = tn(t->a), L = "list<" + E + ">";
            const char* rows[][2] = {
                {"add", ""}, {"insert", ""}, {"removeAt", ""}, {"remove", ""}, {"pop", ""}, {"contains", ""},
                {"indexOf", ""}, {"clear", ""}, {"forEach", ""}, {"map", ""}, {"filter", ""}, {"sort", ""},
                {"reverse", ""}, {"slice", ""}, {"join", ""}, {"copy", ""}, {"isEmpty", ""}};
            std::string details[] = {
                "add(" + E + ")", "insert(int, " + E + ")", "removeAt(int) -> " + E, "remove(" + E + ") -> bool",
                "pop() -> " + E, "contains(" + E + ") -> bool", "indexOf(" + E + ") -> int", "clear()",
                "forEach(func)", "map(func) -> list<any>", "filter(func) -> " + L, "sort(func = ...)", "reverse()",
                "slice(int, int = ...) -> " + L, "join(str = ...) -> str", "copy() -> " + L, "isEmpty() -> bool"};
            for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) add(rows[i][0], CkMethod, details[i]);
        } else if (t->k == TK::Dict) {
            std::string K = tn(t->a), V = tn(t->b);
            add("keys", CkMethod, "keys() -> list<" + K + ">");
            add("values", CkMethod, "values() -> list<" + V + ">");
            add("has", CkMethod, "has(" + K + ") -> bool");
            add("contains", CkMethod, "contains(" + K + ") -> bool");
            add("remove", CkMethod, "remove(" + K + ") -> bool");
            add("get", CkMethod, "get(" + K + ", " + V + ") -> " + V);
            add("clear", CkMethod, "clear()");
            add("copy", CkMethod, "copy() -> dict<" + K + ", " + V + ">");
            add("isEmpty", CkMethod, "isEmpty() -> bool");
        } else if (t->k == TK::Str) {
            ObjNamespace* ns = vm.namespaceFor("str", false);
            if (ns) {
                for (auto& kv : ns->members) {
                    std::string n = kv.first->s;
                    if (n == "length") continue;
                    std::string d = nativeDetail("str." + n, n);
                    // drop the receiver parameter: str.toUpper(str) -> s.toUpper()
                    size_t open = d.find('(');
                    size_t comma = d.find(',', open);
                    size_t close = d.find(')', open);
                    if (open != std::string::npos && close != std::string::npos) {
                        d = d.substr(0, open + 1) + (comma != std::string::npos && comma < close ? d.substr(comma + 2) : d.substr(close));
                    }
                    add(n, CkMethod, d);
                }
            }
        }
    }

    void memberCompletions(int offset) {
        if (sink.receiverEnum) {
            for (auto& m : sink.receiverEnum->members) add(m, CkEnumMember, sink.receiverEnum->name + "." + m);
            return;
        }
        if (sink.receiverModule) {
            for (auto& kv : sink.receiverModule->globals) globalItem(kv.first, kv.second);
            return;
        }
        if (!sink.receiverNamespace.empty()) {
            namespaceMembers(sink.receiverNamespace);
            return;
        }
        TypeP t = sink.receiverType;
        if (!t) return;
        switch (t->k) {
        case TK::Class: classMembers(t->cls, insideClass(t->cls, offset)); break;
        case TK::Interface:
            for (auto& kv : t->iface->methods) add(kv.first, CkMethod, sigLine(kv.first, *kv.second));
            break;
        case TK::Native:
            for (auto& kv : t->native->props) add(kv.first, CkProperty, kv.second.type + " " + kv.first);
            for (auto& kv : t->native->methods) add(kv.first, CkMethod, kv.first + kv.second.sig);
            break;
        case TK::List: case TK::Dict: case TK::Str: builtinMembers(t); break;
        case TK::Tuple: add("length", CkProperty, "int length"); break;
        case TK::Enum:
            add("name", CkProperty, "str name");
            add("index", CkProperty, "int index");
            break;
        default: break;
        }
    }

    void scopeCompletions(int offset) {
        // locals: innermost (latest) declaration of a name wins
        std::vector<const AnalysisSink::ScopeVar*> visible;
        for (auto& v : sink.locals) {
            if (v.fileId == mainFile && offset >= v.visibleFrom && offset <= v.visibleTo) visible.push_back(&v);
        }
        std::sort(visible.begin(), visible.end(), [] (auto* a, auto* b) { return a->visibleFrom > b->visibleFrom; });
        for (auto* v : visible) {
            if (v->type && v->type->k == TK::Sig) add(v->name, CkFunction, sigLine(v->name, *v->type->sig));
            else add(v->name, v->isConst ? CkConstant : CkVariable,
                     std::string(v->isParam ? "(parameter) " : "") + T::name(v->type) + " " + v->name);
        }
        for (auto& c : sink.classes) {
            if (c.fileId == mainFile && offset >= c.start && offset <= c.end) {
                classMembers(c.cls, true);
                add("this", CkKeyword, "this");
            }
        }
        if (sink.mainSyms) {
            for (auto& kv : sink.mainSyms->globals) globalItem(kv.first, kv.second);
        }
        for (auto& n : sink.importNames) add(n, CkModule, "module " + n);
        for (auto& kv : vm.builtins->members) {
            const std::string& n = kv.first->s;
            Value v = kv.second;
            if (v.isObj() && v.o->type == OT::Namespace) add(n, CkModule, "namespace " + n);
            else if (v.isObj() && v.o->type == OT::Native) add(n, CkFunction, nativeDetail(n, n));
            else add(n, CkVariable, nativeDetail(n, n));
        }
        add("wait", CkFunction, "wait(float seconds = 0.0)");
        for (auto& kv : vm.nativeTypeNames) add(kv.first, CkClass, "class " + kv.first + " (native)");
        static const char* types[] = {"int", "float", "bool", "str", "any", "list", "dict", "func"};
        for (const char* t : types) add(t, CkKeyword, "type " + std::string(t));
        if (sink.mainSyms && sink.mainSyms->extTypes) {
            static const char* ext[] = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
                                        "byte", "f32", "f64"};
            for (const char* t : ext) add(t, CkKeyword, "type " + std::string(t));
        }
        static const char* keywords[] = {"func", "coroutine", "start", "return", "if", "else", "for", "in", "while",
                                         "match", "break", "continue", "try", "catch", "throw", "class", "interface",
                                         "implements", "enum", "import", "as", "enable", "const", "private", "public",
                                         "true", "false", "null"};
        for (const char* k : keywords) add(k, CkKeyword, "keyword");
    }
};

void outline(const std::vector<StmtP>& program, std::vector<AnalysisSymbol>& out) {
    for (auto& sp : program) {
        Stmt* s = sp.get();
        AnalysisSymbol sym;
        sym.start = s->span.start;
        sym.end = s->span.end;
        switch (s->k) {
        case SK::Func:
            sym.name = s->func->name;
            sym.kind = SkFunction;
            sym.detail = s->func->isCoroutine ? "coroutine" : "func";
            sym.selStart = s->func->nameSpan.start;
            sym.selEnd = s->func->nameSpan.end;
            break;
        case SK::Class: {
            sym.name = s->cls->name;
            sym.kind = SkClass;
            sym.selStart = s->cls->nameSpan.start;
            sym.selEnd = s->cls->nameSpan.end;
            for (auto& f : s->cls->fields) {
                AnalysisSymbol c;
                c.name = f.name;
                c.kind = SkField;
                c.detail = typeExprText(f.type.get());
                c.start = c.selStart = f.span.start;
                c.end = c.selEnd = f.span.end;
                sym.children.push_back(c);
            }
            for (auto& m : s->cls->methods) {
                AnalysisSymbol c;
                c.name = m->name;
                c.kind = m->name == "init" ? SkConstructor : SkMethod;
                c.start = m->span.start;
                c.end = m->span.end;
                c.selStart = m->nameSpan.start;
                c.selEnd = m->nameSpan.end;
                sym.children.push_back(c);
            }
            break;
        }
        case SK::Interface:
            sym.name = s->iface->name;
            sym.kind = SkInterface;
            sym.selStart = s->iface->nameSpan.start;
            sym.selEnd = s->iface->nameSpan.end;
            for (auto& m : s->iface->methods) {
                AnalysisSymbol c;
                c.name = m->name;
                c.kind = SkMethod;
                c.start = m->span.start;
                c.end = m->span.end;
                c.selStart = m->nameSpan.start;
                c.selEnd = m->nameSpan.end;
                sym.children.push_back(c);
            }
            break;
        case SK::Enum:
            sym.name = s->en->name;
            sym.kind = SkEnum;
            sym.selStart = s->en->nameSpan.start;
            sym.selEnd = s->en->nameSpan.end;
            for (size_t i = 0; i < s->en->members.size(); i++) {
                AnalysisSymbol c;
                c.name = s->en->members[i];
                c.kind = SkEnumMember;
                c.start = c.selStart = s->en->memberSpans[i].start;
                c.end = c.selEnd = s->en->memberSpans[i].end;
                sym.children.push_back(c);
            }
            break;
        case SK::VarDecl:
            for (auto& v : s->vars) {
                AnalysisSymbol c;
                c.name = v.name;
                c.kind = s->isConst ? SkConstant : SkVariable;
                c.detail = typeExprText(v.type.get());
                c.start = s->span.start;
                c.end = s->span.end;
                c.selStart = v.span.start;
                c.selEnd = v.span.end;
                out.push_back(c);
            }
            continue;
        default:
            continue;
        }
        out.push_back(std::move(sym));
    }
}

std::string diagMessage(const Diagnostic& d) {
    std::string m = d.title;
    if (!d.label.empty()) m += ": " + d.label;
    for (auto& n : d.notes) m += "\nnote: " + n;
    return m;
}

void runAnalysis(const AnalysisRequest& req, const std::string& text, AnalysisResult& out, bool wantUses,
                 bool memberMode, int completionOffset) {
    TaipanState* st = taipan_create();
    VM& vm = st->vm;
    vm.cacheEnabled = false;
    if (req.standalone) vm.openStandalone();
    std::vector<Diagnostic> diags;
    for (auto& b : req.bindingFiles) vm.loadBindings(b, diags);
    size_t bindingDiags = diags.size();

    Loader& loader = *vm.loader;
    loader.root = !req.root.empty() ? req.root : fs::path(req.path).parent_path().string();
    for (auto& kv : req.overlays) loader.overlays[Loader::overlayKey(kv.first)] = kv.second;

    AnalysisSink sink;
    if (memberMode) sink.placeholder = kPlaceholder;
    CompileOptions opt;
    opt.sink = &sink;
    try {
        loader.compileSource(req.path, loader.displayName(req.path), loader.keyForPath(req.path), text, diags, opt);
    } catch (...) {
        Diagnostic d;
        d.title = "internal error";
        d.label = "the analyzer failed on this file";
        diags.push_back(d);
    }
    int mainFile = sink.mainSyms ? sink.mainSyms->fileId : -1;

    if (wantUses) {
        for (size_t i = 0; i < diags.size(); i++) {
            const Diagnostic& d = diags[i];
            if (d.sev == Diagnostic::Warning && !vm.warnings) continue;
            AnalysisDiag ad;
            ad.error = d.sev == Diagnostic::Error;
            ad.message = diagMessage(d);
            if (d.fileId == mainFile && mainFile >= 0) {
                ad.start = d.span.start;
                ad.end = d.span.end;
            } else if (d.fileId >= 0 && d.fileId < (int)vm.sources.size()) {
                // a problem inside an imported file or a binding file
                int line, col;
                vm.sources[d.fileId].lineCol(d.span.start, line, col);
                ad.message = vm.sources[d.fileId].display + ":" + std::to_string(line) + ": " + ad.message;
            } else if (i < bindingDiags) {
                ad.message = "binding file: " + ad.message;
            }
            out.diags.push_back(ad);
        }
        for (auto& u : sink.uses) {
            if (u.fileId != mainFile) continue;
            AnalysisUse au;
            au.start = u.span.start;
            au.end = u.span.end;
            au.detail = u.detail;
            if (u.declFile >= 0 && u.declFile < (int)vm.sources.size() && !vm.sources[u.declFile].path.empty()) {
                au.declPath = u.declFile == mainFile ? req.path : vm.sources[u.declFile].path;
                au.declStart = u.declSpan.start;
                au.declEnd = u.declSpan.end;
            }
            out.uses.push_back(au);
        }
        if (sink.mainSyms) outline(sink.mainSyms->program, out.symbols);
    }
    if (completionOffset >= 0) {
        Builder b {vm, loader, sink, req, mainFile, out, {}};
        if (memberMode) b.memberCompletions(completionOffset);
        else b.scopeCompletions(completionOffset);
    }
    taipan_destroy(st);
}

bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

} // namespace

AnalysisResult analyze(const AnalysisRequest& req) {
    AnalysisResult out;
    if (req.completionOffset < 0) {
        runAnalysis(req, req.text, out, true, false, -1);
        return out;
    }
    int off = std::min<int>(req.completionOffset, (int)req.text.size());
    int start = off;
    while (start > 0 && isIdentChar(req.text[start - 1])) start--;
    int dot = start - 1;
    while (dot >= 0 && (req.text[dot] == ' ' || req.text[dot] == '\t')) dot--;
    if (dot >= 0 && req.text[dot] == '.') {
        // `recv.na|` -> check `recv.__tp_complete__` to learn the receiver's type
        int end = off;
        while (end < (int)req.text.size() && isIdentChar(req.text[end])) end++;
        std::string patched = req.text.substr(0, start) + kPlaceholder + req.text.substr(end);
        runAnalysis(req, patched, out, false, true, start);
    } else {
        runAnalysis(req, req.text, out, false, false, start);
    }
    return out;
}

} // namespace tp
