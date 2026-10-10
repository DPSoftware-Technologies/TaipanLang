// ast.h - abstract syntax tree produced by the parser.
#pragma once

#include "lexer.h"

namespace tp {

struct Expr;
struct Stmt;
struct Block;
struct FuncDecl;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct TypeExpr {
    enum Kind { Named, List, Dict, Tuple } kind = Named;
    std::string name;  // Named: "int", "Enemy", "enemies.ai.Goblin", ...
    std::vector<std::unique_ptr<TypeExpr>> args;
    bool nullable = false;
    Span span;
};
using TypeExprP = std::unique_ptr<TypeExpr>;

struct Param {
    TypeExprP type;
    std::string name;
    ExprP def;
    bool variadic = false;
    Span span;
};

enum class EK {
    Int, Float, Str, FStr, Bool, Null, Ident, This, Member, Index, Call,
    Unary, Binary, Logical, Coalesce, List, Dict, Lambda, Start
};

struct Expr {
    EK k;
    Span span;
    int64_t ival = 0;
    double fval = 0;
    bool bval = false;
    std::string name;          // Ident / Member name / Str value
    Span nameSpan;             // Member: span of the member name
    Tok op = Tok::End;         // Unary / Binary / Logical
    ExprP a, b;                // operands, callee, object
    std::vector<ExprP> args;   // call args, list items, dict values, f-string parts
    std::vector<ExprP> keys;   // dict keys
    std::vector<std::string> strParts; // FStr: literal pieces (size = args.size() + 1)
    // Lambda
    std::vector<Param> params;
    std::vector<TypeExprP> rets;
    bool hasArrowType = false;
    std::unique_ptr<Block> body;
    ExprP exprBody;
    explicit Expr(EK kind) : k(kind) {}
};

struct Block {
    std::vector<StmtP> stmts;
    Span span;
};

struct FuncDecl {
    std::string name;
    Span nameSpan;
    std::vector<Param> params;
    std::vector<TypeExprP> rets;   // empty = returns nothing; >1 = multiple returns
    std::unique_ptr<Block> body;   // null in interfaces
    bool isCoroutine = false;
    bool isPrivate = false;
    bool isStatic = false;
    bool isOperator = false;     // `operator +(...)`; name is "op+" (unary minus: "op_neg")
    std::string opSymbol;
    Span span;
};

struct FieldDecl {
    TypeExprP type;
    std::string name;
    ExprP init;
    bool isConst = false;
    bool isPrivate = false;
    bool isStatic = false;
    Span span;
};

struct ClassDecl {
    std::string name;
    Span nameSpan;
    std::vector<std::string> implements;
    std::vector<Span> implSpans;
    std::vector<FieldDecl> fields;
    std::vector<std::unique_ptr<FuncDecl>> methods;
};

struct InterfaceDecl {
    std::string name;
    Span nameSpan;
    std::vector<std::unique_ptr<FuncDecl>> methods;
};

struct EnumDecl {
    std::string name;
    Span nameSpan;
    std::vector<std::string> members;
    std::vector<Span> memberSpans;
};

struct VarTarget {
    TypeExprP type;
    std::string name;
    Span span;
};

struct MatchArm {
    std::vector<ExprP> patterns;
    bool isElse = false;
    std::unique_ptr<Block> body;
    Span span;
};

enum class SK {
    Expr, VarDecl, Assign, IncDec, Block, If, While, ForIn, ForC, Match,
    Break, Continue, Return, Try, Throw, Func, Class, Interface, Enum, Import, Enable
};

struct Stmt {
    SK k;
    Span span;
    ExprP e;                       // expression / condition / value / iterable / subject
    ExprP target;                  // Assign / IncDec
    Tok op = Tok::End;             // Assign operator, ++ / --
    std::vector<VarTarget> vars;   // VarDecl (several = destructuring), ForIn loop variables
    bool isConst = false;
    std::unique_ptr<Block> body;
    std::unique_ptr<Block> elseBody;
    StmtP init, step;              // ForC
    std::vector<MatchArm> arms;
    std::vector<ExprP> values;     // Return
    std::string name;              // catch variable, import path, enable feature
    std::string alias;             // import alias
    Span nameSpan;
    std::unique_ptr<FuncDecl> func;
    std::unique_ptr<ClassDecl> cls;
    std::unique_ptr<InterfaceDecl> iface;
    std::unique_ptr<EnumDecl> en;
    explicit Stmt(SK kind) : k(kind) {}
};

} // namespace tp
