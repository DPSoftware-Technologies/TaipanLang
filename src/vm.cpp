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

static inline Value popv(std::vector<Value>& s) {
    Value v = s.back();
    s.pop_back();
    return v;
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
    std::vector<Value>& S = th->stack;
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

void VM::callSetup(ObjThread* th, int argc) {
    std::vector<Value>& S = th->stack;
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
        auto* n = static_cast<ObjNative*>(callee.o);
        // Copy the arguments (the native may re-enter the VM and grow the
        // stack). Small calls use a stack buffer; missing args read as null.
        Value small[8];
        std::vector<Value> big;
        Value* args = small;
        if (argc > 8) {
            big.assign(S.begin() + ci + 1, S.end());
            args = big.data();
        } else {
            for (int i = 0; i < argc; i++) small[i] = S[ci + 1 + i];
        }
        Value r = n->fn(*this, args, argc);
        S.resize(ci);
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
        if (isStr(b->method) && b->recv.isObj() && b->recv.o->type == OT::Handle) {
            std::vector<Value> args(S.begin() + ci + 1, S.end());
            Value r = callNativeMethod(static_cast<ObjHandle*>(b->recv.o), asStr(b->method)->s, args.data(), argc);
            S.resize(ci);
            S.push_back(r);
            return;
        }
        S[ci] = b->method;
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
    std::vector<Value>& S = th->stack;
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
    case OT::Module: case OT::Namespace: {
        S[ri] = getField(recv, name);
        callSetup(th, argc);
        return;
    }
    case OT::Handle: {
        std::vector<Value> args(S.begin() + ri + 1, S.end());
        Value r = callNativeMethod(static_cast<ObjHandle*>(recv.o), name->s, args.data(), argc);
        S.resize(ri);
        S.push_back(r);
        return;
    }
    default:
        if (invokeBuiltin(th, name, argc)) return;
        rtError("unknown method", "'" + typeName(recv) + "' has no method '" + name->s + "'");
    }
}

