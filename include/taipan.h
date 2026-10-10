// taipan.h - Taipan scripting language, public embedding API.
//
// One header is all a host engine needs:
//
//     TaipanState* state = taipan_create();
//     taipan_load_file(state, "enemy_ai.tp");
//     taipan_run(state);
//     taipan_call_function(state, "onPlayerDeath");
//     taipan_destroy(state);
//
// Native functions all share one signature: TaipanValue fn (TaipanArgs args)
// Native classes are exposed with a builder (see taipan_register_class).

#ifndef TAIPAN_H
#define TAIPAN_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define TAIPAN_VERSION "0.2.0"

struct TaipanState;

// ---------------------------------------------------------------------------
// TaipanValue - a script value as seen by the host.
//
// Primitives and strings are copied into the TaipanValue. Lists, dicts,
// functions and script objects are references into the VM: they stay valid
// while they are reachable from the script (or while inside a native call,
// or when pinned with taipan_pin).
// ---------------------------------------------------------------------------
class TaipanValue {
public:
    enum class Type { Null, Bool, Int, Float, String, Object, List, Dict, Function, Ref, Value };

    TaipanValue() = default;

    static TaipanValue null();
    static TaipanValue fromBool(bool b);
    static TaipanValue fromInt(int64_t i);
    static TaipanValue fromFloat(double f);
    static TaipanValue fromString(std::string s);
    // A handle to an engine-owned native object. The engine keeps ownership;
    // call taipan_invalidate_object() when the object is destroyed.
    static TaipanValue fromObject(const char* className, void* ptr);
    // A copy of a registered value type (taipan_register_value), e.g. a Vector3.
    template <class T> static TaipanValue fromValue(const T& value);

    Type type() const { return type_; }
    std::string typeName() const;
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInt() const { return type_ == Type::Int; }
    bool isFloat() const { return type_ == Type::Float; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Float; }
    bool isString() const { return type_ == Type::String; }
    bool isObject() const { return type_ == Type::Object; }
    bool isList() const { return type_ == Type::List; }
    bool isDict() const { return type_ == Type::Dict; }
    bool isFunction() const { return type_ == Type::Function; }
    bool isValue() const { return type_ == Type::Value; }

    bool asBool() const;
    int64_t asInt() const;          // floats are truncated
    double asFloat() const;         // ints are widened
    const std::string& asString() const;
    void* asObject() const;         // nullptr if not an object or stale
    template <class T> T* as() const { return static_cast<T*>(asObject()); }
    const std::string& objectClass() const { return str_; }
    template <class T> const T* asValue() const; // null unless this holds that value type
    const void* valuePtr() const;                // raw storage of a value type

    // Containers (List / Dict refs).
    int length() const;
    TaipanValue at(int index) const;
    TaipanValue get(const std::string& key) const;

    std::string toString() const;

private:
    Type type_ = Type::Null;
    union {
        bool b;
        int64_t i;
        double f;
        void* p;
    } u_ {};
    std::string str_;                // String payload, or Object / value class name
    TaipanState* owner_ = nullptr;   // owning VM for references
    std::shared_ptr<void> box_;      // a value type created on the host side
    static TaipanValue boxed(const std::string& className, std::shared_ptr<void> data);
    friend struct TaipanInternal;
};

// ---------------------------------------------------------------------------
// TaipanArgs - the arguments of a native call.
// ---------------------------------------------------------------------------
//
// The typed getters read the arguments directly and are the fast way to use
// them; operator[] converts every argument to a TaipanValue on first use.
class TaipanArgs {
public:
    int count() const { return count_; }
    const TaipanValue& operator[](int i) const;
    TaipanValue::Type typeAt(int i) const;   // Type::Null for missing arguments
    bool isNull(int i) const { return typeAt(i) == TaipanValue::Type::Null; }

    int64_t getInt(int i) const;             // floats are truncated
    double getFloat(int i) const;            // ints are widened
    bool getBool(int i) const;
    std::string getString(int i) const;
    void* getObject(int i, const char* className = nullptr) const;
    template <class T> T* getObject(int i) const { return static_cast<T*>(getObject(i, nullptr)); }
    const void* getValue(int i, const char* className) const; // a value type argument's storage

