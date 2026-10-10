// internal.h - shared internals of the Taipan VM: values, heap objects,
// bytecode prototypes, opcodes, diagnostics and the VM state itself.
#pragma once

#include "taipan.h"

#include <cmath>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tp {

struct Obj;
struct ObjString;
struct ObjThread;
struct ObjModule;
struct ObjClosure;
struct ObjNamespace;
struct NativeClass;
struct VM;
struct Loader;

// ---------------------------------------------------------------------------
// Values. Primitives live inline (no heap, no GC); everything else is an Obj.
// ---------------------------------------------------------------------------
enum class VT : uint8_t { Null, Bool, Int, Float, Obj };

struct Value {
    VT t;
    union {
        bool b;
        int64_t i;
        double f;
        Obj* o;
    };
    Value() : t(VT::Null), i(0) {}
    // Copy the tag and the payload separately. A single 16-byte vector copy
    // right after the two smaller stores that wrote a value cannot be
    // store-forwarded by the CPU and stalls the interpreter loop.
    Value(const Value& o) : t(o.t), i(o.i) {}
    Value& operator=(const Value& o) {
        t = o.t;
        i = o.i;
        return *this;
    }
    static Value boolean(bool v) { Value r; r.t = VT::Bool; r.b = v; return r; }
    static Value integer(int64_t v) { Value r; r.t = VT::Int; r.i = v; return r; }
    static Value number(double v) { Value r; r.t = VT::Float; r.f = v; return r; }
    static Value object(Obj* v) { Value r; r.t = VT::Obj; r.o = v; return r; }
    bool isNull() const { return t == VT::Null; }
    bool isBool() const { return t == VT::Bool; }
    bool isInt() const { return t == VT::Int; }
    bool isFloat() const { return t == VT::Float; }
    bool isNum() const { return t == VT::Int || t == VT::Float; }
    bool isObj() const { return t == VT::Obj; }
    double num() const { return t == VT::Int ? (double)i : f; }
};

enum class OT : uint8_t {
    String, List, Tuple, Dict, Closure, Upvalue, Native, Class, Instance, Bound,
    Module, Namespace, Enum, EnumValue, Handle, Thread, Value
};

struct Obj {
    OT type;
    uint8_t mark = 0;        // == VM::curMark when reached in the current major cycle
    bool old = false;        // generational: survived a collection
    bool minorMark = false;  // reached in the current minor collection
    uint8_t age = 0;         // minor collections survived while young
    bool remembered = false; // old object holding young references (remembered set)
    uint32_t gcSize = 0;     // bytes accounted at allocation
    Obj* next = nullptr;
    explicit Obj(OT t) : type(t) {}
    virtual ~Obj() = default;
};

struct ObjString : Obj {
    std::string s;
    uint64_t hash = 0;
    ObjString() : Obj(OT::String) {}
};

struct ObjList : Obj { // also used for tuples (type == OT::Tuple)
    std::vector<Value> items;
    explicit ObjList(OT t = OT::List) : Obj(t) {}
};

struct ValueHash { size_t operator()(const Value& v) const; };
struct ValueEq { bool operator()(const Value& a, const Value& b) const; };

struct ObjDict : Obj {
    std::vector<std::pair<Value, Value>> entries; // insertion ordered
    std::unordered_map<Value, size_t, ValueHash, ValueEq> index;
    ObjDict() : Obj(OT::Dict) {}
    Value* find(const Value& k);
    void put(const Value& k, const Value& v); // caller runs the write barrier
    bool remove(const Value& k);
    void clear() { entries.clear(); index.clear(); }
};

struct ObjUpvalue : Obj {
    ObjThread* th = nullptr; // while open: lives in th->stack[slot]
    int slot = 0;
    Value closed;
    bool open = true;
    ObjUpvalue* nextOpen = nullptr;
    ObjUpvalue() : Obj(OT::Upvalue) {}
    Value& ref();
};

struct Proto;

