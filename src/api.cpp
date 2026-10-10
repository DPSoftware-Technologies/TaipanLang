// api.cpp - the public embedding API declared in taipan.h.
#include "loader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace tp;

// Grants the implementation access to the private parts of the API classes.
struct TaipanInternal {
    static TaipanValue make(TaipanValue::Type t, TaipanState* owner = nullptr) {
        TaipanValue v;
        v.type_ = t;
        v.owner_ = owner;
        return v;
    }
    static void setRef(TaipanValue& v, void* p) { v.u_.p = p; }
    static void setStr(TaipanValue& v, const std::string& s) { v.str_ = s; }
    static const void* box(const TaipanValue& v) { return v.box_.get(); }
    static void* ref(const TaipanValue& v) { return v.u_.p; }
    static TaipanState* owner(const TaipanValue& v) { return v.owner_; }
    static TaipanArgs args(TaipanState* s, const Value* argv, int argc) {
        static_assert(sizeof(TaipanArgs::Raw) == sizeof(Value), "TaipanArgs::Raw must mirror tp::Value");
        TaipanArgs a;
        a.state_ = s;
        a.count_ = argc;
        for (int i = 0; i < argc; i++) {
            TaipanArgs::Raw r;
            std::memcpy(&r, &argv[i], sizeof r);
            if (i < TaipanArgs::kInline) a.inline_[i] = r;
            else a.extra_.push_back(r);
        }
        return a;
    }
    static Value value(const TaipanArgs& a, int i) {
        Value v;
        if (i >= 0 && i < a.count_) std::memcpy(static_cast<void*>(&v), &a.raw(i), sizeof v);
        return v;
    }
    static std::vector<TaipanValue>& boxed(const TaipanArgs& a) { return a.boxed_; }
};

// ===========================================================================
// TaipanValue
// ===========================================================================
TaipanValue TaipanValue::null() { return TaipanValue(); }

TaipanValue TaipanValue::fromBool(bool b) {
    TaipanValue v;
    v.type_ = Type::Bool;
    v.u_.b = b;
    return v;
}

TaipanValue TaipanValue::fromInt(int64_t i) {
    TaipanValue v;
    v.type_ = Type::Int;
    v.u_.i = i;
    return v;
}

TaipanValue TaipanValue::fromFloat(double f) {
    TaipanValue v;
    v.type_ = Type::Float;
    v.u_.f = f;
    return v;
}

TaipanValue TaipanValue::fromString(std::string s) {
    TaipanValue v;
    v.type_ = Type::String;
    v.str_ = std::move(s);
    return v;
}

TaipanValue TaipanValue::boxed(const std::string& className, std::shared_ptr<void> data) {
    TaipanValue v;
    v.type_ = Type::Value;
    v.str_ = className;
    v.box_ = std::move(data);
    return v;
}

const void* TaipanValue::valuePtr() const {
    if (type_ != Type::Value) return nullptr;
    if (box_) return box_.get();
    return u_.p ? static_cast<ObjValue*>(u_.p)->data : nullptr;
}

TaipanValue TaipanValue::fromObject(const char* className, void* ptr) {
    if (!ptr) return TaipanValue();
    TaipanValue v;
    v.type_ = Type::Object;
    v.str_ = className ? className : "native";
    v.u_.p = ptr;
    return v;
}

std::string TaipanValue::typeName() const {
    switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Float: return "float";
    case Type::String: return "str";
    case Type::Object: return str_;
    case Type::List: return "list";
    case Type::Dict: return "dict";
    case Type::Function: return "func";
    case Type::Ref: return owner_ ? owner_->vm.typeName(Value::object(static_cast<Obj*>(u_.p))) : "object";
    case Type::Value: return str_;
    }
    return "?";
}

bool TaipanValue::asBool() const {
    if (type_ == Type::Bool) return u_.b;
    if (type_ == Type::Int) return u_.i != 0;
    if (type_ == Type::Float) return u_.f != 0;
    return type_ != Type::Null;
}

int64_t TaipanValue::asInt() const {
    if (type_ == Type::Int) return u_.i;
    if (type_ == Type::Float) return (int64_t)u_.f;
    if (type_ == Type::Bool) return u_.b ? 1 : 0;
    return 0;
}

double TaipanValue::asFloat() const {
    if (type_ == Type::Float) return u_.f;
    if (type_ == Type::Int) return (double)u_.i;
    if (type_ == Type::Bool) return u_.b ? 1 : 0;
    return 0;
}

const std::string& TaipanValue::asString() const {
    static const std::string empty;
    return type_ == Type::String ? str_ : empty;
}

void* TaipanValue::asObject() const { return type_ == Type::Object ? u_.p : nullptr; }

int TaipanValue::length() const {
    if (type_ == Type::String) return (int)str_.size();
    if (!owner_) return 0;
    Obj* o = static_cast<Obj*>(u_.p);
    if (type_ == Type::List) return (int)static_cast<ObjList*>(o)->items.size();
    if (type_ == Type::Dict) return (int)static_cast<ObjDict*>(o)->entries.size();
    return 0;
}