    TaipanState* state() const { return state_; }

    // Call a script function value (e.g. a callback passed in by the script).
    TaipanValue call(const TaipanValue& fn, const std::vector<TaipanValue>& args = {}) const;

    // Raise a script error. Catchable with try/catch on the script side.
    [[noreturn]] void error(const std::string& message) const;

private:
    struct Raw {                 // a copy of the VM's own value representation
        uint64_t lo, hi;
    };
    static const int kInline = 6;
    TaipanState* state_ = nullptr;
    int count_ = 0;
    Raw inline_[kInline] = {};
    std::vector<Raw> extra_;     // arguments beyond kInline
    mutable std::vector<TaipanValue> boxed_;
    const Raw& raw(int i) const { return i < kInline ? inline_[i] : extra_[i - kInline]; }
    friend struct TaipanInternal;
};

using TaipanNativeFn = std::function<TaipanValue(TaipanArgs)>;
using TaipanMethodFn = std::function<TaipanValue(void* self, TaipanArgs)>;
using TaipanGetterFn = std::function<TaipanValue(void* self)>;
using TaipanSetterFn = std::function<void(void* self, const TaipanValue& value)>;

// ---------------------------------------------------------------------------
// Template glue used by the class builder (C++ value <-> TaipanValue).
// ---------------------------------------------------------------------------
namespace taipan_detail {

[[noreturn]] void fail(const std::string& message);

template <class T> struct ClassName {
    static std::string& get() {
        static std::string name;
        return name;
    }
};

template <class T, class Enable = void> struct Conv;

template <class T>
struct Conv<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type> {
    static T fromValue(const TaipanValue& v) {
        if (!v.isNumber()) fail("expected 'int', found '" + v.typeName() + "'");
        return (T)v.asInt();
    }
    static T fromArgs(const TaipanArgs& a, int i) { return (T)a.getInt(i); }
    static TaipanValue to(T x) { return TaipanValue::fromInt((int64_t)x); }
    static std::string name() { return "int"; }
};

template <class T> struct Conv<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
    static T fromValue(const TaipanValue& v) {
        if (!v.isNumber()) fail("expected 'float', found '" + v.typeName() + "'");
        return (T)v.asFloat();
    }
    static T fromArgs(const TaipanArgs& a, int i) { return (T)a.getFloat(i); }
    static TaipanValue to(T x) { return TaipanValue::fromFloat((double)x); }
    static std::string name() { return "float"; }
};

template <> struct Conv<bool> {
    static bool fromValue(const TaipanValue& v) {
        if (!v.isBool()) fail("expected 'bool', found '" + v.typeName() + "'");
        return v.asBool();
    }
    static bool fromArgs(const TaipanArgs& a, int i) { return a.getBool(i); }
    static TaipanValue to(bool x) { return TaipanValue::fromBool(x); }
    static std::string name() { return "bool"; }
};

template <> struct Conv<std::string> {
    static std::string fromValue(const TaipanValue& v) {
        if (!v.isString()) fail("expected 'str', found '" + v.typeName() + "'");
        return v.asString();
    }
    static std::string fromArgs(const TaipanArgs& a, int i) { return a.getString(i); }
    static TaipanValue to(const std::string& x) { return TaipanValue::fromString(x); }
    static std::string name() { return "str"; }
};

template <> struct Conv<const char*> {
    static TaipanValue to(const char* x) { return x ? TaipanValue::fromString(x) : TaipanValue(); }
    static std::string name() { return "str"; }
};

template <> struct Conv<TaipanValue> {
    static TaipanValue fromValue(const TaipanValue& v) { return v; }
    static TaipanValue fromArgs(const TaipanArgs& a, int i) { return a[i]; }
    static TaipanValue to(const TaipanValue& x) { return x; }
    static std::string name() { return "any"; }
};