struct ObjClosure : Obj {
    Proto* proto = nullptr;
    std::vector<ObjUpvalue*> upvals;
    ObjModule* module = nullptr;
    ObjClosure() : Obj(OT::Closure) {}
};

using NativeImpl = std::function<Value(VM&, Value* argv, int argc)>;

struct ObjNative : Obj {
    ObjString* name = nullptr;
    NativeImpl fn;
    ObjNative() : Obj(OT::Native) {}
};

enum ZeroKind : uint8_t { ZNull, ZInt, ZFloat, ZBool, ZStr, ZList, ZDict };

struct FieldDesc {
    ObjString* name;
    uint8_t zero;
};

struct ObjClass : Obj {
    ObjString* name = nullptr;
    ObjString* qual = nullptr; // module-qualified, used by runtime type checks
    std::vector<FieldDesc> fields;
    std::unordered_map<ObjString*, int> fieldIndex;
    std::unordered_map<ObjString*, Value> methods;
    Value init;                // constructor closure (field initializers + init body)
    std::vector<ObjString*> interfaces;
    uint64_t uid = 0;          // unique per class version (renewed by hot reload); inline-cache key
    std::vector<Value> statics;                       // static fields
    std::unordered_map<ObjString*, int> staticIndex;
    std::unordered_map<ObjString*, Value> staticMethods;
    ObjClass() : Obj(OT::Class) {}
};

struct ObjInstance : Obj {
    ObjClass* cls = nullptr;
    std::vector<Value> fields;
    ObjInstance() : Obj(OT::Instance) {}
};

struct ObjBound : Obj {
    Value recv;
    Value method;
    ObjBound() : Obj(OT::Bound) {}
};

struct ObjModule : Obj {
    ObjString* name = nullptr;
    std::string path;
    std::vector<Value> globals;
    std::vector<uint8_t> defined;
    std::unordered_map<ObjString*, int> names;
    Value mainFn;
    bool executed = false;
    bool reloading = false;
    int fileId = -1;
    ObjModule() : Obj(OT::Module) {}
};

struct ObjNamespace : Obj {
    ObjString* name = nullptr;
    std::unordered_map<ObjString*, Value> members;
    Value call;                // `Vector3(1, 2, 3)`: the constructor of the native type named like this
    ObjNamespace() : Obj(OT::Namespace) {}
};

struct ObjEnumValue;

struct ObjEnum : Obj {
    ObjString* name = nullptr;
    ObjString* qual = nullptr;
    std::vector<ObjEnumValue*> values;
    ObjEnum() : Obj(OT::Enum) {}
};

struct ObjEnumValue : Obj {
    ObjEnum* en = nullptr;
    int index = 0;
    ObjString* name = nullptr;
    ObjEnumValue() : Obj(OT::EnumValue) {}
};

// Handle to an engine-owned native object. The engine owns the memory;
// the script only holds this handle, which goes stale on invalidation.
struct ObjHandle : Obj {
    NativeClass* cls = nullptr;
    void* ptr = nullptr;
    bool alive = true;
    ObjHandle() : Obj(OT::Handle) {}
};

// A thread's value stack. Raw storage so the interpreter can keep a plain
// pointer to the top; `top` is synchronized whenever control leaves the
// interpreter loop. Upvalues refer to slots by index, so growth is safe.
struct ValueStack {
    Value* data = nullptr;
    size_t cap = 0;
    size_t top = 0;

    ValueStack() = default;
    ValueStack(const ValueStack&) = delete;
    ValueStack& operator=(const ValueStack&) = delete;
    ~ValueStack() { std::free(data); }