TaipanValue TaipanValue::at(int index) const {
    if (type_ != Type::List || !owner_) return TaipanValue();
    auto* l = static_cast<ObjList*>(u_.p);
    if (index < 0 || index >= (int)l->items.size()) return TaipanValue();
    return valueToHost(owner_->vm, l->items[index]);
}

TaipanValue TaipanValue::get(const std::string& key) const {
    if (type_ != Type::Dict || !owner_) return TaipanValue();
    VM& vm = owner_->vm;
    Value* v = static_cast<ObjDict*>(u_.p)->find(vm.strv(key));
    return v ? valueToHost(vm, *v) : TaipanValue();
}

std::string TaipanValue::toString() const {
    switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return u_.b ? "true" : "false";
    case Type::Int: return std::to_string(u_.i);
    case Type::Float: return formatFloat(u_.f);
    case Type::String: return str_;
    case Type::Object: return "<" + str_ + ">";
    case Type::Value:
        if (owner_ && u_.p) return owner_->vm.toStr(Value::object(static_cast<Obj*>(u_.p)));
        return "<" + str_ + ">";
    default:
        if (owner_) return owner_->vm.toStr(Value::object(static_cast<Obj*>(u_.p)));
        return "<object>";
    }
}

// ===========================================================================
// TaipanArgs
// ===========================================================================
const TaipanValue& TaipanArgs::operator[](int i) const {
    static const TaipanValue none;
    if (i < 0 || i >= count_) return none;
    if (boxed_.empty()) {
        VM& vm = state_->vm;
        boxed_.reserve(count_);
        for (int k = 0; k < count_; k++) boxed_.push_back(valueToHost(vm, TaipanInternal::value(*this, k)));
    }
    return boxed_[i];
}

static std::string argWhat(int i) { return "argument " + std::to_string(i + 1); }

[[noreturn]] static void argTypeError(const TaipanArgs& a, int i, const char* expected) {
    if (i >= a.count()) a.error(argWhat(i) + ": expected '" + expected + "', but it was not given");
    Value v = TaipanInternal::value(a, i);
    a.error(argWhat(i) + ": expected '" + expected + "', found '" + a.state()->vm.typeName(v) + "'");
}

TaipanValue::Type TaipanArgs::typeAt(int i) const {
    Value v = TaipanInternal::value(*this, i);
    switch (v.t) {
    case VT::Null: return TaipanValue::Type::Null;
    case VT::Bool: return TaipanValue::Type::Bool;
    case VT::Int: return TaipanValue::Type::Int;
    case VT::Float: return TaipanValue::Type::Float;
    case VT::Obj: break;
    }
    switch (v.o->type) {
    case OT::String: return TaipanValue::Type::String;
    case OT::Handle: return TaipanValue::Type::Object;
    case OT::Value: return TaipanValue::Type::Value;
    case OT::List: return TaipanValue::Type::List;
    case OT::Dict: return TaipanValue::Type::Dict;
    case OT::Closure: case OT::Native: case OT::Bound: return TaipanValue::Type::Function;
    default: return TaipanValue::Type::Ref;
    }
}

int64_t TaipanArgs::getInt(int i) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t == VT::Int) return v.i;
    if (v.t == VT::Float) return (int64_t)v.f;
    argTypeError(*this, i, "int");
}

double TaipanArgs::getFloat(int i) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t == VT::Float) return v.f;
    if (v.t == VT::Int) return (double)v.i;
    argTypeError(*this, i, "float");
}

bool TaipanArgs::getBool(int i) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t == VT::Bool) return v.b;
    argTypeError(*this, i, "bool");
}

std::string TaipanArgs::getString(int i) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t == VT::Obj && v.o->type == OT::String) return static_cast<ObjString*>(v.o)->s;
    argTypeError(*this, i, "str");
}

void* TaipanArgs::getObject(int i, const char* className) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t != VT::Obj || v.o->type != OT::Handle) argTypeError(*this, i, className ? className : "native object");
    auto* h = static_cast<ObjHandle*>(v.o);
    std::string actual = h->cls ? h->cls->name : "native";
    if (className && actual != className) {
        bool alias = false;
        if (h->cls) {
            for (auto& a : h->cls->aliases) alias |= a == className;
        }
        if (!alias) error(argWhat(i) + ": expected '" + std::string(className) + "', found '" + actual + "'");
    }
    if (!h->alive) error("use of invalid '" + actual + "' handle - the object was destroyed");
    return h->ptr;
}

const void* TaipanArgs::getValue(int i, const char* className) const {
    Value v = TaipanInternal::value(*this, i);
    if (v.t != VT::Obj || v.o->type != OT::Value) argTypeError(*this, i, className && *className ? className : "value");
    auto* nv = static_cast<ObjValue*>(v.o);
    if (className && *className && nv->cls->name != className) {
        bool alias = false;
        for (auto& a : nv->cls->aliases) alias |= a == className;
        if (!alias) error(argWhat(i) + ": expected '" + std::string(className) + "', found '" + nv->cls->name + "'");
    }
    return nv->data;
}

