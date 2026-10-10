// compiler.cpp - static type checking and bytecode generation.
//
// A single walk over the AST checks types and emits bytecode at the same
// time. Top-level declarations are hoisted first so functions and classes can
// reference each other regardless of order.
#include "compiler.h"
#include "parser.h"

#include <algorithm>
#include <climits>

namespace tp {

// ===========================================================================
// Type helpers
// ===========================================================================
namespace T {

static TypeP make(TK k) {
    auto t = std::make_shared<Type>();
    t->k = k;
    return t;
}

TypeP error() { static TypeP t = make(TK::Error); return t; }
TypeP voidT() { static TypeP t = make(TK::Void); return t; }
TypeP any() { static TypeP t = make(TK::Any); return t; }
TypeP null() { static TypeP t = make(TK::Null); return t; }
TypeP integer() { static TypeP t = make(TK::Int); return t; }
TypeP floating() { static TypeP t = make(TK::Float); return t; }
TypeP boolean() { static TypeP t = make(TK::Bool); return t; }
TypeP str() { static TypeP t = make(TK::Str); return t; }
TypeP func() { static TypeP t = make(TK::Func); return t; }

TypeP list(TypeP elem) {
    auto t = std::make_shared<Type>();
    t->k = TK::List;
    t->a = std::move(elem);
    return t;
}

TypeP dict(TypeP key, TypeP val) {
    auto t = std::make_shared<Type>();
    t->k = TK::Dict;
    t->a = std::move(key);
    t->b = std::move(val);
    return t;
}

TypeP tuple(std::vector<TypeP> elems) {
    auto t = std::make_shared<Type>();
    t->k = TK::Tuple;
    t->elems = std::move(elems);
    return t;
}

TypeP sig(SigP s) {
    auto t = std::make_shared<Type>();
    t->k = TK::Sig;
    t->sig = std::move(s);
    return t;
}

TypeP cls(ClassInfo* c) {
    auto t = std::make_shared<Type>();
    t->k = TK::Class;
    t->cls = c;
    return t;
}

TypeP iface(InterfaceInfo* i) {
    auto t = std::make_shared<Type>();
    t->k = TK::Interface;
    t->iface = i;
    return t;
}

TypeP en(EnumInfo* e) {
    auto t = std::make_shared<Type>();
    t->k = TK::Enum;
    t->en = e;
    return t;
}

TypeP native(NativeClass* n) {
    auto t = std::make_shared<Type>();
    t->k = TK::Native;
    t->native = n;
    return t;
}

TypeP module(ModuleSymbols* m) {
    auto t = std::make_shared<Type>();
    t->k = TK::Module;
    t->mod = m;
    return t;
}

TypeP ns(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->k = TK::Namespace;
    t->nsName = name;
    return t;
}

TypeP nullable(TypeP t, bool n) {
    if (!t || t->nullable == n) return t;
    if (t->k == TK::Any || t->k == TK::Error || t->k == TK::Null || t->k == TK::Void) return t;
    auto c = std::make_shared<Type>(*t);
    c->nullable = n;
    return c;
}

TypeP withExt(TypeP t, int ext) {
    auto c = std::make_shared<Type>(*t);
    c->ext = ext;
    return c;
}

static const char* extName(int ext) {
    static const char* names[] = {"", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
                                  "f32", "f64"};
    return names[ext];
}

std::string name(const TypeP& t) {
    if (!t) return "?";
    std::string s;
    switch (t->k) {
    case TK::Error: return "<error>";
    case TK::Void: return "nothing";
    case TK::Any: return "any";
    case TK::Null: return "null";
    case TK::Int: s = t->ext ? extName(t->ext) : "int"; break;
    case TK::Float: s = t->ext ? extName(t->ext) : "float"; break;
    case TK::Bool: s = "bool"; break;
    case TK::Str: s = "str"; break;
    case TK::List: s = "list<" + name(t->a) + ">"; break;
    case TK::Dict: s = "dict<" + name(t->a) + ", " + name(t->b) + ">"; break;
    case TK::Tuple: {
        s = "(";
        for (size_t i = 0; i < t->elems.size(); i++) s += (i ? ", " : "") + name(t->elems[i]);
        s += ")";
        break;
    }
    case TK::Func: s = "func"; break;
    case TK::Sig: {
        s = t->sig->isCoroutine ? "coroutine(" : "func(";
        for (size_t i = 0; i < t->sig->params.size(); i++) {
            s += (i ? ", " : "") + name(t->sig->params[i]);
            if (t->sig->variadic && i + 1 == t->sig->params.size()) s += "...";
        }
        s += ")";
        if (t->sig->ret && t->sig->ret->k != TK::Void) s += " -> " + name(t->sig->ret);
        break;
    }
    case TK::Class: s = t->cls->name; break;
    case TK::Interface: s = t->iface->name; break;
    case TK::Enum: s = t->en->name; break;
    case TK::Native: s = t->native->aliases.empty() ? t->native->name : t->native->aliases[0]; break;
    case TK::Module: return "module '" + t->mod->key + "'";
    case TK::Namespace: return "namespace '" + t->nsName + "'";
    }
    if (t->nullable) s += "?";
    return s;
}

bool same(const TypeP& a, const TypeP& b) {
    if (!a || !b) return a == b;
    if (a->k != b->k || a->nullable != b->nullable) return false;
    switch (a->k) {
    case TK::Int: case TK::Float: return a->ext == b->ext;
    case TK::List: return same(a->a, b->a);
    case TK::Dict: return same(a->a, b->a) && same(a->b, b->b);
    case TK::Tuple:
        if (a->elems.size() != b->elems.size()) return false;
        for (size_t i = 0; i < a->elems.size(); i++) {
            if (!same(a->elems[i], b->elems[i])) return false;
        }
        return true;
    case TK::Sig: {
        const SigInfo& x = *a->sig;
        const SigInfo& y = *b->sig;
        if (x.params.size() != y.params.size() || x.variadic != y.variadic || x.required != y.required) return false;
        for (size_t i = 0; i < x.params.size(); i++) {
            if (!same(x.params[i], y.params[i])) return false;
        }
        return same(x.ret, y.ret);
    }
    case TK::Class: return a->cls == b->cls;
    case TK::Interface: return a->iface == b->iface;
    case TK::Enum: return a->en == b->en;
    case TK::Native: return a->native == b->native;
    case TK::Module: return a->mod == b->mod;
    case TK::Namespace: return a->nsName == b->nsName;
    default: return true;
    }
}

} // namespace T

static bool isNumeric(const TypeP& t) { return !t->nullable && (t->k == TK::Int || t->k == TK::Float); }
static bool isLoose(const TypeP& t) { return t->k == TK::Any || t->k == TK::Error; }

// ===========================================================================
// Compiler
// ===========================================================================
namespace {

struct Local {
    std::string name;
    TypeP type;
    int depth = 0;
    bool isConst = false;
    bool captured = false;
    bool used = false;
    bool isParam = false;
    Span span;
};

struct UpvalDesc {
    int index;
    bool isLocal;
    std::string name;
    TypeP type;
    bool isConst;
    Span span;
};

struct Loop {
    int scopeDepth;
    int tryDepth;
    std::vector<int> breaks;
    std::vector<int> continues;
    bool hasBreak = false;
};

struct FuncState {
    FuncState* enclosing = nullptr;
    Proto* proto = nullptr;
    std::vector<Local> locals;
    std::vector<UpvalDesc> upvals;
    int scopeDepth = 0;
    TypeP ret;                 // Void, a type, a tuple; null = untyped lambda (any)
    bool isCoroutine = false;
    bool isModuleMain = false;
    bool isCtor = false;
    ClassInfo* cls = nullptr;
    ClassInfo* staticCls = nullptr; // static method (or static initializer) of this class: statics, no `this`
    std::vector<Loop> loops;
    int tryDepth = 0;
    std::string name;
    std::unordered_map<std::string, int> strConsts;
    std::unordered_map<int64_t, int> intConsts;
    int lastOp = -1;     // start of the most recently emitted instruction (peephole)
    int lastTarget = -1; // most recent jump-target position (never fuse across it)
};

struct Res {
    enum K { None, Local, Upval, Global, Member, Builtin, Static } k = None;
    int index = -1;
    TypeP type;
    bool isConst = false;
    Span declSpan;
    int declFile = -1;
    ClassInfo* cls = nullptr;
    const ClassInfo::Field* field = nullptr;
    const ClassInfo::Method* method = nullptr;
    ModuleSymbols::Global* g = nullptr;
};

struct Fact {
    std::string name;
    TypeP type;
};

class Compiler {
public:
    Compiler(VM& vm, ModuleSymbols& syms, ImportResolver& resolver, std::vector<Diagnostic>& diags,
             const CompileOptions& opt)
        : vm_(vm), syms_(syms), resolver_(resolver), diags_(diags), opt_(opt), sink_(opt.sink) {}

    Proto* run() {
        FuncState main;
        main.proto = vm_.newProto();
        main.proto->name = "<main>";
        main.proto->fileId = syms_.fileId;
        main.isModuleMain = true;
        main.ret = T::voidT();
        main.name = "<main>";
        main.locals.push_back(Local {"", T::any(), 0, false, false, true, false, {}});
        fs_ = &main;
        narrow_.emplace_back();

        if (opt_.reload) {
            for (auto& n : opt_.existingGlobals) syms_.slotNames.push_back(n);
        }
        hoist();
        Span end = {0, 0};
        bool exits = false;
        bool warned = false;
        for (auto& s : syms_.program) {
            if (exits && !warned) {
                warning(s->span, "unreachable code", "this code will never run");
                warned = true;
            }
            if (topLevelStmt(s.get())) exits = true;
            end = s->span;
        }
        emitOp(OP_NIL, end);
        emitOp(OP_RETURN, end);
        main.proto->maxStack = computeMaxStack(main.proto);
        fs_ = nullptr;
        if (errors_ > 0) return nullptr;
        return main.proto;
    }

private:
    VM& vm_;
    ModuleSymbols& syms_;
    ImportResolver& resolver_;
    std::vector<Diagnostic>& diags_;
    const CompileOptions& opt_;
    FuncState* fs_ = nullptr;
    int errors_ = 0;
    std::vector<std::unordered_map<std::string, TypeP>> narrow_;
    struct ImportRef {
        ModuleSymbols* mod;
        int slot;
    };
    std::unordered_map<std::string, ImportRef> imports_;     // dotted path or alias -> module
    std::unordered_set<std::string> nativeImports_;           // `import engine.types` style no-ops
    std::unordered_map<std::string, SigP> sigCache_;

    // ----- language-server analysis (only active with a sink) -----
    AnalysisSink* sink_ = nullptr;
    std::vector<int> scopeEnds_; // end offset of the innermost scope (local visibility)

    void recordUse(Span sp, const std::string& detail, int declFile, Span declSpan) {
        if (!sink_) return;
        sink_->uses.push_back(AnalysisSink::Use {syms_.fileId, sp, detail, declFile, declSpan});
    }

