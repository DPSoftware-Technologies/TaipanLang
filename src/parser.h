// parser.h - recursive-descent parser for Taipan.
#pragma once

#include "ast.h"

namespace tp {

std::vector<StmtP> parseProgram(const SourceFile& src, int fileId, std::vector<Diagnostic>& diags);

// Native signature strings such as "(str, int=, any...) -> list<str>".
// A trailing '=' marks an optional parameter, "..." a variadic one.
struct SigParse {
    std::vector<TypeExprP> params;
    std::vector<bool> optional;
    bool variadic = false;
    std::vector<TypeExprP> rets;  // empty = void
    bool ok = false;
};
SigParse parseSignature(const std::string& text);

// Binding stub files (.tpi) describe what a host engine registers natively,
// so tools (the checker, the language server) know it without the engine:
//
//     namespace engine {
//         func spawn(str prefab, int x = 0, int y = 0) -> Entity
//         float gravity
//     }
//     class Entity as entity {
//         int hp
//         readonly str name
//         func takeDamage(int amount)
//     }
//     value class Vector3 {
//         float x
//         func init(float x, float y, float z)        // constructor: Vector3(1, 2, 3)
//         operator +(Vector3 o) -> Vector3
//         operator *(float k, Vector3 v) -> Vector3   // two parameters: left operand first
//         operator -() -> Vector3
//         static func Distance(Vector3 a, Vector3 b) -> float
//         static Vector3 zero
//     }
struct BindingFunc {
    std::string qualified;   // "engine.spawn", or "log" at the top level
    std::vector<Param> params;
    std::vector<TypeExprP> rets;
    Span span;
};
struct BindingValue {
    std::string qualified;
    TypeExprP type;
};
struct BindingProp {
    std::string name;
    TypeExprP type;
    bool writable = true;
};
struct BindingOp {
    std::string symbol;      // "+", "==", ...
    std::vector<Param> params;
    std::vector<TypeExprP> rets;
};
struct BindingClass {
    std::string name;
    bool isValue = false;
    std::vector<std::string> aliases;
    std::vector<BindingProp> props;
    std::vector<BindingFunc> methods;  // "init" is the constructor
    std::vector<BindingOp> ops;
};
struct BindingFile {
    std::vector<std::string> namespaces;
    std::vector<BindingFunc> funcs;
    std::vector<BindingValue> values;
    std::vector<BindingClass> classes;
};
bool parseBindings(const SourceFile& src, int fileId, BindingFile& out, std::vector<Diagnostic>& diags);

// Source text of a type ("list<int>", "Entity?") and of a function signature
// in the native signature format ("(str, int=, any...) -> Entity").
std::string typeExprText(const TypeExpr* t);
std::string bindingSignature(const BindingFunc& f);

} // namespace tp