TaipanValue TaipanArgs::call(const TaipanValue& fn, const std::vector<TaipanValue>& args) const {
    VM& vm = state_->vm;
    std::vector<Value> vals;
    size_t mark = vm.tempRoots.size();
    Value f = hostToValue(vm, fn);
    vm.tempRoots.push_back(f);
    for (auto& a : args) {
        vals.push_back(hostToValue(vm, a));
        vm.tempRoots.push_back(vals.back());
    }
    Value r = vm.callValue(f, vals);
    vm.tempRoots.resize(mark);
    TaipanValue out = valueToHost(vm, r);
    if (out.type() == TaipanValue::Type::List || out.type() == TaipanValue::Type::Dict ||
        out.type() == TaipanValue::Type::Function || out.type() == TaipanValue::Type::Ref) {
        vm.tempRoots.push_back(r); // keep alive until the native call returns
    }
    return out;
}

void TaipanArgs::error(const std::string& message) const { throw HostError {message}; }

void taipan_detail::fail(const std::string& message) { throw HostError {message}; }

// ===========================================================================
// Conversions
// ===========================================================================
namespace tp {

Value hostToValue(VM& vm, const TaipanValue& v) {
    switch (v.type()) {
    case TaipanValue::Type::Null: return Value();
    case TaipanValue::Type::Bool: return Value::boolean(v.asBool());
    case TaipanValue::Type::Int: return Value::integer(v.asInt());
    case TaipanValue::Type::Float: return Value::number(v.asFloat());
    case TaipanValue::Type::String: return vm.strv(v.asString());
    case TaipanValue::Type::Object: return vm.handleFor(v.objectClass(), v.asObject());
    case TaipanValue::Type::Value: {
        if (!TaipanInternal::box(v)) {
            void* p = TaipanInternal::ref(v);
            return p ? Value::object(static_cast<Obj*>(p)) : Value();
        }
        NativeClass* nc = vm.nativeTypeByName(v.objectClass());
        if (!nc || !nc->isValue || !nc->copy) {
            throw HostError {"'" + v.objectClass() + "' is not a registered value type (taipan_register_value)"};
        }
        return vm.newValueObj(nc, TaipanInternal::box(v));
    }
    default: {
        void* p = TaipanInternal::ref(v);
        return p ? Value::object(static_cast<Obj*>(p)) : Value();
    }
    }
}

TaipanValue valueToHost(VM& vm, const Value& v) {
    switch (v.t) {
    case VT::Null: return TaipanValue();
    case VT::Bool: return TaipanValue::fromBool(v.b);
    case VT::Int: return TaipanValue::fromInt(v.i);
    case VT::Float: return TaipanValue::fromFloat(v.f);
    case VT::Obj: break;
    }
    switch (v.o->type) {
    case OT::String: return TaipanValue::fromString(static_cast<ObjString*>(v.o)->s);
    case OT::Value: {
        TaipanValue r = TaipanInternal::make(TaipanValue::Type::Value, vm.state);
        TaipanInternal::setStr(r, static_cast<ObjValue*>(v.o)->cls->name);
        TaipanInternal::setRef(r, v.o);
        return r;
    }
    case OT::Handle: {
        auto* h = static_cast<ObjHandle*>(v.o);
        // a stale handle keeps its class name but has a null pointer
        TaipanValue r = TaipanInternal::make(TaipanValue::Type::Object);
        TaipanInternal::setStr(r, h->cls ? h->cls->name : "native");
        TaipanInternal::setRef(r, h->alive ? h->ptr : nullptr);
        return r;
    }
    default: {
        TaipanValue::Type t = TaipanValue::Type::Ref;
        if (v.o->type == OT::List) t = TaipanValue::Type::List;
        else if (v.o->type == OT::Dict) t = TaipanValue::Type::Dict;
        else if (v.o->type == OT::Closure || v.o->type == OT::Native || v.o->type == OT::Bound) t = TaipanValue::Type::Function;
        TaipanValue r = TaipanInternal::make(t, vm.state);
        TaipanInternal::setRef(r, v.o);
        return r;
    }
    }
}

TaipanArgs makeArgs(VM& vm, const Value* argv, int argc) { return TaipanInternal::args(vm.state, argv, argc); }

Value VM::handleFor(const std::string& className, void* ptr) {
    if (!ptr) return Value();
    auto it = handles.find(ptr);
    if (it != handles.end()) {
        ObjHandle* h = it->second;
        if (h->alive && (!h->cls || h->cls->name == className)) {
            if (phase == GcPhase::Sweep && h->mark != curMark) h->mark = curMark;
            return Value::object(h);
        }
    }
    NativeClass* nc = nativeTypeByName(className);
    if (!nc) {
        auto c = std::make_unique<NativeClass>();
        c->name = className;
        nc = c.get();
        nativeClasses[className] = std::move(c);
        nativeTypeNames[className] = nc;
    }
    ObjHandle* h = alloc<ObjHandle>();
    h->cls = nc;
    h->ptr = ptr;
    handles[ptr] = h;
    return Value::object(h);
}

// Runs host code, turning host exceptions into catchable script errors.
template <class F> static auto guardHost(VM& vm, F&& f) -> decltype(f()) {
    size_t mark = vm.tempRoots.size();
    vm.hostDepth++;
    try {
        auto r = f();
        vm.hostDepth--;
        vm.tempRoots.resize(mark);
        return r;
    } catch (HostError& e) {
        vm.hostDepth--;
        vm.tempRoots.resize(mark);
        vm.rtError("native error", e.message);
    } catch (ScriptError&) {
        vm.hostDepth--;
        vm.tempRoots.resize(mark);
        throw;
    } catch (std::exception& e) {
        vm.hostDepth--;
        vm.tempRoots.resize(mark);
        vm.rtError("native error", e.what());
    }
}

static void staleError(VM& vm, ObjHandle* h) {
    std::string n = h->cls ? h->cls->name : "native";
    vm.rtError("use of invalid " + n + " handle", "this " + n + " was destroyed - the handle is no longer valid");
}

Value handleGet(VM& vm, ObjHandle* h, ObjString* name) {
    if (!h->alive) staleError(vm, h);
    if (h->cls) {
        auto p = h->cls->props.find(name->s);
        if (p != h->cls->props.end() && p->second.get) {
            return guardHost(vm, [&]() { return hostToValue(vm, p->second.get(h->ptr)); });
        }
        if (p != h->cls->props.end()) {
            vm.rtError("missing native", "'" + h->cls->name + "." + name->s +
                                             "' is declared in a binding file but the host did not register it");
        }
        if (h->cls->methods.count(name->s)) {
            ObjBound* b = vm.alloc<ObjBound>();
            b->recv = Value::object(h);
            b->method = Value::object(name);
            return Value::object(b);
        }
    }
    vm.rtError("unknown member", "'" + vm.typeName(Value::object(h)) + "' has no property '" + name->s + "'");
}

void handleSet(VM& vm, ObjHandle* h, ObjString* name, const Value& v) {
    if (!h->alive) staleError(vm, h);
    if (h->cls) {
        auto p = h->cls->props.find(name->s);
        if (p != h->cls->props.end()) {
            if (!p->second.set && p->second.writable) {
                vm.rtError("missing native", "'" + h->cls->name + "." + name->s +
                                                 "' is declared in a binding file but the host did not register it");
            }
            if (!p->second.set) vm.rtError("read-only property", "'" + name->s + "' cannot be assigned");
            TaipanValue hv = valueToHost(vm, v);
            guardHost(vm, [&]() {
                p->second.set(h->ptr, hv);
                return 0;
            });
            return;
        }
    }
    vm.rtError("unknown member", "'" + vm.typeName(Value::object(h)) + "' has no property '" + name->s + "'");
}

Value VM::callNativeOp(ObjValue* self, const NativeOp& op, const Value* other, int n) {
    if (!op.fn) {
        rtError("missing native", "operator '" + op.op + "' of '" + self->cls->name +
                                      "' is declared in a binding file but the host did not register it");
    }
    TaipanArgs args = makeArgs(*this, other, n);
    TaipanMethodFn fn = op.fn;
    void* data = self->data;
    return guardHost(*this, [&]() { return hostToValue(*this, fn(data, std::move(args))); });
}

Value VM::callNativeMethod(ObjHandle* h, const std::string& name, Value* argv, int argc) {
    if (!h->alive) staleError(*this, h);
    NativeMethod* m = nullptr;
    if (h->cls) {
        auto it = h->cls->methods.find(name);
        if (it != h->cls->methods.end()) m = &it->second;
    }
    if (!m) rtError("unknown method", "'" + typeName(Value::object(h)) + "' has no method '" + name + "'");
    return callNativeMethodResolved(h, m, name, argv, argc);
}

Value VM::callNativeMethodResolved(ObjHandle* h, NativeMethod* m, const std::string& name, Value* argv, int argc) {
    if (!h->alive) staleError(*this, h);
    if (!m->fn) {
        rtError("missing native", "'" + h->cls->name + "." + name + "' is declared in a binding file but the host did not register it");
    }
    TaipanArgs args = makeArgs(*this, argv, argc);
    TaipanMethodFn fn = m->fn; // copy: the method table may change during the call
    void* self = h->ptr;
    return guardHost(*this, [&]() { return hostToValue(*this, fn(self, std::move(args))); });
}

} // namespace tp