bool VM::invokeBuiltin(ObjThread* th, ObjString* name, int argc) {
    std::vector<Value>& S = th->stack;
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
    std::vector<Value>& S = from->stack;
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

static bool compareValues(VM& vm, const Value& a, const Value& b, int op) {
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

VM::RunResult VM::execute(ObjThread* th, size_t baseFrames) {
    std::vector<Value>& S = th->stack;
    CallFrame* frame;
    ObjClosure* cl;
    Proto* p;
    const int32_t* code;
    int base;
#define LOAD()                          \
    do {                                \
        frame = &th->frames.back();     \
        cl = frame->closure;            \
        p = cl->proto;                  \
        code = p->code.data();          \
        base = frame->base;             \
    } while (0)
#define READ() (code[frame->ip++])
    LOAD();
    for (;;) {
        if (gcRequested || gcStress) safepoint();
        frame->lastOp = frame->ip;
        int32_t op = READ();
        switch (op) {
        case OP_CONST: S.push_back(p->consts[READ()]); break;
        case OP_NIL: S.push_back(Value()); break;
        case OP_TRUE: S.push_back(Value::boolean(true)); break;
        case OP_FALSE: S.push_back(Value::boolean(false)); break;
        case OP_POP: S.pop_back(); break;
        case OP_POPN: S.resize(S.size() - READ()); break;
        case OP_DUP: {
            Value v = S.back();
            S.push_back(v);
            break;
        }
        case OP_DUP2: {
            Value a = S[S.size() - 2], b = S.back();
            S.push_back(a);
            S.push_back(b);
            break;
        }
        case OP_GET_LOCAL: {
            Value v = S[base + READ()];
            S.push_back(v);
            break;
        }
        case OP_SET_LOCAL: {
            int slot = READ();
            S[base + slot] = S.back();
            S.pop_back();
            break;
        }
        case OP_GET_UPVAL: {
            Value v = cl->upvals[READ()]->ref();
            S.push_back(v);
            break;
        }
        case OP_SET_UPVAL: {
            ObjUpvalue* up = cl->upvals[READ()];
            up->ref() = S.back();
            if (!up->open) barrier(up, S.back());
            S.pop_back();
            break;
        }
        case OP_GET_GLOBAL: {
            int g = READ();
            ObjModule* m = cl->module;
            if (g >= (int)m->defined.size() || !m->defined[g]) {
                std::string name = "?";
                for (auto& kv : m->names) {
                    if (kv.second == g) name = kv.first->s;
                }
                rtError("uninitialized global", "'" + name + "' is used before its declaration ran");
            }
            S.push_back(m->globals[g]);
            break;
        }
        case OP_SET_GLOBAL: {
            int g = READ();
            defineGlobal(cl->module, g, S.back());
            S.pop_back();
            break;
        }
        case OP_GET_BUILTIN: {
            ObjString* name = asStr(p->consts[READ()]);
            auto it = builtins->members.find(name);
            if (it == builtins->members.end()) rtError("unknown builtin", "'" + name->s + "' is not registered in this VM");
            S.push_back(it->second);
            break;
        }
        case OP_GET_FIELD: {
            ObjString* name = asStr(p->consts[READ()]);
            Value obj = S.back();
            Value v = getField(obj, name);
            S.back() = v;
            LOAD();
            break;
        }
        case OP_SET_FIELD: {
            ObjString* name = asStr(p->consts[READ()]);
            Value v = S.back();
            Value obj = S[S.size() - 2];
            setField(obj, name, v);
            S.resize(S.size() - 2);
            LOAD();
            break;
        }
        case OP_GET_INDEX: {
            Value idx = S.back();
            Value obj = S[S.size() - 2];
            Value v = getIndex(obj, idx);
            S.pop_back();
            S.back() = v;
            break;
        }
        case OP_SET_INDEX: {
            Value v = S.back();
            Value idx = S[S.size() - 2];
            Value obj = S[S.size() - 3];
            setIndex(obj, idx, v);
            S.resize(S.size() - 3);
            break;
        }
        case OP_ADD: {
            Value b = S.back(), a = S[S.size() - 2];
            Value r;
            if (a.t == VT::Int && b.t == VT::Int) r = Value::integer((int64_t)((uint64_t)a.i + (uint64_t)b.i));
            else if (a.isNum() && b.isNum()) r = Value::number(a.num() + b.num());
            else if (isStr(a) || isStr(b)) r = strv(toStr(a) + toStr(b));
            else arithError(*this, "+", a, b);
            S.pop_back();
            S.back() = r;
            break;
        }
        case OP_SUB: case OP_MUL: {
            Value b = S.back(), a = S[S.size() - 2];
            Value r;
            if (a.t == VT::Int && b.t == VT::Int) {
                uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i;
                r = Value::integer((int64_t)(op == OP_SUB ? x - y : x * y));
            } else if (a.isNum() && b.isNum()) {
                r = Value::number(op == OP_SUB ? a.num() - b.num() : a.num() * b.num());
            } else {
                arithError(*this, op == OP_SUB ? "-" : "*", a, b);
            }
            S.pop_back();
            S.back() = r;
            break;
        }
        case OP_DIV: case OP_MOD: {
            Value b = S.back(), a = S[S.size() - 2];
            Value r;
            if (a.t == VT::Int && b.t == VT::Int) {
                if (b.i == 0) rtError("division by zero", op == OP_DIV ? "integer division by zero" : "integer modulo by zero");
                if (b.i == -1) r = Value::integer(op == OP_DIV ? (int64_t)(0ULL - (uint64_t)a.i) : 0);
                else r = Value::integer(op == OP_DIV ? a.i / b.i : a.i % b.i);
            } else if (a.isNum() && b.isNum()) {
                r = Value::number(op == OP_DIV ? a.num() / b.num() : std::fmod(a.num(), b.num()));
            } else {
                arithError(*this, op == OP_DIV ? "/" : "%", a, b);
            }
            S.pop_back();
            S.back() = r;
            break;
        }
        case OP_NEG: {
            Value& a = S.back();
            if (a.isInt()) a.i = (int64_t)(0ULL - (uint64_t)a.i);
            else if (a.isFloat()) a.f = -a.f;
            else rtError("type mismatch", "cannot negate a value of type '" + typeName(a) + "'");
            break;
        }
        case OP_NOT: S.back() = Value::boolean(!truthy(S.back())); break;
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
            Value b = S.back(), a = S[S.size() - 2];
            static const char* names[] = {"&", "|", "^", "<<", ">>"};
            if (!a.isInt() || !b.isInt()) arithError(*this, names[op - OP_BAND], a, b);
            int64_t r = 0;
            switch (op) {
            case OP_BAND: r = a.i & b.i; break;
            case OP_BOR: r = a.i | b.i; break;
            case OP_BXOR: r = a.i ^ b.i; break;
            case OP_SHL: r = (int64_t)((uint64_t)a.i << (b.i & 63)); break;
            default: r = a.i >> (b.i & 63); break;
            }
            S.pop_back();
            S.back() = Value::integer(r);
            break;
        }
        case OP_BNOT: {
            if (!S.back().isInt()) rtError("type mismatch", "'~' expects 'int', found '" + typeName(S.back()) + "'");
            S.back().i = ~S.back().i;
            break;
        }
        case OP_EQ: case OP_NE: {
            bool eq = equals(S[S.size() - 2], S.back());
            S.pop_back();
            S.back() = Value::boolean(op == OP_EQ ? eq : !eq);
            break;
        }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            bool r = compareValues(*this, S[S.size() - 2], S.back(), op - OP_LT);
            S.pop_back();
            S.back() = Value::boolean(r);
            break;
        }
        case OP_JUMP: {
            int off = READ();
            frame->ip += off;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int off = READ();
            if (!truthy(S.back())) frame->ip += off;
            S.pop_back();
            break;
        }
        case OP_JUMP_IF_TRUE: {
            int off = READ();
            if (truthy(S.back())) frame->ip += off;
            S.pop_back();
            break;
        }
        case OP_AND_JUMP: {
            int off = READ();
            if (!truthy(S.back())) frame->ip += off;
            else S.pop_back();
            break;
        }
        case OP_OR_JUMP: {
            int off = READ();
            if (truthy(S.back())) frame->ip += off;
            else S.pop_back();
            break;
        }
        case OP_COALESCE_JUMP: {
            int off = READ();
            if (!S.back().isNull()) frame->ip += off;
            else S.pop_back();
            break;
        }
        case OP_LOOP: {
            int off = READ();
            frame->ip -= off;
            break;
        }
        case OP_CALL: {
            int argc = READ();
            callSetup(th, argc);
            LOAD();
            break;
        }
        case OP_INVOKE: {
            ObjString* name = asStr(p->consts[READ()]);
            int argc = READ();
            invoke(th, name, argc);
            LOAD();
            break;
        }
        case OP_CLOSURE: {
            Proto* fp = p->protos[READ()];
            int n = READ();
            ObjClosure* c = alloc<ObjClosure>(n * sizeof(void*));
            c->proto = fp;
            c->module = cl->module;
            c->upvals.reserve(n);
            for (int i = 0; i < n; i++) {
                int isLocal = READ();
                int idx = READ();
                c->upvals.push_back(isLocal ? captureUpvalue(th, base + idx) : cl->upvals[idx]);
            }
            S.push_back(Value::object(c));
            break;
        }
        case OP_CLOSE_UPVALS: closeUpvalues(th, base + READ()); break;
        case OP_RETURN: {
            Value result = S.back();
            if (frame->isCtor) result = S[base];
            closeUpvalues(th, base);
            int fi = (int)th->frames.size() - 1;
            while (!th->handlers.empty() && th->handlers.back().frameIndex >= fi) th->handlers.pop_back();
            S.resize(base);
            S.push_back(result);
            th->frames.pop_back();
            if (th->frames.size() <= baseFrames) return RunResult::Done;
            LOAD();
            break;
        }
        case OP_BUILD_LIST: case OP_TUPLE: {
            int n = READ();
            ObjList* l = newList(op == OP_TUPLE ? OT::Tuple : OT::List);
            l->items.assign(S.end() - n, S.end());
            S.resize(S.size() - n);
            S.push_back(Value::object(l));
            break;
        }
        case OP_BUILD_DICT: {
            int n = READ();
            ObjDict* d = newDict();
            size_t start = S.size() - 2 * n;
            for (int i = 0; i < n; i++) {
                const Value& k = S[start + 2 * i];
                if (k.isNull()) rtError("invalid key", "null cannot be used as a dict key");
                d->put(k, S[start + 2 * i + 1]);
            }
            S.resize(start);
            S.push_back(Value::object(d));
            break;
        }
        case OP_UNPACK: {
            int n = READ();
            Value v = S.back();
            if (!v.isObj() || (v.o->type != OT::Tuple && v.o->type != OT::List)) {
                rtError("type mismatch", "expected " + std::to_string(n) + " values, found a single '" + typeName(v) + "'");
            }
            auto* l = static_cast<ObjList*>(v.o);
            if ((int)l->items.size() != n) {
                rtError("wrong number of values", "expected " + std::to_string(n) + " values, found " + std::to_string(l->items.size()));
            }
            S.pop_back();
            for (auto& x : l->items) S.push_back(x);
            break;
        }
        case OP_FSTRING: {
            int n = READ();
            std::string r;
            for (size_t i = S.size() - n; i < S.size(); i++) r += toStr(S[i]);
            Value s = strv(r);
            S.resize(S.size() - n);
            S.push_back(s);
            break;
        }
        case OP_ITER_NEXT: {
            int slot = READ();
            int nvars = READ();
            int off = READ();
            Value coll = S[base + slot];
            int64_t idx = S[base + slot + 1].i;
            if (!coll.isObj()) {
                rtError(coll.isNull() ? "null reference" : "not iterable",
                        coll.isNull() ? "cannot iterate over null" : "cannot iterate over a value of type '" + typeName(coll) + "'");
            }
            switch (coll.o->type) {
            case OT::List: case OT::Tuple: {
                auto* l = static_cast<ObjList*>(coll.o);
                if (idx >= (int64_t)l->items.size()) {
                    frame->ip += off;
                    break;
                }
                if (nvars == 2) S.push_back(Value::integer(idx));
                S.push_back(l->items[(size_t)idx]);
                S[base + slot + 1].i = idx + 1;
                break;
            }
            case OT::Dict: {
                auto* d = static_cast<ObjDict*>(coll.o);
                if (idx >= (int64_t)d->entries.size()) {
                    frame->ip += off;
                    break;
                }
                S.push_back(d->entries[(size_t)idx].first);
                if (nvars == 2) S.push_back(d->entries[(size_t)idx].second);
                S[base + slot + 1].i = idx + 1;
                break;
            }
            case OT::String: {
                auto* s = asStr(coll);
                if (idx >= (int64_t)s->s.size()) {
                    frame->ip += off;
                    break;
                }
                if (nvars == 2) S.push_back(Value::integer(idx));
                S.push_back(strv(std::string(1, s->s[(size_t)idx])));
                S[base + slot + 1].i = idx + 1;
                break;
            }
            default:
                rtError("not iterable", "cannot iterate over a value of type '" + typeName(coll) + "'");
            }
            break;
        }
        case OP_RANGE_NEXT: {
            int slot = READ();
            int off = READ();
            int64_t i = S[base + slot].i, end = S[base + slot + 1].i, step = S[base + slot + 2].i;
            if (step == 0) rtError("invalid range", "range() step cannot be 0");
            if (step > 0 ? i < end : i > end) S.push_back(Value::integer(i));
            else frame->ip += off;
            break;
        }
        case OP_RANGE_STEP: {
            int slot = READ();
            S[base + slot].i += S[base + slot + 2].i;
            break;
        }
        case OP_TRY: {
            int off = READ();
            th->handlers.push_back(Handler {(int)th->frames.size() - 1, (int)S.size(), frame->ip + off});
            break;
        }
        case OP_END_TRY: th->handlers.pop_back(); break;
        case OP_THROW: {
            Value v = S.back();
            S.pop_back();
            if (!th->handlers.empty() && th->handlers.back().frameIndex >= (int)baseFrames) {
                // fast path: the handler is in this run, jump to it without a C++ exception
                ScriptError e;
                e.payload = v;
                unwindTo(th, baseFrames, e);
                LOAD();
                break;
            }
            throwValue(v, "uncaught error", toStr(v));
        }
        case OP_CHECK_TYPE: {
            ObjString* spec = asStr(p->consts[READ()]);
            S.back() = checkType(S.back(), spec);
            break;
        }
        case OP_CONVERT: S.back() = convert(S.back(), READ()); break;
        case OP_CLASS: {
            ObjString* name = asStr(p->consts[READ()]);
            ObjString* qual = asStr(p->consts[READ()]);
            ObjClass* c = alloc<ObjClass>();
            c->name = name;
            c->qual = qual;
            S.push_back(Value::object(c));
            break;
        }
        case OP_FIELD: {
            ObjString* name = asStr(p->consts[READ()]);
            int zero = READ();
            auto* c = static_cast<ObjClass*>(S.back().o);
            c->fieldIndex[name] = (int)c->fields.size();
            c->fields.push_back(FieldDesc {name, (uint8_t)zero});
            break;
        }
        case OP_METHOD: {
            ObjString* name = asStr(p->consts[READ()]);
            Value m = S.back();
            auto* c = static_cast<ObjClass*>(S[S.size() - 2].o);
            if (name == names.init) c->init = m;
            else c->methods[name] = m;
            barrier(c, m);
            S.pop_back();
            break;
        }
        case OP_IMPLEMENTS: {
            ObjString* name = asStr(p->consts[READ()]);
            static_cast<ObjClass*>(S.back().o)->interfaces.push_back(name);
            break;
        }
        case OP_DEF_CLASS: {
            int g = READ();
            auto* c = static_cast<ObjClass*>(S.back().o);
            ObjModule* m = cl->module;
            if (g < (int)m->defined.size() && m->defined[g] && m->globals[g].isObj() &&
                m->globals[g].o->type == OT::Class && static_cast<ObjClass*>(m->globals[g].o)->qual == c->qual) {
                // hot reload: update the existing class in place so live instances get the new code
                auto* old = static_cast<ObjClass*>(m->globals[g].o);
                old->methods = c->methods;
                old->init = c->init;
                old->interfaces = c->interfaces;
                for (auto& f : c->fields) {
                    if (!old->fieldIndex.count(f.name)) {
                        old->fieldIndex[f.name] = (int)old->fields.size();
                        old->fields.push_back(f);
                    }
                }
                touch(old);
            } else {
                defineGlobal(m, g, S.back());
            }
            S.pop_back();
            break;
        }
        case OP_ENUM: {
            ObjString* name = asStr(p->consts[READ()]);
            ObjString* qual = asStr(p->consts[READ()]);
            int n = READ();
            ObjEnum* e = alloc<ObjEnum>();
            e->name = name;
            e->qual = qual;
            S.push_back(Value::object(e)); // rooted while members are created
            for (int i = 0; i < n; i++) {
                ObjEnumValue* v = alloc<ObjEnumValue>();
                v->en = e;
                v->index = i;
                v->name = asStr(p->consts[READ()]);
                e->values.push_back(v);
            }
            break;
        }
        case OP_START: {
            int argc = READ();
            startCoroutine(th, argc);
            LOAD();
            break;
        }
        case OP_WAIT: {
            Value secs = S.back();
            S.pop_back();
            if (!th->isCoroutine) {
                rtError("wait outside coroutine", "wait() can only be used inside a coroutine - launch it with 'start'");
            }
            if (th->nativeDepth > 0) {
                rtError("wait inside callback", "cannot wait() inside a function called from native code (e.g. forEach)");
            }
            double s = secs.isNum() ? secs.num() : 0;
            S.push_back(Value()); // the value of the wait() call when resumed
            if (th->stopRequested) {
                th->state = ThreadState::Dead;
                return RunResult::Yielded;
            }
            th->wakeAt = time + (s > 0 ? s : 0);
            th->state = ThreadState::Suspended;
            return RunResult::Yielded;
        }
        case OP_IMPORT: {
            ObjString* key = asStr(p->consts[READ()]);
            Value m = importModule(key->s);
            S.push_back(m);
            LOAD();
            break;
        }
        case OP_ARG_GIVEN: {
            int i = READ();
            int off = READ();
            if (frame->argc > i) frame->ip += off;
            break;
        }
        case OP_JUMP_IF_DEFINED: {
            int g = READ();
            int off = READ();
            ObjModule* m = cl->module;
            if (m->reloading && g < (int)m->defined.size() && m->defined[g]) frame->ip += off;
            break;
        }
        case OP_JUMP_IF_RELOADING: {
            int off = READ();
            if (cl->module->reloading) frame->ip += off;
            break;
        }
        default:
            rtError("internal error", "bad opcode " + std::to_string(op));
        }
    }
#undef LOAD
#undef READ
}

} // namespace tp