template <class T> struct Conv<T*, typename std::enable_if<std::is_class<T>::value>::type> {
    static T* fromValue(const TaipanValue& v) {
        if (v.isNull()) return nullptr;
        if (!v.isObject()) fail("expected '" + name() + "', found '" + v.typeName() + "'");
        const std::string& expected = ClassName<T>::get();
        if (!expected.empty() && v.objectClass() != expected) {
            fail("expected '" + expected + "', found '" + v.objectClass() + "'");
        }
        void* p = v.asObject();
        if (!p) fail("use of invalid '" + v.objectClass() + "' handle - the object was destroyed");
        return static_cast<T*>(p);
    }
    static T* fromArgs(const TaipanArgs& a, int i) {
        if (a.isNull(i)) return nullptr;
        const std::string& expected = ClassName<T>::get();
        return static_cast<T*>(a.getObject(i, expected.empty() ? nullptr : expected.c_str()));
    }
    static TaipanValue to(T* x) {
        if (!x) return TaipanValue();
        return TaipanValue::fromObject(ClassName<T>::get().c_str(), x);
    }
    static std::string name() {
        const std::string& n = ClassName<T>::get();
        return n.empty() ? std::string("any") : n;
    }
};

// Registered value types (taipan_register_value) are passed by value.
template <class T>
struct Conv<T, typename std::enable_if<std::is_class<T>::value && !std::is_same<T, std::string>::value &&
                                       !std::is_same<T, TaipanValue>::value>::type> {
    static T fromValue(const TaipanValue& v) {
        const T* p = v.template asValue<T>();
        if (!p) fail("expected '" + name() + "', found '" + v.typeName() + "'");
        return *p;
    }
    static T fromArgs(const TaipanArgs& a, int i) {
        return *static_cast<const T*>(a.getValue(i, ClassName<T>::get().c_str()));
    }
    static TaipanValue to(const T& x) { return TaipanValue::fromValue(x); }
    static std::string name() {
        const std::string& n = ClassName<T>::get();
        return n.empty() ? std::string("any") : n;
    }
};

// The signature R(A...) of a function pointer, lambda or other callable.
template <class F> struct FnTraits : FnTraits<decltype(&F::operator())> {};
template <class R, class... A> struct FnTraits<R(A...)> { using Sig = R(A...); };
template <class R, class... A> struct FnTraits<R (*)(A...)> { using Sig = R(A...); };
template <class C, class R, class... A> struct FnTraits<R (C::*)(A...)> { using Sig = R(A...); };
template <class C, class R, class... A> struct FnTraits<R (C::*)(A...) const> { using Sig = R(A...); };

template <class R> inline std::string returnName() {
    if constexpr(std::is_void<R>::value) return "void";
    else return Conv<typename std::decay<R>::type>::name();
}

template <class... A> inline std::string signature(const std::string& ret) {
    std::string s = "(";
    std::string names[] = {std::string(), Conv<typename std::decay<A>::type>::name()...};
    for (std::size_t i = 1; i < sizeof(names) / sizeof(names[0]); i++) {
        if (i > 1) s += ", ";
        s += names[i];
    }
    return s + ") -> " + ret;
}

template <class C, class M, class R, class... A, std::size_t... I>
inline TaipanValue invoke(C* self, M method, const TaipanArgs& args, std::index_sequence<I...>) {
    if (args.count() < (int)sizeof...(A)) {
        fail("expected " + std::to_string(sizeof...(A)) + " arguments, found " + std::to_string(args.count()));
    }
    if constexpr(std::is_void<R>::value) {
        (self->*method)(Conv<typename std::decay<A>::type>::fromArgs(args, (int)I)...);
        return TaipanValue();
    } else {
        return Conv<typename std::decay<R>::type>::to(
            (self->*method)(Conv<typename std::decay<A>::type>::fromArgs(args, (int)I)...));
    }
}