// ===========================================================================
// Class builder
// ===========================================================================
TaipanClassBuilder& TaipanClassBuilder::method(const char* name, TaipanMethodFn fn, const char* signature) {
    addMethod(name, std::move(fn), signature ? signature : "");
    return *this;
}

TaipanClassBuilder& TaipanClassBuilder::property(const char* name, TaipanGetterFn getter, TaipanSetterFn setter,
                                                 const char* type) {
    bool writable = (bool)setter;
    addProperty(name, std::move(getter), std::move(setter), type ? type : "any", writable);
    return *this;
}

TaipanClassBuilder& TaipanClassBuilder::alias(const char* typeName) {
    auto* nc = static_cast<NativeClass*>(cls_);
    nc->aliases.push_back(typeName);
    state_->vm.nativeTypeNames[typeName] = nc;
    return *this;
}

void TaipanClassBuilder::addProperty(const char* name, TaipanGetterFn getter, TaipanSetterFn setter,
                                     const std::string& type, bool writable) {
    auto* nc = static_cast<NativeClass*>(cls_);
    NativeProp p;
    p.get = std::move(getter);
    p.set = std::move(setter);
    p.type = type;
    p.writable = writable && (bool)p.set;
    nc->props[name] = std::move(p);
}

void TaipanClassBuilder::addOperator(const std::string& symbol, bool reversed, const std::string& other,
                                     const std::string& result, TaipanMethodFn fn) {
    auto* nc = static_cast<NativeClass*>(cls_);
    for (auto& o : nc->ops) {
        if (o.op == symbol && o.reversed == reversed && o.other == other) {
            o.result = result;
            o.fn = std::move(fn);
            return;
        }
    }
    nc->ops.push_back(NativeOp {symbol, reversed, other, result, std::move(fn)});
}