    void grow(size_t need) {
        size_t n = cap ? cap * 2 : 64;
        while (n < need) n *= 2;
        Value* d = static_cast<Value*>(std::malloc(n * sizeof(Value)));
        if (!d) throw std::bad_alloc();
        for (size_t i = 0; i < top; i++) new (&d[i]) Value(data[i]);
        std::free(data);
        data = d;
        cap = n;
    }
    void reserve(size_t n) { if (n > cap) grow(n); }
    size_t size() const { return top; }
    bool empty() const { return top == 0; }
    void clear() { top = 0; }
    Value& operator[](size_t i) { return data[i]; }
    const Value& operator[](size_t i) const { return data[i]; }
    Value& back() { return data[top - 1]; }
    Value* begin() { return data; }
    Value* end() { return data + top; }
    const Value* begin() const { return data; }
    const Value* end() const { return data + top; }
    void push_back(const Value& v) {
        if (top == cap) {
            Value copy = v; // v may live in the old buffer
            grow(top + 1);
            data[top++] = copy;
            return;
        }
        data[top++] = v;
    }
    void pop_back() { --top; }
    void resize(size_t n) {
        if (n > cap) grow(n);
        for (size_t i = top; i < n; i++) data[i] = Value();
        top = n;
    }
    void assign(const Value* first, const Value* last) {
        size_t n = (size_t)(last - first);
        reserve(n);
        for (size_t i = 0; i < n; i++) data[i] = first[i];
        top = n;
    }
};

// A native value type instance (e.g. a C++ Vector3) owned by the script:
// garbage-collected, immutable from scripts, copied in and out of C++.
struct ObjValue : Obj {
    NativeClass* cls = nullptr;
    void* data = nullptr;
    ObjValue() : Obj(OT::Value) {}
    ~ObjValue() override;
};

struct CallFrame {
    ObjClosure* closure;
    int ip;
    int base;    // stack index of slot 0
    int argc;    // arguments actually supplied (for default parameters)
    int lastOp;  // ip of the instruction being executed (error locations)
    bool isCtor;
};

struct Handler {
    int frameIndex;
    int stackSize;
    int catchIp;
};

enum class ThreadState : uint8_t { Fresh, Running, Suspended, Dead };

// A coroutine (or the main thread): its own value stack and call frames.
struct ObjThread : Obj {
    ValueStack stack;
    std::vector<CallFrame> frames;
    std::vector<Handler> handlers;
    ObjUpvalue* openUpvals = nullptr;
    ThreadState state = ThreadState::Fresh;
    double wakeAt = 0;
    int nativeDepth = 0;   // > 0 while a native function re-entered the VM
    bool isCoroutine = false;
    bool stopRequested = false;
    ObjThread() : Obj(OT::Thread) {}
};

// ---------------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------------
struct Span {
    int start = 0;
    int end = 0;
};