// Wraps a free callable R(A...) as a native function.
template <class Sig> struct FreeCall;
template <class R, class... A> struct FreeCall<R(A...)> {
    template <class F, std::size_t... I>
    static TaipanValue call(const F& f, const TaipanArgs& args, std::index_sequence<I...>) {
        if (args.count() < (int)sizeof...(A)) {
            fail("expected " + std::to_string(sizeof...(A)) + " arguments, found " + std::to_string(args.count()));
        }
        if constexpr(std::is_void<R>::value) {
            f(Conv<typename std::decay<A>::type>::fromArgs(args, (int)I)...);
            return TaipanValue();
        } else {
            return Conv<typename std::decay<R>::type>::to(f(Conv<typename std::decay<A>::type>::fromArgs(args, (int)I)...));
        }
    }
    template <class F> static TaipanNativeFn make(F f) {
        return [f](TaipanArgs args) { return call(f, args, std::index_sequence_for<A...> {}); };
    }
    static std::string sig() { return signature<A...>(returnName<R>()); }
};

// Constructs a value type T from arguments A...
template <class Sig> struct CtorCall;
template <class T, class... A> struct CtorCall<T(A...)> {
    template <std::size_t... I> static TaipanValue call(const TaipanArgs& args, std::index_sequence<I...>) {
        if (args.count() < (int)sizeof...(A)) {
            fail("expected " + std::to_string(sizeof...(A)) + " arguments, found " + std::to_string(args.count()));
        }
        return Conv<T>::to(T(Conv<typename std::decay<A>::type>::fromArgs(args, (int)I)...));
    }
    static TaipanNativeFn make() {
        return [](TaipanArgs args) { return call(args, std::index_sequence_for<A...> {}); };
    }
    static std::string sig() { return signature<A...>(Conv<T>::name()); }
};

} // namespace taipan_detail

template <class T> TaipanValue TaipanValue::fromValue(const T& value) {
    return boxed(taipan_detail::ClassName<T>::get(), std::make_shared<T>(value));
}

template <class T> const T* TaipanValue::asValue() const {
    if (type_ != Type::Value) return nullptr;
    const std::string& n = taipan_detail::ClassName<T>::get();
    if (!n.empty() && n != str_) return nullptr;
    return static_cast<const T*>(valuePtr());
}

// ---------------------------------------------------------------------------
// Native class builder:
//
//     taipan_register_class(state, "Entity")
//         .property("x", &Entity::x)
//         .property("hp", &Entity::hp)
//         .method("takeDamage", &Entity::takeDamage);
// ---------------------------------------------------------------------------
class TaipanClassBuilder {
public:
    TaipanClassBuilder(TaipanState* state, void* cls, std::string name)
        : state_(state), cls_(cls), name_(std::move(name)) {}

    template <class C, class F> TaipanClassBuilder& property(const char* name, F C::* member) {
        static_assert(!std::is_function<F>::value, "use .method() for member functions");
        bind<C>();
        using D = typename std::remove_const<F>::type;
        TaipanGetterFn getter = [member](void* self) {
            return taipan_detail::Conv<D>::to(static_cast<C*>(self)->*member);
        };
        TaipanSetterFn setter;
        if constexpr(!std::is_const<F>::value) {
            setter = [member](void* self, const TaipanValue& v) {
                static_cast<C*>(self)->*member = taipan_detail::Conv<D>::fromValue(v);
            };
        }
        addProperty(name, getter, setter, taipan_detail::Conv<D>::name(), !std::is_const<F>::value);
        return *this;
    }

    template <class C, class R, class... A> TaipanClassBuilder& method(const char* name, R(C::*m)(A...)) {
        bind<C>();
        TaipanMethodFn fn = [m](void* self, TaipanArgs args) {
            return taipan_detail::invoke<C, R(C::*)(A...), R, A...>(
                static_cast<C*>(self), m, args, std::index_sequence_for<A...> {});
        };
        addMethod(name, fn, taipan_detail::signature<A...>(taipan_detail::returnName<R>()));
        return *this;
    }

    template <class C, class R, class... A> TaipanClassBuilder& method(const char* name, R(C::*m)(A...) const) {
        bind<C>();
        TaipanMethodFn fn = [m](void* self, TaipanArgs args) {
            return taipan_detail::invoke<C, R(C::*)(A...) const, R, A...>(
                static_cast<C*>(self), m, args, std::index_sequence_for<A...> {});
        };
        addMethod(name, fn, taipan_detail::signature<A...>(taipan_detail::returnName<R>()));
        return *this;
    }