static NativeImpl wrapHostFn(TaipanNativeFn f) {
    return [f](VM& v, Value* argv, int argc) {
        TaipanArgs args = makeArgs(v, argv, argc);
        return guardHost(v, [&]() { return hostToValue(v, f(std::move(args))); });
    };
}

void TaipanClassBuilder::addConstructor(TaipanNativeFn fn, const std::string& signature) {
    VM& vm = state_->vm;
    auto* nc = static_cast<NativeClass*>(cls_);
    nc->ctorSig = signature;
    vm.gcPause++;
    ObjNamespace* ns = vm.namespaceFor(name_, true);
    ns->call = Value::object(vm.newNative(name_, wrapHostFn(std::move(fn))));
    vm.barrier(ns, ns->call);
    vm.nsEpoch++;
    vm.gcPause--;
}

void TaipanClassBuilder::addStatic(const char* name, TaipanNativeFn fn, const std::string& signature) {
    taipan_register_function(state_, (name_ + "." + name).c_str(), std::move(fn), signature.c_str());
}

void TaipanClassBuilder::addConstant(const char* name, const TaipanValue& value, const std::string& type) {
    VM& vm = state_->vm;
    vm.gcPause++;
    ObjNamespace* ns = vm.namespaceFor(name_, true);
    ObjString* key = vm.str(name);
    Value v = hostToValue(vm, value);
    ns->members[key] = v;
    vm.barrier(ns, Value::object(key));
    vm.barrier(ns, v);
    std::string qualified = name_ + "." + name;
    vm.nativeSigs[qualified] = type;
    vm.hostConstants.insert(qualified);
    vm.nsEpoch++;
    vm.gcPause--;
}

void TaipanClassBuilder::setToString(std::function<std::string(const void*)> fn) {
    static_cast<NativeClass*>(cls_)->toString = std::move(fn);
}

void TaipanClassBuilder::addMethod(const char* name, TaipanMethodFn fn, const std::string& signature) {
    auto* nc = static_cast<NativeClass*>(cls_);
    nc->methods[name] = NativeMethod {std::move(fn), signature};
}

// ===========================================================================
// State and loading
// ===========================================================================
TaipanState* taipan_create() {
    auto* s = new TaipanState();
    s->vm.state = s;
    return s;
}

void taipan_destroy(TaipanState* state) { delete state; }

void taipan_open_standalone(TaipanState* state) { state->vm.openStandalone(); }

static bool finishLoad(VM& vm, Loader::Entry* e, std::vector<Diagnostic>& diags) {
    vm.reportAll(diags);
    if (!e) return false;
    vm.mainModule = vm.loader->instantiate(e);
    return true;
}

bool taipan_load_file(TaipanState* state, const char* path) {
    VM& vm = state->vm;
    vm.lastError.clear();
    std::vector<Diagnostic> diags;
    Loader::Entry* e = vm.loader->loadMain(path, diags);
    return finishLoad(vm, e, diags);
}

bool taipan_load_string(TaipanState* state, const char* source, const char* chunkName) {
    VM& vm = state->vm;
    vm.lastError.clear();
    std::vector<Diagnostic> diags;
    Loader::Entry* e = vm.loader->loadString(source, chunkName ? chunkName : "<string>", diags);
    return finishLoad(vm, e, diags);
}

bool taipan_check_file(TaipanState* state, const char* path) {
    VM& vm = state->vm;
    vm.lastError.clear();
    bool cache = vm.cacheEnabled;
    vm.cacheEnabled = false; // always re-check from source
    std::vector<Diagnostic> diags;
    Loader::Entry* e = vm.loader->loadMain(path, diags);
    vm.cacheEnabled = cache;
    vm.reportAll(diags);
    return e != nullptr;
}

static bool runProtected(VM& vm, const std::function<void()>& f) {
    ObjThread* saved = vm.current;
    if (vm.hostDepth == 0) vm.current = vm.mainThread;
    try {
        f();
    } catch (ScriptError& e) {
        vm.current = saved;
        if (vm.hostDepth > 0) throw;
        vm.reportUncaught(e);
        return false;
    }
    vm.current = saved;
    return true;
}

bool taipan_run(TaipanState* state) {
    VM& vm = state->vm;
    if (!vm.mainModule) {
        vm.emitError("error: nothing to run\n  = load a script with taipan_load_file first\n");
        return false;
    }
    return runProtected(vm, [&]() { vm.runModuleMain(vm.mainModule, false); });
}