#define TAIPAN_OPCODES(X) \
    X(CONST, 1) X(NIL, 0) X(TRUE, 0) X(FALSE, 0) X(POP, 0) X(POPN, 1) X(DUP, 0) X(DUP2, 0) \
    X(GET_LOCAL, 1) X(SET_LOCAL, 1) X(GET_UPVAL, 1) X(SET_UPVAL, 1) \
    X(GET_GLOBAL, 1) X(SET_GLOBAL, 1) X(GET_BUILTIN, 1) \
    X(GET_FIELD, 1) X(SET_FIELD, 1) X(GET_INDEX, 0) X(SET_INDEX, 0) \
    X(ADD, 0) X(SUB, 0) X(MUL, 0) X(DIV, 0) X(MOD, 0) X(NEG, 0) X(NOT, 0) \
    X(BAND, 0) X(BOR, 0) X(BXOR, 0) X(SHL, 0) X(SHR, 0) X(BNOT, 0) \
    X(EQ, 0) X(NE, 0) X(LT, 0) X(LE, 0) X(GT, 0) X(GE, 0) \
    X(JUMP, 1) X(JUMP_IF_FALSE, 1) X(JUMP_IF_TRUE, 1) X(AND_JUMP, 1) X(OR_JUMP, 1) \
    X(COALESCE_JUMP, 1) X(LOOP, 1) \
    X(CALL, 1) X(INVOKE, 2) X(CLOSURE, -1) X(CLOSE_UPVALS, 1) X(RETURN, 0) \
    X(BUILD_LIST, 1) X(BUILD_DICT, 1) X(TUPLE, 1) X(UNPACK, 1) X(FSTRING, 1) \
    X(ITER_NEXT, 3) X(RANGE_NEXT, 2) X(RANGE_STEP, 1) \
    X(TRY, 1) X(END_TRY, 0) X(THROW, 0) X(CHECK_TYPE, 1) X(CONVERT, 1) \
    X(CLASS, 2) X(FIELD, 2) X(METHOD, 1) X(IMPLEMENTS, 1) X(DEF_CLASS, 1) X(ENUM, -1) \
    X(START, 1) X(WAIT, 0) X(IMPORT, 1) X(ARG_GIVEN, 2) X(JUMP_IF_DEFINED, 2) X(JUMP_IF_RELOADING, 1) \
    /* fused / specialized instructions produced by the compiler's peephole pass */ \
    X(ADD_K, 1) X(SUB_K, 1) X(MUL_K, 1) X(DIV_K, 1) X(MOD_K, 1) \
    X(LT_K, 1) X(LE_K, 1) X(GT_K, 1) X(GE_K, 1) X(EQ_K, 1) X(NE_K, 1) \
    X(JLT, 1) X(JLE, 1) X(JGT, 1) X(JGE, 1) X(JEQ, 1) X(JNE, 1) \
    X(JLT_K, 2) X(JLE_K, 2) X(JGT_K, 2) X(JGE_K, 2) X(JEQ_K, 2) X(JNE_K, 2) \
    X(ADD_LOCAL, 1) X(SUB_LOCAL, 1) X(ADD_LOCAL_K, 2) X(SUB_LOCAL_K, 2) \
    X(GET_FIELD_I, 2) X(SET_FIELD_I, 2) X(GET_LFIELD, 3) \
    X(INVOKE_C, 3) X(GET_BUILTIN_C, 2) X(RANGE_INIT, 3) X(RANGE_LOOP, 3) \
    X(STATIC_FIELD, 2) X(STATIC_METHOD, 1)

enum Op : int32_t {
#define X(name, n) OP_##name,
    TAIPAN_OPCODES(X)
#undef X
    OP__COUNT
};

const char* opName(int op);
int opOperands(const int32_t* code, int ip); // operand count of the instruction at ip
struct Proto;
int computeMaxStack(const Proto* p);           // deepest value-stack use of one call frame

// CONVERT codes
enum ConvCode : int32_t {
    CV_TO_FLOAT = 1, CV_TO_INT = 2,
    CV_I8 = 10, CV_I16, CV_I32, CV_I64, CV_U8, CV_U16, CV_U32, CV_U64, CV_F32
};

struct Proto {
    std::string name;
    int fileId = -1;
    std::vector<int32_t> code;
    std::vector<Span> spans;   // one per code word
    std::vector<Value> consts;
    std::vector<Proto*> protos;
    int numParams = 0;         // includes the variadic parameter
    int minParams = 0;
    bool variadic = false;
    bool isCoroutine = false;
    bool isMethod = false;
    int numUpvals = 0;
    int maxStack = 0;          // stack slots one frame needs (callee, params, locals, temporaries)
    struct InlineCache {       // per call site: last receiver class / namespace and what it resolved to
        uint64_t key = 0;
        uint64_t epoch = 0;
        Value value;
    };
    std::vector<InlineCache> caches;
};

// ---------------------------------------------------------------------------
// Source files and Rust-style diagnostics
// ---------------------------------------------------------------------------
struct SourceFile {
    std::string path;    // as given / resolved on disk
    std::string display; // shown in diagnostics
    std::string text;
    std::vector<int> lineStarts;
    void computeLines();
    void lineCol(int offset, int& line, int& col) const; // 1-based
    std::string lineText(int line) const;
};

struct Diagnostic {
    enum Severity { Error, Warning } sev = Error;
    std::string title;
    int fileId = -1;
    Span span;
    std::string label;
    std::vector<std::string> notes;
};