    // Untyped escape hatches: raw callbacks with an optional signature
    // string such as "(int, str) -> bool" for compile-time checking.
    TaipanClassBuilder& method(const char* name, TaipanMethodFn fn, const char* signature = nullptr);
    TaipanClassBuilder& property(const char* name, TaipanGetterFn getter, TaipanSetterFn setter = nullptr,
                                 const char* type = "any");

    // Extra type names the script may use for this class (e.g. "entity").
    TaipanClassBuilder& alias(const char* typeName);

    // ----- value types (taipan_register_value) and static members -----
    // Constructor callable from scripts as `Vector3(1, 2, 3)`:
    //     .constructor<Vector3(float, float, float)>()
    template <class Sig> TaipanClassBuilder& constructor() {
        using C = taipan_detail::CtorCall<Sig>;
        addConstructor(C::make(), C::sig());
        return *this;
    }

    // Overloaded operator from any callable. Two parameters make a binary
    // operator (either side may be this type: `float * Vector3` works too);
    // one parameter makes unary minus. Ops: + - * / % == < <= > >=
    //     .op("+", [](const Vector3& a, const Vector3& b) { return a + b; })
    template <class F> TaipanClassBuilder& op(const char* symbol, F f) {
        return opImpl(symbol, f, static_cast<typename taipan_detail::FnTraits<F>::Sig*>(nullptr));
    }

    // Static function: `Vector3.Distance(a, b)` in scripts.
    template <class F> TaipanClassBuilder& staticMethod(const char* name, F f) {
        using Call = taipan_detail::FreeCall<typename taipan_detail::FnTraits<F>::Sig>;
        addStatic(name, Call::make(f), Call::sig());
        return *this;
    }

    // Static constant: `Vector3.zero` in scripts.
    template <class V> TaipanClassBuilder& constant(const char* name, const V& value) {
        using D = typename std::decay<V>::type;
        addConstant(name, taipan_detail::Conv<D>::to(value), taipan_detail::Conv<D>::name());
        return *this;
    }

    // How print / f-strings show a value: .toString([](const Vector3& v) { ... })
    template <class F> TaipanClassBuilder& toString(F f) {
        return toStringImpl(f, static_cast<typename taipan_detail::FnTraits<F>::Sig*>(nullptr));
    }

private:
    template <class F, class R, class A0> TaipanClassBuilder& opImpl(const char* symbol, F f, R (*)(A0)) {
        using S = typename std::decay<A0>::type;
        using Rd = typename std::decay<R>::type;
        TaipanMethodFn fn = [f](void* self, TaipanArgs) {
            return taipan_detail::Conv<Rd>::to(f(*static_cast<const S*>(self)));
        };
        addOperator(std::string(symbol) == "-" ? "neg" : symbol, false, "", taipan_detail::Conv<Rd>::name(), fn);
        return *this;
    }
    template <class F, class R, class A0, class A1> TaipanClassBuilder& opImpl(const char* symbol, F f, R (*)(A0, A1)) {
        using L = typename std::decay<A0>::type;
        using Rt = typename std::decay<A1>::type;
        using Rd = typename std::decay<R>::type;
        TaipanMethodFn fn;
        if (taipan_detail::Conv<L>::name() == name_) {
            fn = [f](void* self, TaipanArgs a) {
                return taipan_detail::Conv<Rd>::to(f(*static_cast<const L*>(self), taipan_detail::Conv<Rt>::fromArgs(a, 0)));
            };
            addOperator(symbol, false, taipan_detail::Conv<Rt>::name(), taipan_detail::Conv<Rd>::name(), fn);
        } else {
            fn = [f](void* self, TaipanArgs a) {
                return taipan_detail::Conv<Rd>::to(f(taipan_detail::Conv<L>::fromArgs(a, 0), *static_cast<const Rt*>(self)));
            };
            addOperator(symbol, true, taipan_detail::Conv<L>::name(), taipan_detail::Conv<Rd>::name(), fn);
        }
        return *this;
    }
    template <class F, class R, class A0> TaipanClassBuilder& toStringImpl(F f, R (*)(A0)) {
        using S = typename std::decay<A0>::type;
        setToString([f](const void* p) { return std::string(f(*static_cast<const S*>(p))); });
        return *this;
    }
    void addOperator(const std::string& symbol, bool reversed, const std::string& other, const std::string& result,
                     TaipanMethodFn fn);
    void addConstructor(TaipanNativeFn fn, const std::string& signature);
    void addStatic(const char* name, TaipanNativeFn fn, const std::string& signature);
    void addConstant(const char* name, const TaipanValue& value, const std::string& type);
    void setToString(std::function<std::string(const void*)> fn);