static Value lookupFunction(VM& vm, const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        std::string prefix = name.substr(0, dot);
        ObjString* member = vm.str(name.substr(dot + 1));
        auto mi = vm.modules.find(prefix);
        if (mi != vm.modules.end()) {
            auto it = mi->second->names.find(member);
            if (it != mi->second->names.end() && mi->second->defined[it->second]) return mi->second->globals[it->second];
            return Value();
        }
        if (ObjNamespace* ns = vm.namespaceFor(prefix, false)) {
            auto it = ns->members.find(member);
            if (it != ns->members.end()) return it->second;
        }
        return Value();
    }
    ObjString* key = vm.str(name);
    std::vector<ObjModule*> order;
    if (vm.mainModule) order.push_back(vm.mainModule);
    for (auto& kv : vm.modules) {
        if (kv.second != vm.mainModule) order.push_back(kv.second);
    }
    for (ObjModule* m : order) {
        auto it = m->names.find(key);
        if (it != m->names.end() && it->second < (int)m->defined.size() && m->defined[it->second]) {
            return m->globals[it->second];
        }
    }
    auto it = vm.builtins->members.find(key);
    return it != vm.builtins->members.end() ? it->second : Value();
}

static bool isCallable(const Value& v) {
    return v.isObj() && (v.o->type == OT::Closure || v.o->type == OT::Native || v.o->type == OT::Bound ||
                         v.o->type == OT::Class);
}

