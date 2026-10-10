// compiler.h - type checker + bytecode generator (one pass over the AST).
#pragma once

#include "types.h"

namespace tp {

// Collects editor information (hover, go-to-definition, completion) while the
// compiler checks a file. Only used by the language server; when no sink is
// given the compiler records nothing.
struct AnalysisSink {
    struct Use {             // a name in the source and what it refers to
        int fileId = -1;
        Span span;
        std::string detail;  // hover text, e.g. "func heal(int amount)"
        int declFile = -1;   // where it was declared (-1 = built-in)
        Span declSpan;
    };
    struct ScopeVar {        // a local variable / parameter and where it is visible
        std::string name;
        TypeP type;
        bool isParam = false;
        bool isConst = false;
        int fileId = -1;
        int visibleFrom = 0;
        int visibleTo = 0;
    };
    struct ClassScope {
        ClassInfo* cls;
        int fileId;
        int start, end;
    };
    int fileId = -1;                 // the file being analyzed (others are imports)
    std::vector<Use> uses;
    std::vector<ScopeVar> locals;
    std::vector<ClassScope> classes;
    std::vector<std::string> importNames;

    // Member completion: the compiler reports what is left of a `.` placed
    // right before the placeholder member name.
    std::string placeholder;         // e.g. "__tp_complete__"
    bool haveReceiver = false;
    TypeP receiverType;              // value receiver (class, list, str, ...)
    EnumInfo* receiverEnum = nullptr;
    ModuleSymbols* receiverModule = nullptr;
    std::string receiverNamespace;
    ClassInfo* receiverStatics = nullptr;        // `ClassName.` -> its static members
    ModuleSymbols* mainSyms = nullptr;                // symbols of the analyzed file
    std::vector<std::unique_ptr<ModuleSymbols>> keep; // symbols of files that failed to compile
};

struct CompileOptions {
    bool reload = false;                       // hot reload: keep existing global slots
    std::vector<std::string> existingGlobals;  // slot order of the module being reloaded
    AnalysisSink* sink = nullptr;              // language server: record symbols
};

// Implemented by the module loader: finds, parses and checks another .tp file.
struct ImportResolver {
    virtual ~ImportResolver() = default;
    // Returns null on failure (diagnostics already added).
    virtual ModuleSymbols* resolveImport(const std::string& dotted, int fromFile, Span span,
                                         std::vector<Diagnostic>& diags) = 0;
};

// Checks and compiles syms.program. Returns the module's main function, or
// null if there were errors.
Proto* compileModule(VM& vm, ModuleSymbols& syms, ImportResolver& resolver, std::vector<Diagnostic>& diags,
                     const CompileOptions& opt);

} // namespace tp
