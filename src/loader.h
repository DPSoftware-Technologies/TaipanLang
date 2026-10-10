// loader.h - module loading: path resolution, compilation, bytecode cache,
// runtime instantiation and hot reload.
#pragma once

#include "compiler.h"

namespace tp {

struct Loader : ImportResolver {
    struct Entry {
        std::string key;   // dotted module name
        std::string path;  // file on disk ("" for strings)
        int fileId = -1;
        uint64_t hash = 0;
        Proto* main = nullptr;
        std::vector<std::string> globals; // slot names
        std::unique_ptr<ModuleSymbols> syms; // null when loaded from the bytecode cache
        std::vector<std::pair<std::string, uint64_t>> deps; // transitive imports (path, source hash)
    };

    explicit Loader(VM& vm) : vm(vm) {}

    VM& vm;
    std::unordered_map<std::string, std::unique_ptr<Entry>> entries; // by key
    std::vector<std::unique_ptr<Entry>> graveyard; // replaced entries (types may still be referenced)
    std::vector<std::string> compiling;            // import cycle detection
    std::vector<std::vector<std::string>> depStack;
    std::string root;                              // script root for imports
    // In-memory file contents that take precedence over the disk (unsaved
    // editor buffers in the language server). Keys come from overlayKey().
    std::unordered_map<std::string, std::string> overlays;
    static std::string overlayKey(const std::string& path);

    ModuleSymbols* resolveImport(const std::string& dotted, int fromFile, Span span,
                                 std::vector<Diagnostic>& diags) override;

    Entry* loadMain(const std::string& path, std::vector<Diagnostic>& diags);
    Entry* loadString(const std::string& source, const std::string& chunk, std::vector<Diagnostic>& diags);
    ObjModule* instantiate(Entry* e);
    ObjModule* instantiateKey(const std::string& key, std::vector<Diagnostic>& diags);
    bool reload(const std::string& path, std::vector<Diagnostic>& diags);
    Entry* compileSource(const std::string& path, const std::string& display, const std::string& key,
                         std::string text, std::vector<Diagnostic>& diags, const CompileOptions& opt);
    std::string keyForPath(const std::string& path);
    std::string displayName(const std::string& path);

private:
    bool readSource(const std::string& path, std::string& out);
    Entry* compileFile(const std::string& path, const std::string& key, std::vector<Diagnostic>& diags,
                       const CompileOptions& opt);
    Entry* fromCache(const std::string& path, const std::string& key, const std::string& text);
    void writeCache(Entry* e);
    std::string cachePath(const std::string& path);
    bool resolvePath(const std::string& dotted, std::string& path, std::string& key, std::string& err);
};

} // namespace tp