bool taipan_reload_file(TaipanState* state, const char* path) {
    VM& vm = state->vm;
    vm.lastError.clear();
    std::vector<Diagnostic> diags;
    std::vector<ObjModule*> updated;
    bool ok = vm.loader->reload(path, diags, updated);
    vm.reportAll(diags);
    if (!ok) return false;
    for (ObjModule* m : updated) {
        if (!runProtected(vm, [&]() { vm.runModuleMain(m, true); })) ok = false;
        auto it = m->names.find(vm.str("onReload"));
        if (it != m->names.end() && m->defined[it->second] && isCallable(m->globals[it->second])) {
            Value fn = m->globals[it->second];
            if (!runProtected(vm, [&]() { vm.callValue(fn, {}); })) ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Script instances
// ---------------------------------------------------------------------------
struct TaipanScript {
    ObjModule* module = nullptr;
};

static Value scriptGlobal(VM& vm, TaipanScript* script, const char* name) {
    if (!script || !script->module) return Value();
    ObjModule* m = script->module;
    auto it = m->names.find(vm.str(name));
    if (it == m->names.end() || it->second >= (int)m->defined.size() || !m->defined[it->second]) return Value();
    return m->globals[it->second];
}

TaipanScript* taipan_script_new(TaipanState* state, const char* path) {
    VM& vm = state->vm;
    vm.lastError.clear();
    std::vector<Diagnostic> diags;
    Loader::Entry* e = vm.loader->loadMain(path, diags);
    vm.reportAll(diags);
    if (!e) return nullptr;
    auto* s = new TaipanScript();
    s->module = vm.loader->newInstance(e);
    return s;
}

bool taipan_script_run(TaipanState* state, TaipanScript* script) {
    VM& vm = state->vm;
    if (!script || !script->module) return false;
    return runProtected(vm, [&]() { vm.runModuleMain(script->module, false); });
}

TaipanValue taipan_script_call(TaipanState* state, TaipanScript* script, const char* function,
                               const std::vector<TaipanValue>& args) {
    VM& vm = state->vm;
    vm.lastCallOk = true;
    Value fn = scriptGlobal(vm, script, function);
    if (!isCallable(fn)) {
        Diagnostic d;
        d.title = "unknown function";
        d.label = std::string("no function named '") + function + "' in this script";
        vm.report(d);
        vm.lastCallOk = false;
        return TaipanValue();
    }
    TaipanValue out;
    size_t mark = vm.tempRoots.size();
    bool ok = runProtected(vm, [&]() {
        std::vector<Value> vals;
        vm.tempRoots.push_back(fn);
        for (auto& a : args) {
            vals.push_back(hostToValue(vm, a));
            vm.tempRoots.push_back(vals.back());
        }
        out = valueToHost(vm, vm.callValue(fn, vals));
    });
    vm.tempRoots.resize(mark);
    vm.lastCallOk = ok;
    return out;
}

bool taipan_script_has_function(TaipanState* state, TaipanScript* script, const char* function) {
    return isCallable(scriptGlobal(state->vm, script, function));
}

TaipanValue taipan_script_get(TaipanState* state, TaipanScript* script, const char* global) {
    return valueToHost(state->vm, scriptGlobal(state->vm, script, global));
}

void taipan_script_set(TaipanState* state, TaipanScript* script, const char* global, const TaipanValue& value) {
    VM& vm = state->vm;
    if (!script || !script->module) return;
    auto it = script->module->names.find(vm.str(global));
    if (it == script->module->names.end()) return;
    vm.defineGlobal(script->module, it->second, hostToValue(vm, value));
}

void taipan_script_free(TaipanState* state, TaipanScript* script) {
    if (!script) return;
    VM& vm = state->vm;
    // coroutines started by this script stop with it
    std::vector<ObjThread*> owned;
    for (ObjThread* t : vm.coroutines) {
        if (t == vm.current || t->state == ThreadState::Running) continue;
        for (auto& f : t->frames) {
            if (f.closure->module == script->module) {
                owned.push_back(t);
                break;
            }
        }
    }
    for (ObjThread* t : owned) vm.finishThread(t);
    auto& v = vm.instances;
    v.erase(std::remove(v.begin(), v.end(), script->module), v.end());
    delete script;
}

TaipanValue taipan_call_function(TaipanState* state, const char* name, const std::vector<TaipanValue>& args) {
    VM& vm = state->vm;
    vm.lastCallOk = true;
    Value fn = lookupFunction(vm, name);
    if (!isCallable(fn)) {
        Diagnostic d;
        d.title = "unknown function";
        d.label = std::string("no function named '") + name + "' is defined";
        vm.report(d);
        vm.lastCallOk = false;
        return TaipanValue();
    }
    TaipanValue out;
    size_t mark = vm.tempRoots.size();
    bool ok = runProtected(vm, [&]() {
        std::vector<Value> vals;
        vm.tempRoots.push_back(fn);
        for (auto& a : args) {
            vals.push_back(hostToValue(vm, a));
            vm.tempRoots.push_back(vals.back());
        }
        Value r = vm.callValue(fn, vals);
        out = valueToHost(vm, r);
    });
    vm.tempRoots.resize(mark);
    vm.lastCallOk = ok;
    return out;
}

bool taipan_has_function(TaipanState* state, const char* name) { return isCallable(lookupFunction(state->vm, name)); }

bool taipan_call_ok(TaipanState* state) { return state->vm.lastCallOk; }

void taipan_update(TaipanState* state, double dt) {
    VM& vm = state->vm;
    runProtected(vm, [&]() { vm.update(dt); });
}

int taipan_coroutine_count(TaipanState* state) {
    int n = 0;
    for (auto* t : state->vm.coroutines) {
        if (t->state != ThreadState::Dead) n++;
    }
    return n;
}

double taipan_next_wake(TaipanState* state) {
    VM& vm = state->vm;
    double best = -1;
    for (auto* t : vm.coroutines) {
        if (t->state != ThreadState::Suspended) continue;
        double d = std::max(0.0, t->wakeAt - vm.time);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// ===========================================================================
// Bindings and globals
// ===========================================================================
void taipan_register_function(TaipanState* state, const char* name, TaipanNativeFn fn, const char* signature) {
    VM& vm = state->vm;
    TaipanNativeFn f = std::move(fn);
    vm.hostNatives.insert(name);
    vm.defineNative(name, [f](VM& v, Value* argv, int argc) {
        TaipanArgs args = makeArgs(v, argv, argc);
        return guardHost(v, [&]() { return hostToValue(v, f(std::move(args))); });
    }, signature ? signature : "");
}

TaipanClassBuilder taipan_register_class(TaipanState* state, const char* name) {
    VM& vm = state->vm;
    auto it = vm.nativeClasses.find(name);
    NativeClass* nc;
    if (it != vm.nativeClasses.end()) {
        nc = it->second.get();
    } else {
        auto c = std::make_unique<NativeClass>();
        c->name = name;
        nc = c.get();
        vm.nativeClasses[name] = std::move(c);
    }
    vm.nativeTypeNames[name] = nc;
    return TaipanClassBuilder(state, nc, name);
}

bool taipan_load_bindings(TaipanState* state, const char* path) {
    VM& vm = state->vm;
    std::vector<Diagnostic> diags;
    bool ok = vm.loadBindings(path, diags);
    vm.reportAll(diags);
    return ok;
}

bool taipan_dump_bindings(TaipanState* state, const char* path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    std::string text = state->vm.dumpBindings();
    out.write(text.data(), (std::streamsize)text.size());
    return (bool)out;
}

TaipanClassBuilder taipan_register_value_type(TaipanState* state, const char* name, std::size_t size,
                                              std::size_t align, void (*copy)(void*, const void*),
                                              void (*destroy)(void*)) {
    TaipanClassBuilder b = taipan_register_class(state, name);
    NativeClass* nc = state->vm.nativeClasses[name].get();
    nc->isValue = true;
    nc->size = size;
    nc->align = align;
    nc->copy = copy;
    nc->destroy = destroy;
    return b;
}

void taipan_invalidate_object(TaipanState* state, void* ptr) {
    VM& vm = state->vm;
    auto it = vm.handles.find(ptr);
    if (it == vm.handles.end()) return;
    it->second->alive = false;
    it->second->ptr = nullptr;
    vm.handles.erase(it);
}

TaipanValue taipan_get_global(TaipanState* state, const char* name) {
    VM& vm = state->vm;
    ObjString* key = vm.str(name);
    if (ObjModule* m = vm.mainModule) {
        auto it = m->names.find(key);
        if (it != m->names.end() && it->second < (int)m->defined.size() && m->defined[it->second]) {
            return valueToHost(vm, m->globals[it->second]);
        }
    }
    auto it = vm.builtins->members.find(key);
    return it != vm.builtins->members.end() ? valueToHost(vm, it->second) : TaipanValue();
}

void taipan_set_global(TaipanState* state, const char* name, const TaipanValue& value) {
    VM& vm = state->vm;
    vm.gcPause++;
    ObjString* key = vm.str(name);
    Value v = hostToValue(vm, value);
    if (ObjModule* m = vm.mainModule) {
        auto it = m->names.find(key);
        if (it != m->names.end()) {
            vm.defineGlobal(m, it->second, v);
            vm.gcPause--;
            return;
        }
    }
    vm.builtins->members[key] = v;
    vm.nsEpoch++;
    vm.barrier(vm.builtins, Value::object(key));
    vm.barrier(vm.builtins, v);
    vm.gcPause--;
}

void taipan_set_global_int(TaipanState* s, const char* n, int64_t v) { taipan_set_global(s, n, TaipanValue::fromInt(v)); }
void taipan_set_global_float(TaipanState* s, const char* n, double v) { taipan_set_global(s, n, TaipanValue::fromFloat(v)); }
void taipan_set_global_bool(TaipanState* s, const char* n, bool v) { taipan_set_global(s, n, TaipanValue::fromBool(v)); }
void taipan_set_global_string(TaipanState* s, const char* n, const char* v) {
    taipan_set_global(s, n, TaipanValue::fromString(v ? v : ""));
}

TaipanValue taipan_new_list(TaipanState* state, const std::vector<TaipanValue>& items) {
    VM& vm = state->vm;
    vm.gcPause++;
    ObjList* l = vm.newList();
    for (auto& i : items) l->items.push_back(hostToValue(vm, i));
    if (vm.hostDepth > 0) vm.tempRoots.push_back(Value::object(l));
    vm.gcPause--;
    return valueToHost(vm, Value::object(l));
}

TaipanValue taipan_new_dict(TaipanState* state) {
    VM& vm = state->vm;
    ObjDict* d = vm.newDict();
    if (vm.hostDepth > 0) vm.tempRoots.push_back(Value::object(d));
    return valueToHost(vm, Value::object(d));
}

void taipan_list_add(TaipanState* state, const TaipanValue& list, const TaipanValue& item) {
    if (!list.isList()) return;
    VM& vm = state->vm;
    auto* l = static_cast<ObjList*>(TaipanInternal::ref(list));
    Value v = hostToValue(vm, item);
    l->items.push_back(v);
    vm.barrier(l, v);
}

void taipan_dict_set(TaipanState* state, const TaipanValue& dict, const std::string& key, const TaipanValue& value) {
    if (!dict.isDict()) return;
    VM& vm = state->vm;
    auto* d = static_cast<ObjDict*>(TaipanInternal::ref(dict));
    Value k = vm.strv(key);
    Value v = hostToValue(vm, value);
    d->put(k, v);
    vm.barrier(d, k);
    vm.barrier(d, v);
}

void taipan_pin(TaipanState* state, const TaipanValue& value) {
    VM& vm = state->vm;
    vm.pins.push_back(hostToValue(vm, value));
}

void taipan_unpin(TaipanState* state, const TaipanValue& value) {
    VM& vm = state->vm;
    Value v = hostToValue(vm, value);
    for (auto it = vm.pins.begin(); it != vm.pins.end(); ++it) {
        if (vm.equals(*it, v)) {
            vm.pins.erase(it);
            return;
        }
    }
}

// ===========================================================================
// Configuration and GC
// ===========================================================================
const std::string& taipan_last_error(TaipanState* state) { return state->vm.lastError; }

void taipan_set_error_handler(TaipanState* state, std::function<void(const std::string&)> handler) {
    state->vm.errorHandler = std::move(handler);
}

void taipan_set_print_handler(TaipanState* state, std::function<void(const std::string&)> handler) {
    state->vm.printHandler = std::move(handler);
}

void taipan_set_script_root(TaipanState* state, const char* directory) {
    state->vm.scriptRoot = directory ? directory : "";
    state->vm.loader->root.clear();
}

void taipan_set_bytecode_cache(TaipanState* state, bool enabled, const char* directory) {
    state->vm.cacheEnabled = enabled;
    if (directory) state->vm.cacheDir = directory;
}

void taipan_set_warnings(TaipanState* state, bool enabled) { state->vm.warnings = enabled; }

void taipan_set_dump_bytecode(TaipanState* state, bool enabled) { state->vm.dumpBytecode = enabled; }

void taipan_gc_collect(TaipanState* state) { state->vm.fullCollect(); }

void taipan_set_gc_stress(TaipanState* state, bool enabled) { state->vm.gcStress = enabled; }

void taipan_set_gc_tiny(TaipanState* state, bool enabled) {
    if (enabled) state->vm.setTinyGc();
}

std::size_t taipan_gc_object_count(TaipanState* state) { return state->vm.objectCount; }

std::string taipan_gc_stats(TaipanState* state) {
    VM& vm = state->vm;
    return "gc: " + std::to_string(vm.objectCount) + " live objects (" + std::to_string(vm.youngBytes) + " young / " +
           std::to_string(vm.oldBytes) + " old bytes), " + std::to_string(vm.minorCount) + " minor + " +
           std::to_string(vm.majorCount) + " major collections, " + std::to_string(vm.freedCount) + " objects freed";
}