    std::string sigText(const std::string& kw, const std::string& name, const SigInfo& s) {
        std::string r = (kw.empty() ? "" : kw + " ") + name + "(";
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

    void recordTypeUse(Span sp, const TypeP& t) {
        if (!sink_ || !t) return;
        switch (t->k) {
        case TK::Class:
            recordUse(sp, "class " + t->cls->name, t->cls->fileId, t->cls->decl ? t->cls->decl->nameSpan : Span {});
            break;
        case TK::Interface: recordUse(sp, "interface " + t->iface->name, t->iface->fileId, t->iface->nameSpan); break;
        case TK::Enum: recordUse(sp, "enum " + t->en->name, t->en->fileId, t->en->nameSpan); break;
        case TK::Native: recordUse(sp, "class " + t->native->name + " (native)", -1, {}); break;
        default: break;
        }
    }

    // Hover text and declaration of a resolved name.
    void recordRes(Span sp, const Res& r, const std::string& name) {
        if (!sink_ || r.k == Res::None) return;
        std::string d;
        int file = r.declFile;
        Span decl = r.declSpan;
        switch (r.k) {
        case Res::Local: case Res::Upval:
            if (r.type && r.type->k == TK::Sig) d = sigText("func", name, *r.type->sig);
            else if (r.k == Res::Local && fs_->locals[r.index].isParam) d = "(parameter) " + T::name(r.type) + " " + name;
            else d = std::string(r.isConst ? "const " : "local ") + T::name(r.type) + " " + name;
            break;
        case Res::Global:
            switch (r.g->kind) {
            case ModuleSymbols::Global::Func:
                d = sigText(r.g->type->sig->isCoroutine ? "coroutine" : "func", name, *r.g->type->sig);
                break;
            case ModuleSymbols::Global::Class:
                d = "class " + name;
                if (r.g->cls && r.g->cls->ctor && !r.g->cls->ctor->params.empty()) d = sigText("class", name, *r.g->cls->ctor);
                break;
            case ModuleSymbols::Global::Enum: d = "enum " + name; break;
            default: d = std::string(r.isConst ? "const " : "") + T::name(r.g->type ? r.g->type : T::any()) + " " + name;
            }
            break;
        case Res::Member:
            if (r.method) {
                d = sigText(r.method->sig->isCoroutine ? "coroutine" : "func", r.cls->name + "." + name, *r.method->sig);
                if (r.method->decl) decl = r.method->decl->nameSpan;
            } else {
                d = "(field) " + T::name(r.type) + " " + r.cls->name + "." + name;
            }
            break;
        case Res::Static:
            if (r.method) d = sigText("static func", r.cls->name + "." + name, *r.method->sig);
            else d = "(static) " + std::string(r.isConst ? "const " : "") + T::name(r.type) + " " + r.cls->name + "." + name;
            break;
        case Res::Builtin:
            file = -1;
            if (r.type && r.type->k == TK::Namespace) d = "namespace " + name;
            else if (r.type && r.type->k == TK::Sig) d = sigText("func", name, *r.type->sig);
            else d = T::name(r.type) + " " + name;
            break;
        default: break;
        }
        recordUse(sp, d, file, decl);
    }

    // ----- diagnostics -----
    void error(Span sp, const std::string& title, const std::string& label, std::vector<std::string> notes = {}) {
        Diagnostic d;
        d.sev = Diagnostic::Error;
        d.title = title;
        d.fileId = syms_.fileId;
        d.span = sp;
        d.label = label;
        d.notes = std::move(notes);
        diags_.push_back(d);
        errors_++;
    }

    void warning(Span sp, const std::string& title, const std::string& label) {
        Diagnostic d;
        d.sev = Diagnostic::Warning;
        d.title = title;
        d.fileId = syms_.fileId;
        d.span = sp;
        d.label = label;
        diags_.push_back(d);
    }

    int lineOf(int fileId, int offset) {
        if (fileId < 0 || fileId >= (int)vm_.sources.size()) return 0;
        int line, col;
        vm_.sources[fileId].lineCol(offset, line, col);
        return line;
    }

    // ----- emission -----
    Proto* proto() { return fs_->proto; }

    int emit(int32_t word, Span sp) {
        proto()->code.push_back(word);
        proto()->spans.push_back(sp);
        return (int)proto()->code.size() - 1;
    }

    void emitOp(Op op, Span sp) {
        fs_->lastOp = here();
        emit(op, sp);
    }
    void emitOp1(Op op, int32_t a, Span sp) {
        emitOp(op, sp);
        emit(a, sp);
    }
    void emitOp2(Op op, int32_t a, int32_t b, Span sp) {
        emitOp(op, sp);
        emit(a, sp);
        emit(b, sp);
    }

    // ----- peephole fusion -----
    // The last instruction may be merged with the next one only if nothing
    // jumps to the position between them.
    bool lastIs(Op op, int words) {
        return fs_->lastOp >= 0 && fs_->lastOp + words == here() && fs_->lastTarget != here() &&
               proto()->code[fs_->lastOp] == op;
    }

    int emitJump(Op op, Span sp) {
        if (op == OP_JUMP_IF_FALSE && fs_->lastOp >= 0 && fs_->lastTarget != here()) {
            // compare + branch -> one instruction
            int at = fs_->lastOp;
            int last = proto()->code[at];
            Op fused = OP__COUNT;
            if (at + 1 == here()) {
                switch (last) {
                case OP_LT: fused = OP_JLT; break;
                case OP_LE: fused = OP_JLE; break;
                case OP_GT: fused = OP_JGT; break;
                case OP_GE: fused = OP_JGE; break;
                case OP_EQ: fused = OP_JEQ; break;
                case OP_NE: fused = OP_JNE; break;
                case OP_NOT: fused = OP_JUMP_IF_TRUE; break;
                default: break;
                }
            } else if (at + 2 == here()) {
                switch (last) {
                case OP_LT_K: fused = OP_JLT_K; break;
                case OP_LE_K: fused = OP_JLE_K; break;
                case OP_GT_K: fused = OP_JGT_K; break;
                case OP_GE_K: fused = OP_JGE_K; break;
                case OP_EQ_K: fused = OP_JEQ_K; break;
                case OP_NE_K: fused = OP_JNE_K; break;
                default: break;
                }
            }
            if (fused != OP__COUNT) {
                proto()->code[at] = fused;
                return emit(0, proto()->spans[at]);
            }
        }
        emitOp(op, sp);
        return emit(0, sp);
    }

    // Jump offsets are relative to the word after the operand.
    void patchJump(int at) {
        proto()->code[at] = (int32_t)proto()->code.size() - (at + 1);
        fs_->lastTarget = here();
    }

    void patchJumpTo(int at, int target) {
        proto()->code[at] = target - (at + 1);
        if (target == here()) fs_->lastTarget = here();
    }

    // Marks the current position as a jump target (loop starts, continue points).
    int label() {
        fs_->lastTarget = here();
        return here();
    }

    void emitLoop(int target, Span sp) {
        emitOp(OP_LOOP, sp);
        int at = emit(0, sp);
        proto()->code[at] = (at + 1) - target;
    }

    int newCache() {
        proto()->caches.emplace_back();
        return (int)proto()->caches.size() - 1;
    }

    // Field read on an object whose static class is known: index + name guard.
    void emitFieldGet(int idx, const std::string& name, Span sp) {
        int k = strConst(name);
        if (lastIs(OP_GET_LOCAL, 2)) { // local.field -> one instruction
            int at = fs_->lastOp;
            proto()->code[at] = OP_GET_LFIELD;
            emit(idx, proto()->spans[at]);
            emit(k, proto()->spans[at]);
            return;
        }
        emitOp2(OP_GET_FIELD_I, idx, k, sp);
    }

    void emitFieldSet(int idx, const std::string& name, Span sp) {
        if (idx >= 0) emitOp2(OP_SET_FIELD_I, idx, strConst(name), sp);
        else emitOp1(OP_SET_FIELD, strConst(name), sp);
    }

    void emitInvoke(const std::string& name, int argc, Span sp, bool cached) {
        if (!cached) {
            emitOp2(OP_INVOKE, strConst(name), argc, sp);
            return;
        }
        emitOp(OP_INVOKE_C, sp);
        emit(strConst(name), sp);
        emit(argc, sp);
        emit(newCache(), sp);
    }

    // Static type of a side-effect-free expression made of literals, names and
    // arithmetic, without emitting code; null if the expression is anything else.
    TypeP peekType(Expr* e) {
        switch (e->k) {
        case EK::Int: return T::integer();
        case EK::Float: return T::floating();
        case EK::Str: return T::str();
        case EK::Ident: {
            Res r = resolve(e->name, false);
            if (r.k != Res::Local && r.k != Res::Upval && r.k != Res::Global) return nullptr;
            return r.type;
        }
        case EK::Binary: {
            if (e->op != Tok::Plus && e->op != Tok::Minus && e->op != Tok::Star) return nullptr;
            TypeP a = peekType(e->a.get());
            TypeP b = peekType(e->b.get());
            if (!a || !b || a->nullable || b->nullable) return nullptr;
            if (a->k == TK::Int && b->k == TK::Int && !a->ext && !b->ext) return T::integer();
            if (isNumeric(a) && isNumeric(b) && !a->ext && !b->ext) return T::floating();
            return nullptr;
        }
        default: return nullptr;
        }
    }

    int here() { return (int)proto()->code.size(); }

    int constant(const Value& v) {
        proto()->consts.push_back(v);
        return (int)proto()->consts.size() - 1;
    }

    int strConst(const std::string& s) {
        auto it = fs_->strConsts.find(s);
        if (it != fs_->strConsts.end()) return it->second;
        int k = constant(vm_.strv(s));
        fs_->strConsts[s] = k;
        return k;
    }

    int intConst(int64_t i) {
        auto it = fs_->intConsts.find(i);
        if (it != fs_->intConsts.end()) return it->second;
        int k = constant(Value::integer(i));
        fs_->intConsts[i] = k;
        return k;
    }

    // ----- scopes -----
    void beginScope() {
        fs_->scopeDepth++;
        narrow_.emplace_back();
    }

    void popLocalsCode(int depth, Span sp) {
        int count = 0;
        int firstCaptured = -1;
        for (int i = (int)fs_->locals.size() - 1; i >= 0 && fs_->locals[i].depth > depth; i--) {
            count++;
            if (fs_->locals[i].captured) firstCaptured = i;
        }
        if (firstCaptured >= 0) emitOp1(OP_CLOSE_UPVALS, firstCaptured, sp);
        if (count == 1) emitOp(OP_POP, sp);
        else if (count > 1) emitOp1(OP_POPN, count, sp);
    }

    void endScope(Span sp) {
        popLocalsCode(fs_->scopeDepth - 1, sp);
        while (!fs_->locals.empty() && fs_->locals.back().depth >= fs_->scopeDepth) {
            Local& l = fs_->locals.back();
            if (!l.used && !l.isParam && !l.name.empty() && l.name[0] != '_' && l.name[0] != '$' && vm_.warnings) {
                warning(l.span, "unused variable", "'" + l.name + "' is declared but never used");
            }
            fs_->locals.pop_back();
        }
        fs_->scopeDepth--;
        narrow_.pop_back();
    }

    int addLocal(const std::string& name, TypeP type, Span sp, bool isConst = false) {
        for (int i = (int)fs_->locals.size() - 1; i >= 0; i--) {
            Local& l = fs_->locals[i];
            if (l.depth < fs_->scopeDepth) break;
            if (l.name == name && !name.empty() && name[0] != '$') {
                error(sp, "duplicate declaration", "'" + name + "' is already declared in this scope",
                      {"previous declaration on line " + std::to_string(lineOf(syms_.fileId, l.span.start))});
                break;
            }
        }
        Local l;
        l.name = name;
        l.type = type;
        l.depth = fs_->scopeDepth;
        l.isConst = isConst;
        l.span = sp;
        fs_->locals.push_back(l);
        // A new declaration masks narrowing of an outer variable with the same name.
        narrow_.back().erase(name);
        if (sink_ && !name.empty() && name[0] != '$') {
            AnalysisSink::ScopeVar v;
            v.name = name;
            v.type = type;
            v.isConst = isConst;
            v.fileId = syms_.fileId;
            v.visibleFrom = sp.end;
            v.visibleTo = scopeEnds_.empty() ? INT_MAX : scopeEnds_.back();
            sink_->locals.push_back(v);
            if (type && type->k == TK::Sig) recordUse(sp, sigText("func", name, *type->sig), syms_.fileId, sp);
            else recordUse(sp, std::string(isConst ? "const " : "local ") + T::name(type) + " " + name, syms_.fileId, sp);
        }
        return (int)fs_->locals.size() - 1;
    }

    int hiddenLocal(const std::string& name) {
        Local l;
        l.name = name;
        l.type = T::any();
        l.depth = fs_->scopeDepth;
        l.used = true;
        fs_->locals.push_back(l);
        return (int)fs_->locals.size() - 1;
    }

    bool atModuleTop() const { return fs_->isModuleMain && fs_->scopeDepth == 0; }

    // ----- narrowing (null checks) -----
    TypeP narrowed(const std::string& name) {
        for (int i = (int)narrow_.size() - 1; i >= 0; i--) {
            auto it = narrow_[i].find(name);
            if (it != narrow_[i].end()) return it->second;
        }
        return nullptr;
    }

    void forget(const std::string& name) {
        for (auto& f : narrow_) f.erase(name);
    }

    // Facts known about identifiers when `e` evaluates to `whenTrue`.
    void facts(Expr* e, bool whenTrue, std::vector<Fact>& out) {
        if (e->k == EK::Unary && e->op == Tok::Bang) {
            facts(e->a.get(), !whenTrue, out);
            return;
        }
        if (e->k == EK::Logical) {
            if (e->op == Tok::AndAnd && whenTrue) {
                facts(e->a.get(), true, out);
                facts(e->b.get(), true, out);
            } else if (e->op == Tok::OrOr && !whenTrue) {
                facts(e->a.get(), false, out);
                facts(e->b.get(), false, out);
            }
            return;
        }
        if (e->k == EK::Binary && (e->op == Tok::Ne || e->op == Tok::Eq)) {
            Expr* id = nullptr;
            if (e->a->k == EK::Ident && e->b->k == EK::Null) id = e->a.get();
            if (e->b->k == EK::Ident && e->a->k == EK::Null) id = e->b.get();
            if (!id) return;
            bool nonNull = (e->op == Tok::Ne) == whenTrue;
            if (!nonNull) return;
            Res r = resolve(id->name, false);
            if (r.k == Res::Local || r.k == Res::Upval || r.k == Res::Global) {
                TypeP t = narrowed(id->name);
                if (!t) t = r.type;
                if (t && t->nullable) out.push_back({id->name, T::nullable(t, false)});
            }
        }
    }

    void applyFacts(const std::vector<Fact>& fs) {
        for (auto& f : fs) narrow_.back()[f.name] = f.type;
    }

    // ===================================================================
    // Type resolution
    // ===================================================================
    int extFromName(const std::string& n) {
        static const std::unordered_map<std::string, int> m = {
            {"int8", ExtI8}, {"int16", ExtI16}, {"int32", ExtI32}, {"int64", ExtI64}, {"uint8", ExtU8},
            {"uint16", ExtU16}, {"uint32", ExtU32}, {"uint64", ExtU64}, {"byte", ExtU8}, {"f32", ExtF32},
            {"f64", ExtF64}};
        auto it = m.find(n);
        return it == m.end() ? 0 : it->second;
    }

    ImportRef* importFor(const std::string& dotted) {
        auto it = imports_.find(dotted);
        return it == imports_.end() ? nullptr : &it->second;
    }

    TypeP typeFromGlobal(ModuleSymbols::Global& g) {
        if (g.kind == ModuleSymbols::Global::Class) return T::cls(g.cls);
        if (g.kind == ModuleSymbols::Global::Interface) return T::iface(g.iface);
        if (g.kind == ModuleSymbols::Global::Enum) return T::en(g.en);
        return nullptr;
    }

    TypeP resolveType(TypeExpr* te) {
        if (!te) return T::any();
        TypeP t;
        const std::string& n = te->name;
        if (te->kind == TypeExpr::List) {
            t = T::list(resolveType(te->args[0].get()));
        } else if (te->kind == TypeExpr::Dict) {
            TypeP k = resolveType(te->args[0].get());
            if (!(isLoose(k) || k->k == TK::Str || k->k == TK::Int || k->k == TK::Float || k->k == TK::Bool ||
                  k->k == TK::Enum || k->k == TK::Class || k->k == TK::Native)) {
                error(te->args[0]->span, "invalid dict key type", "'" + T::name(k) + "' cannot be used as a dict key");
            }
            t = T::dict(k, resolveType(te->args[1].get()));
        } else if (n == "int") {
            t = T::integer();
        } else if (n == "float") {
            t = T::floating();
        } else if (n == "bool") {
            t = T::boolean();
        } else if (n == "str") {
            t = T::str();
        } else if (n == "any") {
            t = T::any();
        } else if (n == "func") {
            t = T::func();
        } else if (n == "void" || n == "null") {
            error(te->span, "invalid type",
                  n == "void" ? "there is no 'void' type - omit '->' for functions that return nothing"
                              : "'null' is a value, not a type - mark a type nullable with '?' (e.g. 'int?')");
            return T::error();
        } else if (int ext = extFromName(n)) {
            if (!syms_.extTypes) {
                error(te->span, "extended type not enabled", "'" + n + "' requires 'enable ExtType' at the top of the file");
                return T::error();
            }
            t = T::withExt(ext >= ExtF32 ? T::floating() : T::integer(), ext);
        } else {
            t = resolveNamedType(n, te->span);
            if (!t) return T::error();
        }
        if (te->nullable) {
            if (t->k == TK::Any) {
                error(te->span, "invalid type", "'any' already allows null; write 'any' instead of 'any?'");
            }
            t = T::nullable(t, true);
        }
        return t;
    }

    TypeP resolveNamedType(const std::string& n, Span sp) {
        size_t dot = n.rfind('.');
        if (dot != std::string::npos) {
            std::string modPath = n.substr(0, dot);
            std::string member = n.substr(dot + 1);
            ImportRef* ir = importFor(modPath);
            if (!ir) {
                error(sp, "unknown type", "no imported module named '" + modPath + "'");
                return nullptr;
            }
            auto it = ir->mod->globals.find(member);
            TypeP t = it == ir->mod->globals.end() ? nullptr : typeFromGlobal(it->second);
            if (!t) {
                error(sp, "unknown type", "module '" + modPath + "' has no type named '" + member + "'");
                return nullptr;
            }
            recordTypeUse(sp, t);
            return t;
        }
        auto it = syms_.globals.find(n);
        if (it != syms_.globals.end()) {
            if (TypeP t = typeFromGlobal(it->second)) {
                recordTypeUse(sp, t);
                return t;
            }
        }
        if (NativeClass* nc = vm_.nativeTypeByName(n)) {
            recordTypeUse(sp, T::native(nc));
            return T::native(nc);
        }
        std::vector<std::string> notes;
        if (n == "entity" || n == "vec2" || n == "vec3" || n == "color") {
            notes.push_back("game types are provided by the host engine (taipan_register_class), not the core language");
        }
        error(sp, "unknown type", "cannot find type '" + n + "' in this scope", notes);
        return nullptr;
    }

    TypeP returnTypeOf(const std::vector<TypeExprP>& rets) {
        if (rets.empty()) return T::voidT();
        if (rets.size() == 1) return resolveType(rets[0].get());
        std::vector<TypeP> el;
        for (auto& r : rets) el.push_back(resolveType(r.get()));
        return T::tuple(el);
    }

    SigP sigOf(const std::vector<Param>& params, const std::vector<TypeExprP>& rets, bool isCoroutine) {
        auto s = std::make_shared<SigInfo>();
        bool seenDefault = false;
        for (size_t i = 0; i < params.size(); i++) {
            const Param& p = params[i];
            TypeP t = resolveType(p.type.get());
            if (p.variadic) {
                if (i + 1 != params.size()) {
                    error(p.span, "invalid variadic parameter", "'...' parameter must be the last parameter");
                }
                if (p.def) error(p.def->span, "invalid default", "a variadic parameter cannot have a default");
                s->variadic = true;
            } else if (p.def) {
                seenDefault = true;
            } else if (seenDefault) {
                error(p.span, "parameter order", "required parameter '" + p.name + "' follows a parameter with a default value");
            } else {
                s->required++;
            }
            s->params.push_back(t);
            s->names.push_back(p.name);
        }
        s->ret = returnTypeOf(rets);
        s->isCoroutine = isCoroutine;
        return s;
    }

    SigP sigFromString(const std::string& text) {
        auto it = sigCache_.find(text);
        if (it != sigCache_.end()) return it->second;
        AnalysisSink* savedSink = sink_;
        sink_ = nullptr;
        struct Restore {
            AnalysisSink*& slot;
            AnalysisSink* value;
            ~Restore() { slot = value; }
        } restore {sink_, savedSink};
        SigParse sp = parseSignature(text);
        SigP s;
        if (sp.ok) {
            s = std::make_shared<SigInfo>();
            int errBefore = errors_;
            size_t diagBefore = diags_.size();
            for (size_t i = 0; i < sp.params.size(); i++) {
                s->params.push_back(resolveType(sp.params[i].get()));
                if (!sp.optional[i] && !(sp.variadic && i + 1 == sp.params.size())) s->required = (int)i + 1;
            }
            s->variadic = sp.variadic;
            s->ret = returnTypeOf(sp.rets);
            if (errors_ != errBefore) { // unknown types in a host signature: fall back to untyped
                errors_ = errBefore;
                diags_.resize(diagBefore);
                s = nullptr;
            }
        }
        sigCache_[text] = s;
        return s;
    }

    // Static type of a registered native: a signature "(...) -> T" or a plain type "float".
    TypeP nativeType(const std::string& key) {
        auto si = vm_.nativeSigs.find(key);
        if (si == vm_.nativeSigs.end() || si->second.empty()) return T::any();
        if (si->second[0] == '(') {
            SigP s = sigFromString(si->second);
            return s ? T::sig(s) : T::any();
        }
        return typeFromString(si->second);
    }

    TypeP typeFromString(const std::string& text) {
        SigP s = sigFromString("(" + text + ")");
        if (!s || s->params.size() != 1) return T::any();
        return s->params[0];
    }

    // ===================================================================
    // Hoisting of top-level declarations
    // ===================================================================
    int globalSlot(const std::string& name) {
        for (size_t i = 0; i < syms_.slotNames.size(); i++) {
            if (syms_.slotNames[i] == name) return (int)i;
        }
        syms_.slotNames.push_back(name);
        return (int)syms_.slotNames.size() - 1;
    }

    ModuleSymbols::Global* declareGlobal(const std::string& name, ModuleSymbols::Global::Kind kind, Span sp) {
        auto it = syms_.globals.find(name);
        if (it != syms_.globals.end()) {
            error(sp, "duplicate declaration", "'" + name + "' is already declared in this module",
                  {"previous declaration on line " + std::to_string(lineOf(syms_.fileId, it->second.span.start))});
            return &it->second;
        }
        ModuleSymbols::Global g;
        g.kind = kind;
        g.name = name;
        g.slot = globalSlot(name);
        g.span = sp;
        auto& ref = syms_.globals[name];
        ref = g;
        return &ref;
    }

    void hoist() {
        // 1. enable flags, imports, type names
        for (auto& sp : syms_.program) {
            Stmt* s = sp.get();
            if (s->k == SK::Enable) {
                if (s->name == "ExtType") syms_.extTypes = true;
                else error(s->nameSpan, "unknown feature", "no feature named '" + s->name + "' (available: ExtType)");
            } else if (s->k == SK::Import) {
                hoistImport(s);
            } else if (s->k == SK::Class) {
                auto ci = std::make_unique<ClassInfo>();
                ci->name = s->cls->name;
                ci->qual = syms_.key + ":" + ci->name;
                ci->decl = s->cls.get();
                ci->mod = &syms_;
                ci->fileId = syms_.fileId;
                auto* g = declareGlobal(ci->name, ModuleSymbols::Global::Class, s->cls->nameSpan);
                g->cls = ci.get();
                g->isConst = true;
                syms_.classes.push_back(std::move(ci));
            } else if (s->k == SK::Interface) {
                auto ii = std::make_unique<InterfaceInfo>();
                ii->name = s->iface->name;
                ii->qual = syms_.key + ":" + ii->name;
                ii->nameSpan = s->iface->nameSpan;
                ii->fileId = syms_.fileId;
                auto* g = declareGlobal(ii->name, ModuleSymbols::Global::Interface, s->iface->nameSpan);
                g->iface = ii.get();
                g->isConst = true;
                syms_.ifaces.push_back(std::move(ii));
            } else if (s->k == SK::Enum) {
                auto ei = std::make_unique<EnumInfo>();
                ei->name = s->en->name;
                ei->qual = syms_.key + ":" + ei->name;
                std::unordered_set<std::string> seen;
                for (size_t i = 0; i < s->en->members.size(); i++) {
                    if (!seen.insert(s->en->members[i]).second) {
                        error(s->en->memberSpans[i], "duplicate enum member", "'" + s->en->members[i] + "' appears twice");
                    }
                }
                ei->members = s->en->members;
                ei->memberSpans = s->en->memberSpans;
                ei->nameSpan = s->en->nameSpan;
                ei->fileId = syms_.fileId;
                auto* g = declareGlobal(ei->name, ModuleSymbols::Global::Enum, s->en->nameSpan);
                g->en = ei.get();
                g->isConst = true;
                g->type = T::any();
                syms_.enums.push_back(std::move(ei));
            }
        }
        // 2. interface signatures
        for (auto& sp : syms_.program) {
            if (sp->k != SK::Interface) continue;
            InterfaceInfo* ii = syms_.globals[sp->iface->name].iface;
            if (!ii) continue;
            for (auto& m : sp->iface->methods) {
                if (ii->methods.count(m->name)) error(m->nameSpan, "duplicate method", "'" + m->name + "' is declared twice");
                ii->methods[m->name] = sigOf(m->params, m->rets, m->isCoroutine);
                ii->methodSpans[m->name] = m->nameSpan;
            }
        }
        // 3. class members
        for (auto& sp : syms_.program) {
            if (sp->k != SK::Class) continue;
            ClassInfo* ci = syms_.globals[sp->cls->name].cls;
            if (ci) hoistClass(ci);
        }
        // 4. functions and variables
        for (auto& sp : syms_.program) {
            Stmt* s = sp.get();
            if (s->k == SK::Func) {
                auto* g = declareGlobal(s->func->name, ModuleSymbols::Global::Func, s->func->nameSpan);
                g->type = T::sig(sigOf(s->func->params, s->func->rets, s->func->isCoroutine));
                g->isConst = true;
            } else if (s->k == SK::VarDecl) {
                for (auto& v : s->vars) {
                    auto* g = declareGlobal(v.name, ModuleSymbols::Global::Var, v.span);
                    g->type = resolveType(v.type.get());
                    g->isConst = s->isConst;
                }
            }
        }
    }

    void hoistImport(Stmt* s) {
        std::string root = s->name.substr(0, s->name.find('.'));
        bool isNativeNs = false;
        for (auto& kv : vm_.builtins->members) {
            if (kv.first->s == root && kv.second.isObj() && kv.second.o->type == OT::Namespace) isNativeNs = true;
        }
        if (isNativeNs) { // e.g. `import engine.types`: host bindings are always present
            nativeImports_.insert(s->name);
            return;
        }
        ModuleSymbols* m = resolver_.resolveImport(s->name, syms_.fileId, s->nameSpan, diags_);
        if (!m) {
            errors_++;
            return;
        }
        std::string hidden = "@" + m->key;
        auto it = syms_.globals.find(hidden);
        int slot;
        if (it == syms_.globals.end()) {
            ModuleSymbols::Global g;
            g.kind = ModuleSymbols::Global::Import;
            g.name = hidden;
            g.slot = globalSlot(hidden);
            g.mod = m;
            g.isConst = true;
            g.type = T::module(m);
            g.span = s->nameSpan;
            syms_.globals[hidden] = g;
            slot = g.slot;
        } else {
            slot = it->second.slot;
        }
        std::string bind = s->alias.empty() ? s->name : s->alias;
        if (!s->alias.empty() && syms_.globals.count(s->alias)) {
            error(s->span, "duplicate declaration", "alias '" + s->alias + "' collides with an existing name");
        }
        imports_[bind] = ImportRef {m, slot};
        if (sink_) sink_->importNames.push_back(bind.substr(0, bind.find('.')));
    }

    bool memberExists(ClassInfo* ci, const std::string& name) {
        return ci->fieldIdx.count(name) || ci->methods.count(name) || ci->staticIdx.count(name) ||
               ci->staticMethods.count(name);
    }

    void hoistClass(ClassInfo* ci) {
        ClassDecl* cd = ci->decl;
        for (auto& f : cd->fields) {
            if (f.isStatic) {
                if (memberExists(ci, f.name)) {
                    error(f.span, "duplicate member", "'" + f.name + "' is already a member of class '" + ci->name + "'");
                    continue;
                }
                ClassInfo::Field fi;
                fi.name = f.name;
                fi.type = resolveType(f.type.get());
                fi.isConst = f.isConst;
                fi.isPrivate = f.isPrivate;
                fi.span = f.span;
                fi.decl = &f;
                fi.zero = zeroKindOf(fi.type);
                bool hasZero = fi.type->nullable || isLoose(fi.type) || fi.type->k == TK::Func || fi.zero != ZNull ||
                               fi.type->k == TK::Enum;
                if (!f.init && !hasZero) {
                    error(f.span, "missing initializer", "static field '" + f.name + "' of type '" + T::name(fi.type) +
                                                             "' has no zero value - give it an initializer or make it nullable");
                }
                ci->staticIdx[f.name] = (int)ci->statics.size();
                ci->statics.push_back(fi);
                continue;
            }
            if (ci->fieldIdx.count(f.name)) {
                error(f.span, "duplicate field", "'" + f.name + "' is declared twice in class '" + ci->name + "'");
                continue;
            }
            ClassInfo::Field fi;
            fi.name = f.name;
            fi.type = resolveType(f.type.get());
            fi.isConst = f.isConst;
            fi.isPrivate = f.isPrivate;
            fi.span = f.span;
            fi.decl = &f;
            fi.zero = zeroKindOf(fi.type);
            ci->fieldIdx[f.name] = (int)ci->fields.size();
            ci->fields.push_back(fi);
        }
        for (auto& m : cd->methods) {
            if (memberExists(ci, m->name)) {
                std::string shown = m->isOperator ? "operator " + m->opSymbol : m->name;
                error(m->nameSpan, "duplicate member", "'" + shown + "' is already a member of class '" + ci->name + "'");
                continue;
            }
            ClassInfo::Method mi;
            mi.name = m->name;
            mi.sig = sigOf(m->params, m->rets, m->isCoroutine);
            mi.isPrivate = m->isPrivate;
            mi.decl = m.get();
            if (m->isStatic) {
                if (m->name == "init") error(m->nameSpan, "invalid constructor", "'init' cannot be static");
                ci->staticMethods[m->name] = mi;
                continue;
            }
            if (m->isOperator) {
                const std::string& sym = m->opSymbol;
                bool unary = m->name == "op_neg";
                bool cmp = sym == "==" || sym == "<" || sym == "<=" || sym == ">" || sym == ">=";
                if (!unary && m->params.size() != 1) {
                    error(m->nameSpan, "invalid operator", "operator '" + sym + "' takes exactly one parameter (the right-hand side)");
                }
                if (cmp && mi.sig->ret->k != TK::Bool) {
                    error(m->nameSpan, "invalid operator", "operator '" + sym + "' must return 'bool'");
                } else if (!cmp && mi.sig->ret->k == TK::Void) {
                    error(m->nameSpan, "invalid operator", "operator '" + sym + "' must return a value (add '-> Type')");
                }
                if (m->isCoroutine) error(m->nameSpan, "invalid operator", "an operator cannot be a coroutine");
            }
            if (m->name == "init") {
                if (!m->rets.empty()) error(m->nameSpan, "invalid constructor", "'init' cannot declare a return type");
                if (m->isCoroutine) error(m->nameSpan, "invalid constructor", "'init' cannot be a coroutine");
                ci->ctor = mi.sig;
            }
            ci->methods[m->name] = mi;
        }
        if (!ci->ctor) {
            ci->ctor = std::make_shared<SigInfo>();
            ci->ctor->ret = T::voidT();
        }
        for (size_t i = 0; i < cd->implements.size(); i++) {
            const std::string& iname = cd->implements[i];
            TypeP t = resolveNamedType(iname, cd->implSpans[i]);
            if (!t) continue;
            if (t->k != TK::Interface) {
                error(cd->implSpans[i], "not an interface", "'" + iname + "' is " + T::name(t) + ", not an interface",
                      {"Taipan has no class inheritance; share behaviour through interfaces and composition"});
                continue;
            }
            ci->ifaces.push_back(t->iface);
        }
        // interface conformance is checked once all classes are known
        for (size_t i = 0; i < ci->ifaces.size(); i++) {
            InterfaceInfo* ii = ci->ifaces[i];
            for (auto& kv : ii->methods) {
                auto it = ci->methods.find(kv.first);
                if (it == ci->methods.end()) {
                    error(cd->nameSpan, "missing interface method",
                          "class '" + ci->name + "' does not implement '" + kv.first + "' from '" + ii->name + "'",
                          {"required: func " + kv.first + T::name(T::sig(kv.second)).substr(4)});
                } else if (!T::same(T::sig(it->second.sig), T::sig(kv.second))) {
                    error(it->second.decl->nameSpan, "interface signature mismatch",
                          "'" + kv.first + "' must have signature " + T::name(T::sig(kv.second)) + " to implement '" +
                              ii->name + "'");
                } else if (it->second.isPrivate) {
                    error(it->second.decl->nameSpan, "private interface method",
                          "'" + kv.first + "' implements '" + ii->name + "' and must be public");
                }
            }
        }
    }

    int zeroKindOf(const TypeP& t) {
        if (t->nullable) return ZNull;
        switch (t->k) {
        case TK::Int: return ZInt;
        case TK::Float: return ZFloat;
        case TK::Bool: return ZBool;
        case TK::Str: return ZStr;
        case TK::List: return ZList;
        case TK::Dict: return ZDict;
        default: return ZNull;
        }
    }

    // ===================================================================
    // Name resolution
    // ===================================================================
    int resolveLocal(FuncState* f, const std::string& name) {
        for (int i = (int)f->locals.size() - 1; i >= 0; i--) {
            if (f->locals[i].name == name) return i;
        }
        return -1;
    }

    int addUpval(FuncState* f, int index, bool isLocal, const std::string& name, TypeP type, bool isConst, Span sp) {
        for (size_t i = 0; i < f->upvals.size(); i++) {
            if (f->upvals[i].index == index && f->upvals[i].isLocal == isLocal) return (int)i;
        }
        f->upvals.push_back(UpvalDesc {index, isLocal, name, type, isConst, sp});
        return (int)f->upvals.size() - 1;
    }

    int resolveUpval(FuncState* f, const std::string& name) {
        if (!f->enclosing) return -1;
        int l = resolveLocal(f->enclosing, name);
        if (l >= 0) {
            Local& loc = f->enclosing->locals[l];
            loc.captured = true;
            loc.used = true;
            return addUpval(f, l, true, name, loc.type, loc.isConst, loc.span);
        }
        int u = resolveUpval(f->enclosing, name);
        if (u >= 0) {
            UpvalDesc& d = f->enclosing->upvals[u];
            return addUpval(f, u, false, name, d.type, d.isConst, d.span);
        }
        return -1;
    }

    ClassInfo* enclosingClass() {
        for (FuncState* f = fs_; f; f = f->enclosing) {
            if (f->cls) return f->cls;
        }
        return nullptr;
    }

    // The class whose static members are visible here (instance or static method).
    ClassInfo* staticContext() {
        for (FuncState* f = fs_; f; f = f->enclosing) {
            if (f->cls) return f->cls;
            if (f->staticCls) return f->staticCls;
        }
        return nullptr;
    }

    // Loads a class object of this module (or a direct import) onto the stack.
    void emitStaticClass(ClassInfo* ci, Span sp) {
        if (!emitTypeObject(ci->name, ci->qual, sp)) {
            error(sp, "unknown class", "class '" + ci->name + "' is not reachable here (import its module)");
            emitOp(OP_NIL, sp);
        }
    }

    Res resolve(const std::string& name, bool markUsed = true) {
        Res r;
        int l = resolveLocal(fs_, name);
        if (l >= 0) {
            Local& loc = fs_->locals[l];
            if (markUsed) loc.used = true;
            r.k = Res::Local;
            r.index = l;
            r.type = loc.type;
            r.isConst = loc.isConst;
            r.declSpan = loc.span;
            r.declFile = syms_.fileId;
        } else {
            int u = resolveUpval(fs_, name);
            if (u >= 0) {
                r.k = Res::Upval;
                r.index = u;
                r.type = fs_->upvals[u].type;
                r.isConst = fs_->upvals[u].isConst;
                r.declSpan = fs_->upvals[u].span;
                r.declFile = syms_.fileId;
            } else if (ClassInfo* ci = enclosingClass(); ci && (ci->fieldIdx.count(name) || ci->methods.count(name))) {
                r.k = Res::Member;
                r.cls = ci;
                auto fi = ci->fieldIdx.find(name);
                if (fi != ci->fieldIdx.end()) {
                    r.field = &ci->fields[fi->second];
                    r.type = r.field->type;
                    r.isConst = r.field->isConst;
                    r.declSpan = r.field->span;
                } else {
                    r.method = &ci->methods[name];
                    r.type = T::sig(r.method->sig);
                    r.isConst = true;
                }
                r.declFile = ci->fileId;
            } else if (ClassInfo* sc = staticContext(); sc && (sc->staticIdx.count(name) || sc->staticMethods.count(name))) {
                r.k = Res::Static;
                r.cls = sc;
                auto si = sc->staticIdx.find(name);
                if (si != sc->staticIdx.end()) {
                    r.field = &sc->statics[si->second];
                    r.type = r.field->type;
                    r.isConst = r.field->isConst;
                    r.declSpan = r.field->span;
                } else {
                    r.method = &sc->staticMethods[name];
                    r.type = T::sig(r.method->sig);
                    r.isConst = true;
                    if (r.method->decl) r.declSpan = r.method->decl->nameSpan;
                }
                r.declFile = sc->fileId;
            } else {
                auto it = syms_.globals.find(name);
                if (it != syms_.globals.end() && it->second.kind != ModuleSymbols::Global::Import &&
                    it->second.kind != ModuleSymbols::Global::Interface) {
                    r.k = Res::Global;
                    r.g = &it->second;
                    r.index = it->second.slot;
                    r.type = it->second.type ? it->second.type : T::any();
                    if (it->second.kind == ModuleSymbols::Global::Class) r.type = T::any();
                    r.isConst = it->second.isConst;
                    r.declSpan = it->second.span;
                    r.declFile = syms_.fileId;
                } else {
                    for (auto& kv : vm_.builtins->members) {
                        if (kv.first->s == name) {
                            r.k = Res::Builtin;
                            r.isConst = true;
                            if (kv.second.isObj() && kv.second.o->type == OT::Namespace) {
                                r.type = T::ns(name);
                            } else {
                                r.type = nativeType(name);
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (r.k == Res::Local || r.k == Res::Upval || r.k == Res::Global) {
            if (TypeP n = narrowed(name)) r.type = n;
        }
        return r;
    }

    void emitThis(Span sp) {
        Res r = resolve("this");
        if (r.k == Res::Local) emitOp1(OP_GET_LOCAL, r.index, sp);
        else if (r.k == Res::Upval) emitOp1(OP_GET_UPVAL, r.index, sp);
        else error(sp, "invalid 'this'", "'this' can only be used inside class methods");
    }

    void emitLoad(const Res& r, const std::string& name, Span sp) {
        switch (r.k) {
        case Res::Local: emitOp1(OP_GET_LOCAL, r.index, sp); break;
        case Res::Upval: emitOp1(OP_GET_UPVAL, r.index, sp); break;
        case Res::Global: emitOp1(OP_GET_GLOBAL, r.index, sp); break;
        case Res::Member:
            emitThis(sp);
            if (r.field) emitFieldGet(r.cls->fieldIdx[name], name, sp);
            else emitOp1(OP_GET_FIELD, strConst(name), sp);
            break;
        case Res::Builtin: emitOp2(OP_GET_BUILTIN_C, strConst(name), newCache(), sp); break;
        case Res::Static:
            emitStaticClass(r.cls, sp);
            emitOp1(OP_GET_FIELD, strConst(name), sp);
            break;
        default: break;
        }
    }

    // Store the value on top of the stack (Member stores expect `this` below it).
    void emitStore(const Res& r, const std::string& name, Span sp) {
        switch (r.k) {
        case Res::Local: emitOp1(OP_SET_LOCAL, r.index, sp); break;
        case Res::Upval: emitOp1(OP_SET_UPVAL, r.index, sp); break;
        case Res::Global: emitOp1(OP_SET_GLOBAL, r.index, sp); break;
        case Res::Member: emitFieldSet(r.field ? r.cls->fieldIdx[name] : -1, name, sp); break;
        case Res::Static: emitOp1(OP_SET_FIELD, strConst(name), sp); break;
        default: break;
        }
    }

    void constError(const Res& r, const std::string& name, Span sp) {
        int line = lineOf(r.declFile, r.declSpan.start);
        std::string what = r.method || (r.g && r.g->kind != ModuleSymbols::Global::Var) ? "a declaration" : "const";
        if (r.g && r.g->kind == ModuleSymbols::Global::Func) what = "a function";
        if (r.g && r.g->kind == ModuleSymbols::Global::Class) what = "a class";
        if (r.g && r.g->kind == ModuleSymbols::Global::Enum) what = "an enum";
        if (r.method) what = "a method";
        if (r.k == Res::Builtin) {
            error(sp, "cannot assign to builtin", "'" + name + "' is a built-in and cannot be reassigned");
            return;
        }
        error(sp, "cannot assign to constant",
              "'" + name + "' was declared as " + what + (line ? " on line " + std::to_string(line) : ""));
    }

    // Module path prefix (`enemies.ai` in `enemies.ai.updateAll()`), if any.
    ImportRef* modulePath(Expr* e) {
        std::string path;
        Expr* cur = e;
        std::vector<std::string> parts;
        while (cur->k == EK::Member) {
            parts.push_back(cur->name);
            cur = cur->a.get();
        }
        if (cur->k != EK::Ident) return nullptr;
        Res r = resolve(cur->name, false);
        if (r.k != Res::None && r.k != Res::Builtin) return nullptr;
        path = cur->name;
        for (int i = (int)parts.size() - 1; i >= 0; i--) path += "." + parts[i];
        return importFor(path);
    }

    std::string dottedName(Expr* e) {
        if (e->k == EK::Ident) return e->name;
        if (e->k == EK::Member) {
            std::string base = dottedName(e->a.get());
            return base.empty() ? "" : base + "." + e->name;
        }
        return "";
    }

    // Is `e` a reference to a class (for constructor calls)?
    ClassInfo* classRef(Expr* e, int* slot, ImportRef** viaModule) {
        *viaModule = nullptr;
        if (e->k == EK::Ident) {
            Res r = resolve(e->name, false);
            if (r.k == Res::Global && r.g->kind == ModuleSymbols::Global::Class) {
                *slot = r.g->slot;
                return r.g->cls;
            }
            return nullptr;
        }
        if (e->k == EK::Member) {
            ImportRef* ir = modulePath(e->a.get());
            if (!ir) return nullptr;
            auto it = ir->mod->globals.find(e->name);
            if (it != ir->mod->globals.end() && it->second.kind == ModuleSymbols::Global::Class) {
                *slot = ir->slot;
                *viaModule = ir;
                return it->second.cls;
            }
        }
        return nullptr;
    }

    EnumInfo* enumRef(Expr* e) {
        if (e->k == EK::Ident) {
            Res r = resolve(e->name, false);
            if (r.k == Res::Global && r.g->kind == ModuleSymbols::Global::Enum) return r.g->en;
            return nullptr;
        }
        if (e->k == EK::Member) {
            ImportRef* ir = modulePath(e->a.get());
            if (!ir) return nullptr;
            auto it = ir->mod->globals.find(e->name);
            if (it != ir->mod->globals.end() && it->second.kind == ModuleSymbols::Global::Enum) return it->second.en;
        }
        return nullptr;
    }

    // ===================================================================
    // Coercion / assignability
    // ===================================================================
    std::string rtSpec(const TypeP& t) {
        std::string s;
        switch (t->k) {
        case TK::Int: s = "int"; break;
        case TK::Float: s = "float"; break;
        case TK::Bool: s = "bool"; break;
        case TK::Str: s = "str"; break;
        case TK::List: s = "list"; break;
        case TK::Dict: s = "dict"; break;
        case TK::Tuple: s = "tuple"; break;
        case TK::Func: case TK::Sig: s = "func"; break;
        case TK::Class: s = "C:" + t->cls->qual; break;
        case TK::Interface: s = "I:" + t->iface->qual; break;
        case TK::Enum: s = "E:" + t->en->qual; break;
        case TK::Native: s = "N:" + t->native->name; break;
        default: return "";
        }
        if (t->nullable) s += "?";
        return s;
    }

    int extConv(int ext) {
        if (ext >= ExtI8 && ext <= ExtU64) return CV_I8 + (ext - ExtI8);
        if (ext == ExtF32) return CV_F32;
        return 0;
    }

    bool assignableBase(const TypeP& target, const TypeP& src) {
        if (isLoose(target) || isLoose(src)) return true;
        switch (target->k) {
        case TK::Int: case TK::Float: return src->k == TK::Int || src->k == TK::Float;
        case TK::Bool: case TK::Str: return src->k == target->k;
        case TK::List:
            if (src->k != TK::List) return false;
            return isLoose(target->a) || isLoose(src->a) || T::same(target->a, src->a);
        case TK::Dict:
            return src->k == TK::Dict && (T::same(target->a, src->a) || isLoose(target->a) || isLoose(src->a)) &&
                   (T::same(target->b, src->b) || isLoose(target->b) || isLoose(src->b));
        case TK::Tuple:
            if (src->k != TK::Tuple || src->elems.size() != target->elems.size()) return false;
            for (size_t i = 0; i < src->elems.size(); i++) {
                if (!assignableBase(target->elems[i], src->elems[i])) return false;
            }
            return true;
        case TK::Func: case TK::Sig: return src->k == TK::Func || src->k == TK::Sig;
        case TK::Class: return src->k == TK::Class && src->cls == target->cls;
        case TK::Interface:
            if (src->k == TK::Interface) return src->iface == target->iface;
            if (src->k == TK::Class) {
                return std::find(src->cls->ifaces.begin(), src->cls->ifaces.end(), target->iface) != src->cls->ifaces.end();
            }
            return false;
        case TK::Enum: return src->k == TK::Enum && src->en == target->en;
        case TK::Native: return src->k == TK::Native && src->native == target->native;
        default: return false;
        }
    }

    // Checks that a value of type `src` (on top of the stack) can be stored in
    // `target`, emitting runtime checks / conversions where needed.
    void coerce(const TypeP& target, const TypeP& src, Span sp) {
        if (!target || !src) return;
        if (target->k == TK::Error || src->k == TK::Error) return;
        if (src->k == TK::Void) {
            error(sp, "no value", "this expression does not produce a value");
            return;
        }
        if (target->k == TK::Any) return;
        if (src->k == TK::Any) {
            std::string spec = rtSpec(target);
            if (!spec.empty()) emitOp1(OP_CHECK_TYPE, strConst(spec), sp);
            if (int cv = extConv(target->ext)) emitOp1(OP_CONVERT, cv, sp);
            return;
        }
        if (src->k == TK::Null) {
            if (!target->nullable) {
                error(sp, "type mismatch", "cannot use 'null' as non-nullable type '" + T::name(target) + "'",
                      {"mark the type nullable to allow null: '" + T::name(target) + "?'"});
            }
            return;
        }
        if (src->k == TK::Tuple && target->k != TK::Tuple) {
            error(sp, "type mismatch", "expected '" + T::name(target) + "', found multiple values " + T::name(src),
                  {"unpack them with a multiple declaration, e.g. 'int a, int b = f()'"});
            return;
        }
        if (!assignableBase(target, src)) {
            error(sp, "type mismatch", "expected '" + T::name(target) + "', found '" + T::name(src) + "'");
            return;
        }
        if (src->nullable && !target->nullable) {
            error(sp, "possible null reference",
                  "expected '" + T::name(target) + "', found nullable '" + T::name(src) + "' - check for null before use");
            return;
        }
        if (target->k == TK::Int && src->k == TK::Float) {
            if (vm_.warnings) warning(sp, "implicit conversion", "'" + T::name(src) + "' is truncated to '" + T::name(target) + "'");
            emitOp1(OP_CONVERT, CV_TO_INT, sp);
        } else if (target->k == TK::Float && src->k == TK::Int) {
            emitOp1(OP_CONVERT, CV_TO_FLOAT, sp);
        }
        if ((target->k == TK::Int || target->k == TK::Float) && target->ext != src->ext) {
            if (int cv = extConv(target->ext)) emitOp1(OP_CONVERT, cv, sp);
        }
    }

    bool coerceEmits(const TypeP& target, const TypeP& src) {
        if (!target || !src || isLoose(target) || src->k == TK::Error) return false;
        if (src->k == TK::Any) return true;
        if ((target->k == TK::Int || target->k == TK::Float) && (src->k == TK::Int || src->k == TK::Float)) {
            return target->k != src->k || target->ext != src->ext;
        }
        return false;
    }

    void checkLiteralRange(const TypeP& target, Expr* e) {
        if (!target || target->k != TK::Int || !target->ext || !e || e->k != EK::Int) return;
        int64_t v = e->ival;
        int64_t lo = 0, hi = 0;
        bool check = true;
        switch (target->ext) {
        case ExtI8: lo = -128; hi = 127; break;
        case ExtI16: lo = -32768; hi = 32767; break;
        case ExtI32: lo = INT32_MIN; hi = INT32_MAX; break;
        case ExtU8: lo = 0; hi = 255; break;
        case ExtU16: lo = 0; hi = 65535; break;
        case ExtU32: lo = 0; hi = 4294967295LL; break;
        case ExtU64: lo = 0; hi = INT64_MAX; break;
        default: check = false;
        }
        if (check && (v < lo || v > hi)) {
            error(e->span, "literal out of range",
                  "'" + std::to_string(v) + "' does not fit in '" + T::name(target) + "' (" + std::to_string(lo) + ".." +
                      std::to_string(hi) + ")");
        }
    }

    // Emits the zero value of `t`; returns false if the type has none.
    bool emitZero(const TypeP& t, Span sp) {
        if (t->nullable || t->k == TK::Any || t->k == TK::Error || t->k == TK::Func) {
            emitOp(OP_NIL, sp);
            return true;
        }
        switch (t->k) {
        case TK::Int: emitOp1(OP_CONST, intConst(0), sp); return true;
        case TK::Float: emitOp1(OP_CONST, constant(Value::number(0.0)), sp); return true;
        case TK::Bool: emitOp(OP_FALSE, sp); return true;
        case TK::Str: emitOp1(OP_CONST, strConst(""), sp); return true;
        case TK::List: emitOp1(OP_BUILD_LIST, 0, sp); return true;
        case TK::Dict: emitOp1(OP_BUILD_DICT, 0, sp); return true;
        case TK::Enum: {
            if (t->en->members.empty()) break;
            if (!emitTypeObject(t->en->name, t->en->qual, sp)) break;
            emitOp1(OP_GET_FIELD, strConst(t->en->members[0]), sp);
            return true;
        }
        default: break;
        }
        emitOp(OP_NIL, sp);
        return false;
    }

    // Loads a class/enum object by qualified name (current module or a direct import).
    bool emitTypeObject(const std::string& name, const std::string& qual, Span sp) {
        std::string modKey = qual.substr(0, qual.rfind(':'));
        if (modKey == syms_.key) {
            auto it = syms_.globals.find(name);
            if (it == syms_.globals.end()) return false;
            emitOp1(OP_GET_GLOBAL, it->second.slot, sp);
            return true;
        }
        for (auto& kv : imports_) {
            if (kv.second.mod->key == modKey) {
                emitOp1(OP_GET_GLOBAL, kv.second.slot, sp);
                emitOp1(OP_GET_FIELD, strConst(name), sp);
                return true;
            }
        }
        return false;
    }

    // ===================================================================
    // Statements
    // ===================================================================
    bool isDeclaration(Stmt* s) {
        return s->k == SK::Func || s->k == SK::Class || s->k == SK::Interface || s->k == SK::Enum ||
               s->k == SK::Import || s->k == SK::Enable || s->k == SK::VarDecl;
    }

    // Top-level statement of a module. Returns true if it never completes.
    bool topLevelStmt(Stmt* s) {
        if (isDeclaration(s)) return stmt(s);
        // Hot reload re-runs declarations only, so running scripts keep their state.
        int skip = emitJump(OP_JUMP_IF_RELOADING, s->span);
        bool r = stmt(s);
        patchJump(skip);
        return r;
    }

    bool block(Block* b, const std::vector<Fact>* facts = nullptr) {
        scopeEnds_.push_back(b->span.end);
        beginScope();
        if (facts) applyFacts(*facts);
        bool exits = false;
        bool warned = false;
        for (auto& s : b->stmts) {
            if (exits && !warned && vm_.warnings) {
                warning(s->span, "unreachable code", "this code will never run");
                warned = true;
            }
            if (stmt(s.get())) exits = true;
        }
        endScope({b->span.end - 1, b->span.end});
        scopeEnds_.pop_back();
        return exits;
    }

    bool stmt(Stmt* s) {
        switch (s->k) {
        case SK::Expr: {
            TypeP t = expr(s->e.get());
            (void)t;
            emitOp(OP_POP, s->span);
            return false;
        }
        case SK::VarDecl: varDecl(s); return false;
        case SK::Assign: assign(s); return false;
        case SK::IncDec: incDec(s); return false;
        case SK::Block: return block(s->body.get());
        case SK::If: return ifStmt(s);
        case SK::While: return whileStmt(s);
        case SK::ForIn: return forIn(s);
        case SK::ForC: return forC(s);
        case SK::Match: return matchStmt(s);
        case SK::Break: case SK::Continue: return breakContinue(s);
        case SK::Return: returnStmt(s); return true;
        case SK::Try: return tryStmt(s);
        case SK::Throw:
            expr(s->e.get());
            emitOp(OP_THROW, s->span);
            return true;
        case SK::Func: funcStmt(s); return false;
        case SK::Class: classStmt(s); return false;
        case SK::Interface:
            if (!atModuleTop()) error(s->span, "misplaced declaration", "interfaces must be declared at the top level");
            if (sink_) {
                recordUse(s->iface->nameSpan, "interface " + s->iface->name, syms_.fileId, s->iface->nameSpan);
                for (auto& m : s->iface->methods) {
                    InterfaceInfo* ii = syms_.globals.count(s->iface->name) ? syms_.globals[s->iface->name].iface : nullptr;
                    if (ii && ii->methods.count(m->name)) {
                        recordUse(m->nameSpan, sigText("func", ii->name + "." + m->name, *ii->methods[m->name]), syms_.fileId,
                                  m->nameSpan);
                    }
                }
            }
            return false;
        case SK::Enum: enumStmt(s); return false;
        case SK::Import:
            if (!atModuleTop()) {
                error(s->span, "misplaced import", "imports must be at the top level of a file");
                return false;
            }
            if (!nativeImports_.count(s->name)) {
                std::string bind = s->alias.empty() ? s->name : s->alias;
                ImportRef* ir = importFor(bind);
                if (ir) {
                    emitOp1(OP_IMPORT, strConst(ir->mod->key), s->span);
                    emitOp1(OP_SET_GLOBAL, ir->slot, s->span);
                }
            }
            return false;
        case SK::Enable:
            if (!atModuleTop()) error(s->span, "misplaced directive", "'enable' must be at the top level of a file");
            return false;
        }
        return false;
    }

    TypeP declType(VarTarget& v) {
        if (atModuleTop()) {
            auto it = syms_.globals.find(v.name);
            if (it != syms_.globals.end() && it->second.type) return it->second.type;
        }
        return resolveType(v.type.get());
    }

    void varDecl(Stmt* s) {
        bool top = atModuleTop();
        if (top && sink_) {
            for (auto& v : s->vars) {
                TypeP gt = syms_.globals.count(v.name) ? syms_.globals[v.name].type : nullptr;
                recordUse(v.span, std::string(s->isConst ? "const " : "") + T::name(gt ? gt : T::any()) + " " + v.name,
                          syms_.fileId, v.span);
            }
        }
        if (s->vars.size() == 1) {
            VarTarget& v = s->vars[0];
            TypeP t = declType(v);
            int skip = -1;
            if (top) {
                // on hot reload an existing global keeps its value
                emitOp(OP_JUMP_IF_DEFINED, s->span);
                emit(syms_.globals[v.name].slot, s->span);
                skip = emit(0, s->span);
            }
            if (s->e) {
                TypeP vt = expr(s->e.get(), t);
                coerce(t, vt, s->e->span);
                checkLiteralRange(t, s->e.get());
            } else if (!emitZero(t, v.span)) {
                error(v.span, "missing initializer",
                      "'" + T::name(t) + "' has no zero value - initialize '" + v.name + "' or declare it as '" +
                          T::name(t) + "?'");
            }
            if (top) {
                emitOp1(OP_SET_GLOBAL, syms_.globals[v.name].slot, s->span);
                patchJump(skip);
            } else {
                addLocal(v.name, t, v.span, s->isConst);
            }
            return;
        }
        // Destructuring: int a, int b = f()
        std::vector<TypeP> types;
        for (auto& v : s->vars) types.push_back(declType(v));
        int skip = -1;
        if (top) {
            emitOp(OP_JUMP_IF_DEFINED, s->span);
            emit(syms_.globals[s->vars[0].name].slot, s->span);
            skip = emit(0, s->span);
        }
        TypeP vt = expr(s->e.get(), T::tuple(types));
        std::vector<TypeP> srcTypes(types.size(), T::any());
        if (vt->k == TK::Tuple) {
            if (vt->elems.size() != types.size()) {
                error(s->e->span, "wrong number of values",
                      "expected " + std::to_string(types.size()) + " values, found " + std::to_string(vt->elems.size()));
            } else {
                srcTypes = vt->elems;
            }
        } else if (!isLoose(vt)) {
            error(s->e->span, "type mismatch",
                  "expected " + std::to_string(types.size()) + " values, found a single '" + T::name(vt) + "'");
        }
        emitOp1(OP_UNPACK, (int)types.size(), s->span);
        if (top) {
            for (int i = (int)types.size() - 1; i >= 0; i--) {
                coerce(types[i], srcTypes[i], s->vars[i].span);
                emitOp1(OP_SET_GLOBAL, syms_.globals[s->vars[i].name].slot, s->span);
            }
            patchJump(skip);
        } else {
            std::vector<int> slots;
            for (size_t i = 0; i < types.size(); i++) slots.push_back(addLocal(s->vars[i].name, types[i], s->vars[i].span, s->isConst));
            for (size_t i = 0; i < types.size(); i++) {
                if (coerceEmits(types[i], srcTypes[i]) || !assignableBase(types[i], srcTypes[i]) ||
                    (srcTypes[i]->nullable && !types[i]->nullable)) {
                    emitOp1(OP_GET_LOCAL, slots[i], s->vars[i].span);
                    coerce(types[i], srcTypes[i], s->vars[i].span);
                    emitOp1(OP_SET_LOCAL, slots[i], s->vars[i].span);
                }
            }
        }
    }

    void assign(Stmt* s) {
        Expr* tgt = s->target.get();
        Tok op = s->op;
        Tok bop = op == Tok::PlusEq ? Tok::Plus : op == Tok::MinusEq ? Tok::Minus : op == Tok::StarEq ? Tok::Star
                : op == Tok::SlashEq ? Tok::Slash : op == Tok::PercentEq ? Tok::Percent : Tok::End;
        if (tgt->k == EK::Ident) {
            Res r = resolve(tgt->name, bop != Tok::End);
            recordRes(tgt->span, r, tgt->name);
            if (r.k == Res::None) {
                error(tgt->span, "unknown identifier", "cannot find '" + tgt->name + "' in this scope");
                expr(s->e.get());
                emitOp(OP_POP, s->span);
                return;
            }
            if (r.isConst || r.k == Res::Builtin) constError(r, tgt->name, tgt->span);
            TypeP declared = r.type;
            if (r.k == Res::Local) declared = fs_->locals[r.index].type;
            else if (r.k == Res::Upval) declared = fs_->upvals[r.index].type;
            else if (r.k == Res::Global) declared = r.g->type ? r.g->type : T::any();
            // `local += simple` / `local -= simple`: update the slot in place
            if (r.k == Res::Local && (bop == Tok::Plus || bop == Tok::Minus) && !declared->nullable && !declared->ext) {
                TypeP pt = peekType(s->e.get());
                bool ok = pt && ((declared->k == TK::Int && pt->k == TK::Int) ||
                                 (declared->k == TK::Float && isNumeric(pt)) || (declared->k == TK::Str && bop == Tok::Plus));
                if (ok) {
                    TypeP rt = expr(s->e.get());
                    TypeP vt = binaryResult(bop, r.type, rt, s->span, tgt->span, s->e->span);
                    coerce(declared, vt, s->e->span);
                    if (lastIs(OP_CONST, 2)) {
                        int at = fs_->lastOp;
                        int k = proto()->code[at + 1];
                        proto()->code[at] = bop == Tok::Plus ? OP_ADD_LOCAL_K : OP_SUB_LOCAL_K;
                        proto()->code[at + 1] = r.index;
                        emit(k, proto()->spans[at]);
                    } else {
                        emitOp1(bop == Tok::Plus ? OP_ADD_LOCAL : OP_SUB_LOCAL, r.index, s->span);
                    }
                    return;
                }
            }
            if (r.k == Res::Member) emitThis(tgt->span);
            else if (r.k == Res::Static) emitStaticClass(r.cls, tgt->span);
            TypeP vt;
            if (bop != Tok::End) {
                if (r.k == Res::Member) {
                    emitOp(OP_DUP, tgt->span);
                    if (r.field) emitFieldGet(r.cls->fieldIdx[tgt->name], tgt->name, tgt->span);
                    else emitOp1(OP_GET_FIELD, strConst(tgt->name), tgt->span);
                } else if (r.k == Res::Static) {
                    emitOp(OP_DUP, tgt->span);
                    emitOp1(OP_GET_FIELD, strConst(tgt->name), tgt->span);
                } else {
                    emitLoad(r, tgt->name, tgt->span);
                }
                TypeP rt = expr(s->e.get());
                vt = binaryResult(bop, r.type, rt, s->span, tgt->span, s->e->span);
                emitBinary(bop, s->span);
            } else {
                vt = expr(s->e.get(), declared);
            }
            coerce(declared, vt, s->e->span);
            checkLiteralRange(declared, s->e.get());
            emitStore(r, tgt->name, tgt->span);
            if (r.k != Res::Member && r.k != Res::Static) {
                if (vt->nullable || vt->k == TK::Null || vt->k == TK::Any) forget(tgt->name);
                else if (declared->nullable && vt->k != TK::Error) narrow_.back()[tgt->name] = T::nullable(declared, false);
            }
            return;
        }
        if (tgt->k == EK::Member) {
            lastFieldIdx_ = -1;
            TypeP ft = memberTarget(tgt);
            int fieldIdx = lastFieldIdx_;
            if (bop != Tok::End) {
                emitOp(OP_DUP, tgt->span);
                if (fieldIdx >= 0) emitFieldGet(fieldIdx, tgt->name, tgt->a->span);
                else emitOp1(OP_GET_FIELD, strConst(tgt->name), tgt->a->span);
                TypeP rt = expr(s->e.get());
                TypeP vt = binaryResult(bop, ft, rt, s->span, tgt->span, s->e->span);
                emitBinary(bop, s->span);
                coerce(ft, vt, s->e->span);
            } else {
                TypeP vt = expr(s->e.get(), ft);
                coerce(ft, vt, s->e->span);
                checkLiteralRange(ft, s->e.get());
            }
            emitFieldSet(fieldIdx, tgt->name, tgt->a->span);
            return;
        }
        if (tgt->k == EK::Index) {
            TypeP ct = expr(tgt->a.get());
            TypeP it = expr(tgt->b.get());
            TypeP et = indexType(ct, it, tgt, true);
            if (bop != Tok::End) {
                emitOp(OP_DUP2, tgt->span);
                emitOp(OP_GET_INDEX, tgt->span);
                TypeP rt = expr(s->e.get());
                TypeP vt = binaryResult(bop, et, rt, s->span, tgt->span, s->e->span);
                emitBinary(bop, s->span);
                coerce(et, vt, s->e->span);
            } else {
                TypeP vt = expr(s->e.get(), et);
                coerce(et, vt, s->e->span);
            }
            emitOp(OP_SET_INDEX, tgt->span);
            return;
        }
        error(tgt->span, "invalid assignment target", "cannot assign to this expression");
    }

    int lastFieldIdx_ = -1; // set by memberTarget for a field of a statically known class

    // Pushes the object of a member assignment target and returns the field type.
    TypeP memberTarget(Expr* tgt) {
        {
            int cslot;
            ImportRef* cvia;
            if (ClassInfo* ci = classRef(tgt->a.get(), &cslot, &cvia)) {
                emitOp1(OP_GET_GLOBAL, cslot, tgt->a->span);
                if (cvia) emitOp1(OP_GET_FIELD, strConst(ci->name), tgt->a->span);
                auto si = ci->staticIdx.find(tgt->name);
                if (si == ci->staticIdx.end()) {
                    error(tgt->nameSpan, "unknown static field", "class '" + ci->name + "' has no static field '" + tgt->name + "'");
                    return T::error();
                }
                const ClassInfo::Field& f = ci->statics[si->second];
                recordUse(tgt->nameSpan, "(static) " + T::name(f.type) + " " + ci->name + "." + f.name, ci->fileId, f.span);
                if (f.isPrivate && staticContext() != ci) {
                    error(tgt->nameSpan, "private field", "'" + tgt->name + "' is private to class '" + ci->name + "'");
                }
                if (f.isConst) {
                    error(tgt->nameSpan, "cannot assign to constant",
                          "'" + tgt->name + "' was declared as const on line " + std::to_string(lineOf(ci->fileId, f.span.start)));
                }
                return f.type;
            }
        }
        if (ImportRef* ir = modulePath(tgt->a.get())) {
            emitOp1(OP_GET_GLOBAL, ir->slot, tgt->a->span);
            recordModuleMember(tgt->nameSpan, ir, tgt->name);
            auto it = ir->mod->globals.find(tgt->name);
            if (it == ir->mod->globals.end()) {
                error(tgt->nameSpan, "unknown member", "module '" + ir->mod->key + "' has no member '" + tgt->name + "'");
                return T::error();
            }
            if (it->second.isConst) {
                error(tgt->nameSpan, "cannot assign to constant", "'" + tgt->name + "' in module '" + ir->mod->key + "' is not assignable");
            }
            return it->second.type ? it->second.type : T::any();
        }
        TypeP ot = expr(tgt->a.get());
        recordMember(tgt->nameSpan, ot, tgt->name);
        if (isLoose(ot)) return ot;
        if (ot->nullable) {
            nullError(tgt->a.get(), ot);
            ot = T::nullable(ot, false);
        }
        if (ot->k == TK::Class) {
            auto fi = ot->cls->fieldIdx.find(tgt->name);
            if (fi == ot->cls->fieldIdx.end()) {
                if (ot->cls->methods.count(tgt->name)) {
                    error(tgt->nameSpan, "cannot assign to method", "'" + tgt->name + "' is a method of '" + ot->cls->name + "'");
                } else {
                    error(tgt->nameSpan, "unknown field", "class '" + ot->cls->name + "' has no field '" + tgt->name + "'");
                }
                return T::error();
            }
            const ClassInfo::Field& f = ot->cls->fields[fi->second];
            lastFieldIdx_ = fi->second;
            if (f.isPrivate && enclosingClass() != ot->cls) {
                error(tgt->nameSpan, "private field", "'" + tgt->name + "' is private to class '" + ot->cls->name + "'");
            }
            if (f.isConst) {
                error(tgt->nameSpan, "cannot assign to constant",
                      "'" + tgt->name + "' was declared as const on line " + std::to_string(lineOf(ot->cls->fileId, f.span.start)));
            }
            return f.type;
        }
        if (ot->k == TK::Native && ot->native->isValue) {
            error(tgt->nameSpan, "value types are immutable",
                  "'" + T::name(ot) + "' values cannot be changed - build a new one instead",
                  {"for example: v = " + ot->native->name + "(...) or v = v + delta"});
            return T::error();
        }
        if (ot->k == TK::Native) {
            auto pi = ot->native->props.find(tgt->name);
            if (pi == ot->native->props.end()) {
                error(tgt->nameSpan, "unknown property", "'" + T::name(ot) + "' has no property '" + tgt->name + "'");
                return T::error();
            }
            if (!pi->second.writable) error(tgt->nameSpan, "read-only property", "'" + tgt->name + "' cannot be assigned");
            return typeFromString(pi->second.type);
        }
        error(tgt->a->span, "invalid assignment target", "cannot assign fields on a value of type '" + T::name(ot) + "'");
        return T::error();
    }

    void incDec(Stmt* s) {
        // x++ is x += 1
        Stmt fake(SK::Assign);
        fake.span = s->span;
        fake.op = s->op == Tok::PlusPlus ? Tok::PlusEq : Tok::MinusEq;
        fake.target = std::move(s->target);
        fake.e = std::make_unique<Expr>(EK::Int);
        fake.e->ival = 1;
        fake.e->span = {s->span.end - 2, s->span.end};
        assign(&fake);
        s->target = std::move(fake.target);
    }

    void condition(Expr* e) {
        TypeP t = expr(e, T::boolean());
        if (!isLoose(t) && !(t->k == TK::Bool && !t->nullable)) {
            error(e->span, "type mismatch", "expected 'bool', found '" + T::name(t) + "'",
                  t->nullable ? std::vector<std::string> {"compare explicitly: 'x != null'"} : std::vector<std::string> {});
        }
    }

    bool ifStmt(Stmt* s) {
        condition(s->e.get());
        int elseJump = emitJump(OP_JUMP_IF_FALSE, s->e->span);
        std::vector<Fact> whenTrue, whenFalse;
        facts(s->e.get(), true, whenTrue);
        facts(s->e.get(), false, whenFalse);
        bool thenExits = block(s->body.get(), &whenTrue);
        if (!s->elseBody) {
            patchJump(elseJump);
            if (thenExits) applyFacts(whenFalse); // `if x == null { return }` narrows the rest
            return false;
        }
        int endJump = emitJump(OP_JUMP, s->span);
        patchJump(elseJump);
        bool elseExits = block(s->elseBody.get(), &whenFalse);
        patchJump(endJump);
        if (thenExits && !elseExits) applyFacts(whenFalse);
        if (elseExits && !thenExits) applyFacts(whenTrue);
        return thenExits && elseExits;
    }

    bool whileStmt(Stmt* s) {
        int start = label();
        condition(s->e.get());
        int exit = emitJump(OP_JUMP_IF_FALSE, s->e->span);
        fs_->loops.push_back(Loop {fs_->scopeDepth, fs_->tryDepth, {}, {}});
        std::vector<Fact> whenTrue;
        facts(s->e.get(), true, whenTrue);
        block(s->body.get(), &whenTrue);
        Loop loop = fs_->loops.back();
        fs_->loops.pop_back();
        for (int c : loop.continues) patchJumpTo(c, start);
        emitLoop(start, s->span);
        patchJump(exit);
        for (int b : loop.breaks) patchJump(b);
        bool infinite = s->e->k == EK::Bool && s->e->bval;
        return infinite && !loop.hasBreak;
    }

    bool isRangeCall(Expr* e) {
        if (e->k != EK::Call || e->a->k != EK::Ident || e->a->name != "range") return false;
        if (e->args.empty() || e->args.size() > 3) return false;
        Res r = resolve("range", false);
        return r.k == Res::Builtin;
    }

    void declareLoopVar(VarTarget& v, const TypeP& elemT, Span sp) {
        TypeP t = resolveType(v.type.get());
        int slot = addLocal(v.name, t, v.span);
        if (!isLoose(elemT) && !(assignableBase(t, elemT) && (!elemT->nullable || t->nullable))) {
            error(v.span, "type mismatch", "loop variable is '" + T::name(t) + "' but the items are '" + T::name(elemT) + "'");
            return;
        }
        if (coerceEmits(t, elemT)) {
            emitOp1(OP_GET_LOCAL, slot, sp);
            coerce(t, elemT, sp);
            emitOp1(OP_SET_LOCAL, slot, sp);
        } else if (t->k == TK::Int && elemT->k == TK::Float) {
            emitOp1(OP_GET_LOCAL, slot, sp);
            coerce(t, elemT, sp);
            emitOp1(OP_SET_LOCAL, slot, sp);
        }
    }

    bool forIn(Stmt* s) {
        scopeEnds_.push_back(s->span.end);
        beginScope();
        Expr* it = s->e.get();
        int loopStart, exitJump, base;
        std::vector<TypeP> elemTypes;
        if (isRangeCall(it) && s->vars.size() == 1) {
            base = (int)fs_->locals.size();
            std::vector<ExprP>& a = it->args;
            auto intArg = [&](Expr* e) {
                TypeP t = expr(e, T::integer());
                if (!isLoose(t) && t->k != TK::Int) {
                    error(e->span, "type mismatch", "range() expects 'int', found '" + T::name(t) + "'");
                } else {
                    coerce(T::integer(), t, e->span);
                }
            };
            if (a.size() == 1) {
                emitOp1(OP_CONST, intConst(0), it->span);
                intArg(a[0].get());
            } else {
                intArg(a[0].get());
                intArg(a[1].get());
            }
            if (a.size() == 3) intArg(a[2].get());
            else emitOp1(OP_CONST, intConst(1), it->span);
            hiddenLocal("$i");
            hiddenLocal("$end");
            hiddenLocal("$step");
            TypeP vt = resolveType(s->vars[0].type.get());
            if (vt->k == TK::Int && !vt->nullable && !vt->ext) {
                // One instruction per iteration: RANGE_LOOP steps, tests and
                // writes the loop variable's slot directly.
                emitOp(OP_NIL, it->span);
                int var = addLocal(s->vars[0].name, vt, s->vars[0].span);
                emitOp(OP_RANGE_INIT, it->span);
                emit(base, it->span);
                emit(var, it->span);
                int exitJ = emit(0, it->span);
                fs_->loops.push_back(Loop {fs_->scopeDepth, fs_->tryDepth, {}, {}});
                int bodyStart = label();
                beginScope();
                for (auto& st : s->body->stmts) stmt(st.get());
                endScope({s->body->span.end - 1, s->body->span.end});
                Loop loop = fs_->loops.back();
                fs_->loops.pop_back();
                int cont = label();
                for (int c : loop.continues) patchJumpTo(c, cont);
                // a closure that captured the loop variable keeps this iteration's value
                if (fs_->locals[var].captured) emitOp1(OP_CLOSE_UPVALS, var, s->span);
                emitOp(OP_RANGE_LOOP, s->span);
                emit(base, s->span);
                emit(var, s->span);
                int at = emit(0, s->span);
                proto()->code[at] = (at + 1) - bodyStart;
                patchJump(exitJ);
                for (int b : loop.breaks) patchJump(b);
                endScope(s->span);
                scopeEnds_.pop_back();
                return false;
            }
            loopStart = label();
            emitOp(OP_RANGE_NEXT, it->span);
            emit(base, it->span);
            exitJump = emit(0, it->span);
            elemTypes.push_back(T::integer());
        } else {
            TypeP ct = expr(it);
            base = (int)fs_->locals.size();
            hiddenLocal("$coll");
            emitOp1(OP_CONST, intConst(0), it->span);
            hiddenLocal("$idx");
            if (ct->nullable && !isLoose(ct)) nullError(it, ct);
            TypeP c = T::nullable(ct, false);
            size_t n = s->vars.size();
            switch (c->k) {
            case TK::List:
                if (n == 1) elemTypes = {c->a};
                else elemTypes = {T::integer(), c->a};
                break;
            case TK::Dict:
                if (n == 1) elemTypes = {c->a};
                else elemTypes = {c->a, c->b};
                break;
            case TK::Str:
                if (n == 1) elemTypes = {T::str()};
                else elemTypes = {T::integer(), T::str()};
                break;
            case TK::Tuple: case TK::Any: case TK::Error:
                elemTypes.assign(n, T::any());
                break;
            default:
                error(it->span, "not iterable", "cannot iterate over a value of type '" + T::name(c) + "'",
                      {"iterate over a list, dict, str, or range(n)"});
                elemTypes.assign(n, T::error());
            }
            loopStart = label();
            emitOp(OP_ITER_NEXT, it->span);
            emit(base, it->span);
            emit((int)n, it->span);
            exitJump = emit(0, it->span);
        }
        fs_->loops.push_back(Loop {fs_->scopeDepth, fs_->tryDepth, {}, {}});
        // loop variables + body in their own scope (fresh per iteration for closures)
        beginScope();
        for (size_t i = 0; i < s->vars.size(); i++) declareLoopVar(s->vars[i], elemTypes[i], s->vars[i].span);
        bool exits = false;
        for (auto& st : s->body->stmts) {
            if (stmt(st.get())) exits = true;
        }
        (void)exits;
        endScope({s->body->span.end - 1, s->body->span.end});
        Loop loop = fs_->loops.back();
        fs_->loops.pop_back();
        int cont = label();
        for (int c : loop.continues) patchJumpTo(c, cont);
        if (isRangeCall(it) && s->vars.size() == 1) emitOp1(OP_RANGE_STEP, base, s->span);
        emitLoop(loopStart, s->span);
        patchJump(exitJump);
        for (int b : loop.breaks) patchJump(b);
        endScope(s->span);
        scopeEnds_.pop_back();
        return false;
    }

    bool forC(Stmt* s) {
        scopeEnds_.push_back(s->span.end);
        beginScope();
        if (s->init) stmt(s->init.get());
        int start = label();
        int exit = -1;
        if (s->e) {
            condition(s->e.get());
            exit = emitJump(OP_JUMP_IF_FALSE, s->e->span);
        }
        fs_->loops.push_back(Loop {fs_->scopeDepth, fs_->tryDepth, {}, {}});
        block(s->body.get());
        Loop loop = fs_->loops.back();
        fs_->loops.pop_back();
        int cont = label();
        for (int c : loop.continues) patchJumpTo(c, cont);
        if (s->step) stmt(s->step.get());
        emitLoop(start, s->span);
        if (exit >= 0) patchJump(exit);
        for (int b : loop.breaks) patchJump(b);
        endScope(s->span);
        scopeEnds_.pop_back();
        return !s->e && !loop.hasBreak;
    }

    bool breakContinue(Stmt* s) {
        bool isBreak = s->k == SK::Break;
        if (fs_->loops.empty()) {
            error(s->span, isBreak ? "break outside loop" : "continue outside loop",
                  std::string("'") + (isBreak ? "break" : "continue") + "' can only be used inside a loop");
            return true;
        }
        Loop& loop = fs_->loops.back();
        popLocalsCode(loop.scopeDepth, s->span);
        for (int i = loop.tryDepth; i < fs_->tryDepth; i++) emitOp(OP_END_TRY, s->span);
        int j = emitJump(OP_JUMP, s->span);
        if (isBreak) {
            loop.breaks.push_back(j);
            loop.hasBreak = true;
        } else {
            loop.continues.push_back(j);
        }
        return true;
    }

    bool comparable(const TypeP& a, const TypeP& b) {
        if (isLoose(a) || isLoose(b)) return true;
        if (a->k == TK::Null || b->k == TK::Null) return true; // null checks are always allowed
        if ((a->k == TK::Int || a->k == TK::Float) && (b->k == TK::Int || b->k == TK::Float)) return true;
        if (a->k != b->k) {
            if (a->k == TK::Interface || b->k == TK::Interface) return true;
            return false;
        }
        if (a->k == TK::Enum) return a->en == b->en;
        if (a->k == TK::Class) return a->cls == b->cls;
        return true;
    }

    bool matchStmt(Stmt* s) {
        beginScope();
        TypeP st = expr(s->e.get());
        int subj = hiddenLocal("$match");
        std::vector<int> endJumps;
        bool hasElse = false;
        bool allExit = true;
        std::unordered_set<std::string> covered;
        for (size_t ai = 0; ai < s->arms.size(); ai++) {
            MatchArm& arm = s->arms[ai];
            if (arm.isElse) {
                if (ai + 1 != s->arms.size()) {
                    error(arm.span, "unreachable match arm", "'else' must be the last arm");
                }
                hasElse = true;
                if (!block(arm.body.get())) allExit = false;
                endJumps.push_back(emitJump(OP_JUMP, arm.span));
                continue;
            }
            std::vector<int> toBody;
            for (auto& p : arm.patterns) {
                emitOp1(OP_GET_LOCAL, subj, p->span);
                TypeP pt = expr(p.get(), st);
                if (!comparable(st, pt)) {
                    error(p->span, "type mismatch",
                          "pattern of type '" + T::name(pt) + "' can never match a subject of type '" + T::name(st) + "'");
                }
                if (st->k == TK::Enum && p->k == EK::Member) covered.insert(p->name);
                emitOp(OP_EQ, p->span);
                toBody.push_back(emitJump(OP_JUMP_IF_TRUE, p->span));
            }
            int next = emitJump(OP_JUMP, arm.span);
            for (int j : toBody) patchJump(j);
            if (!block(arm.body.get())) allExit = false;
            endJumps.push_back(emitJump(OP_JUMP, arm.span));
            patchJump(next);
        }
        bool exhaustive = hasElse;
        if (!hasElse && st->k == TK::Enum && !st->nullable) {
            std::vector<std::string> missing;
            for (auto& m : st->en->members) {
                if (!covered.count(m)) missing.push_back(st->en->name + "." + m);
            }
            if (missing.empty()) {
                exhaustive = true;
            } else if (vm_.warnings) {
                std::string list;
                for (size_t i = 0; i < missing.size() && i < 4; i++) list += (i ? ", " : "") + missing[i];
                if (missing.size() > 4) list += ", ...";
                warning(s->e->span, "non-exhaustive match", "missing " + list + " (add the arms or an 'else ->')");
            }
        }
        for (int j : endJumps) patchJump(j);
        endScope(s->span);
        return exhaustive && allExit && !s->arms.empty();
    }

    void returnStmt(Stmt* s) {
        TypeP ret = fs_->ret;
        size_t n = s->values.size();
        if (n == 0) {
            if (ret && ret->k != TK::Void && !fs_->isModuleMain) {
                error(s->span, "missing return value", "this function must return '" + T::name(ret) + "'");
            }
            if (fs_->isCtor) emitOp1(OP_GET_LOCAL, 0, s->span);
            else emitOp(OP_NIL, s->span);
            emitOp(OP_RETURN, s->span);
            return;
        }
        if (fs_->isModuleMain) {
            error(s->values[0]->span, "unexpected return value", "the top level of a script cannot return a value");
        } else if (ret && ret->k == TK::Void) {
            std::string what = fs_->isCoroutine ? "a coroutine" : fs_->isCtor ? "a constructor" : "this function";
            error(s->values[0]->span, "unexpected return value",
                  what + " returns nothing" + (fs_->isCoroutine || fs_->isCtor ? "" : " - add '-> type' to its declaration"));
        }
        if (!ret || ret->k == TK::Void || ret->k == TK::Any || ret->k == TK::Error) {
            for (auto& v : s->values) expr(v.get());
            if (n > 1) emitOp1(OP_TUPLE, (int)n, s->span);
        } else if (ret->k == TK::Tuple) {
            if (n == 1) {
                TypeP t = expr(s->values[0].get(), ret);
                coerce(ret, t, s->values[0]->span);
            } else if (n != ret->elems.size()) {
                error(s->span, "wrong number of return values",
                      "expected " + std::to_string(ret->elems.size()) + " values " + T::name(ret) + ", found " + std::to_string(n));
                for (auto& v : s->values) expr(v.get());
                emitOp1(OP_TUPLE, (int)n, s->span);
            } else {
                for (size_t i = 0; i < n; i++) {
                    TypeP t = expr(s->values[i].get(), ret->elems[i]);
                    coerce(ret->elems[i], t, s->values[i]->span);
                }
                emitOp1(OP_TUPLE, (int)n, s->span);
            }
        } else {
            if (n > 1) {
                error(s->values[1]->span, "too many return values", "this function returns a single '" + T::name(ret) + "'");
            }
            TypeP t = expr(s->values[0].get(), ret);
            coerce(ret, t, s->values[0]->span);
            checkLiteralRange(ret, s->values[0].get());
            for (size_t i = 1; i < n; i++) {
                expr(s->values[i].get());
                emitOp(OP_POP, s->span);
            }
        }
        emitOp(OP_RETURN, s->span);
    }

    bool tryStmt(Stmt* s) {
        int handler = emitJump(OP_TRY, s->span);
        fs_->tryDepth++;
        bool bodyExits = block(s->body.get());
        fs_->tryDepth--;
        emitOp(OP_END_TRY, s->span);
        int end = emitJump(OP_JUMP, s->span);
        patchJump(handler);
        scopeEnds_.push_back(s->elseBody->span.end);
        beginScope();
        if (!s->name.empty()) {
            int slot = addLocal(s->name, T::any(), s->nameSpan);
            fs_->locals[slot].used = true;
        } else {
            hiddenLocal("$error");
        }
        bool catchExits = false;
        for (auto& st : s->elseBody->stmts) {
            if (stmt(st.get())) catchExits = true;
        }
        endScope({s->elseBody->span.end - 1, s->elseBody->span.end});
        scopeEnds_.pop_back();
        patchJump(end);
        return bodyExits && catchExits;
    }

    // Compiles a function body into a new proto and emits CLOSURE in the current function.
    void function(FuncDecl* fd, const SigP& sig, ClassInfo* cls, const std::string& name, bool isLambda,
                  Expr* lambdaExpr, bool isCtor = false, ClassInfo* staticOf = nullptr) {
        FuncState f;
        f.enclosing = fs_;
        f.proto = vm_.newProto();
        f.proto->name = name;
        f.proto->fileId = syms_.fileId;
        f.proto->isCoroutine = sig->isCoroutine;
        f.proto->isMethod = cls != nullptr;
        f.isCoroutine = sig->isCoroutine;
        f.cls = cls;
        f.staticCls = staticOf;
        f.isCtor = isCtor;
        f.name = name;
        f.ret = sig->ret;
        if (isLambda && lambdaExpr && !lambdaExpr->hasArrowType) f.ret = nullptr;
        fs_ = &f;
        // A function body may run at any time, so null-check narrowing from
        // the enclosing code does not carry into it.
        std::vector<std::unordered_map<std::string, TypeP>> savedNarrow;
        savedNarrow.swap(narrow_);
        narrow_.emplace_back();
        Local self;
        self.name = cls ? "this" : "";
        self.type = cls ? T::cls(cls) : T::any();
        self.used = true;
        f.locals.push_back(self);

        const std::vector<Param>& params = isLambda ? lambdaExpr->params : fd->params;
        int bodyEnd = isLambda ? lambdaExpr->span.end : (fd->body ? fd->body->span.end : fd->span.end);
        scopeEnds_.push_back(bodyEnd);
        f.proto->numParams = (int)params.size();
        f.proto->minParams = sig->required;
        f.proto->variadic = sig->variadic;
        for (size_t i = 0; i < params.size(); i++) {
            TypeP pt = sig->params[i];
            if (params[i].variadic) pt = T::list(pt);
            int slot = addLocal(params[i].name, pt, params[i].span);
            fs_->locals[slot].isParam = true;
            if (sink_) {
                sink_->locals.back().isParam = true;
                sink_->uses.back().detail = "(parameter) " + T::name(pt) + " " + params[i].name;
            }
        }
        // default parameter values (evaluated in the callee when the caller omitted them)
        for (size_t i = 0; i < params.size(); i++) {
            const Param& p = params[i];
            if (!p.def) continue;
            emitOp(OP_ARG_GIVEN, p.def->span);
            emit((int)i, p.def->span);
            int skip = emit(0, p.def->span);
            TypeP t = expr(p.def.get(), sig->params[i]);
            coerce(sig->params[i], t, p.def->span);
            emitOp1(OP_SET_LOCAL, (int)i + 1, p.def->span);
            patchJump(skip);
        }
        if (isCtor && cls) {
            // field initializers run before the body of init()
            for (auto& field : cls->fields) {
                Span sp = field.decl ? field.decl->span : field.span;
                emitOp1(OP_GET_LOCAL, 0, sp);
                if (field.decl && field.decl->init) {
                    TypeP t = expr(field.decl->init.get(), field.type);
                    coerce(field.type, t, field.decl->init->span);
                    checkLiteralRange(field.type, field.decl->init.get());
                } else if (!emitZero(field.type, sp)) {
                    error(sp, "missing initializer",
                          "field '" + field.name + "' of type '" + T::name(field.type) + "' has no zero value - give it an initializer or make it nullable");
                }
                emitFieldSet(cls->fieldIdx[field.name], field.name, sp);
            }
        }
        bool exits = false;
        Span endSpan;
        if (isLambda && lambdaExpr->exprBody) {
            TypeP t = expr(lambdaExpr->exprBody.get(), f.ret);
            if (f.ret) coerce(f.ret, t, lambdaExpr->exprBody->span);
            emitOp(OP_RETURN, lambdaExpr->exprBody->span);
            exits = true;
            endSpan = lambdaExpr->exprBody->span;
        } else {
            Block* body = isLambda ? lambdaExpr->body.get() : fd->body.get();
            if (body) {
                beginScope();
                bool warned = false;
                for (auto& st : body->stmts) {
                    if (exits && !warned && vm_.warnings) {
                        warning(st->span, "unreachable code", "this code will never run");
                        warned = true;
                    }
                    if (stmt(st.get())) exits = true;
                }
                endScope({body->span.end - 1, body->span.end});
                endSpan = {body->span.end - 1, body->span.end};
            }
        }
        if (!exits && f.ret && f.ret->k != TK::Void && f.ret->k != TK::Any && f.ret->k != TK::Error) {
            Span at = fd ? fd->nameSpan : lambdaExpr->span;
            error(at, "missing return", "'" + name + "' must return '" + T::name(f.ret) + "' on every path");
        }
        if (!exits) {
            if (isCtor) emitOp1(OP_GET_LOCAL, 0, endSpan);
            else emitOp(OP_NIL, endSpan);
            emitOp(OP_RETURN, endSpan);
        }
        narrow_.swap(savedNarrow);
        scopeEnds_.pop_back();
        f.proto->maxStack = computeMaxStack(f.proto);
        fs_ = f.enclosing;
        f.proto->numUpvals = (int)f.upvals.size();
        proto()->protos.push_back(f.proto);
        Span sp = fd ? fd->nameSpan : lambdaExpr->span;
        emitOp(OP_CLOSURE, sp);
        emit((int)proto()->protos.size() - 1, sp);
        emit((int)f.upvals.size(), sp);
        for (auto& u : f.upvals) {
            emit(u.isLocal ? 1 : 0, sp);
            emit(u.index, sp);
        }
    }

    void funcStmt(Stmt* s) {
        FuncDecl* fd = s->func.get();
        if (atModuleTop()) {
            auto& g = syms_.globals[fd->name];
            SigP sig = g.kind == ModuleSymbols::Global::Func && g.type && g.type->sig
                           ? g.type->sig : sigOf(fd->params, fd->rets, fd->isCoroutine);
            recordUse(fd->nameSpan, sigText(fd->isCoroutine ? "coroutine" : "func", fd->name, *sig), syms_.fileId,
                      fd->nameSpan);
            function(fd, sig, nullptr, fd->name, false, nullptr);
            emitOp1(OP_SET_GLOBAL, g.slot, fd->nameSpan);
            return;
        }
        SigP sig = sigOf(fd->params, fd->rets, fd->isCoroutine);
        int slot = addLocal(fd->name, T::sig(sig), fd->nameSpan, true);
        fs_->locals[slot].used = true;
        function(fd, sig, nullptr, fd->name, false, nullptr);
    }

    void classStmt(Stmt* s) {
        if (!atModuleTop()) {
            error(s->span, "misplaced declaration", "classes must be declared at the top level");
            return;
        }
        ClassInfo* ci = syms_.globals[s->cls->name].cls;
        if (!ci) return;
        Span sp = s->cls->nameSpan;
        if (sink_) {
            sink_->classes.push_back(AnalysisSink::ClassScope {ci, syms_.fileId, s->span.start, s->span.end});
            recordUse(sp, "class " + ci->name, syms_.fileId, sp);
            for (auto& f : ci->fields) {
                recordUse(f.span, "(field) " + T::name(f.type) + " " + ci->name + "." + f.name, syms_.fileId, f.span);
            }
            for (auto& kv : ci->methods) {
                if (!kv.second.decl) continue;
                std::string kw = kv.second.decl->isOperator ? "operator" : kv.second.sig->isCoroutine ? "coroutine" : "func";
                std::string shown = kv.second.decl->isOperator ? kv.second.decl->opSymbol : ci->name + "." + kv.first;
                recordUse(kv.second.decl->nameSpan, sigText(kw, shown, *kv.second.sig), syms_.fileId, kv.second.decl->nameSpan);
            }
            for (auto& f : ci->statics) {
                recordUse(f.span, "(static) " + T::name(f.type) + " " + ci->name + "." + f.name, syms_.fileId, f.span);
            }
            for (auto& kv : ci->staticMethods) {
                if (!kv.second.decl) continue;
                recordUse(kv.second.decl->nameSpan, sigText("static func", ci->name + "." + kv.first, *kv.second.sig),
                          syms_.fileId, kv.second.decl->nameSpan);
            }
        }
        emitOp2(OP_CLASS, strConst(ci->name), strConst(ci->qual), sp);
        for (auto& f : ci->fields) emitOp2(OP_FIELD, strConst(f.name), f.zero, f.span);
        for (auto* ii : ci->ifaces) emitOp1(OP_IMPLEMENTS, strConst(ii->qual), sp);
        for (auto& f : ci->statics) emitOp2(OP_STATIC_FIELD, strConst(f.name), f.zero, f.span);
        for (auto& m : s->cls->methods) {
            if (!m->isStatic) continue;
            auto it = ci->staticMethods.find(m->name);
            if (it == ci->staticMethods.end() || it->second.decl != m.get()) continue;
            function(m.get(), it->second.sig, nullptr, ci->name + "." + m->name, false, nullptr, false, ci);
            emitOp1(OP_STATIC_METHOD, strConst(m->name), m->nameSpan);
        }
        bool hasInit = false;
        for (auto& m : s->cls->methods) {
            auto it = ci->methods.find(m->name);
            if (it == ci->methods.end() || it->second.decl != m.get()) continue;
            bool isCtor = m->name == "init";
            hasInit |= isCtor;
            function(m.get(), it->second.sig, ci, ci->name + "." + m->name, false, nullptr, isCtor);
            emitOp1(OP_METHOD, strConst(m->name), m->nameSpan);
        }
        if (!hasInit) {
            FuncDecl synth;
            synth.name = "init";
            synth.nameSpan = sp;
            synth.body = std::make_unique<Block>();
            synth.body->span = {sp.end, sp.end};
            function(&synth, ci->ctor, ci, ci->name + ".init", false, nullptr, true);
            emitOp1(OP_METHOD, strConst("init"), sp);
        }
        int slot = syms_.globals[ci->name].slot;
        emitOp1(OP_DEF_CLASS, slot, sp);
        // static initializers run once (a hot reload keeps the current values)
        bool any = false;
        for (auto& f : ci->statics) any |= (f.decl && f.decl->init) || f.type->k == TK::Enum;
        if (any) {
            int skip = emitJump(OP_JUMP_IF_RELOADING, sp);
            ClassInfo* saved = fs_->staticCls;
            fs_->staticCls = ci;
            for (auto& f : ci->statics) {
                bool hasInit = f.decl && f.decl->init;
                if (!hasInit && f.type->k != TK::Enum) continue;
                emitOp1(OP_GET_GLOBAL, slot, f.span);
                if (hasInit) {
                    TypeP t = expr(f.decl->init.get(), f.type);
                    coerce(f.type, t, f.decl->init->span);
                    checkLiteralRange(f.type, f.decl->init.get());
                } else {
                    emitZero(f.type, f.span);
                }
                emitOp1(OP_SET_FIELD, strConst(f.name), f.span);
            }
            fs_->staticCls = saved;
            patchJump(skip);
        }
    }

    void enumStmt(Stmt* s) {
        if (!atModuleTop()) {
            error(s->span, "misplaced declaration", "enums must be declared at the top level");
            return;
        }
        EnumInfo* ei = syms_.globals[s->en->name].en;
        if (!ei) return;
        Span sp = s->en->nameSpan;
        if (sink_) {
            recordUse(sp, "enum " + ei->name, syms_.fileId, sp);
            for (size_t i = 0; i < ei->members.size() && i < ei->memberSpans.size(); i++) {
                recordUse(ei->memberSpans[i], "enum member " + ei->name + "." + ei->members[i], syms_.fileId, ei->memberSpans[i]);
            }
        }
        emitOp(OP_ENUM, sp);
        emit(strConst(ei->name), sp);
        emit(strConst(ei->qual), sp);
        emit((int)ei->members.size(), sp);
        for (auto& m : ei->members) emit(strConst(m), sp);
        emitOp1(OP_SET_GLOBAL, syms_.globals[ei->name].slot, sp);
    }

    // ===================================================================
    // Expressions
    // ===================================================================
    void nullError(Expr* e, const TypeP& t) {
        std::string what = e->k == EK::Ident ? "'" + e->name + "'" : "this expression";
        error(e->span, "possible null reference",
              what + " is type '" + T::name(t) + "' (nullable) - check for null before use");
    }

    void emitBinary(Tok op, Span sp) {
        if (lastIs(OP_CONST, 2)) {
            // `x OP constant` -> one instruction reading the constant directly
            Op k = OP__COUNT;
            switch (op) {
            case Tok::Plus: k = OP_ADD_K; break;
            case Tok::Minus: k = OP_SUB_K; break;
            case Tok::Star: k = OP_MUL_K; break;
            case Tok::Slash: k = OP_DIV_K; break;
            case Tok::Percent: k = OP_MOD_K; break;
            case Tok::Lt: k = OP_LT_K; break;
            case Tok::Le: k = OP_LE_K; break;
            case Tok::Gt: k = OP_GT_K; break;
            case Tok::Ge: k = OP_GE_K; break;
            case Tok::Eq: k = OP_EQ_K; break;
            case Tok::Ne: k = OP_NE_K; break;
            default: break;
            }
            if (k != OP__COUNT) {
                int at = fs_->lastOp;
                proto()->code[at] = k;
                proto()->spans[at] = sp;
                proto()->spans[at + 1] = sp;
                return;
            }
        }
        switch (op) {
        case Tok::Plus: emitOp(OP_ADD, sp); break;
        case Tok::Minus: emitOp(OP_SUB, sp); break;
        case Tok::Star: emitOp(OP_MUL, sp); break;
        case Tok::Slash: emitOp(OP_DIV, sp); break;
        case Tok::Percent: emitOp(OP_MOD, sp); break;
        case Tok::Eq: emitOp(OP_EQ, sp); break;
        case Tok::Ne: emitOp(OP_NE, sp); break;
        case Tok::Lt: emitOp(OP_LT, sp); break;
        case Tok::Le: emitOp(OP_LE, sp); break;
        case Tok::Gt: emitOp(OP_GT, sp); break;
        case Tok::Ge: emitOp(OP_GE, sp); break;
        case Tok::Amp: emitOp(OP_BAND, sp); break;
        case Tok::Pipe: emitOp(OP_BOR, sp); break;
        case Tok::Caret: emitOp(OP_BXOR, sp); break;
        case Tok::Shl: emitOp(OP_SHL, sp); break;
        case Tok::Shr: emitOp(OP_SHR, sp); break;
        default: break;
        }
    }

    TypeP expr(Expr* e, TypeP expect = nullptr) {
        switch (e->k) {
        case EK::Int:
            if (expect && expect->k == TK::Float && !expect->ext) {
                emitOp1(OP_CONST, constant(Value::number((double)e->ival)), e->span);
                return T::floating();
            }
            emitOp1(OP_CONST, intConst(e->ival), e->span);
            return T::integer();
        case EK::Float:
            emitOp1(OP_CONST, constant(Value::number(e->fval)), e->span);
            return T::floating();
        case EK::Str:
            emitOp1(OP_CONST, strConst(e->name), e->span);
            return T::str();
        case EK::Bool:
            emitOp(e->bval ? OP_TRUE : OP_FALSE, e->span);
            return T::boolean();
        case EK::Null:
            emitOp(OP_NIL, e->span);
            return T::null();
        case EK::FStr: {
            int n = 0;
            for (size_t i = 0; i < e->strParts.size(); i++) {
                if (!e->strParts[i].empty()) {
                    emitOp1(OP_CONST, strConst(e->strParts[i]), e->span);
                    n++;
                }
                if (i < e->args.size()) {
                    TypeP t = expr(e->args[i].get());
                    if (t->k == TK::Void) error(e->args[i]->span, "no value", "this expression does not produce a value");
                    n++;
                }
            }
            if (n == 0) emitOp1(OP_CONST, strConst(""), e->span);
            else emitOp1(OP_FSTRING, n, e->span);
            return T::str();
        }
        case EK::Ident: return identExpr(e);
        case EK::This: {
            ClassInfo* ci = enclosingClass();
            if (!ci) {
                error(e->span, "invalid 'this'", "'this' can only be used inside class methods");
                emitOp(OP_NIL, e->span);
                return T::error();
            }
            emitThis(e->span);
            return T::cls(ci);
        }
        case EK::Member: return memberExpr(e);
        case EK::Index: {
            TypeP ct = expr(e->a.get());
            TypeP it = expr(e->b.get(), ct->k == TK::Dict ? ct->a : T::integer());
            TypeP r = indexType(ct, it, e, false);
            emitOp(OP_GET_INDEX, e->a->span);
            return r;
        }
        case EK::Call: return callExpr(e, false);
        case EK::Start: {
            callExpr(e->a.get(), true);
            return T::any();
        }
        case EK::Unary: {
            TypeP t = expr(e->a.get());
            if (e->op == Tok::Minus) {
                emitOp(OP_NEG, e->span);
                if (isLoose(t)) return t;
                if (isNumeric(t)) return T::nullable(t, false);
                if (TypeP o = unaryOverload(t)) return o;
                error(e->span, "type mismatch", "cannot negate a value of type '" + T::name(t) + "'");
                return T::error();
            }
            if (e->op == Tok::Bang) {
                if (!isLoose(t) && !(t->k == TK::Bool && !t->nullable)) {
                    error(e->span, "type mismatch", "'!' expects 'bool', found '" + T::name(t) + "'");
                }
                emitOp(OP_NOT, e->span);
                return T::boolean();
            }
            if (!isLoose(t) && !(t->k == TK::Int && !t->nullable)) {
                error(e->span, "type mismatch", "'~' expects 'int', found '" + T::name(t) + "'");
            }
            emitOp(OP_BNOT, e->span);
            return T::integer();
        }
        case EK::Binary: {
            TypeP l = expr(e->a.get());
            TypeP r = expr(e->b.get(), (l->k == TK::Float && (e->op != Tok::Eq)) ? l : nullptr);
            TypeP res = binaryResult(e->op, l, r, e->span, e->a->span, e->b->span);
            emitBinary(e->op, e->span);
            return res;
        }
        case EK::Logical: {
            TypeP l = expr(e->a.get(), T::boolean());
            if (!isLoose(l) && !(l->k == TK::Bool && !l->nullable)) {
                error(e->a->span, "type mismatch", "expected 'bool', found '" + T::name(l) + "'");
            }
            int j = emitJump(e->op == Tok::AndAnd ? OP_AND_JUMP : OP_OR_JUMP, e->span);
            std::vector<Fact> fs;
            facts(e->a.get(), e->op == Tok::AndAnd, fs);
            narrow_.emplace_back();
            applyFacts(fs);
            TypeP r = expr(e->b.get(), T::boolean());
            narrow_.pop_back();
            if (!isLoose(r) && !(r->k == TK::Bool && !r->nullable)) {
                error(e->b->span, "type mismatch", "expected 'bool', found '" + T::name(r) + "'");
            }
            patchJump(j);
            return T::boolean();
        }
        case EK::Coalesce: {
            TypeP l = expr(e->a.get());
            int j = emitJump(OP_COALESCE_JUMP, e->span);
            TypeP base = T::nullable(l, false);
            TypeP r = expr(e->b.get(), isLoose(l) ? nullptr : base);
            if (!isLoose(l)) {
                if (!l->nullable && l->k != TK::Null && vm_.warnings) {
                    warning(e->a->span, "unnecessary '?\?'", "'" + T::name(l) + "' is never null");
                }
                if (!isLoose(r) && r->k != TK::Null && !assignableBase(base, r)) {
                    error(e->b->span, "type mismatch", "expected '" + T::name(base) + "', found '" + T::name(r) + "'");
                } else if (base->k == TK::Float && r->k == TK::Int) {
                    emitOp1(OP_CONVERT, CV_TO_FLOAT, e->b->span);
                }
            }
            patchJump(j);
            if (isLoose(l)) return l;
            if (r->k == TK::Null) return T::nullable(base, true);
            return T::nullable(base, r->nullable);
        }
        case EK::List: return listExpr(e, expect);
        case EK::Dict: return dictExpr(e, expect);
        case EK::Lambda: {
            SigP sig = sigOf(e->params, e->rets, false);
            if (!e->hasArrowType) sig->ret = T::any();
            function(nullptr, sig, nullptr, "<lambda>", true, e);
            return T::sig(sig);
        }
        }
        return T::error();
    }

    TypeP binaryResult(Tok op, const TypeP& l, const TypeP& r, Span sp, Span lsp, Span rsp) {
        (void)lsp;
        (void)rsp;
        if (l->k == TK::Error || r->k == TK::Error) return T::error();
        auto opName = [](Tok t) -> std::string {
            switch (t) {
            case Tok::Plus: return "+";
            case Tok::Minus: return "-";
            case Tok::Star: return "*";
            case Tok::Slash: return "/";
            case Tok::Percent: return "%";
            case Tok::Lt: return "<";
            case Tok::Le: return "<=";
            case Tok::Gt: return ">";
            case Tok::Ge: return ">=";
            case Tok::Amp: return "&";
            case Tok::Pipe: return "|";
            case Tok::Caret: return "^";
            case Tok::Shl: return "<<";
            case Tok::Shr: return ">>";
            default: return "?";
            }
        };
        auto fail = [&]() {
            error(sp, "type mismatch",
                  "cannot apply '" + opName(op) + "' to '" + T::name(l) + "' and '" + T::name(r) + "'");
            return T::error();
        };
        if (l->k == TK::Void || r->k == TK::Void) {
            error(sp, "no value", "an operand does not produce a value");
            return T::error();
        }
        switch (op) {
        case Tok::Eq: case Tok::Ne:
            if (!comparable(l, r) && !opOverload(op, l, r, sp)) {
                error(sp, "type mismatch", "'" + T::name(l) + "' and '" + T::name(r) + "' can never be equal");
            }
            return T::boolean();
        case Tok::Plus:
            if ((l->k == TK::Str && !l->nullable) || (r->k == TK::Str && !r->nullable)) return T::str();
            [[fallthrough]];
        case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent:
            if (l->k == TK::Any || r->k == TK::Any) return T::any();
            if (!isNumeric(l) || !isNumeric(r)) {
                if (TypeP t = opOverload(op, l, r, sp)) return t;
                return fail();
            }
            if (l->k == TK::Float || r->k == TK::Float) return T::floating();
            return T::integer();
        case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge:
            if (l->k == TK::Any || r->k == TK::Any) return T::boolean();
            if ((isNumeric(l) && isNumeric(r)) || (l->k == TK::Str && r->k == TK::Str && !l->nullable && !r->nullable)) {
                return T::boolean();
            }
            if (opOverload(op, l, r, sp)) return T::boolean();
            fail();
            return T::boolean();
        case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr:
            if (l->k == TK::Any || r->k == TK::Any) return T::any();
            if (l->k != TK::Int || r->k != TK::Int || l->nullable || r->nullable) return fail();
            return T::integer();
        default:
            return fail();
        }
    }

    static std::string opSymbol(Tok op) {
        switch (op) {
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::Lt: return "<";
        case Tok::Le: return "<=";
        case Tok::Gt: return ">";
        case Tok::Ge: return ">=";
        case Tok::Eq: case Tok::Ne: return "==";
        default: return "";
        }
    }

    bool accepts(const TypeP& target, const TypeP& src) {
        if (isLoose(target) || isLoose(src)) return true;
        return assignableBase(target, src) && !(src->nullable && !target->nullable);
    }

    // Result type of an overloaded operator (script `operator` methods or
    // operators registered on native types), or null if there is none.
    TypeP opOverload(Tok op, const TypeP& l, const TypeP& r, Span sp) {
        std::string sym = opSymbol(op);
        if (sym.empty()) return nullptr;
        auto find = [&](const TypeP& a, const TypeP& b, const std::string& s) -> TypeP {
            if (a->k == TK::Class && !a->nullable) {
                auto mi = a->cls->methods.find("op" + s);
                if (mi != a->cls->methods.end()) {
                    const SigInfo& sig = *mi->second.sig;
                    if (!sig.params.empty() && !accepts(sig.params[0], b)) {
                        error(sp, "type mismatch", "operator '" + s + "' of '" + a->cls->name + "' expects '" +
                                                       T::name(sig.params[0]) + "', found '" + T::name(b) + "'");
                    }
                    return sig.ret;
                }
            }
            if (a->k == TK::Native && !a->nullable) {
                for (auto& o : a->native->ops) {
                    if (o.op == s && !o.reversed && !o.other.empty() && accepts(typeFromString(o.other), b)) {
                        return typeFromString(o.result);
                    }
                }
            }
            if (b->k == TK::Native && !b->nullable) {
                for (auto& o : b->native->ops) {
                    if (o.op == s && o.reversed && accepts(typeFromString(o.other), a)) return typeFromString(o.result);
                }
            }
            return nullptr;
        };
        if (TypeP t = find(l, r, sym)) return t;
        // >, <= and >= fall back to < (as at runtime)
        if ((op == Tok::Gt || op == Tok::Le) && find(r, l, "<")) return T::boolean();
        if (op == Tok::Ge && find(l, r, "<")) return T::boolean();
        return nullptr;
    }

    TypeP unaryOverload(const TypeP& t) {
        if (t->k == TK::Class && !t->nullable) {
            auto mi = t->cls->methods.find("op_neg");
            if (mi != t->cls->methods.end()) return mi->second.sig->ret;
        }
        if (t->k == TK::Native && !t->nullable) {
            for (auto& o : t->native->ops) {
                if (o.op == "neg" && o.other.empty()) return typeFromString(o.result);
            }
        }
        return nullptr;
    }

    TypeP identExpr(Expr* e) {
        Res r = resolve(e->name);
        if (r.k == Res::None) {
            std::vector<std::string> notes;
            if (e->name == "input" || e->name == "file") {
                notes.push_back("'" + e->name + "' is only available in the standalone runner (blocking I/O)");
            }
            if (e->name == "wait") notes.push_back("wait(seconds) must be called, inside a coroutine");
            bool isPrefix = false;
            for (auto& kv : imports_) {
                if (kv.first.rfind(e->name + ".", 0) == 0) isPrefix = true;
            }
            if (isPrefix) {
                error(e->span, "module path is not a value", "'" + e->name + "' is part of an imported module path; access its members instead");
            } else {
                error(e->span, "unknown identifier", "cannot find '" + e->name + "' in this scope", notes);
            }
            emitOp(OP_NIL, e->span);
            return T::error();
        }
        emitLoad(r, e->name, e->span);
        recordRes(e->span, r, e->name);
        return r.type ? r.type : T::any();
    }

    bool isPlaceholder(const std::string& name) {
        return sink_ && !sink_->placeholder.empty() && name == sink_->placeholder;
    }

    // Hover / definition for `recv.name`, and the completion receiver when
    // `name` is the completion placeholder.
    void recordMember(Span sp, const TypeP& recv, const std::string& name) {
        if (!sink_ || !recv) return;
        TypeP t = T::nullable(recv, false);
        if (isPlaceholder(name)) {
            sink_->haveReceiver = true;
            if (t->k == TK::Namespace) sink_->receiverNamespace = t->nsName;
            else sink_->receiverType = t;
            return;
        }
        switch (t->k) {
        case TK::Class: {
            ClassInfo* ci = t->cls;
            auto fi = ci->fieldIdx.find(name);
            if (fi != ci->fieldIdx.end()) {
                const auto& f = ci->fields[fi->second];
                recordUse(sp, "(field) " + T::name(f.type) + " " + ci->name + "." + name, ci->fileId, f.span);
                return;
            }
            auto mi = ci->methods.find(name);
            if (mi != ci->methods.end()) {
                recordUse(sp, sigText(mi->second.sig->isCoroutine ? "coroutine" : "func", ci->name + "." + name, *mi->second.sig),
                          ci->fileId, mi->second.decl ? mi->second.decl->nameSpan : Span {});
            }
            return;
        }
        case TK::Interface: {
            auto mi = t->iface->methods.find(name);
            if (mi != t->iface->methods.end()) {
                recordUse(sp, sigText("func", t->iface->name + "." + name, *mi->second), t->iface->fileId,
                          t->iface->methodSpans[name]);
            }
            return;
        }
        case TK::Native: {
            const std::string& cname = t->native->name;
            auto pi = t->native->props.find(name);
            if (pi != t->native->props.end()) {
                recordUse(sp, "(property) " + T::name(typeFromString(pi->second.type)) + " " + cname + "." + name, -1, {});
                return;
            }
            auto mi = t->native->methods.find(name);
            if (mi != t->native->methods.end()) {
                SigP s = mi->second.sig.empty() ? nullptr : sigFromString(mi->second.sig);
                recordUse(sp, s ? sigText("func", cname + "." + name, *s) : "func " + cname + "." + name + "(any...)", -1, {});
            }
            return;
        }
        case TK::Namespace: {
            std::string key = t->nsName + "." + name;
            ObjNamespace* ns = vm_.namespaceFor(key, false);
            if (ns) {
                recordUse(sp, "namespace " + key, -1, {});
                return;
            }
            TypeP mt = nativeType(key);
            if (mt->k == TK::Sig) recordUse(sp, sigText("func", key, *mt->sig), -1, {});
            else recordUse(sp, T::name(mt) + " " + key, -1, {});
            return;
        }
        case TK::List: case TK::Dict: case TK::Str: case TK::Tuple: {
            if (name == "length") {
                recordUse(sp, "(property) int length", -1, {});
                return;
            }
            if (SigP s = builtinMethod(t, name)) recordUse(sp, sigText("func", T::name(t) + "." + name, *s), -1, {});
            return;
        }
        case TK::Enum:
            if (name == "name") recordUse(sp, "(property) str name", -1, {});
            if (name == "index") recordUse(sp, "(property) int index", -1, {});
            return;
        default:
            return;
        }
    }

    void recordEnumMember(Span sp, EnumInfo* en, const std::string& name) {
        if (!sink_) return;
        if (isPlaceholder(name)) {
            sink_->haveReceiver = true;
            sink_->receiverEnum = en;
            return;
        }
        for (size_t i = 0; i < en->members.size(); i++) {
            if (en->members[i] == name) {
                recordUse(sp, "enum member " + en->name + "." + name, en->fileId,
                          i < en->memberSpans.size() ? en->memberSpans[i] : en->nameSpan);
            }
        }
    }

    void recordModuleMember(Span sp, ImportRef* ir, const std::string& name) {
        if (!sink_) return;
        if (isPlaceholder(name)) {
            sink_->haveReceiver = true;
            sink_->receiverModule = ir->mod;
            return;
        }
        auto it = ir->mod->globals.find(name);
        if (it == ir->mod->globals.end() || it->second.kind == ModuleSymbols::Global::Import) return;
        ModuleSymbols::Global& g = it->second;
        if (g.kind == ModuleSymbols::Global::Interface) {
            recordUse(sp, "interface " + name, ir->mod->fileId, g.span);
            return;
        }
        Res r;
        r.k = Res::Global;
        r.g = &g;
        r.type = g.type ? g.type : T::any();
        r.isConst = g.isConst;
        r.declSpan = g.span;
        r.declFile = ir->mod->fileId;
        recordRes(sp, r, ir->mod->key + "." + name);
    }

    TypeP builtinPropType(const TypeP& t, const std::string& name) {
        if (name == "length" && (t->k == TK::List || t->k == TK::Dict || t->k == TK::Str || t->k == TK::Tuple)) {
            return T::integer();
        }
        return nullptr;
    }

    TypeP memberExpr(Expr* e) {
        {
            int cslot;
            ImportRef* cvia;
            if (ClassInfo* ci = classRef(e->a.get(), &cslot, &cvia)) {
                // ClassName.staticMember
                emitOp1(OP_GET_GLOBAL, cslot, e->a->span);
                if (cvia) emitOp1(OP_GET_FIELD, strConst(ci->name), e->a->span);
                emitOp1(OP_GET_FIELD, strConst(e->name), e->a->span);
                if (isPlaceholder(e->name)) {
                    sink_->haveReceiver = true;
                    sink_->receiverStatics = ci;
                    return T::error();
                }
                auto si = ci->staticIdx.find(e->name);
                if (si != ci->staticIdx.end()) {
                    const ClassInfo::Field& f = ci->statics[si->second];
                    recordUse(e->nameSpan, "(static) " + T::name(f.type) + " " + ci->name + "." + f.name, ci->fileId, f.span);
                    if (f.isPrivate && staticContext() != ci) {
                        error(e->nameSpan, "private field", "'" + e->name + "' is private to class '" + ci->name + "'");
                    }
                    return f.type;
                }
                auto mi = ci->staticMethods.find(e->name);
                if (mi != ci->staticMethods.end()) {
                    recordUse(e->nameSpan, sigText("static func", ci->name + "." + e->name, *mi->second.sig), ci->fileId,
                              mi->second.decl ? mi->second.decl->nameSpan : Span {});
                    return T::sig(mi->second.sig);
                }
                std::vector<std::string> notes;
                if (ci->fieldIdx.count(e->name) || ci->methods.count(e->name)) {
                    notes.push_back("'" + e->name + "' is an instance member; use it on an object of class '" + ci->name + "'");
                }
                error(e->nameSpan, "unknown static member", "class '" + ci->name + "' has no static member '" + e->name + "'", notes);
                return T::error();
            }
        }
        if (EnumInfo* en = enumRef(e->a.get())) {
            expr(e->a.get());
            recordEnumMember(e->nameSpan, en, e->name);
            if (std::find(en->members.begin(), en->members.end(), e->name) == en->members.end()) {
                error(e->nameSpan, "unknown enum member", "enum '" + en->name + "' has no member '" + e->name + "'");
                emitOp1(OP_GET_FIELD, strConst(e->name), e->a->span);
                return T::error();
            }
            emitOp1(OP_GET_FIELD, strConst(e->name), e->a->span);
            return T::en(en);
        }
        if (ImportRef* ir = modulePath(e->a.get())) {
            emitOp1(OP_GET_GLOBAL, ir->slot, e->a->span);
            recordModuleMember(e->nameSpan, ir, e->name);
            auto it = ir->mod->globals.find(e->name);
            if (it == ir->mod->globals.end() || it->second.kind == ModuleSymbols::Global::Import) {
                error(e->nameSpan, "unknown member", "module '" + ir->mod->key + "' has no member '" + e->name + "'");
                emitOp1(OP_GET_FIELD, strConst(e->name), e->a->span);
                return T::error();
            }
            emitOp1(OP_GET_FIELD, strConst(e->name), e->a->span);
            if (it->second.kind == ModuleSymbols::Global::Interface) {
                error(e->nameSpan, "not a value", "'" + e->name + "' is an interface, not a value");
                return T::error();
            }
            return it->second.type && it->second.kind != ModuleSymbols::Global::Class ? it->second.type : T::any();
        }
        TypeP ot = expr(e->a.get());
        Span rs = e->a->span;
        recordMember(e->nameSpan, ot, e->name);
        if (ot->k == TK::Error) {
            emitOp1(OP_GET_FIELD, strConst(e->name), rs);
            return T::error();
        }
        if (ot->k == TK::Any) {
            emitOp1(OP_GET_FIELD, strConst(e->name), rs);
            return T::any();
        }
        if (ot->k == TK::Void) {
            error(rs, "no value", "this expression does not produce a value");
            return T::error();
        }
        if (ot->nullable || ot->k == TK::Null) {
            nullError(e->a.get(), ot);
            emitOp1(OP_GET_FIELD, strConst(e->name), rs);
            return T::error();
        }
        if (ot->k == TK::Class && ot->cls->fieldIdx.count(e->name)) {
            emitFieldGet(ot->cls->fieldIdx[e->name], e->name, rs);
        } else {
            emitOp1(OP_GET_FIELD, strConst(e->name), rs);
        }
        if (TypeP bt = builtinPropType(ot, e->name)) return bt;
        switch (ot->k) {
        case TK::Class: {
            ClassInfo* ci = ot->cls;
            auto fi = ci->fieldIdx.find(e->name);
            if (fi != ci->fieldIdx.end()) {
                const auto& f = ci->fields[fi->second];
                if (f.isPrivate && enclosingClass() != ci) {
                    error(e->nameSpan, "private field", "'" + e->name + "' is private to class '" + ci->name + "'");
                }
                return f.type;
            }
            auto mi = ci->methods.find(e->name);
            if (mi != ci->methods.end()) {
                if (mi->second.isPrivate && enclosingClass() != ci) {
                    error(e->nameSpan, "private method", "'" + e->name + "' is private to class '" + ci->name + "'");
                }
                return T::sig(mi->second.sig);
            }
            error(e->nameSpan, "unknown member", "class '" + ci->name + "' has no field or method '" + e->name + "'");
            return T::error();
        }
        case TK::Interface: {
            auto mi = ot->iface->methods.find(e->name);
            if (mi != ot->iface->methods.end()) return T::sig(mi->second);
            error(e->nameSpan, "unknown member", "interface '" + ot->iface->name + "' has no method '" + e->name + "'");
            return T::error();
        }
        case TK::Native: {
            auto pi = ot->native->props.find(e->name);
            if (pi != ot->native->props.end()) return typeFromString(pi->second.type);
            auto mi = ot->native->methods.find(e->name);
            if (mi != ot->native->methods.end()) {
                SigP s = mi->second.sig.empty() ? nullptr : sigFromString(mi->second.sig);
                return s ? T::sig(s) : T::func();
            }
            error(e->nameSpan, "unknown member", "'" + T::name(ot) + "' has no property or method '" + e->name + "'");
            return T::error();
        }
        case TK::Namespace: {
            ObjNamespace* ns = vm_.namespaceFor(ot->nsName, false);
            bool found = false;
            Value v;
            if (ns) {
                for (auto& kv : ns->members) {
                    if (kv.first->s == e->name) {
                        found = true;
                        v = kv.second;
                    }
                }
            }
            if (!found) {
                error(e->nameSpan, "unknown member", "'" + ot->nsName + "' has no member '" + e->name + "'");
                return T::error();
            }
            if (v.isObj() && v.o->type == OT::Namespace) return T::ns(ot->nsName + "." + e->name);
            return nativeType(ot->nsName + "." + e->name);
        }
        case TK::Enum:
            if (e->name == "name") return T::str();
            if (e->name == "index") return T::integer();
            break;
        default:
            break;
        }
        error(e->nameSpan, "unknown member", "type '" + T::name(ot) + "' has no member '" + e->name + "'");
        return T::error();
    }

    TypeP indexType(const TypeP& ct, const TypeP& it, Expr* e, bool assigning) {
        if (isLoose(ct)) return T::any();
        if (ct->nullable) {
            nullError(e->a.get(), ct);
            return T::error();
        }
        switch (ct->k) {
        case TK::List:
            if (!isLoose(it) && it->k != TK::Int) error(e->b->span, "type mismatch", "list index must be 'int', found '" + T::name(it) + "'");
            return ct->a;
        case TK::Dict:
            if (!isLoose(it) && !assignableBase(ct->a, it)) {
                error(e->b->span, "type mismatch", "expected key of type '" + T::name(ct->a) + "', found '" + T::name(it) + "'");
            }
            return ct->b;
        case TK::Str:
            if (assigning) error(e->span, "strings are immutable", "cannot assign to a character of a 'str'");
            if (!isLoose(it) && it->k != TK::Int) error(e->b->span, "type mismatch", "string index must be 'int'");
            return T::str();
        case TK::Tuple:
            if (assigning) error(e->span, "tuples are immutable", "cannot assign into multiple return values");
            return T::any();
        default:
            error(e->a->span, "not indexable", "cannot index a value of type '" + T::name(ct) + "'");
            return T::error();
        }
    }

    TypeP listExpr(Expr* e, const TypeP& expect) {
        TypeP elem = expect && expect->k == TK::List ? expect->a : nullptr;
        TypeP inferred;
        bool mixed = false;
        for (auto& a : e->args) {
            TypeP t = expr(a.get(), elem);
            if (elem) {
                coerce(elem, t, a->span);
            } else if (!inferred) {
                inferred = t;
            } else if (!T::same(inferred, t)) {
                mixed = true;
            }
        }
        emitOp1(OP_BUILD_LIST, (int)e->args.size(), e->span);
        if (elem) return T::list(elem);
        if (!inferred || mixed || inferred->k == TK::Null || inferred->k == TK::Void) return T::list(T::any());
        return T::list(inferred);
    }

    TypeP dictExpr(Expr* e, const TypeP& expect) {
        TypeP kt = expect && expect->k == TK::Dict ? expect->a : nullptr;
        TypeP vt = expect && expect->k == TK::Dict ? expect->b : nullptr;
        TypeP ik, iv;
        bool mixedK = false, mixedV = false;
        for (size_t i = 0; i < e->keys.size(); i++) {
            TypeP k = expr(e->keys[i].get(), kt);
            if (kt) coerce(kt, k, e->keys[i]->span);
            else if (!ik) ik = k;
            else if (!T::same(ik, k)) mixedK = true;
            TypeP v = expr(e->args[i].get(), vt);
            if (vt) coerce(vt, v, e->args[i]->span);
            else if (!iv) iv = v;
            else if (!T::same(iv, v)) mixedV = true;
        }
        emitOp1(OP_BUILD_DICT, (int)e->keys.size(), e->span);
        if (kt) return T::dict(kt, vt);
        TypeP k = (!ik || mixedK) ? T::any() : ik;
        TypeP v = (!iv || mixedV || iv->k == TK::Null) ? T::any() : iv;
        return T::dict(k, v);
    }

    // ----- calls -----
    void callArgs(const std::vector<ExprP>& args, const SigInfo* sig, Span callSpan, const std::string& fname) {
        if (!sig) {
            for (auto& a : args) {
                TypeP t = expr(a.get());
                if (t->k == TK::Void) error(a->span, "no value", "this expression does not produce a value");
            }
            return;
        }
        int argc = (int)args.size();
        int fixed = (int)sig->params.size() - (sig->variadic ? 1 : 0);
        if (argc < sig->required) {
            std::string missing;
            for (int i = argc; i < sig->required && i < (int)sig->names.size(); i++) missing += (missing.empty() ? "'" : ", '") + sig->names[i] + "'";
            error(callSpan, "missing arguments",
                  "'" + fname + "' expects " + (sig->required == fixed && !sig->variadic ? "" : "at least ") +
                      std::to_string(sig->required) + " argument" + (sig->required == 1 ? "" : "s") + ", found " +
                      std::to_string(argc) + (missing.empty() ? "" : " (missing " + missing + ")"));
        } else if (!sig->variadic && argc > fixed) {
            error(args[fixed]->span, "too many arguments",
                  "'" + fname + "' takes " + std::to_string(fixed) + " argument" + (fixed == 1 ? "" : "s") + ", found " + std::to_string(argc));
        }
        for (int i = 0; i < argc; i++) {
            TypeP pt;
            if (i < fixed) pt = sig->params[i];
            else if (sig->variadic) pt = sig->params.back();
            TypeP t = expr(args[i].get(), pt);
            if (pt) {
                coerce(pt, t, args[i]->span);
                checkLiteralRange(pt, args[i].get());
            } else if (t->k == TK::Void) {
                error(args[i]->span, "no value", "this expression does not produce a value");
            }
        }
    }

    TypeP retOf(const SigInfo* sig) {
        if (!sig || !sig->ret) return T::any();
        return sig->ret;
    }

    SigP methodSig(std::initializer_list<TypeP> params, int required, TypeP ret, bool variadic = false) {
        auto s = std::make_shared<SigInfo>();
        s->params = params;
        s->required = required;
        s->ret = std::move(ret);
        s->variadic = variadic;
        for (size_t i = 0; i < s->params.size(); i++) s->names.push_back("arg" + std::to_string(i + 1));
        return s;
    }

    SigP builtinMethod(const TypeP& t, const std::string& name) {
        TypeP V = T::voidT(), I = T::integer(), B = T::boolean(), S = T::str(), F = T::func();
        if (t->k == TK::List) {
            TypeP E = t->a;
            if (name == "add") return methodSig({E}, 1, V);
            if (name == "insert") return methodSig({I, E}, 2, V);
            if (name == "removeAt") return methodSig({I}, 1, E);
            if (name == "remove") return methodSig({E}, 1, B);
            if (name == "pop") return methodSig({}, 0, E);
            if (name == "contains") return methodSig({E}, 1, B);
            if (name == "indexOf") return methodSig({E}, 1, I);
            if (name == "clear") return methodSig({}, 0, V);
            if (name == "forEach") return methodSig({F}, 1, V);
            if (name == "map") return methodSig({F}, 1, T::list(T::any()));
            if (name == "filter") return methodSig({F}, 1, T::list(E));
            if (name == "sort") return methodSig({F}, 0, V);
            if (name == "reverse") return methodSig({}, 0, V);
            if (name == "slice") return methodSig({I, I}, 1, T::list(E));
            if (name == "join") return methodSig({S}, 0, S);
            if (name == "copy") return methodSig({}, 0, T::list(E));
            if (name == "isEmpty") return methodSig({}, 0, B);
        } else if (t->k == TK::Dict) {
            TypeP K = t->a, Vv = t->b;
            if (name == "keys") return methodSig({}, 0, T::list(K));
            if (name == "values") return methodSig({}, 0, T::list(Vv));
            if (name == "has" || name == "contains") return methodSig({K}, 1, B);
            if (name == "remove") return methodSig({K}, 1, B);
            if (name == "get") return methodSig({K, Vv}, 2, Vv);
            if (name == "clear") return methodSig({}, 0, V);
            if (name == "copy") return methodSig({}, 0, T::dict(K, Vv));
            if (name == "isEmpty") return methodSig({}, 0, B);
        } else if (t->k == TK::Str) {
            // s.toUpper() is str.toUpper(s)
            auto si = vm_.nativeSigs.find("str." + name);
            if (si != vm_.nativeSigs.end()) {
                SigP full = sigFromString(si->second);
                if (full && !full->params.empty()) {
                    auto s = std::make_shared<SigInfo>(*full);
                    s->params.erase(s->params.begin());
                    if (!s->names.empty()) s->names.erase(s->names.begin());
                    s->required = std::max(0, s->required - 1);
                    return s;
                }
            }
        }
        return nullptr;
    }

    TypeP waitCall(Expr* e) {
        if (e->args.size() > 1) error(e->args[1]->span, "too many arguments", "wait() takes at most one argument (seconds)");
        if (e->args.empty()) {
            emitOp1(OP_CONST, constant(Value::number(0.0)), e->span);
        } else {
            TypeP t = expr(e->args[0].get(), T::floating());
            coerce(T::floating(), t, e->args[0]->span);
        }
        emitOp(OP_WAIT, e->span);
        return T::voidT();
    }

    TypeP callExpr(Expr* e, bool isStart) {
        Expr* callee = e->a.get();
        int argc = (int)e->args.size();
        Op callOp = isStart ? OP_START : OP_CALL;
        // wait(seconds): the coroutine yield intrinsic
        if (callee->k == EK::Ident && callee->name == "wait" && resolve("wait", false).k == Res::None) {
            if (isStart) error(e->span, "invalid start", "'wait' cannot be started as a coroutine");
            return waitCall(e);
        }
        // constructor
        int slot;
        ImportRef* via;
        if (ClassInfo* ci = classRef(callee, &slot, &via)) {
            if (sink_) {
                if (via) recordModuleMember(callee->nameSpan, via, callee->name);
                else recordRes(callee->span, resolve(callee->name, false), callee->name);
            }
            emitOp1(OP_GET_GLOBAL, slot, callee->span);
            if (via) emitOp1(OP_GET_FIELD, strConst(ci->name), callee->span);
            callArgs(e->args, ci->ctor.get(), e->span, ci->name);
            emitOp1(callOp, argc, e->span);
            return T::cls(ci);
        }
        if (callee->k == EK::Ident) {
            Res r = resolve(callee->name);
            if (r.k == Res::Member) {
                recordRes(callee->span, r, callee->name);
                emitThis(callee->span);
                const SigInfo* sig = r.method ? r.method->sig.get() : (r.type && r.type->k == TK::Sig ? r.type->sig.get() : nullptr);
                if (isStart) emitOp1(OP_GET_FIELD, strConst(callee->name), callee->span);
                callArgs(e->args, sig, e->span, callee->name);
                if (isStart) emitOp1(OP_START, argc, e->span);
                else emitInvoke(callee->name, argc, callee->span, r.method != nullptr);
                if (r.field && !isLoose(r.type) && r.type->k != TK::Func && r.type->k != TK::Sig) {
                    error(callee->span, "not callable", "field '" + callee->name + "' of type '" + T::name(r.type) + "' is not a function");
                }
                return sig ? retOf(sig) : T::any();
            }
        }
        if (callee->k == EK::Ident) {
            Res r = resolve(callee->name, false);
            if (r.k == Res::Static) {
                // a static method called by its bare name inside its class
                resolve(callee->name);
                recordRes(callee->span, r, callee->name);
                const SigInfo* sig = r.method ? r.method->sig.get() : (r.type && r.type->k == TK::Sig ? r.type->sig.get() : nullptr);
                emitStaticClass(r.cls, callee->span);
                if (isStart) emitOp1(OP_GET_FIELD, strConst(callee->name), callee->span);
                callArgs(e->args, sig, e->span, callee->name);
                if (isStart) emitOp1(OP_START, argc, e->span);
                else emitInvoke(callee->name, argc, callee->span, false);
                return sig ? retOf(sig) : T::any();
            }
            // Vector3(1, 2, 3): constructor of a native value type
            NativeClass* nc = vm_.nativeTypeByName(callee->name);
            if (nc && !nc->ctorSig.empty() && (r.k == Res::Builtin || r.k == Res::None)) {
                SigP s = sigFromString(nc->ctorSig);
                if (s) sigHold_.push_back(s);
                recordUse(callee->span, s ? sigText("class", nc->name, *s) : "class " + nc->name, -1, {});
                emitOp2(OP_GET_BUILTIN_C, strConst(nc->name), newCache(), callee->span);
                callArgs(e->args, s.get(), e->span, nc->name);
                emitOp1(callOp, argc, e->span);
                return T::native(nc);
            }
        }
        if (callee->k == EK::Member) {
            {
                int cslot;
                ImportRef* cvia;
                if (ClassInfo* ci = classRef(callee->a.get(), &cslot, &cvia)) {
                    // ClassName.staticMethod(...)
                    emitOp1(OP_GET_GLOBAL, cslot, callee->a->span);
                    if (cvia) emitOp1(OP_GET_FIELD, strConst(ci->name), callee->a->span);
                    const SigInfo* sig = nullptr;
                    if (isPlaceholder(callee->name)) {
                        sink_->haveReceiver = true;
                        sink_->receiverStatics = ci;
                    } else if (ci->staticMethods.count(callee->name)) {
                        const ClassInfo::Method& m = ci->staticMethods[callee->name];
                        sig = m.sig.get();
                        recordUse(callee->nameSpan, sigText("static func", ci->name + "." + callee->name, *m.sig), ci->fileId,
                                  m.decl ? m.decl->nameSpan : Span {});
                        if (m.isPrivate && staticContext() != ci) {
                            error(callee->nameSpan, "private method", "'" + callee->name + "' is private to class '" + ci->name + "'");
                        }
                    } else if (ci->staticIdx.count(callee->name)) {
                        const ClassInfo::Field& f = ci->statics[ci->staticIdx[callee->name]];
                        if (f.type->k == TK::Sig) sig = f.type->sig.get();
                        else if (!isLoose(f.type) && f.type->k != TK::Func) {
                            error(callee->nameSpan, "not callable", "static field '" + callee->name + "' is not a function");
                        }
                    } else {
                        std::vector<std::string> notes;
                        if (ci->methods.count(callee->name)) {
                            notes.push_back("'" + callee->name + "' is an instance method; call it on an object of class '" + ci->name + "'");
                        }
                        error(callee->nameSpan, "unknown static method",
                              "class '" + ci->name + "' has no static method '" + callee->name + "'", notes);
                    }
                    if (isStart) emitOp1(OP_GET_FIELD, strConst(callee->name), callee->a->span);
                    callArgs(e->args, sig, e->span, callee->name);
                    if (isStart) emitOp1(OP_START, argc, e->span);
                    else emitInvoke(callee->name, argc, callee->a->span, false);
                    return sig ? retOf(sig) : T::any();
                }
            }
            if (EnumInfo* en = enumRef(callee->a.get())) {
                (void)en;
                TypeP t = expr(callee);
                error(callee->span, "not callable", "enum members are values, not functions");
                return t;
            }
            ImportRef* ir = modulePath(callee->a.get());
            if (ir) {
                emitOp1(OP_GET_GLOBAL, ir->slot, callee->a->span);
                recordModuleMember(callee->nameSpan, ir, callee->name);
                auto it = ir->mod->globals.find(callee->name);
                const SigInfo* sig = nullptr;
                if (it == ir->mod->globals.end() || it->second.kind == ModuleSymbols::Global::Import) {
                    error(callee->nameSpan, "unknown member", "module '" + ir->mod->key + "' has no member '" + callee->name + "'");
                } else if (it->second.type && it->second.type->k == TK::Sig) {
                    sig = it->second.type->sig.get();
                }
                if (isStart) emitOp1(OP_GET_FIELD, strConst(callee->name), callee->a->span);
                callArgs(e->args, sig, e->span, callee->name);
                if (isStart) emitOp1(OP_START, argc, e->span);
                else emitOp2(OP_INVOKE, strConst(callee->name), argc, callee->a->span);
                return sig ? retOf(sig) : T::any();
            }
            // method call on a value
            TypeP ot = expr(callee->a.get());
            const std::string& m = callee->name;
            Span rs = callee->a->span;
            recordMember(callee->nameSpan, ot, m);
            const SigInfo* sig = nullptr;
            TypeP ret = T::any();
            bool known = false;
            if (ot->k == TK::Void) {
                error(rs, "no value", "this expression does not produce a value");
            } else if ((ot->nullable || ot->k == TK::Null) && !isLoose(ot)) {
                nullError(callee->a.get(), ot);
            } else if (!isLoose(ot)) {
                switch (ot->k) {
                case TK::Class: {
                    ClassInfo* ci = ot->cls;
                    auto mi = ci->methods.find(m);
                    auto fi = ci->fieldIdx.find(m);
                    if (mi != ci->methods.end()) {
                        if (mi->second.isPrivate && enclosingClass() != ci) {
                            error(callee->nameSpan, "private method", "'" + m + "' is private to class '" + ci->name + "'");
                        }
                        if (m == "init") error(callee->nameSpan, "invalid call", "'init' is called automatically by '" + ci->name + "(...)'");
                        sig = mi->second.sig.get();
                        known = true;
                    } else if (fi != ci->fieldIdx.end()) {
                        const auto& f = ci->fields[fi->second];
                        if (f.isPrivate && enclosingClass() != ci) {
                            error(callee->nameSpan, "private field", "'" + m + "' is private to class '" + ci->name + "'");
                        }
                        if (f.type->nullable && !isLoose(f.type)) {
                            error(callee->nameSpan, "possible null reference", "'" + m + "' is type '" + T::name(f.type) + "' (nullable) - check for null before calling");
                        } else if (f.type->k == TK::Sig) {
                            sig = f.type->sig.get();
                        } else if (!isLoose(f.type) && f.type->k != TK::Func) {
                            error(callee->nameSpan, "not callable", "field '" + m + "' of type '" + T::name(f.type) + "' is not a function");
                        }
                        known = true;
                    } else {
                        error(callee->nameSpan, "unknown method", "class '" + ci->name + "' has no method '" + m + "'");
                    }
                    break;
                }
                case TK::Interface: {
                    auto mi = ot->iface->methods.find(m);
                    if (mi != ot->iface->methods.end()) {
                        sig = mi->second.get();
                        known = true;
                    } else {
                        error(callee->nameSpan, "unknown method", "interface '" + ot->iface->name + "' has no method '" + m + "'");
                    }
                    break;
                }
                case TK::Native: {
                    auto mi = ot->native->methods.find(m);
                    if (mi != ot->native->methods.end()) {
                        SigP s = mi->second.sig.empty() ? nullptr : sigFromString(mi->second.sig);
                        if (s) {
                            sig = s.get();
                            sigHold_.push_back(s);
                        }
                        known = true;
                    } else {
                        error(callee->nameSpan, "unknown method", "'" + T::name(ot) + "' has no method '" + m + "'");
                    }
                    break;
                }
                case TK::Namespace: {
                    ObjNamespace* ns = vm_.namespaceFor(ot->nsName, false);
                    bool found = false;
                    if (ns) {
                        for (auto& kv : ns->members) found |= kv.first->s == m;
                    }
                    if (!found) {
                        error(callee->nameSpan, "unknown member", "'" + ot->nsName + "' has no member '" + m + "'");
                    } else {
                        auto si = vm_.nativeSigs.find(ot->nsName + "." + m);
                        SigP s = si != vm_.nativeSigs.end() ? sigFromString(si->second) : nullptr;
                        if (s) {
                            sig = s.get();
                            sigHold_.push_back(s);
                        }
                        known = true;
                        if (ot->nsName == "math" && (m == "abs" || m == "min" || m == "max")) {
                            // numeric promotion: int in, int out
                            if (isStart) emitOp1(OP_GET_FIELD, strConst(m), rs);
                            bool allInt = true;
                            for (auto& a : e->args) {
                                TypeP at = expr(a.get());
                                if (!isLoose(at) && !isNumeric(at)) {
                                    error(a->span, "type mismatch", "math." + m + " expects a number, found '" + T::name(at) + "'");
                                }
                                if (at->k != TK::Int) allInt = false;
                            }
                            size_t need = m == "abs" ? 1 : 2;
                            if (e->args.size() < need) {
                                error(e->span, "missing arguments", "'math." + m + "' expects " + std::to_string(need) + " argument" + (need == 1 ? "" : "s"));
                            } else if (m == "abs" && e->args.size() > 1) {
                                error(e->args[1]->span, "too many arguments", "'math.abs' takes 1 argument");
                            }
                            if (isStart) emitOp1(OP_START, argc, e->span);
                            else emitInvoke(m, argc, rs, true);
                            return allInt ? T::integer() : T::floating();
                        }
                    }
                    break;
                }
                case TK::List: case TK::Dict: case TK::Str: {
                    SigP s = builtinMethod(ot, m);
                    if (s) {
                        sigHold_.push_back(s);
                        sig = s.get();
                        known = true;
                    } else {
                        error(callee->nameSpan, "unknown method", "type '" + T::name(ot) + "' has no method '" + m + "'");
                    }
                    if (isStart) error(e->span, "invalid start", "built-in methods cannot be started as coroutines");
                    break;
                }
                case TK::Enum:
                    error(callee->nameSpan, "unknown method", "enum values have no methods");
                    break;
                default:
                    error(callee->nameSpan, "unknown method", "type '" + T::name(ot) + "' has no method '" + m + "'");
                    break;
                }
            }
            (void)known;
            if (isStart) emitOp1(OP_GET_FIELD, strConst(m), rs);
            callArgs(e->args, sig, e->span, m);
            if (isStart) emitOp1(OP_START, argc, e->span);
            else {
                bool cached = !isLoose(ot) && !ot->nullable &&
                              (ot->k == TK::Class || ot->k == TK::Interface || ot->k == TK::Namespace || ot->k == TK::Native);
                emitInvoke(m, argc, rs, cached);
            }
            if (sig) ret = retOf(sig);
            return ret;
        }
        // any other callee value
        TypeP ct = expr(callee);
        const SigInfo* sig = nullptr;
        std::string fname = callee->k == EK::Ident ? callee->name : "function";
        if (ct->k == TK::Sig) {
            sig = ct->sig.get();
        } else if (ct->nullable && !isLoose(ct)) {
            nullError(callee, ct);
        } else if (!isLoose(ct) && ct->k != TK::Func) {
            if (ct->k != TK::Void) error(callee->span, "not callable", "a value of type '" + T::name(ct) + "' cannot be called");
        }
        callArgs(e->args, sig, e->span, fname);
        emitOp1(callOp, argc, e->span);
        return sig ? retOf(sig) : T::any();
    }

    std::vector<SigP> sigHold_;
};

} // namespace

Proto* compileModule(VM& vm, ModuleSymbols& syms, ImportResolver& resolver, std::vector<Diagnostic>& diags,
                     const CompileOptions& opt) {
    Compiler c(vm, syms, resolver, diags, opt);
    return c.run();
}

} // namespace tp
