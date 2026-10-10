// types.h - static type representation used by the checker/compiler.
#pragma once

#include "ast.h"

#include <map>

namespace tp {

struct ClassInfo;
struct InterfaceInfo;
struct EnumInfo;
struct ModuleSymbols;

enum class TK {
    Error,      // poisoned by an earlier error; compatible with everything
    Void,       // "returns nothing"
    Any, Null, Int, Float, Bool, Str,
    List, Dict, Tuple,
    Func,       // the untyped `func` type (any callable)
    Sig,        // a callable with a known signature
    Class, Interface, Enum, Native, Module, Namespace
};

struct Type;
using TypeP = std::shared_ptr<const Type>;

struct SigInfo {
    std::vector<TypeP> params;   // variadic param: element type
    std::vector<std::string> names;
    int required = 0;
    bool variadic = false;
    TypeP ret;                   // Void, a type, or a Tuple for multiple returns
    bool isCoroutine = false;
};
using SigP = std::shared_ptr<SigInfo>;

enum ExtKind { ExtNone = 0, ExtI8, ExtI16, ExtI32, ExtI64, ExtU8, ExtU16, ExtU32, ExtU64, ExtF32, ExtF64 };

struct Type {
    TK k = TK::Error;
    bool nullable = false;
    int ext = ExtNone;          // ExtType width for Int / Float
    TypeP a, b;                 // list element; dict key / value
    std::vector<TypeP> elems;   // tuple
    SigP sig;
    ClassInfo* cls = nullptr;
    InterfaceInfo* iface = nullptr;
    EnumInfo* en = nullptr;
    NativeClass* native = nullptr;
    ModuleSymbols* mod = nullptr;
    std::string nsName;         // namespace name (math, engine, ...)
};

struct ClassInfo {
    std::string name, qual;
    struct Field {
        std::string name;
        TypeP type;
        bool isConst = false;
        bool isPrivate = false;
        Span span;
        int zero = 0;
        FieldDecl* decl = nullptr;
    };
    struct Method {
        std::string name;
        SigP sig;
        bool isPrivate = false;
        FuncDecl* decl = nullptr;
    };
    std::vector<Field> fields;
    std::map<std::string, int> fieldIdx;
    std::map<std::string, Method> methods;
    std::vector<InterfaceInfo*> ifaces;
    std::vector<Field> statics;                  // static fields
    std::map<std::string, int> staticIdx;
    std::map<std::string, Method> staticMethods;
    SigP ctor;
    ClassDecl* decl = nullptr;
    ModuleSymbols* mod = nullptr;
    int fileId = -1;
};

struct InterfaceInfo {
    std::string name, qual;
    std::map<std::string, SigP> methods;
    std::map<std::string, Span> methodSpans;
    Span nameSpan;
    int fileId = -1;
};

struct EnumInfo {
    std::string name, qual;
    std::vector<std::string> members;
    std::vector<Span> memberSpans;
    Span nameSpan;
    int fileId = -1;
};

struct ModuleSymbols {
    std::string key;    // dotted module name, e.g. "enemies.ai"
    std::string path;
    int fileId = -1;
    struct Global {
        enum Kind { Var, Func, Class, Interface, Enum, Import } kind = Var;
        std::string name;
        int slot = -1;
        TypeP type;
        bool isConst = false;
        Span span;
        ClassInfo* cls = nullptr;
        InterfaceInfo* iface = nullptr;
        EnumInfo* en = nullptr;
        ModuleSymbols* mod = nullptr; // Import
    };
    std::map<std::string, Global> globals;
    std::vector<std::string> slotNames; // slot -> name
    std::vector<std::unique_ptr<ClassInfo>> classes;
    std::vector<std::unique_ptr<InterfaceInfo>> ifaces;
    std::vector<std::unique_ptr<EnumInfo>> enums;
    std::vector<StmtP> program; // AST kept alive (ClassInfo points into it)
    bool extTypes = false;
};

// Type constructors / helpers (compiler.cpp)
namespace T {
TypeP error();
TypeP voidT();
TypeP any();
TypeP null();
TypeP integer();
TypeP floating();
TypeP boolean();
TypeP str();
TypeP func();
TypeP list(TypeP elem);
TypeP dict(TypeP key, TypeP val);
TypeP tuple(std::vector<TypeP> elems);
TypeP sig(SigP s);
TypeP cls(ClassInfo* c);
TypeP iface(InterfaceInfo* i);
TypeP en(EnumInfo* e);
TypeP native(NativeClass* n);
TypeP module(ModuleSymbols* m);
TypeP ns(const std::string& name);
TypeP nullable(TypeP t, bool n = true);
TypeP withExt(TypeP t, int ext);
std::string name(const TypeP& t);
bool same(const TypeP& a, const TypeP& b);
} // namespace T

} // namespace tp