std::string formatDiagnostic(const SourceFile* file, const Diagnostic& d);

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
struct ScriptError {
    Value payload;         // what `catch error` receives
    std::string title;
    std::string label;
    int fileId = -1;
    Span span;
    std::vector<std::pair<Proto*, int>> frames; // (proto, ip), innermost first; formatted only if reported
    bool located = false;
};

struct HostError { // thrown by TaipanArgs::error / taipan_detail::fail
    std::string message;
};

// ---------------------------------------------------------------------------
// Native classes registered by the host
// ---------------------------------------------------------------------------
struct NativeProp {
    TaipanGetterFn get;
    TaipanSetterFn set;
    std::string type;
    bool writable = true;
};

struct NativeMethod {
    TaipanMethodFn fn;
    std::string sig;
};

// An overloaded operator on a native class. `fn(self, args)` receives the
// other operand as args[0]; `reversed` entries handle `other OP self`.
struct NativeOp {
    std::string op;          // "+", "-", "*", "/", "%", "==", "<", "<=", ">", ">=", "neg"
    bool reversed = false;
    std::string other;       // type name of the other operand ("" for unary)
    std::string result;      // result type name
    TaipanMethodFn fn;
};

struct NativeClass {
    std::string name;
    std::vector<std::string> aliases;
    std::unordered_map<std::string, NativeProp> props;
    std::unordered_map<std::string, NativeMethod> methods;
    // value types (taipan_register_value): stored inside the script object
    bool isValue = false;
    size_t size = 0;
    size_t align = 0;
    void (*copy)(void* dst, const void* src) = nullptr; // placement copy-construct
    void (*destroy)(void* p) = nullptr;
    std::vector<NativeOp> ops;
    std::string ctorSig;     // non-empty when the type has a constructor
    std::function<std::string(const void*)> toString;
};

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ULL);

// ---------------------------------------------------------------------------
// The VM
// ---------------------------------------------------------------------------
enum class GcPhase : uint8_t { Idle, Mark, Sweep };

struct VM {
    VM();
    ~VM();

    TaipanState* state = nullptr;

    // ----- garbage collector (gc.cpp) -----
    Obj* youngList = nullptr;
    Obj* oldList = nullptr;
    size_t youngBytes = 0;
    size_t oldBytes = 0;
    size_t objectCount = 0;
    size_t minorLimit = 2u << 20;
    size_t majorLimit = 8u << 20;
    size_t debt = 0;            // bytes allocated since the last incremental step
    GcPhase phase = GcPhase::Idle;
    uint8_t curMark = 1;
    std::vector<Obj*> gray;
    std::vector<Obj*> remembered;
    Obj** sweepLink = nullptr;  // incremental sweep cursor
    int sweepList = 0;          // 0 = old list, 1 = young list, 2 = done
    int gcPause = 0;            // > 0: no collection (compiling, etc.)
    bool gcStress = false;
    size_t stepBytes = 64u << 10; // allocation between incremental steps
    size_t stepBudget = 2000;     // objects processed per incremental step
    unsigned stressCounter = 0;
    size_t minorCount = 0, majorCount = 0, freedCount = 0;
    size_t minMajorLimit = 8u << 20;
    uint64_t nextClassUid = 1;  // ObjClass::uid source
    uint64_t nsEpoch = 1;       // bumped whenever a namespace or builtin changes (invalidates caches)
    void setTinyGc() { minorLimit = 4096; majorLimit = minMajorLimit = 16384; stepBytes = 512; stepBudget = 8; }
    bool gcRequested = false;
    std::vector<Value> tempRoots; // values held by C++ code during native calls
    std::vector<Value> pins;

