// vm.cpp - the bytecode interpreter: call frames, coroutines, exceptions.
#include "loader.h"

#include <algorithm>

namespace tp {

const char* opName(int op) {
    static const char* names[] = {
#define X(name, n) #name,
        TAIPAN_OPCODES(X)
#undef X
    };
    return op >= 0 && op < OP__COUNT ? names[op] : "?";
}

int opOperands(const int32_t* code, int ip) {
    static const int counts[] = {
#define X(name, n) n,
        TAIPAN_OPCODES(X)
#undef X
    };
    int op = code[ip];
    if (op == OP_CLOSURE) return 2 + 2 * code[ip + 2];
    if (op == OP_ENUM) return 3 + code[ip + 3];
    return counts[op];
}

// Deepest value-stack use of one frame, found by walking every path through
// the bytecode. pushFrame reserves this much, so the interpreter loop can
// push without bounds checks.
int computeMaxStack(const Proto* p) {
    const int32_t* c = p->code.data();
    int n = (int)p->code.size();
    std::vector<int> seen(n, -1);
    std::vector<std::pair<int, int>> work;
    int start = 1 + p->numParams;
    int maxDepth = start;
    work.emplace_back(0, start);
    auto enqueue = [&](int ip, int d) {
        if (ip >= 0 && ip < n && seen[ip] < d) work.emplace_back(ip, d);
    };
    while (!work.empty()) {
        int ip = work.back().first;
        int d = work.back().second;
        work.pop_back();
        while (ip >= 0 && ip < n) {
            if (seen[ip] >= d) break;
            seen[ip] = d;
            if (d > maxDepth) maxDepth = d;
            int op = c[ip];
            int next = ip + 1 + opOperands(c, ip);
            auto target = [&](int k) { return ip + 1 + k + 1 + c[ip + 1 + k]; };
            bool fall = true;
            switch (op) {
            case OP_CONST: case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_DUP: case OP_GET_LOCAL: case OP_GET_UPVAL:
            case OP_GET_GLOBAL: case OP_GET_BUILTIN: case OP_CLOSURE: case OP_CLASS: case OP_ENUM: case OP_IMPORT:
            case OP_GET_LFIELD: case OP_GET_BUILTIN_C:
                d += 1;
                break;
            case OP_DUP2: d += 2; break;
            case OP_POP: case OP_SET_LOCAL: case OP_SET_UPVAL: case OP_SET_GLOBAL: case OP_METHOD: case OP_DEF_CLASS:
            case OP_GET_INDEX: case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_BAND: case OP_BOR:
            case OP_BXOR: case OP_SHL: case OP_SHR: case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            case OP_ADD_LOCAL: case OP_SUB_LOCAL: case OP_STATIC_METHOD:
                d -= 1;
                break;
            case OP_POPN: d -= c[ip + 1]; break;
            case OP_SET_FIELD: case OP_SET_FIELD_I: d -= 2; break;
            case OP_SET_INDEX: d -= 3; break;
            case OP_CALL: case OP_START: d -= c[ip + 1]; break;
            case OP_INVOKE: case OP_INVOKE_C: d -= c[ip + 2]; break;
            case OP_BUILD_LIST: case OP_TUPLE: case OP_FSTRING: d += 1 - c[ip + 1]; break;
            case OP_BUILD_DICT: d += 1 - 2 * c[ip + 1]; break;
            case OP_UNPACK: d += c[ip + 1] - 1; break;
            case OP_JUMP: enqueue(target(0), d); fall = false; break;
            case OP_LOOP: enqueue(ip + 2 - c[ip + 1], d); fall = false; break;
            case OP_RANGE_LOOP: enqueue(ip + 4 - c[ip + 3], d); break;
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: d -= 1; enqueue(target(0), d); break;
            case OP_AND_JUMP: case OP_OR_JUMP: case OP_COALESCE_JUMP: enqueue(target(0), d); d -= 1; break;
            case OP_JLT: case OP_JLE: case OP_JGT: case OP_JGE: case OP_JEQ: case OP_JNE: d -= 2; enqueue(target(0), d); break;
            case OP_JLT_K: case OP_JLE_K: case OP_JGT_K: case OP_JGE_K: case OP_JEQ_K: case OP_JNE_K:
                d -= 1;
                enqueue(target(1), d);
                break;
            case OP_ITER_NEXT: enqueue(target(2), d); d += c[ip + 2]; break;
            case OP_RANGE_NEXT: enqueue(target(1), d); d += 1; break;
            case OP_RANGE_INIT: enqueue(target(2), d); break;
            case OP_TRY: enqueue(target(0), d + 1); break; // the handler starts with the error pushed
            case OP_ARG_GIVEN: case OP_JUMP_IF_DEFINED: enqueue(target(1), d); break;
            case OP_JUMP_IF_RELOADING: enqueue(target(0), d); break;
            case OP_RETURN: case OP_THROW: fall = false; break;
            default: break; // net zero
            }
            if (!fall) break;
            ip = next;
        }
    }
    return maxDepth;
}

static inline bool isStr(const Value& v) { return v.t == VT::Obj && v.o->type == OT::String; }
static inline ObjString* asStr(const Value& v) { return static_cast<ObjString*>(v.o); }

VM::VM() {
    gcPause++;
    builtins = newNamespace("builtins");
    mainThread = newThread();
    current = mainThread;
    auto perm = [this](const char* s) {
        ObjString* o = str(s);
        pins.push_back(Value::object(o));
        return o;
    };
    names.length = perm("length");
    names.init = perm("init");
    names.empty = perm("");
    const char* lm[] = {"add", "insert", "removeAt", "remove", "pop", "contains", "indexOf", "clear", "forEach",
                        "map", "filter", "sort", "reverse", "slice", "join", "copy", "isEmpty"};
    for (int i = 0; i < (int)(sizeof lm / sizeof lm[0]); i++) listMethods[perm(lm[i])] = i;
    const char* dm[] = {"keys", "values", "has", "contains", "remove", "get", "clear", "copy", "isEmpty"};
    for (int i = 0; i < (int)(sizeof dm / sizeof dm[0]); i++) dictMethods[perm(dm[i])] = i;
    threadMethods[perm("stop")] = 0;
    threadMethods[perm("isRunning")] = 1;
    loader = makeLoader(*this);
    openCore();
    gcPause--;
}

Proto* VM::newProto() {
    protos.push_back(std::make_unique<Proto>());
    return protos.back().get();
}

int VM::addSource(const std::string& path, const std::string& display, std::string text) {
    SourceFile f;
    f.path = path;
    f.display = display;
    f.text = std::move(text);
    f.computeLines();
    sources.push_back(std::move(f));
    return (int)sources.size() - 1;
}

ObjModule* VM::newModule(const std::string& key, const std::string& path, int fileId) {
    ObjModule* m = alloc<ObjModule>();
    m->name = str(key);
    m->path = path;
    m->fileId = fileId;
    return m;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
std::string VM::traceback(const ScriptError& e) {
    if (e.frames.size() < 2) return "";
    std::string s = "traceback: ";
    for (size_t i = 0; i < e.frames.size(); i++) {
        Proto* p = e.frames[i].first;
        int ip = e.frames[i].second;
        std::string where = p->name;
        if (p->fileId >= 0 && p->fileId < (int)sources.size() && ip >= 0 && ip < (int)p->spans.size()) {
            int line, col;
            sources[p->fileId].lineCol(p->spans[ip].start, line, col);
            where += " (" + sources[p->fileId].display + ":" + std::to_string(line) + ")";
        }
        if (i) s += " <- ";
        s += where;
    }
    return s;
}

void VM::locate(ScriptError& e) {
    ObjThread* th = current;
    if (!th || th->frames.empty()) return;
    const CallFrame& f = th->frames.back();
    Proto* p = f.closure->proto;
    if (f.lastOp >= 0 && f.lastOp < (int)p->spans.size()) {
        e.span = p->spans[f.lastOp];
        e.fileId = p->fileId;
        e.located = true;
    }
    // cheap capture; the text is built only if the error is reported
    for (int i = (int)th->frames.size() - 1; i >= 0 && (int)e.frames.size() < 12; i--) {
        e.frames.emplace_back(th->frames[i].closure->proto, th->frames[i].lastOp);
    }
}

void VM::rtError(const std::string& title, const std::string& label) {
    ScriptError e;
    e.payload = strv(label);
    e.title = title;
    e.label = label;
    locate(e);
    throw e;
}

void VM::throwValue(const Value& payload, const std::string& title, const std::string& label) {
    ScriptError e;
    e.payload = payload;
    e.title = title;
    e.label = label;
    locate(e);
    throw e;
}

void VM::reportUncaught(const ScriptError& e) {
    Diagnostic d;
    d.sev = Diagnostic::Error;
    d.title = e.title.empty() ? "runtime error" : e.title;
    d.label = e.label;
    d.fileId = e.located ? e.fileId : -1;
    d.span = e.span;
    std::string trace = traceback(e);
    if (!trace.empty()) d.notes.push_back(trace);
    report(d);
}

// ---------------------------------------------------------------------------
// Upvalues
// ---------------------------------------------------------------------------
ObjUpvalue* VM::captureUpvalue(ObjThread* th, int slot) {
    ObjUpvalue* prev = nullptr;
    ObjUpvalue* up = th->openUpvals;
    while (up && up->slot > slot) {
        prev = up;
        up = up->nextOpen;
    }
    if (up && up->slot == slot) return up;
    ObjUpvalue* created = alloc<ObjUpvalue>();
    created->th = th;
    created->slot = slot;
    created->nextOpen = up;
    if (prev) prev->nextOpen = created;
    else th->openUpvals = created;
    return created;
}

void VM::closeUpvalues(ObjThread* th, int fromSlot) {
    while (th->openUpvals && th->openUpvals->slot >= fromSlot) {
        ObjUpvalue* up = th->openUpvals;
        up->closed = th->stack[up->slot];
        up->open = false;
        up->th = nullptr;
        th->openUpvals = up->nextOpen;
        barrier(up, up->closed);
    }
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------
void VM::pushFrame(ObjThread* th, ObjClosure* cl, int calleeIdx, int argc, bool isCtor) {
    Proto* p = cl->proto;
    if (th->frames.size() >= 4000) rtError("stack overflow", "too many nested calls (infinite recursion?)");
    ValueStack& S = th->stack;
    if (p->maxStack == 0) p->maxStack = computeMaxStack(p);
    S.reserve((size_t)calleeIdx + (size_t)p->maxStack + 16);
    int fixed = p->variadic ? p->numParams - 1 : p->numParams;
    if (argc < p->minParams || (!p->variadic && argc > p->numParams)) {
        std::string expect = p->variadic ? "at least " + std::to_string(p->minParams)
                           : p->minParams == p->numParams ? std::to_string(p->numParams)
                           : std::to_string(p->minParams) + " to " + std::to_string(p->numParams);
        rtError("wrong number of arguments",
                "'" + p->name + "' expects " + expect + " argument" + (p->numParams == 1 ? "" : "s") + ", found " +
                    std::to_string(argc));
    }
    if (p->variadic) {
        int extra = argc - fixed;
        ObjList* rest = newList();
        if (extra > 0) {
            rest->items.assign(S.end() - extra, S.end());
            S.resize(S.size() - extra);
        } else {
            for (int i = argc; i < fixed; i++) S.push_back(Value());
        }
        S.push_back(Value::object(rest));
    } else {
        for (int i = argc; i < p->numParams; i++) S.push_back(Value());
    }
    th->frames.push_back(CallFrame {cl, 0, calleeIdx, argc, 0, isCtor});
}

// Natives read their arguments in place. The slots just above the
// arguments are set to null, so a native may read a missing argument as null.
// `argv` is only valid until the native calls back into the VM.
static inline Value callNativeInPlace(VM& vm, ObjThread* th, ObjNative* n, int ci, int argc) {
    ValueStack& S = th->stack;
    S.reserve(S.top + 8);
    Value* argv = S.data + ci + 1;
    for (int i = argc; i < 4; i++) argv[i] = Value();
    return n->fn(vm, argv, argc);
}

void VM::callSetup(ObjThread* th, int argc) {
    ValueStack& S = th->stack;
    int ci = (int)S.size() - argc - 1;
    Value callee = S[ci];
    if (!callee.isObj()) {
        rtError(callee.isNull() ? "null reference" : "not callable",
                callee.isNull() ? "cannot call null" : "a value of type '" + typeName(callee) + "' cannot be called");
    }
    switch (callee.o->type) {
    case OT::Closure:
        pushFrame(th, static_cast<ObjClosure*>(callee.o), ci, argc, false);
        return;
    case OT::Native: {
        Value r = callNativeInPlace(*this, th, static_cast<ObjNative*>(callee.o), ci, argc);
        S.top = (size_t)ci;
        S.push_back(r);
        return;
    }
    case OT::Bound: {
        auto* b = static_cast<ObjBound*>(callee.o);
        S[ci] = b->recv;
        if (b->method.isObj() && b->method.o->type == OT::Closure) {
            pushFrame(th, static_cast<ObjClosure*>(b->method.o), ci, argc, false);
            return;
        }
        if (isStr(b->method) && b->recv.isObj() && b->recv.o->type == OT::Value) {
            S[ci] = b->recv;
            invoke(th, asStr(b->method), argc);
            return;
        }
        if (isStr(b->method) && b->recv.isObj() && b->recv.o->type == OT::Handle) {
            Value r = callNativeMethod(static_cast<ObjHandle*>(b->recv.o), asStr(b->method)->s, S.data + ci + 1, argc);
            S.top = (size_t)ci;
            S.push_back(r);
            return;
        }
        S[ci] = b->method;
        callSetup(th, argc);
        return;
    }
    case OT::Namespace: {
        auto* ns = static_cast<ObjNamespace*>(callee.o);
        if (ns->call.t != VT::Obj) rtError("not callable", "'" + ns->name->s + "' has no constructor");
        S[ci] = ns->call;
        callSetup(th, argc);
        return;
    }
    case OT::Class: {
        auto* c = static_cast<ObjClass*>(callee.o);
        ObjInstance* inst = alloc<ObjInstance>(c->fields.size() * sizeof(Value));
        inst->cls = c;
        inst->fields.resize(c->fields.size());
        S[ci] = Value::object(inst);
        if (c->init.isObj() && c->init.o->type == OT::Closure) {
            pushFrame(th, static_cast<ObjClosure*>(c->init.o), ci, argc, true);
        } else if (argc > 0) {
            rtError("wrong number of arguments", "'" + c->name->s + "' takes no constructor arguments");
        }
        return;
    }
    default:
        rtError("not callable", "a value of type '" + typeName(callee) + "' cannot be called");
    }
}

void VM::invoke(ObjThread* th, ObjString* name, int argc) {
    ValueStack& S = th->stack;
    int ri = (int)S.size() - argc - 1;
    Value recv = S[ri];
    if (!recv.isObj()) {
        if (recv.isNull()) rtError("null reference", "cannot call method '" + name->s + "' on null");
        rtError("unknown method", "'" + typeName(recv) + "' has no method '" + name->s + "'");
    }
    switch (recv.o->type) {
    case OT::Instance: {
        auto* inst = static_cast<ObjInstance*>(recv.o);
        auto fi = inst->cls->fieldIndex.find(name);
        if (fi != inst->cls->fieldIndex.end()) {
            S[ri] = instanceField(inst, fi->second);
            callSetup(th, argc);
            return;
        }
        auto mi = inst->cls->methods.find(name);
        if (mi != inst->cls->methods.end()) {
            if (mi->second.isObj() && mi->second.o->type == OT::Closure) {
                pushFrame(th, static_cast<ObjClosure*>(mi->second.o), ri, argc, false);
            } else {
                S[ri] = mi->second;
                callSetup(th, argc);
            }
            return;
        }
        rtError("unknown method", "'" + inst->cls->name->s + "' has no method '" + name->s + "'");
    }
    case OT::Module: case OT::Namespace: case OT::Class: {
        S[ri] = getField(recv, name);
        callSetup(th, argc);
        return;
    }
    case OT::Handle: {
        Value r = callNativeMethod(static_cast<ObjHandle*>(recv.o), name->s, S.data + ri + 1, argc);
        S.top = (size_t)ri;
        S.push_back(r);
        return;
    }
    case OT::Value: {
        auto* v = static_cast<ObjValue*>(recv.o);
        auto it = v->cls->methods.find(name->s);
        if (it == v->cls->methods.end()) rtError("unknown method", "'" + v->cls->name + "' has no method '" + name->s + "'");
        if (!it->second.fn) {
            rtError("missing native", "'" + v->cls->name + "." + name->s + "' is declared in a binding file but the host did not register it");
        }
        TaipanArgs args = makeArgs(*this, S.data + ri + 1, argc);
        TaipanMethodFn fn = it->second.fn;
        Value r;
        size_t mark = tempRoots.size();
        hostDepth++;
        try {
            r = hostToValue(*this, fn(v->data, std::move(args)));
        } catch (HostError& e) {
            hostDepth--;
            tempRoots.resize(mark);
            rtError("native error", e.message);
        } catch (...) {
            hostDepth--;
            tempRoots.resize(mark);
            throw;
        }
        hostDepth--;
        tempRoots.resize(mark);
        S.top = (size_t)ri;
        S.push_back(r);
        return;
    }
    default:
        if (invokeBuiltin(th, name, argc)) return;
        rtError("unknown method", "'" + typeName(recv) + "' has no method '" + name->s + "'");
    }
}

bool VM::invokeBuiltin(ObjThread* th, ObjString* name, int argc) {
    ValueStack& S = th->stack;
    int ri = (int)S.size() - argc - 1;
    Value recv = S[ri];
    Value small[8];
    std::vector<Value> big;
    Value* args = small;
    if (argc > 8) {
        big.assign(S.begin() + ri + 1, S.end());
        args = big.data();
    } else {
        for (int i = 0; i < argc; i++) small[i] = S[ri + 1 + i];
    }
    static const int listMin[] = {1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0};
    static const int dictMin[] = {0, 0, 1, 1, 1, 2, 0, 0, 0};
    auto arity = [&](int need) {
        if (argc < need) {
            rtError("missing arguments", "'" + name->s + "' expects " + std::to_string(need) + " argument" +
                                             (need == 1 ? "" : "s") + ", found " + std::to_string(argc));
        }
    };
    Value r;
    switch (recv.o->type) {
    case OT::List: {
        auto it = listMethods.find(name);
        if (it == listMethods.end()) return false;
        arity(listMin[it->second]);
        r = builtinListMethod(static_cast<ObjList*>(recv.o), it->second, args, argc);
        break;
    }
    case OT::Dict: {
        auto it = dictMethods.find(name);
        if (it == dictMethods.end()) return false;
        arity(dictMin[it->second]);
        r = builtinDictMethod(static_cast<ObjDict*>(recv.o), it->second, args, argc);
        break;
    }
    case OT::String: {
        ObjNamespace* ns = namespaceFor("str", false);
        if (!ns) return false;
        auto it = ns->members.find(name);
        if (it == ns->members.end() || !it->second.isObj() || it->second.o->type != OT::Native) return false;
        std::vector<Value> full;
        full.push_back(recv);
        full.insert(full.end(), args, args + argc);
        full.resize(std::max(argc + 1, 8));
        r = static_cast<ObjNative*>(it->second.o)->fn(*this, full.data(), argc + 1);
        break;
    }
    case OT::Thread: {
        auto it = threadMethods.find(name);
        if (it == threadMethods.end()) return false;
        auto* t = static_cast<ObjThread*>(recv.o);
        if (it->second == 0) {
            if (t->state == ThreadState::Running) t->stopRequested = true; // stops at its next wait()
            else if (t->state != ThreadState::Dead) finishThread(t);
        } else {
            r = Value::boolean(t->state != ThreadState::Dead && !t->stopRequested);
        }
        break;
    }
    default:
        return false;
    }
    S.resize(ri);
    S.push_back(r);
    return true;
}

Value VM::callValue(const Value& fn, const std::vector<Value>& args) {
    ObjThread* th = current;
    if (nativeCallDepth > 200) rtError("stack overflow", "too many nested native calls");
    size_t savedTop = th->stack.size();
    size_t savedFrames = th->frames.size();
    th->stack.push_back(fn);
    for (auto& a : args) th->stack.push_back(a);
    th->nativeDepth++;
    nativeCallDepth++;
    try {
        callSetup(th, (int)args.size());
        if (th->frames.size() > savedFrames) run(th, savedFrames);
    } catch (...) {
        th->nativeDepth--;
        nativeCallDepth--;
        closeUpvalues(th, (int)savedTop);
        th->frames.resize(savedFrames);
        th->stack.resize(savedTop);
        throw;
    }
    th->nativeDepth--;
    nativeCallDepth--;
    Value result = th->stack.back();
    th->stack.resize(savedTop);
    return result;
}

// Calls a script method with `recv` as `this` (used by overloaded operators).
Value VM::callMethod(const Value& recv, ObjClosure* method, const std::vector<Value>& args) {
    ObjThread* th = current;
    if (nativeCallDepth > 200) rtError("stack overflow", "too many nested native calls");
    size_t savedTop = th->stack.size();
    size_t savedFrames = th->frames.size();
    th->stack.push_back(recv);
    for (auto& a : args) th->stack.push_back(a);
    th->nativeDepth++;
    nativeCallDepth++;
    try {
        pushFrame(th, method, (int)savedTop, (int)args.size(), false);
        run(th, savedFrames);
    } catch (...) {
        th->nativeDepth--;
        nativeCallDepth--;
        closeUpvalues(th, (int)savedTop);
        th->frames.resize(savedFrames);
        th->stack.resize(savedTop);
        throw;
    }
    th->nativeDepth--;
    nativeCallDepth--;
    Value result = th->stack.back();
    th->stack.resize(savedTop);
    return result;
}

// ---------------------------------------------------------------------------
// Overloaded operators
// ---------------------------------------------------------------------------
bool VM::matchesType(const Value& v, const std::string& type) {
    if (type.empty() || type == "any") return true;
    switch (v.t) {
    case VT::Null: return !type.empty() && type.back() == '?';
    case VT::Bool: return type == "bool";
    case VT::Int: return type == "int" || type == "float";
    case VT::Float: return type == "float";
    case VT::Obj: break;
    }
    std::string base = !type.empty() && type.back() == '?' ? type.substr(0, type.size() - 1) : type;
    NativeClass* cls = nullptr;
    if (v.o->type == OT::Value) cls = static_cast<ObjValue*>(v.o)->cls;
    else if (v.o->type == OT::Handle) cls = static_cast<ObjHandle*>(v.o)->cls;
    if (cls) {
        if (cls->name == base) return true;
        for (auto& a : cls->aliases) {
            if (a == base) return true;
        }
        return false;
    }
    if (v.o->type == OT::String) return base == "str";
    if (v.o->type == OT::Instance) return static_cast<ObjInstance*>(v.o)->cls->name->s == base;
    if (v.o->type == OT::List) return base.rfind("list", 0) == 0;
    if (v.o->type == OT::Dict) return base.rfind("dict", 0) == 0;
    return false;
}

static ObjClosure* scriptOperator(VM& vm, const Value& v, const std::string& name) {
    if (v.t != VT::Obj || v.o->type != OT::Instance) return nullptr;
    auto* cls = static_cast<ObjInstance*>(v.o)->cls;
    auto it = cls->methods.find(vm.str(name));
    if (it == cls->methods.end() || it->second.t != VT::Obj || it->second.o->type != OT::Closure) return nullptr;
    return static_cast<ObjClosure*>(it->second.o);
}

bool VM::tryBinaryOperator(const char* op, const Value& a, const Value& b, Value& out) {
    if (ObjClosure* m = scriptOperator(*this, a, std::string("op") + op)) {
        out = callMethod(a, m, {b});
        return true;
    }
    if (a.t == VT::Obj && a.o->type == OT::Value) {
        auto* self = static_cast<ObjValue*>(a.o);
        for (auto& o : self->cls->ops) {
            if (!o.reversed && o.op == op && !o.other.empty() && matchesType(b, o.other)) {
                out = callNativeOp(self, o, &b, 1);
                return true;
            }
        }
    }
    if (b.t == VT::Obj && b.o->type == OT::Value) {
        auto* self = static_cast<ObjValue*>(b.o);
        for (auto& o : self->cls->ops) {
            if (o.reversed && o.op == op && matchesType(a, o.other)) {
                out = callNativeOp(self, o, &a, 1);
                return true;
            }
        }
    }
    return false;
}

bool VM::tryUnaryOperator(const char* op, const Value& a, Value& out) {
    if (ObjClosure* m = scriptOperator(*this, a, std::string("op_") + op)) {
        out = callMethod(a, m, {});
        return true;
    }
    if (a.t == VT::Obj && a.o->type == OT::Value) {
        auto* self = static_cast<ObjValue*>(a.o);
        for (auto& o : self->cls->ops) {
            if (o.op == op && o.other.empty()) {
                out = callNativeOp(self, o, nullptr, 0);
                return true;
            }
        }
    }
    return false;
}

bool VM::overloadedEquals(const Value& a, const Value& b, bool& result) {
    Value out;
    if (!tryBinaryOperator("==", a, b, out)) return false;
    result = truthy(out);
    return true;
}

// ---------------------------------------------------------------------------
// Coroutines
// ---------------------------------------------------------------------------
void VM::finishThread(ObjThread* th) {
    if (!th->stack.empty()) closeUpvalues(th, 0);
    th->stack.clear();
    th->frames.clear();
    th->handlers.clear();
    th->state = ThreadState::Dead;
    coroutines.erase(std::remove(coroutines.begin(), coroutines.end(), th), coroutines.end());
}

ObjThread* VM::startCoroutine(ObjThread* from, int argc) {
    ValueStack& S = from->stack;
    int ci = (int)S.size() - argc - 1;
    ObjThread* co = newThread();
    co->isCoroutine = true;
    co->stack.assign(S.begin() + ci, S.end());
    S.resize(ci);
    S.push_back(Value::object(co)); // the result of `start`, and keeps co rooted
    coroutines.push_back(co);
    ObjThread* saved = current;
    current = co;
    co->state = ThreadState::Running;
    try {
        callSetup(co, argc);
        if (!co->frames.empty() && run(co, 0) == RunResult::Yielded && co->state != ThreadState::Dead) {
            co->state = ThreadState::Suspended;
        } else {
            co->state = ThreadState::Dead;
        }
    } catch (ScriptError& e) {
        reportUncaught(e);
        co->state = ThreadState::Dead;
    }
    current = saved;
    if (co->state == ThreadState::Dead) finishThread(co);
    return co;
}

void VM::resume(ObjThread* th) {
    ObjThread* saved = current;
    current = th;
    th->state = ThreadState::Running;
    try {
        if (run(th, 0) == RunResult::Yielded && th->state != ThreadState::Dead) th->state = ThreadState::Suspended;
        else th->state = ThreadState::Dead;
    } catch (ScriptError& e) {
        reportUncaught(e);
        th->state = ThreadState::Dead;
    }
    current = saved;
    if (th->state == ThreadState::Dead) finishThread(th);
}

void VM::update(double dt) {
    time += dt;
    std::vector<ObjThread*> due;
    for (auto* t : coroutines) {
        if (t->state == ThreadState::Suspended && t->wakeAt <= time + 1e-9) due.push_back(t);
    }
    std::stable_sort(due.begin(), due.end(), [](ObjThread* a, ObjThread* b) { return a->wakeAt < b->wakeAt; });
    size_t mark = tempRoots.size();
    for (auto* t : due) tempRoots.push_back(Value::object(t));
    for (auto* t : due) {
        if (t->state == ThreadState::Suspended) resume(t);
    }
    tempRoots.resize(mark);
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
bool VM::runModuleMain(ObjModule* m, bool reloading) {
    m->executed = true;
    m->reloading = reloading;
    try {
        callValue(m->mainFn, {});
    } catch (...) {
        m->reloading = false;
        throw;
    }
    m->reloading = false;
    return true;
}

Value VM::importModule(const std::string& key) {
    auto it = modules.find(key);
    ObjModule* m = it != modules.end() ? it->second : nullptr;
    if (!m) {
        std::vector<Diagnostic> diags;
        m = loader->instantiateKey(key, diags);
        reportAll(diags);
        if (!m) rtError("import failed", "could not load module '" + key + "'");
    }
    if (!m->executed) runModuleMain(m, false);
    return Value::object(m);
}

void VM::defineGlobal(ObjModule* m, int slot, const Value& v) {
    if (slot >= (int)m->globals.size()) {
        m->globals.resize(slot + 1);
        m->defined.resize(slot + 1);
    }
    Value& old = m->globals[slot];
    if (m->reloading && m->defined[slot] && v.isObj() && v.o->type == OT::Enum && old.isObj() &&
        old.o->type == OT::Enum && static_cast<ObjEnum*>(old.o)->qual == static_cast<ObjEnum*>(v.o)->qual) {
        // keep enum identity across hot reload so existing values still compare equal
        auto* oe = static_cast<ObjEnum*>(old.o);
        auto* ne = static_cast<ObjEnum*>(v.o);
        for (auto* nv : ne->values) {
            bool found = false;
            for (auto* ov : oe->values) found |= ov->name == nv->name;
            if (!found) {
                nv->en = oe;
                nv->index = (int)oe->values.size();
                oe->values.push_back(nv);
            }
        }
        touch(oe);
        return;
    }
    old = v;
    m->defined[slot] = 1;
    barrier(m, v);
}

// ---------------------------------------------------------------------------
// Fields, indexing, type checks
// ---------------------------------------------------------------------------
Value VM::zeroValue(uint8_t kind) {
    switch (kind) {
    case ZInt: return Value::integer(0);
    case ZFloat: return Value::number(0);
    case ZBool: return Value::boolean(false);
    case ZStr: return Value::object(names.empty);
    case ZList: return Value::object(newList());
    case ZDict: return Value::object(newDict());
    default: return Value();
    }
}

Value VM::instanceField(ObjInstance* inst, int idx) {
    if (idx >= (int)inst->fields.size()) {
        // a field added by hot reload: give existing instances its zero value
        size_t old = inst->fields.size();
        inst->fields.resize(inst->cls->fields.size());
        for (size_t i = old; i < inst->fields.size(); i++) inst->fields[i] = zeroValue(inst->cls->fields[i].zero);
        touch(inst);
    }
    return inst->fields[idx];
}

Value VM::getField(const Value& obj, ObjString* name) {
    if (!obj.isObj()) {
        if (obj.isNull()) rtError("null reference", "cannot read '" + name->s + "' of null");
        rtError("unknown member", "'" + typeName(obj) + "' has no member '" + name->s + "'");
    }
    Obj* o = obj.o;
    switch (o->type) {
    case OT::Instance: {
        auto* inst = static_cast<ObjInstance*>(o);
        auto fi = inst->cls->fieldIndex.find(name);
        if (fi != inst->cls->fieldIndex.end()) return instanceField(inst, fi->second);
        auto mi = inst->cls->methods.find(name);
        if (mi != inst->cls->methods.end()) {
            ObjBound* b = alloc<ObjBound>();
            b->recv = obj;
            b->method = mi->second;
            return Value::object(b);
        }
        rtError("unknown member", "'" + inst->cls->name->s + "' has no member '" + name->s + "'");
    }
    case OT::Module: {
        auto* m = static_cast<ObjModule*>(o);
        auto it = m->names.find(name);
        if (it == m->names.end() || it->second >= (int)m->globals.size()) {
            rtError("unknown member", "module '" + m->name->s + "' has no member '" + name->s + "'");
        }
        if (!m->defined[it->second]) {
            rtError("uninitialized global", "'" + name->s + "' in module '" + m->name->s + "' is used before its declaration ran");
        }
        return m->globals[it->second];
    }
    case OT::Namespace: {
        auto* ns = static_cast<ObjNamespace*>(o);
        auto it = ns->members.find(name);
        if (it == ns->members.end()) rtError("unknown member", "'" + ns->name->s + "' has no member '" + name->s + "'");
        return it->second;
    }
    case OT::Class: {
        auto* c = static_cast<ObjClass*>(o);
        auto si = c->staticIndex.find(name);
        if (si != c->staticIndex.end()) return c->statics[si->second];
        auto mi = c->staticMethods.find(name);
        if (mi != c->staticMethods.end()) return mi->second;
        rtError("unknown member", "class '" + c->name->s + "' has no static member '" + name->s + "'");
    }
    case OT::Enum: {
        auto* e = static_cast<ObjEnum*>(o);
        for (auto* v : e->values) {
            if (v->name == name) return Value::object(v);
        }
        rtError("unknown enum member", "enum '" + e->name->s + "' has no member '" + name->s + "'");
    }
    case OT::EnumValue: {
        auto* ev = static_cast<ObjEnumValue*>(o);
        if (name->s == "name") return Value::object(ev->name);
        if (name->s == "index") return Value::integer(ev->index);
        break;
    }
    case OT::List: case OT::Tuple:
        if (name == names.length) return Value::integer((int64_t)static_cast<ObjList*>(o)->items.size());
        break;
    case OT::Dict:
        if (name == names.length) return Value::integer((int64_t)static_cast<ObjDict*>(o)->entries.size());
        break;
    case OT::String:
        if (name == names.length) return Value::integer((int64_t)static_cast<ObjString*>(o)->s.size());
        break;
    case OT::Handle:
        return handleGet(*this, static_cast<ObjHandle*>(o), name);
    case OT::Value: {
        auto* v = static_cast<ObjValue*>(o);
        auto p = v->cls->props.find(name->s);
        if (p != v->cls->props.end() && p->second.get) return hostToValue(*this, p->second.get(v->data));
        if (v->cls->methods.count(name->s)) {
            ObjBound* b = alloc<ObjBound>();
            b->recv = obj;
            b->method = Value::object(name);
            return Value::object(b);
        }
        break;
    }
    default:
        break;
    }
    rtError("unknown member", "'" + typeName(obj) + "' has no member '" + name->s + "'");
}

void VM::setField(const Value& obj, ObjString* name, const Value& v) {
    if (!obj.isObj()) {
        if (obj.isNull()) rtError("null reference", "cannot set '" + name->s + "' on null");
        rtError("unknown member", "'" + typeName(obj) + "' has no field '" + name->s + "'");
    }
    Obj* o = obj.o;
    switch (o->type) {
    case OT::Instance: {
        auto* inst = static_cast<ObjInstance*>(o);
        auto fi = inst->cls->fieldIndex.find(name);
        if (fi == inst->cls->fieldIndex.end()) {
            rtError("unknown field", "'" + inst->cls->name->s + "' has no field '" + name->s + "'");
        }
        instanceField(inst, fi->second);
        inst->fields[fi->second] = v;
        barrier(inst, v);
        return;
    }
    case OT::Module: {
        auto* m = static_cast<ObjModule*>(o);
        auto it = m->names.find(name);
        if (it == m->names.end()) rtError("unknown member", "module '" + m->name->s + "' has no member '" + name->s + "'");
        defineGlobal(m, it->second, v);
        return;
    }
    case OT::Handle:
        handleSet(*this, static_cast<ObjHandle*>(o), name, v);
        return;
    case OT::Class: {
        auto* c = static_cast<ObjClass*>(o);
        auto si = c->staticIndex.find(name);
        if (si == c->staticIndex.end()) {
            rtError("unknown field", "class '" + c->name->s + "' has no static field '" + name->s + "'");
        }
        c->statics[si->second] = v;
        barrier(c, v);
        return;
    }
    case OT::Value:
        rtError("value types are immutable",
                "'" + static_cast<ObjValue*>(o)->cls->name + "' values cannot be changed - create a new one");
    default:
        rtError("unknown field", "cannot set field '" + name->s + "' on a value of type '" + typeName(obj) + "'");
    }
}

static int64_t checkIndex(VM& vm, const Value& idx, size_t size, const char* what) {
    if (!idx.isInt()) vm.rtError("type mismatch", std::string(what) + " index must be 'int', found '" + vm.typeName(idx) + "'");
    int64_t i = idx.i;
    if (i < 0) i += (int64_t)size;
    if (i < 0 || i >= (int64_t)size) {
        vm.rtError("index out of range", "index " + std::to_string(idx.i) + " is out of range for a " + what + " of length " +
                                             std::to_string(size));
    }
    return i;
}

Value VM::getIndex(const Value& obj, const Value& idx) {
    if (!obj.isObj()) {
        if (obj.isNull()) rtError("null reference", "cannot index null");
        rtError("not indexable", "cannot index a value of type '" + typeName(obj) + "'");
    }
    switch (obj.o->type) {
    case OT::List: case OT::Tuple: {
        auto* l = static_cast<ObjList*>(obj.o);
        return l->items[checkIndex(*this, idx, l->items.size(), "list")];
    }
    case OT::Dict: {
        Value* v = static_cast<ObjDict*>(obj.o)->find(idx);
        if (!v) {
            std::string k = isStr(idx) ? reprString(asStr(idx)->s) : toStr(idx);
            rtError("key not found", "key " + k + " is not in the dict (use .get(key, default) or .has(key))");
        }
        return *v;
    }
    case OT::String: {
        auto* s = asStr(obj);
        int64_t i = checkIndex(*this, idx, s->s.size(), "str");
        return strv(std::string(1, s->s[(size_t)i]));
    }
    default:
        rtError("not indexable", "cannot index a value of type '" + typeName(obj) + "'");
    }
}

void VM::setIndex(const Value& obj, const Value& idx, const Value& v) {
    if (!obj.isObj()) {
        if (obj.isNull()) rtError("null reference", "cannot index null");
        rtError("not indexable", "cannot index a value of type '" + typeName(obj) + "'");
    }
    switch (obj.o->type) {
    case OT::List: {
        auto* l = static_cast<ObjList*>(obj.o);
        l->items[checkIndex(*this, idx, l->items.size(), "list")] = v;
        barrier(l, v);
        return;
    }
    case OT::Dict: {
        auto* d = static_cast<ObjDict*>(obj.o);
        if (idx.isNull()) rtError("invalid key", "null cannot be used as a dict key");
        d->put(idx, v);
        barrier(d, idx);
        barrier(d, v);
        return;
    }
    case OT::String: rtError("strings are immutable", "cannot assign to a character of a 'str'");
    case OT::Tuple: rtError("tuples are immutable", "cannot assign into multiple return values");
    default:
        rtError("not indexable", "cannot index a value of type '" + typeName(obj) + "'");
    }
}

Value VM::checkType(const Value& v, ObjString* specStr) {
    const std::string& spec = specStr->s;
    bool nullable = !spec.empty() && spec.back() == '?';
    std::string base = nullable ? spec.substr(0, spec.size() - 1) : spec;
    auto display = [&]() {
        size_t c = base.rfind(':');
        return (c == std::string::npos ? base : base.substr(c + 1)) + (nullable ? "?" : "");
    };
    auto fail = [&]() {
        rtError("type mismatch", "expected '" + display() + "', found '" + typeName(v) + "'");
    };
    if (v.isNull()) {
        if (!nullable) fail();
        return v;
    }
    if (base == "int") {
        if (!v.isInt()) fail();
    } else if (base == "float") {
        if (v.isInt()) return Value::number((double)v.i);
        if (!v.isFloat()) fail();
    } else if (base == "bool") {
        if (!v.isBool()) fail();
    } else if (base == "str") {
        if (!isStr(v)) fail();
    } else if (base == "list") {
        if (!v.isObj() || v.o->type != OT::List) fail();
    } else if (base == "dict") {
        if (!v.isObj() || v.o->type != OT::Dict) fail();
    } else if (base == "tuple") {
        if (!v.isObj() || v.o->type != OT::Tuple) fail();
    } else if (base == "func") {
        if (!v.isObj() || !(v.o->type == OT::Closure || v.o->type == OT::Native || v.o->type == OT::Bound)) fail();
    } else if (base.size() > 2 && base[1] == ':') {
        std::string name = base.substr(2);
        bool ok = false;
        if (v.isObj()) {
            switch (base[0]) {
            case 'C':
                ok = v.o->type == OT::Instance && static_cast<ObjInstance*>(v.o)->cls->qual->s == name;
                break;
            case 'I':
                if (v.o->type == OT::Instance) {
                    for (auto* i : static_cast<ObjInstance*>(v.o)->cls->interfaces) ok |= i->s == name;
                }
                break;
            case 'E':
                ok = v.o->type == OT::EnumValue && static_cast<ObjEnumValue*>(v.o)->en->qual->s == name;
                break;
            case 'N':
                ok = v.o->type == OT::Handle && static_cast<ObjHandle*>(v.o)->cls &&
                     static_cast<ObjHandle*>(v.o)->cls->name == name;
                break;
            }
        }
        if (!ok) fail();
    }
    return v;
}

Value VM::convert(const Value& v, int code) {
    if (code == CV_TO_FLOAT) return v.isInt() ? Value::number((double)v.i) : v;
    if (code == CV_TO_INT) {
        if (!v.isFloat()) return v;
        double d = v.f;
        if (std::isnan(d)) return Value::integer(0);
        if (d >= 9.2233720368547758e18) return Value::integer(INT64_MAX);
        if (d <= -9.2233720368547758e18) return Value::integer(INT64_MIN);
        return Value::integer((int64_t)d);
    }
    if (code == CV_F32) return v.isNum() ? Value::number((double)(float)v.num()) : v;
    if (!v.isNum()) return v;
    int64_t i = v.isInt() ? v.i : convert(v, CV_TO_INT).i;
    switch (code) {
    case CV_I8: return Value::integer((int8_t)i);
    case CV_I16: return Value::integer((int16_t)i);
    case CV_I32: return Value::integer((int32_t)i);
    case CV_U8: return Value::integer((uint8_t)i);
    case CV_U16: return Value::integer((uint16_t)i);
    case CV_U32: return Value::integer((uint32_t)i);
    default: return Value::integer(i);
    }
}

NativeClass* VM::nativeTypeByName(const std::string& name) const {
    auto it = nativeTypeNames.find(name);
    return it == nativeTypeNames.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Interpreter loop
// ---------------------------------------------------------------------------
VM::RunResult VM::run(ObjThread* th, size_t baseFrames) {
    for (;;) {
        try {
            return execute(th, baseFrames);
        } catch (ScriptError& e) {
            if (!unwindTo(th, baseFrames, e)) throw;
        }
    }
}

bool VM::unwindTo(ObjThread* th, size_t baseFrames, const ScriptError& e) {
    if (!th->handlers.empty()) {
        Handler h = th->handlers.back();
        if (h.frameIndex >= (int)baseFrames && h.frameIndex < (int)th->frames.size()) {
            th->handlers.pop_back();
            closeUpvalues(th, h.stackSize);
            th->frames.resize(h.frameIndex + 1);
            th->stack.resize(h.stackSize);
            th->stack.push_back(e.payload);
            th->frames.back().ip = h.catchIp;
            return true;
        }
    }
    if (th->frames.size() > baseFrames) {
        closeUpvalues(th, th->frames[baseFrames].base);
        th->frames.resize(baseFrames);
    }
    return false;
}

static void arithError(VM& vm, const char* op, const Value& a, const Value& b) {
    vm.rtError("type mismatch", std::string("cannot apply '") + op + "' to '" + vm.typeName(a) + "' and '" + vm.typeName(b) + "'");
}

static bool compareValues(VM& vm, Value a, Value b, int op) {
    // op: 0 <, 1 <=, 2 >, 3 >=
    static const char* names[] = {"<", "<=", ">", ">="};
    int c;
    if (a.isNum() && b.isNum()) {
        if (a.isInt() && b.isInt()) c = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
        else {
            double x = a.num(), y = b.num();
            if (std::isnan(x) || std::isnan(y)) return false;
            c = x < y ? -1 : x > y ? 1 : 0;
        }
    } else if (isStr(a) && isStr(b)) {
        c = asStr(a)->s.compare(asStr(b)->s);
    } else {
        // overloaded operators; >, >= and <= fall back to <
        Value out;
        if (vm.tryBinaryOperator(names[op], a, b, out)) return vm.truthy(out);
        if (op == 2 && vm.tryBinaryOperator("<", b, a, out)) return vm.truthy(out);
        if (op == 3 && vm.tryBinaryOperator("<", a, b, out)) return !vm.truthy(out);
        if (op == 1 && vm.tryBinaryOperator("<", b, a, out)) return !vm.truthy(out);
        arithError(vm, names[op], a, b);
        return false;
    }
    switch (op) {
    case 0: return c < 0;
    case 1: return c <= 0;
    case 2: return c > 0;
    default: return c >= 0;
    }
}

// (forcing inlining with MSVC measured slower: it bloats the interpreter loop)
#if defined(__GNUC__) || defined(__clang__)
#define TP_FORCE_INLINE inline __attribute__((always_inline))
#else
#define TP_FORCE_INLINE inline
#endif

// Fast paths for arithmetic. `op` is always a compile-time constant at the
// call site, so the switch folds away. Returns false when the slow path
// (strings, errors, integer division corner cases) must handle it.
static TP_FORCE_INLINE bool fastArith(int op, Value& a, const Value& b) {
    if (a.t == VT::Int && b.t == VT::Int) {
        uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i;
        switch (op) {
        case OP_ADD: a.i = (int64_t)(x + y); return true;
        case OP_SUB: a.i = (int64_t)(x - y); return true;
        case OP_MUL: a.i = (int64_t)(x * y); return true;
        case OP_DIV:
            if (b.i == 0 || b.i == -1) return false;
            a.i = a.i / b.i;
            return true;
        default:
            if (b.i == 0 || b.i == -1) return false;
            a.i = a.i % b.i;
            return true;
        }
    }
    if (a.isNum() && b.isNum()) {
        double x = a.num(), y = b.num();
        switch (op) {
        case OP_ADD: a = Value::number(x + y); return true;
        case OP_SUB: a = Value::number(x - y); return true;
        case OP_MUL: a = Value::number(x * y); return true;
        case OP_DIV: a = Value::number(x / y); return true;
        default: a = Value::number(std::fmod(x, y)); return true;
        }
    }
    return false;
}

// Operands are passed by value: an overloaded operator runs script code,
// which may grow (reallocate) the value stack.
static Value arithSlow(VM& vm, int op, Value a, Value b) {
    if (op == OP_ADD && (isStr(a) || isStr(b))) return vm.strv(vm.toStr(a) + vm.toStr(b));
    if ((op == OP_DIV || op == OP_MOD) && a.t == VT::Int && b.t == VT::Int) {
        if (b.i == 0) vm.rtError("division by zero", op == OP_DIV ? "integer division by zero" : "integer modulo by zero");
        return Value::integer(op == OP_DIV ? (int64_t)(0ULL - (uint64_t)a.i) : 0); // b == -1
    }
    static const char* names[] = {"+", "-", "*", "/", "%"};
    Value out;
    if (vm.tryBinaryOperator(names[op - OP_ADD], a, b, out)) return out;
    arithError(vm, names[op - OP_ADD], a, b);
    return Value();
}

static TP_FORCE_INLINE bool fastLess(int op, const Value& a, const Value& b, bool& result) {
    if (a.t == VT::Int && b.t == VT::Int) {
        switch (op) {
        case OP_LT: result = a.i < b.i; break;
        case OP_LE: result = a.i <= b.i; break;
        case OP_GT: result = a.i > b.i; break;
        default: result = a.i >= b.i; break;
        }
        return true;
    }
    if (a.t == VT::Float && b.t == VT::Float) {
        switch (op) {
        case OP_LT: result = a.f < b.f; break;
        case OP_LE: result = a.f <= b.f; break;
        case OP_GT: result = a.f > b.f; break;
        default: result = a.f >= b.f; break;
        }
        return true;
    }
    return false;
}

#if defined(__GNUC__) || defined(__clang__)
#define TP_COMPUTED_GOTO 1
#endif

VM::RunResult VM::execute(ObjThread* th, size_t baseFrames) {
    CallFrame* frame;
    ObjClosure* cl;
    Proto* p;
    const int32_t* code;
    const int32_t* ip;
    const int32_t* opStart = nullptr;
    const Value* K;
    Value* slots;
    Value* sp;

    // The frame's ip and the stack top live in registers; SYNC() writes them
    // back before anything that can call out, throw, or collect garbage, and
    // LOAD() re-reads everything after such a call (frames and the stack may
    // have been reallocated).
#define LOAD()                                                  \
    do {                                                        \
        frame = &th->frames.back();                             \
        cl = frame->closure;                                    \
        p = cl->proto;                                          \
        code = p->code.data();                                  \
        ip = code + frame->ip;                                  \
        K = p->consts.data();                                   \
        slots = th->stack.data + frame->base;                   \
        sp = th->stack.data + th->stack.top;                    \
    } while (0)
#define SYNC()                                                  \
    do {                                                        \
        th->stack.top = (size_t)(sp - th->stack.data);          \
        frame->ip = (int)(ip - code);                           \
        frame->lastOp = (int)(opStart - code);                  \
    } while (0)
#define READ() (*ip++)
#define SAFEPOINT()                                             \
    do {                                                        \
        if (gcRequested | gcStress) {                           \
            SYNC();                                             \
            safepoint();                                        \
        }                                                       \
    } while (0)
#define STR(k) static_cast<ObjString*>(K[k].o)

#ifdef TP_COMPUTED_GOTO
#define TP_LABEL(name, n) &&L_##name,
    static void* const labels[] = {TAIPAN_OPCODES(TP_LABEL)};
#undef TP_LABEL
#define CASE(name) L_##name:
#define NEXT                                                    \
    do {                                                        \
        opStart = ip;                                           \
        goto* labels[*ip++];                                    \
    } while (0)
#else
#define CASE(name) case OP_##name:
#define NEXT continue
#endif

    LOAD();
#ifdef TP_COMPUTED_GOTO
    NEXT;
#else
    for (;;) {
        opStart = ip;
        switch (*ip++) {
#endif

    CASE(CONST) { *sp++ = K[READ()]; NEXT; }
    CASE(NIL) { *sp++ = Value(); NEXT; }
    CASE(TRUE) { *sp++ = Value::boolean(true); NEXT; }
    CASE(FALSE) { *sp++ = Value::boolean(false); NEXT; }
    CASE(POP) { --sp; NEXT; }
    CASE(POPN) { sp -= READ(); NEXT; }
    CASE(DUP) {
        *sp = sp[-1];
        sp++;
        NEXT;
    }
    CASE(DUP2) {
        sp[0] = sp[-2];
        sp[1] = sp[-1];
        sp += 2;
        NEXT;
    }
    CASE(GET_LOCAL) { *sp++ = slots[READ()]; NEXT; }
    CASE(SET_LOCAL) { slots[READ()] = *--sp; NEXT; }
    CASE(GET_UPVAL) { *sp++ = cl->upvals[READ()]->ref(); NEXT; }
    CASE(SET_UPVAL) {
        ObjUpvalue* up = cl->upvals[READ()];
        Value v = *--sp;
        up->ref() = v;
        if (!up->open) barrier(up, v);
        NEXT;
    }
    CASE(GET_GLOBAL) {
        int g = READ();
        ObjModule* m = cl->module;
        if (g >= (int)m->defined.size() || !m->defined[g]) {
            SYNC();
            std::string name = "?";
            for (auto& kv : m->names) {
                if (kv.second == g) name = kv.first->s;
            }
            rtError("uninitialized global", "'" + name + "' is used before its declaration ran");
        }
        *sp++ = m->globals[g];
        NEXT;
    }
    CASE(SET_GLOBAL) {
        int g = READ();
        Value v = *--sp;
        ObjModule* m = cl->module;
        if (!m->reloading && g < (int)m->defined.size() && m->defined[g]) {
            m->globals[g] = v; // plain update; defineGlobal handles first definition and hot reload
            barrier(m, v);
        } else {
            defineGlobal(m, g, v);
        }
        NEXT;
    }
    CASE(GET_BUILTIN) {
        ObjString* name = STR(READ());
        auto it = builtins->members.find(name);
        if (it == builtins->members.end()) {
            SYNC();
            rtError("unknown builtin", "'" + name->s + "' is not registered in this VM");
        }
        *sp++ = it->second;
        NEXT;
    }
    CASE(GET_BUILTIN_C) {
        ObjString* name = STR(READ());
        Proto::InlineCache& ic = p->caches[READ()];
        if (ic.epoch != nsEpoch) {
            auto it = builtins->members.find(name);
            if (it == builtins->members.end()) {
                SYNC();
                rtError("unknown builtin", "'" + name->s + "' is not registered in this VM");
            }
            ic.value = it->second;
            ic.epoch = nsEpoch;
        }
        *sp++ = ic.value;
        NEXT;
    }
    CASE(GET_FIELD) {
        ObjString* name = STR(READ());
        SYNC();
        Value v = getField(th->stack.data[th->stack.top - 1], name);
        LOAD();
        sp[-1] = v;
        NEXT;
    }
    CASE(GET_FIELD_I) {
        int idx = READ();
        int k = READ();
        Value o = sp[-1];
        if (o.t == VT::Obj && o.o->type == OT::Instance) {
            auto* inst = static_cast<ObjInstance*>(o.o);
            if (idx < (int)inst->fields.size() && inst->cls->fields[idx].name == K[k].o) {
                sp[-1] = inst->fields[idx];
                NEXT;
            }
        }
        SYNC();
        Value v = getField(o, STR(k));
        LOAD();
        sp[-1] = v;
        NEXT;
    }
    CASE(GET_LFIELD) {
        int slot = READ();
        int idx = READ();
        int k = READ();
        Value o = slots[slot];
        if (o.t == VT::Obj && o.o->type == OT::Instance) {
            auto* inst = static_cast<ObjInstance*>(o.o);
            if (idx < (int)inst->fields.size() && inst->cls->fields[idx].name == K[k].o) {
                *sp++ = inst->fields[idx];
                NEXT;
            }
        }
        SYNC();
        Value v = getField(o, STR(k));
        LOAD();
        *sp++ = v;
        NEXT;
    }
    CASE(SET_FIELD) {
        ObjString* name = STR(READ());
        SYNC();
        setField(th->stack.data[th->stack.top - 2], name, th->stack.data[th->stack.top - 1]);
        LOAD();
        sp -= 2;
        NEXT;
    }
    CASE(SET_FIELD_I) {
        int idx = READ();
        int k = READ();
        Value o = sp[-2];
        Value v = sp[-1];
        if (o.t == VT::Obj && o.o->type == OT::Instance) {
            auto* inst = static_cast<ObjInstance*>(o.o);
            if (idx < (int)inst->fields.size() && inst->cls->fields[idx].name == K[k].o) {
                inst->fields[idx] = v;
                barrier(inst, v);
                sp -= 2;
                NEXT;
            }
        }
        SYNC();
        setField(o, STR(k), v);
        LOAD();
        sp -= 2;
        NEXT;
    }
    CASE(GET_INDEX) {
        Value o = sp[-2];
        Value i = sp[-1];
        if (o.t == VT::Obj && o.o->type == OT::List && i.t == VT::Int) {
            auto* l = static_cast<ObjList*>(o.o);
            if (i.i >= 0 && i.i < (int64_t)l->items.size()) {
                sp[-2] = l->items[(size_t)i.i];
                --sp;
                NEXT;
            }
        }
        SYNC();
        Value v = getIndex(o, i);
        sp[-2] = v;
        --sp;
        NEXT;
    }
    CASE(SET_INDEX) {
        Value o = sp[-3];
        Value i = sp[-2];
        Value v = sp[-1];
        if (o.t == VT::Obj && o.o->type == OT::List && i.t == VT::Int) {
            auto* l = static_cast<ObjList*>(o.o);
            if (i.i >= 0 && i.i < (int64_t)l->items.size()) {
                l->items[(size_t)i.i] = v;
                barrier(l, v);
                sp -= 3;
                NEXT;
            }
        }
        SYNC();
        setIndex(o, i, v);
        sp -= 3;
        NEXT;
    }

#define TP_BINARY(OPC)                                          \
    {                                                           \
        Value b = sp[-1];                                       \
        Value& a = sp[-2];                                      \
        if (!fastArith(OPC, a, b)) {                            \
            Value av = a;                                       \
            SYNC();                                             \
            Value r = arithSlow(*this, OPC, av, b);             \
            LOAD();                                             \
            sp[-2] = r;                                         \
        }                                                       \
        --sp;                                                   \
        NEXT;                                                   \
    }
#define TP_BINARY_K(OPC)                                        \
    {                                                           \
        const Value& b = K[READ()];                             \
        Value& a = sp[-1];                                      \
        if (!fastArith(OPC, a, b)) {                            \
            Value av = a, bv = b;                               \
            SYNC();                                             \
            Value r = arithSlow(*this, OPC, av, bv);            \
            LOAD();                                             \
            sp[-1] = r;                                         \
        }                                                       \
        NEXT;                                                   \
    }
    CASE(ADD) TP_BINARY(OP_ADD)
    CASE(SUB) TP_BINARY(OP_SUB)
    CASE(MUL) TP_BINARY(OP_MUL)
    CASE(DIV) TP_BINARY(OP_DIV)
    CASE(MOD) TP_BINARY(OP_MOD)
    CASE(ADD_K) TP_BINARY_K(OP_ADD)
    CASE(SUB_K) TP_BINARY_K(OP_SUB)
    CASE(MUL_K) TP_BINARY_K(OP_MUL)
    CASE(DIV_K) TP_BINARY_K(OP_DIV)
    CASE(MOD_K) TP_BINARY_K(OP_MOD)
#define TP_LOCAL_OP(OPC, GETB)                                  \
    {                                                           \
        int slot = READ();                                      \
        GETB;                                                   \
        Value& a = slots[slot];                                 \
        if (!fastArith(OPC, a, b)) {                            \
            Value av = a, bv = b;                               \
            SYNC();                                             \
            Value r = arithSlow(*this, OPC, av, bv);            \
            LOAD();                                             \
            slots[slot] = r;                                    \
        }                                                       \
        NEXT;                                                   \
    }
    CASE(ADD_LOCAL) TP_LOCAL_OP(OP_ADD, Value b = *--sp)
    CASE(SUB_LOCAL) TP_LOCAL_OP(OP_SUB, Value b = *--sp)
    CASE(ADD_LOCAL_K) TP_LOCAL_OP(OP_ADD, const Value& b = K[READ()])
    CASE(SUB_LOCAL_K) TP_LOCAL_OP(OP_SUB, const Value& b = K[READ()])

    CASE(NEG) {
        Value& a = sp[-1];
        if (a.t == VT::Int) a.i = (int64_t)(0ULL - (uint64_t)a.i);
        else if (a.t == VT::Float) a.f = -a.f;
        else {
            Value av = a, r;
            SYNC();
            if (!tryUnaryOperator("neg", av, r)) {
                rtError("type mismatch", "cannot negate a value of type '" + typeName(av) + "'");
            }
            LOAD();
            sp[-1] = r;
        }
        NEXT;
    }
    CASE(NOT) { sp[-1] = Value::boolean(!truthy(sp[-1])); NEXT; }
#define TP_BITWISE(OPC, EXPR)                                   \
    {                                                           \
        Value b = sp[-1];                                       \
        Value& a = sp[-2];                                      \
        if (a.t != VT::Int || b.t != VT::Int) {                 \
            SYNC();                                             \
            static const char* names[] = {"&", "|", "^", "<<", ">>"}; \
            arithError(*this, names[OPC - OP_BAND], a, b);      \
        }                                                       \
        a.i = (EXPR);                                           \
        --sp;                                                   \
        NEXT;                                                   \
    }
    CASE(BAND) TP_BITWISE(OP_BAND, a.i & b.i)
    CASE(BOR) TP_BITWISE(OP_BOR, a.i | b.i)
    CASE(BXOR) TP_BITWISE(OP_BXOR, a.i ^ b.i)
    CASE(SHL) TP_BITWISE(OP_SHL, (int64_t)((uint64_t)a.i << (b.i & 63)))
    CASE(SHR) TP_BITWISE(OP_SHR, a.i >> (b.i & 63))
    CASE(BNOT) {
        if (sp[-1].t != VT::Int) {
            SYNC();
            rtError("type mismatch", "'~' expects 'int', found '" + typeName(sp[-1]) + "'");
        }
        sp[-1].i = ~sp[-1].i;
        NEXT;
    }

    // comparisons: value-producing, against a constant, and fused with a jump
#define TP_LESS(OPC, A, B, R)                                   \
    if (!fastLess(OPC, A, B, R)) {                              \
        Value av_ = A, bv_ = B;                                 \
        SYNC();                                                 \
        R = compareValues(*this, av_, bv_, OPC - OP_LT);        \
        LOAD();                                                 \
    }
#define TP_CMP(OPC)                                             \
    {                                                           \
        bool r;                                                 \
        TP_LESS(OPC, sp[-2], sp[-1], r)                         \
        --sp;                                                   \
        sp[-1] = Value::boolean(r);                             \
        NEXT;                                                   \
    }
#define TP_CMP_K(OPC)                                           \
    {                                                           \
        const Value& b = K[READ()];                             \
        bool r;                                                 \
        TP_LESS(OPC, sp[-1], b, r)                              \
        sp[-1] = Value::boolean(r);                             \
        NEXT;                                                   \
    }
#define TP_JCMP(OPC)                                            \
    {                                                           \
        int off = READ();                                       \
        bool r;                                                 \
        TP_LESS(OPC, sp[-2], sp[-1], r)                         \
        sp -= 2;                                                \
        if (!r) ip += off;                                      \
        NEXT;                                                   \
    }
#define TP_JCMP_K(OPC)                                          \
    {                                                           \
        const Value& b = K[READ()];                             \
        int off = READ();                                       \
        bool r;                                                 \
        TP_LESS(OPC, sp[-1], b, r)                              \
        --sp;                                                   \
        if (!r) ip += off;                                      \
        NEXT;                                                   \
    }
    CASE(LT) TP_CMP(OP_LT)
    CASE(LE) TP_CMP(OP_LE)
    CASE(GT) TP_CMP(OP_GT)
    CASE(GE) TP_CMP(OP_GE)
    CASE(LT_K) TP_CMP_K(OP_LT)
    CASE(LE_K) TP_CMP_K(OP_LE)
    CASE(GT_K) TP_CMP_K(OP_GT)
    CASE(GE_K) TP_CMP_K(OP_GE)
    CASE(JLT) TP_JCMP(OP_LT)
    CASE(JLE) TP_JCMP(OP_LE)
    CASE(JGT) TP_JCMP(OP_GT)
    CASE(JGE) TP_JCMP(OP_GE)
    CASE(JLT_K) TP_JCMP_K(OP_LT)
    CASE(JLE_K) TP_JCMP_K(OP_LE)
    CASE(JGT_K) TP_JCMP_K(OP_GT)
    CASE(JGE_K) TP_JCMP_K(OP_GE)
    // `==` on script objects / native values may be an overloaded operator
#define TP_EQUALS(A, B, R)                                      \
    {                                                           \
        Value av_ = A, bv_ = B;                                 \
        if (av_.t == VT::Obj && (av_.o->type == OT::Instance || av_.o->type == OT::Value)) { \
            SYNC();                                             \
            if (!overloadedEquals(av_, bv_, R)) R = equals(av_, bv_); \
            LOAD();                                             \
        } else {                                                \
            R = equals(av_, bv_);                               \
        }                                                       \
    }
    CASE(EQ) {
        bool eq;
        TP_EQUALS(sp[-2], sp[-1], eq)
        --sp;
        sp[-1] = Value::boolean(eq);
        NEXT;
    }
    CASE(NE) {
        bool eq;
        TP_EQUALS(sp[-2], sp[-1], eq)
        --sp;
        sp[-1] = Value::boolean(!eq);
        NEXT;
    }
    CASE(EQ_K) {
        int k = READ();
        bool eq;
        TP_EQUALS(sp[-1], K[k], eq)
        sp[-1] = Value::boolean(eq);
        NEXT;
    }
    CASE(NE_K) {
        int k = READ();
        bool eq;
        TP_EQUALS(sp[-1], K[k], eq)
        sp[-1] = Value::boolean(!eq);
        NEXT;
    }
    CASE(JEQ) {
        int off = READ();
        bool eq;
        TP_EQUALS(sp[-2], sp[-1], eq)
        sp -= 2;
        if (!eq) ip += off;
        NEXT;
    }
    CASE(JNE) {
        int off = READ();
        bool eq;
        TP_EQUALS(sp[-2], sp[-1], eq)
        sp -= 2;
        if (eq) ip += off;
        NEXT;
    }
    CASE(JEQ_K) {
        int k = READ();
        int off = READ();
        bool eq;
        TP_EQUALS(sp[-1], K[k], eq)
        --sp;
        if (!eq) ip += off;
        NEXT;
    }
    CASE(JNE_K) {
        int k = READ();
        int off = READ();
        bool eq;
        TP_EQUALS(sp[-1], K[k], eq)
        --sp;
        if (eq) ip += off;
        NEXT;
    }

    CASE(JUMP) {
        int off = READ();
        ip += off;
        NEXT;
    }
    CASE(JUMP_IF_FALSE) {
        int off = READ();
        if (!truthy(*--sp)) ip += off;
        NEXT;
    }
    CASE(JUMP_IF_TRUE) {
        int off = READ();
        if (truthy(*--sp)) ip += off;
        NEXT;
    }
    CASE(AND_JUMP) {
        int off = READ();
        if (!truthy(sp[-1])) ip += off;
        else --sp;
        NEXT;
    }
    CASE(OR_JUMP) {
        int off = READ();
        if (truthy(sp[-1])) ip += off;
        else --sp;
        NEXT;
    }
    CASE(COALESCE_JUMP) {
        int off = READ();
        if (!sp[-1].isNull()) ip += off;
        else --sp;
        NEXT;
    }
    CASE(LOOP) {
        int off = READ();
        ip -= off;
        SAFEPOINT();
        NEXT;
    }

    CASE(CALL) {
        int argc = READ();
        SAFEPOINT();
        Value callee = sp[-argc - 1];
        if (callee.t == VT::Obj) {
            // natives are called in place; script functions go through pushFrame
            // (an inlined frame push here measured slower: it hurts register
            // allocation for the whole interpreter loop)
            if (callee.o->type == OT::Native) {
                SYNC();
                int ci = (int)th->stack.top - argc - 1;
                Value r = callNativeInPlace(*this, th, static_cast<ObjNative*>(callee.o), ci, argc);
                LOAD();
                sp -= argc + 1;
                *sp++ = r;
                NEXT;
            }
        }
        SYNC();
        callSetup(th, argc);
        LOAD();
        NEXT;
    }
    CASE(INVOKE) {
        ObjString* name = STR(READ());
        int argc = READ();
        SAFEPOINT();
        SYNC();
        invoke(th, name, argc);
        LOAD();
        NEXT;
    }
    CASE(INVOKE_C) {
        int k = READ();
        int argc = READ();
        Proto::InlineCache& ic = p->caches[READ()];
        SAFEPOINT();
        Value recv = sp[-argc - 1];
        ObjString* name = STR(k);
        if (recv.t == VT::Obj) {
            if (recv.o->type == OT::Instance) {
                ObjClass* cls = static_cast<ObjInstance*>(recv.o)->cls;
                if (ic.key != cls->uid) {
                    // cache methods only; a field holding a function takes the slow path
                    auto mi = cls->fieldIndex.count(name) ? cls->methods.end() : cls->methods.find(name);
                    if (mi != cls->methods.end() && mi->second.t == VT::Obj && mi->second.o->type == OT::Closure) {
                        ic.key = cls->uid;
                        ic.value = mi->second;
                    }
                }
                if (ic.key == cls->uid) {
                    SYNC();
                    pushFrame(th, static_cast<ObjClosure*>(ic.value.o), (int)th->stack.top - argc - 1, argc, false);
                    LOAD();
                    NEXT;
                }
            } else if (recv.o->type == OT::Handle) {
                auto* h = static_cast<ObjHandle*>(recv.o);
                uint64_t key = (uint64_t)(uintptr_t)h->cls;
                if (h->cls && ic.key != key) {
                    // NativeClass method entries are never erased, so the pointer stays valid
                    auto it = h->cls->methods.find(name->s);
                    if (it != h->cls->methods.end()) {
                        ic.key = key;
                        ic.value = Value::integer((int64_t)(uintptr_t)&it->second);
                    }
                }
                if (h->cls && ic.key == key) {
                    SYNC();
                    int ci = (int)th->stack.top - argc - 1;
                    auto* m = reinterpret_cast<NativeMethod*>((uintptr_t)ic.value.i);
                    Value r = callNativeMethodResolved(h, m, name->s, th->stack.data + ci + 1, argc);
                    LOAD();
                    sp -= argc + 1;
                    *sp++ = r;
                    NEXT;
                }
            } else if (recv.o->type == OT::Namespace) {
                uint64_t key = (uint64_t)(uintptr_t)recv.o;
                if (ic.key != key || ic.epoch != nsEpoch) {
                    auto* ns = static_cast<ObjNamespace*>(recv.o);
                    auto it = ns->members.find(name);
                    if (it != ns->members.end()) {
                        ic.key = key;
                        ic.epoch = nsEpoch;
                        ic.value = it->second;
                    }
                }
                if (ic.key == key && ic.epoch == nsEpoch) {
                    sp[-argc - 1] = ic.value;
                    SYNC();
                    if (ic.value.t == VT::Obj && ic.value.o->type == OT::Native) {
                        int ci = (int)th->stack.top - argc - 1;
                        Value r = callNativeInPlace(*this, th, static_cast<ObjNative*>(ic.value.o), ci, argc);
                        LOAD();
                        sp -= argc + 1;
                        *sp++ = r;
                        NEXT;
                    }
                    callSetup(th, argc);
                    LOAD();
                    NEXT;
                }
            }
        }
        SYNC();
        invoke(th, name, argc);
        LOAD();
        NEXT;
    }
    CASE(CLOSURE) {
        Proto* fp = p->protos[READ()];
        int n = READ();
        ObjClosure* c = alloc<ObjClosure>(n * sizeof(void*));
        c->proto = fp;
        c->module = cl->module;
        c->upvals.reserve(n);
        int base = frame->base;
        for (int i = 0; i < n; i++) {
            int isLocal = READ();
            int idx = READ();
            c->upvals.push_back(isLocal ? captureUpvalue(th, base + idx) : cl->upvals[idx]);
        }
        *sp++ = Value::object(c);
        NEXT;
    }
    CASE(CLOSE_UPVALS) {
        closeUpvalues(th, frame->base + READ());
        NEXT;
    }
    CASE(RETURN) {
        Value result = sp[-1];
        if (frame->isCtor) result = slots[0];
        int base = frame->base;
        if (th->openUpvals && th->openUpvals->slot >= base) closeUpvalues(th, base);
        if (!th->handlers.empty()) {
            int fi = (int)th->frames.size() - 1;
            while (!th->handlers.empty() && th->handlers.back().frameIndex >= fi) th->handlers.pop_back();
        }
        slots[0] = result;
        th->stack.top = (size_t)base + 1;
        th->frames.pop_back();
        if (th->frames.size() <= baseFrames) return RunResult::Done;
        LOAD();
        NEXT;
    }
    CASE(BUILD_LIST) {
        int n = READ();
        ObjList* l = newList(OT::List);
        l->items.assign(sp - n, sp);
        sp -= n;
        *sp++ = Value::object(l);
        NEXT;
    }
    CASE(TUPLE) {
        int n = READ();
        ObjList* l = newList(OT::Tuple);
        l->items.assign(sp - n, sp);
        sp -= n;
        *sp++ = Value::object(l);
        NEXT;
    }
    CASE(BUILD_DICT) {
        int n = READ();
        ObjDict* d = newDict();
        Value* start = sp - 2 * n;
        for (int i = 0; i < n; i++) {
            if (start[2 * i].isNull()) {
                SYNC();
                rtError("invalid key", "null cannot be used as a dict key");
            }
            d->put(start[2 * i], start[2 * i + 1]);
        }
        sp = start;
        *sp++ = Value::object(d);
        NEXT;
    }
    CASE(UNPACK) {
        int n = READ();
        Value v = sp[-1];
        if (v.t != VT::Obj || (v.o->type != OT::Tuple && v.o->type != OT::List)) {
            SYNC();
            rtError("type mismatch", "expected " + std::to_string(n) + " values, found a single '" + typeName(v) + "'");
        }
        auto* l = static_cast<ObjList*>(v.o);
        if ((int)l->items.size() != n) {
            SYNC();
            rtError("wrong number of values", "expected " + std::to_string(n) + " values, found " + std::to_string(l->items.size()));
        }
        --sp;
        for (int i = 0; i < n; i++) *sp++ = l->items[i];
        NEXT;
    }
    CASE(FSTRING) {
        int n = READ();
        Value s;
        {
            // scoped: a computed-goto NEXT must not skip this destructor
            std::string r;
            for (Value* v = sp - n; v < sp; v++) r += toStr(*v);
            s = strv(r);
        }
        sp -= n;
        *sp++ = s;
        NEXT;
    }
    CASE(ITER_NEXT) {
        int slot = READ();
        int nvars = READ();
        int off = READ();
        Value coll = slots[slot];
        int64_t idx = slots[slot + 1].i;
        if (coll.t != VT::Obj) {
            SYNC();
            rtError(coll.isNull() ? "null reference" : "not iterable",
                    coll.isNull() ? "cannot iterate over null" : "cannot iterate over a value of type '" + typeName(coll) + "'");
        }
        OT type = coll.o->type;
        if (type == OT::List || type == OT::Tuple) {
            auto* l = static_cast<ObjList*>(coll.o);
            if (idx >= (int64_t)l->items.size()) {
                ip += off;
            } else {
                if (nvars == 2) *sp++ = Value::integer(idx);
                *sp++ = l->items[(size_t)idx];
                slots[slot + 1].i = idx + 1;
            }
        } else if (type == OT::Dict) {
            auto* d = static_cast<ObjDict*>(coll.o);
            if (idx >= (int64_t)d->entries.size()) {
                ip += off;
            } else {
                *sp++ = d->entries[(size_t)idx].first;
                if (nvars == 2) *sp++ = d->entries[(size_t)idx].second;
                slots[slot + 1].i = idx + 1;
            }
        } else if (type == OT::String) {
            auto* s = static_cast<ObjString*>(coll.o);
            if (idx >= (int64_t)s->s.size()) {
                ip += off;
            } else {
                if (nvars == 2) *sp++ = Value::integer(idx);
                *sp++ = strv(std::string(1, s->s[(size_t)idx]));
                slots[slot + 1].i = idx + 1;
            }
        } else {
            SYNC();
            rtError("not iterable", "cannot iterate over a value of type '" + typeName(coll) + "'");
        }
        NEXT;
    }
    CASE(RANGE_NEXT) {
        int slot = READ();
        int off = READ();
        int64_t i = slots[slot].i, end = slots[slot + 1].i, step = slots[slot + 2].i;
        if (step == 0) {
            SYNC();
            rtError("invalid range", "range() step cannot be 0");
        }
        if (step > 0 ? i < end : i > end) *sp++ = Value::integer(i);
        else ip += off;
        NEXT;
    }
    CASE(RANGE_STEP) {
        int slot = READ();
        slots[slot].i += slots[slot + 2].i;
        NEXT;
    }
    CASE(RANGE_INIT) {
        // [base] = i, [base+1] = end, [base+2] = step; [var] receives i
        int base = READ();
        int var = READ();
        int off = READ();
        int64_t i = slots[base].i, end = slots[base + 1].i, step = slots[base + 2].i;
        if (step == 0) {
            SYNC();
            rtError("invalid range", "range() step cannot be 0");
        }
        if (step > 0 ? i < end : i > end) slots[var] = Value::integer(i);
        else ip += off;
        NEXT;
    }
    CASE(RANGE_LOOP) {
        int base = READ();
        int var = READ();
        int off = READ();
        int64_t step = slots[base + 2].i;
        int64_t i = (slots[base].i += step);
        if (step > 0 ? i < slots[base + 1].i : i > slots[base + 1].i) {
            slots[var] = Value::integer(i);
            ip -= off;
            SAFEPOINT();
        }
        NEXT;
    }
    CASE(TRY) {
        int off = READ();
        th->handlers.push_back(Handler {(int)th->frames.size() - 1, (int)(sp - th->stack.data), (int)(ip - code) + off});
        NEXT;
    }
    CASE(END_TRY) {
        th->handlers.pop_back();
        NEXT;
    }
    CASE(THROW) {
        Value v = *--sp;
        SYNC();
        if (!th->handlers.empty() && th->handlers.back().frameIndex >= (int)baseFrames) {
            // the handler is in this run: jump to it without a C++ exception
            {
                ScriptError e;
                e.payload = v;
                unwindTo(th, baseFrames, e);
            }
            LOAD();
            NEXT;
        }
        throwValue(v, "uncaught error", toStr(v));
    }
    CASE(CHECK_TYPE) {
        ObjString* spec = STR(READ());
        SYNC();
        sp[-1] = checkType(sp[-1], spec);
        NEXT;
    }
    CASE(CONVERT) {
        int c = READ();
        sp[-1] = convert(sp[-1], c);
        NEXT;
    }
    CASE(CLASS) {
        ObjString* name = STR(READ());
        ObjString* qual = STR(READ());
        ObjClass* c = alloc<ObjClass>();
        c->name = name;
        c->qual = qual;
        c->uid = nextClassUid++;
        *sp++ = Value::object(c);
        NEXT;
    }
    CASE(FIELD) {
        ObjString* name = STR(READ());
        int zero = READ();
        auto* c = static_cast<ObjClass*>(sp[-1].o);
        c->fieldIndex[name] = (int)c->fields.size();
        c->fields.push_back(FieldDesc {name, (uint8_t)zero});
        NEXT;
    }
    CASE(METHOD) {
        ObjString* name = STR(READ());
        Value m = sp[-1];
        auto* c = static_cast<ObjClass*>(sp[-2].o);
        if (name == names.init) c->init = m;
        else c->methods[name] = m;
        barrier(c, m);
        --sp;
        NEXT;
    }
    CASE(STATIC_FIELD) {
        // declare a static field with its zero value (an existing one keeps its value on reload)
        ObjString* name = STR(READ());
        int zero = READ();
        auto* c = static_cast<ObjClass*>(sp[-1].o);
        if (!c->staticIndex.count(name)) {
            c->staticIndex[name] = (int)c->statics.size();
            c->statics.push_back(zeroValue((uint8_t)zero));
            touch(c);
        }
        NEXT;
    }
    CASE(STATIC_METHOD) {
        ObjString* name = STR(READ());
        Value m = sp[-1];
        auto* c = static_cast<ObjClass*>(sp[-2].o);
        c->staticMethods[name] = m;
        barrier(c, m);
        --sp;
        NEXT;
    }
    CASE(IMPLEMENTS) {
        ObjString* name = STR(READ());
        static_cast<ObjClass*>(sp[-1].o)->interfaces.push_back(name);
        NEXT;
    }
    CASE(DEF_CLASS) {
        int g = READ();
        auto* c = static_cast<ObjClass*>(sp[-1].o);
        ObjModule* m = cl->module;
        if (g < (int)m->defined.size() && m->defined[g] && m->globals[g].isObj() &&
            m->globals[g].o->type == OT::Class && static_cast<ObjClass*>(m->globals[g].o)->qual == c->qual) {
            // hot reload: update the existing class in place so live instances get the new code
            auto* old = static_cast<ObjClass*>(m->globals[g].o);
            old->methods = c->methods;
            old->init = c->init;
            old->interfaces = c->interfaces;
            old->staticMethods = c->staticMethods;
            for (auto& kv : c->staticIndex) { // static fields keep their values; new ones are added
                if (!old->staticIndex.count(kv.first)) {
                    old->staticIndex[kv.first] = (int)old->statics.size();
                    old->statics.push_back(c->statics[kv.second]);
                }
            }
            for (auto& f : c->fields) {
                if (!old->fieldIndex.count(f.name)) {
                    old->fieldIndex[f.name] = (int)old->fields.size();
                    old->fields.push_back(f);
                }
            }
            old->uid = nextClassUid++; // invalidates inline caches holding the old methods
            touch(old);
        } else {
            defineGlobal(m, g, sp[-1]);
        }
        --sp;
        NEXT;
    }
    CASE(ENUM) {
        ObjString* name = STR(READ());
        ObjString* qual = STR(READ());
        int n = READ();
        ObjEnum* e = alloc<ObjEnum>();
        e->name = name;
        e->qual = qual;
        *sp++ = Value::object(e);
        for (int i = 0; i < n; i++) {
            ObjEnumValue* v = alloc<ObjEnumValue>();
            v->en = e;
            v->index = i;
            v->name = STR(READ());
            e->values.push_back(v);
        }
        NEXT;
    }
    CASE(START) {
        int argc = READ();
        SYNC();
        startCoroutine(th, argc);
        LOAD();
        NEXT;
    }
    CASE(WAIT) {
        Value secs = *--sp;
        if (!th->isCoroutine) {
            SYNC();
            rtError("wait outside coroutine", "wait() can only be used inside a coroutine - launch it with 'start'");
        }
        if (th->nativeDepth > 0) {
            SYNC();
            rtError("wait inside callback", "cannot wait() inside a function called from native code (e.g. forEach)");
        }
        double s = secs.isNum() ? secs.num() : 0;
        *sp++ = Value(); // the value of the wait() call when resumed
        SYNC();
        if (th->stopRequested) {
            th->state = ThreadState::Dead;
            return RunResult::Yielded;
        }
        th->wakeAt = time + (s > 0 ? s : 0);
        th->state = ThreadState::Suspended;
        return RunResult::Yielded;
    }
    CASE(IMPORT) {
        ObjString* key = STR(READ());
        SYNC();
        Value m = importModule(key->s);
        LOAD();
        *sp++ = m;
        NEXT;
    }
    CASE(ARG_GIVEN) {
        int i = READ();
        int off = READ();
        if (frame->argc > i) ip += off;
        NEXT;
    }
    CASE(JUMP_IF_DEFINED) {
        int g = READ();
        int off = READ();
        ObjModule* m = cl->module;
        if (m->reloading && g < (int)m->defined.size() && m->defined[g]) ip += off;
        NEXT;
    }
    CASE(JUMP_IF_RELOADING) {
        int off = READ();
        if (cl->module->reloading) ip += off;
        NEXT;
    }

#ifndef TP_COMPUTED_GOTO
        default:
            SYNC();
            rtError("internal error", "bad opcode " + std::to_string(code[frame->lastOp]));
        }
    }
#endif
#undef LOAD
#undef SYNC
#undef READ
#undef SAFEPOINT
#undef STR
#undef CASE
#undef NEXT
#undef TP_BINARY
#undef TP_BINARY_K
#undef TP_LOCAL_OP
#undef TP_BITWISE
#undef TP_LESS
#undef TP_CMP
#undef TP_CMP_K
#undef TP_JCMP
#undef TP_JCMP_K
#undef TP_EQUALS
}

} // namespace tp