    template <class C> void bind() { taipan_detail::ClassName<C>::get() = name_; }
    void addProperty(const char* name, TaipanGetterFn getter, TaipanSetterFn setter, const std::string& type,
                     bool writable);
    void addMethod(const char* name, TaipanMethodFn fn, const std::string& signature);

    TaipanState* state_;
    void* cls_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// State lifecycle
// ---------------------------------------------------------------------------
TaipanState* taipan_create();                 // VM with the core standard library
void taipan_destroy(TaipanState* state);
void taipan_open_standalone(TaipanState* state); // input, time.sleep, file.* (blocking / unrestricted I/O)

// ---------------------------------------------------------------------------
// Loading and running scripts. All return false on error; the formatted
// diagnostics go to the error handler and taipan_last_error().
// ---------------------------------------------------------------------------
bool taipan_load_file(TaipanState* state, const char* path);
bool taipan_load_string(TaipanState* state, const char* source, const char* chunkName = "<string>");
bool taipan_run(TaipanState* state);                     // run the most recently loaded script
bool taipan_check_file(TaipanState* state, const char* path); // type-check only, no execution
bool taipan_reload_file(TaipanState* state, const char* path); // hot reload, keeps globals/objects

TaipanValue taipan_call_function(TaipanState* state, const char* name,
                                 const std::vector<TaipanValue>& args = {});
bool taipan_has_function(TaipanState* state, const char* name);
bool taipan_call_ok(TaipanState* state);                  // did the last call finish without error?

// ---------------------------------------------------------------------------
// Script instances: one environment per script, like a Lua environment per
// entity. Every instance of a file has its own globals and runs its own
// top-level code; the compiled bytecode is shared. Hot reload updates all
// live instances of the file and keeps their state.
// ---------------------------------------------------------------------------
struct TaipanScript;
TaipanScript* taipan_script_new(TaipanState* state, const char* path); // null on error
bool taipan_script_run(TaipanState* state, TaipanScript* script);      // run its top-level code
TaipanValue taipan_script_call(TaipanState* state, TaipanScript* script, const char* function,
                               const std::vector<TaipanValue>& args = {});
bool taipan_script_has_function(TaipanState* state, TaipanScript* script, const char* function);
TaipanValue taipan_script_get(TaipanState* state, TaipanScript* script, const char* global);
void taipan_script_set(TaipanState* state, TaipanScript* script, const char* global, const TaipanValue& value);
void taipan_script_free(TaipanState* state, TaipanScript* script);    // its environment can be collected

// Coroutines: advance script time by dt seconds and resume due coroutines.
void taipan_update(TaipanState* state, double dt);
int taipan_coroutine_count(TaipanState* state);
double taipan_next_wake(TaipanState* state);              // seconds until the next resume, -1 if none

// ---------------------------------------------------------------------------
// Native bindings. Register before loading the scripts that use them.
// "engine.spawn" creates the namespace "engine" with member "spawn".
// The optional signature ("(str, int, int) -> Entity") enables static checks.
// ---------------------------------------------------------------------------
void taipan_register_function(TaipanState* state, const char* name, TaipanNativeFn fn,
                              const char* signature = nullptr);
TaipanClassBuilder taipan_register_class(TaipanState* state, const char* name);

// Typed form: arguments, return value and the signature come from the C++
// callable.  taipan_bind_function(state, "Input.GetAxis2", &getAxis2);
template <class F> void taipan_bind_function(TaipanState* state, const char* name, F f) {
    using Call = taipan_detail::FreeCall<typename taipan_detail::FnTraits<F>::Sig>;
    taipan_register_function(state, name, Call::make(f), Call::sig().c_str());
}

// Value types: small C++ types (Vector3, Color, Quaternion) that scripts own
// as immutable values - copied in and out of C++, garbage-collected, with
// operators, constructors and static members:
//
//     taipan_register_value<Vector3>(state, "Vector3")
//         .constructor<Vector3(float, float, float)>()
//         .property("x", &Vector3::x)
//         .op("+", [](const Vector3& a, const Vector3& b) { return a + b; })
//         .op("*", [](const Vector3& a, float k) { return a * k; })
//         .staticMethod("Distance", &Vector3::Distance)
//         .constant("zero", Vector3 {0, 0, 0});
TaipanClassBuilder taipan_register_value_type(TaipanState* state, const char* name, std::size_t size,
                                              std::size_t align, void (*copy)(void*, const void*),
                                              void (*destroy)(void*));
template <class T> TaipanClassBuilder taipan_register_value(TaipanState* state, const char* name) {
    static_assert(std::is_copy_constructible<T>::value, "value types must be copyable");
    taipan_detail::ClassName<T>::get() = name;
    return taipan_register_value_type(
        state, name, sizeof(T), alignof(T), [](void* dst, const void* src) { new (dst) T(*static_cast<const T*>(src)); },
        [](void* p) { static_cast<T*>(p)->~T(); });
}
void taipan_invalidate_object(TaipanState* state, void* ptr); // native object died: handles go stale

// Binding stub files (.tpi) declare a native API without registering it, so
// scripts can be type-checked outside the engine (CI, the language server).
// taipan_dump_bindings writes the API this state has registered as a .tpi.
bool taipan_load_bindings(TaipanState* state, const char* path);
bool taipan_dump_bindings(TaipanState* state, const char* path);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
TaipanValue taipan_get_global(TaipanState* state, const char* name);
void taipan_set_global(TaipanState* state, const char* name, const TaipanValue& value);
void taipan_set_global_int(TaipanState* state, const char* name, int64_t value);
void taipan_set_global_float(TaipanState* state, const char* name, double value);
void taipan_set_global_bool(TaipanState* state, const char* name, bool value);
void taipan_set_global_string(TaipanState* state, const char* name, const char* value);

// Script containers created from the host.
TaipanValue taipan_new_list(TaipanState* state, const std::vector<TaipanValue>& items = {});
TaipanValue taipan_new_dict(TaipanState* state);
void taipan_list_add(TaipanState* state, const TaipanValue& list, const TaipanValue& item);
void taipan_dict_set(TaipanState* state, const TaipanValue& dict, const std::string& key, const TaipanValue& v);

// Keep a reference value alive across frames (until unpinned).
void taipan_pin(TaipanState* state, const TaipanValue& value);
void taipan_unpin(TaipanState* state, const TaipanValue& value);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
const std::string& taipan_last_error(TaipanState* state);
void taipan_set_error_handler(TaipanState* state, std::function<void(const std::string&)> handler);
void taipan_set_print_handler(TaipanState* state, std::function<void(const std::string&)> handler);
void taipan_set_script_root(TaipanState* state, const char* directory); // where imports resolve
void taipan_set_bytecode_cache(TaipanState* state, bool enabled, const char* directory = nullptr);
void taipan_set_warnings(TaipanState* state, bool enabled);
void taipan_set_dump_bytecode(TaipanState* state, bool enabled);

// ---------------------------------------------------------------------------
// Garbage collector (incremental + generational)
// ---------------------------------------------------------------------------
void taipan_gc_collect(TaipanState* state);          // full collection now
void taipan_set_gc_stress(TaipanState* state, bool enabled); // collect at every safepoint (debugging)
void taipan_set_gc_tiny(TaipanState* state, bool enabled);   // tiny thresholds: exercise incremental GC
std::size_t taipan_gc_object_count(TaipanState* state);
std::string taipan_gc_stats(TaipanState* state);             // human-readable collector statistics

#endif // TAIPAN_H