    void link(Obj* o, size_t bytes);
    template <class T> T* alloc(size_t extra = 0) {
        T* o = new T();
        link(o, sizeof(T) + extra);
        return o;
    }
    void safepoint() {
        if ((gcRequested || gcStress) && gcPause == 0) collectStep();
    }
    void collectStep();
    void fullCollect();
    void minorCollect();
    void barrier(Obj* container, const Value& v) {
        if (v.t != VT::Obj) return;
        Obj* o = v.o;
        if (container->old && !o->old && !container->remembered) {
            container->remembered = true;
            remembered.push_back(container);
        }
        if (phase == GcPhase::Mark && container->mark == curMark && o->mark != curMark) markObj(o);
    }
    // Bulk mutation of `o` (several stores at once): re-gray / remember it.
    void touch(Obj* o) {
        if (phase == GcPhase::Mark && o->mark == curMark) gray.push_back(o);
        if (o->old && !o->remembered) {
            o->remembered = true;
            remembered.push_back(o);
        }
    }
    void markObj(Obj* o);
    void markValue(const Value& v) { if (v.t == VT::Obj) markObj(v.o); }
    void markRoots();
    void traverse(Obj* o);
    void minorVisit(const Value& v);
    void minorVisitObj(Obj* o);
    void minorTraverse(Obj* o);
    void freeObject(Obj* o);
    bool sweepSome(size_t budget);
    void finishMajor();
    void atomicPhase();

    // ----- strings (object.cpp) -----
    std::unordered_map<std::string_view, ObjString*> strings;
    ObjString* str(std::string_view s);
    Value strv(std::string_view s) { return Value::object(str(s)); }

    // ----- object helpers (object.cpp) -----
    ObjList* newList(OT t = OT::List);
    ObjDict* newDict();
    ObjThread* newThread();
    ObjNamespace* newNamespace(const std::string& name);
    ObjNative* newNative(const std::string& name, NativeImpl fn);
    std::string toStr(const Value& v, int depth = 0);
    std::string typeName(const Value& v);
    bool equals(const Value& a, const Value& b) const;
    bool truthy(const Value& v) const { return !(v.t == VT::Null || (v.t == VT::Bool && !v.b)); }
    std::string reprString(const std::string& s);

    // ----- runtime (vm.cpp) -----
    ObjNamespace* builtins = nullptr;
    ObjThread* mainThread = nullptr;
    ObjThread* current = nullptr;
    std::vector<ObjThread*> coroutines;
    std::vector<std::unique_ptr<Proto>> protos;
    std::vector<SourceFile> sources;
    std::unordered_map<std::string, ObjModule*> modules;   // by module key
    std::vector<ObjModule*> instances;                      // script instances (taipan_script_new)
    std::unordered_map<std::string, std::unique_ptr<NativeClass>> nativeClasses;
    std::unordered_map<std::string, NativeClass*> nativeTypeNames; // name + aliases
    std::unordered_map<void*, ObjHandle*> handles;
    std::unordered_map<std::string, std::string> nativeSigs; // "math.sqrt" -> "(float) -> float"
    std::unordered_set<std::string> hostNatives;             // registered by the host (for dumping bindings)
    std::unordered_set<std::string> hostConstants;           // static constants of native classes
    bool loadBindings(const std::string& path, std::vector<Diagnostic>& diags); // bindings.cpp
    std::string dumpBindings();
    std::unordered_map<ObjString*, int> checkCache;
    double time = 0;
    int nativeCallDepth = 0;
    int hostDepth = 0;          // > 0 while a host (C++) native function runs

    // settings
    std::string scriptRoot;
    bool cacheEnabled = true;
    std::string cacheDir;
    bool warnings = true;
    bool dumpBytecode = false;
    std::function<void(const std::string&)> errorHandler;
    std::function<void(const std::string&)> printHandler;
    std::string lastError;
    bool lastCallOk = true;
    std::unique_ptr<Loader> loader;
    ObjModule* mainModule = nullptr;

    // pre-interned names used by the interpreter
    struct {
        ObjString *length, *init, *empty;
    } names {};
    std::unordered_map<ObjString*, int> listMethods, dictMethods, threadMethods;

    enum class RunResult { Done, Yielded };
    Proto* newProto();
    int addSource(const std::string& path, const std::string& display, std::string text);
    ObjModule* newModule(const std::string& key, const std::string& path, int fileId);

    RunResult run(ObjThread* th, size_t baseFrames);
    RunResult execute(ObjThread* th, size_t baseFrames);
    bool unwindTo(ObjThread* th, size_t baseFrames, const ScriptError& e);
    void pushFrame(ObjThread* th, ObjClosure* cl, int calleeIdx, int argc, bool isCtor);
    void invoke(ObjThread* th, ObjString* name, int argc);
    void defineGlobal(ObjModule* m, int slot, const Value& v);
    void finishThread(ObjThread* th);
    void callSetup(ObjThread* th, int argc); // callee + args on top of th's stack
    Value callValue(const Value& fn, const std::vector<Value>& args); // re-entrant call
    bool runModuleMain(ObjModule* m, bool reloading);
    ObjThread* startCoroutine(ObjThread* from, int argc);
    void resume(ObjThread* th);
    void update(double dt);
    void reportUncaught(const ScriptError& e);
    void closeUpvalues(ObjThread* th, int fromSlot);
    ObjUpvalue* captureUpvalue(ObjThread* th, int slot);
    [[noreturn]] void rtError(const std::string& title, const std::string& label);
    [[noreturn]] void throwValue(const Value& payload, const std::string& title, const std::string& label);
    void locate(ScriptError& e);
    std::string traceback(const ScriptError& e);
    Value getField(const Value& obj, ObjString* name);
    void setField(const Value& obj, ObjString* name, const Value& v);
    Value getIndex(const Value& obj, const Value& idx);
    void setIndex(const Value& obj, const Value& idx, const Value& v);
    bool invokeBuiltin(ObjThread* th, ObjString* name, int argc); // list/dict/str/thread methods
    Value callNativeMethod(ObjHandle* h, const std::string& name, Value* argv, int argc);
    Value callNativeMethodResolved(ObjHandle* h, NativeMethod* m, const std::string& name, Value* argv, int argc);
    Value checkType(const Value& v, ObjString* spec);
    Value convert(const Value& v, int code);
    Value importModule(const std::string& key);
    Value instanceField(ObjInstance* inst, int idx);
    Value zeroValue(uint8_t kind);
    NativeClass* nativeTypeByName(const std::string& name) const;
    Value handleFor(const std::string& className, void* ptr);
    Value newValueObj(NativeClass* cls, const void* src);   // copies a C++ value into a script object
    bool matchesType(const Value& v, const std::string& type); // runtime check for operator overloads
    // Overloaded operators: true if a script/native operator handled the operation.
    bool tryBinaryOperator(const char* op, const Value& a, const Value& b, Value& out);
    bool tryUnaryOperator(const char* op, const Value& a, Value& out);
    bool overloadedEquals(const Value& a, const Value& b, bool& result);
    Value callMethod(const Value& recv, ObjClosure* method, const std::vector<Value>& args);
    Value callNativeOp(ObjValue* self, const NativeOp& op, const Value* other, int n); // api.cpp

    // ----- diagnostics -----
    void report(const Diagnostic& d);
    void reportAll(const std::vector<Diagnostic>& ds);
    void emitError(const std::string& text);

    // ----- standard library (stdlib.cpp) -----
    void openCore();
    void openStandalone();
    void defineNative(const std::string& qualified, NativeImpl fn, const std::string& sig);
    ObjNamespace* namespaceFor(const std::string& name, bool create);
    Value builtinListMethod(ObjList* l, int id, Value* argv, int argc);
    Value builtinDictMethod(ObjDict* d, int id, Value* argv, int argc);
};

// Disassembler (module.cpp)
std::string disassemble(VM& vm, Proto* p);

// Host value conversion (api.cpp)
Value hostToValue(VM& vm, const TaipanValue& v);
TaipanValue valueToHost(VM& vm, const Value& v);
TaipanArgs makeArgs(VM& vm, const Value* argv, int argc);
Value handleGet(VM& vm, ObjHandle* h, ObjString* name);
void handleSet(VM& vm, ObjHandle* h, ObjString* name, const Value& v);
std::unique_ptr<Loader> makeLoader(VM& vm);
std::string formatFloat(double d);

} // namespace tp

struct TaipanState {
    tp::VM vm;
};
