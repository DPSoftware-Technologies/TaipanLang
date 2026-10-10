// analysis.h - editor analysis of one Taipan file (used by the language
// server). Positions are byte offsets into the analyzed text.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace tp {

struct AnalysisRequest {
    std::string path;          // absolute path of the file
    std::string text;          // current contents (may be unsaved)
    std::string root;          // script root for imports ("" = the file's folder)
    std::unordered_map<std::string, std::string> overlays; // other open files: path -> text
    std::vector<std::string> bindingFiles;                 // .tpi stubs of the host's native API
    bool standalone = true;    // include the standalone-only built-ins (input, file.*)
    int completionOffset = -1; // >= 0: also compute completions at this offset
};

// LSP-compatible kind numbers.
enum CompletionKind { CkMethod = 2, CkFunction = 3, CkField = 5, CkVariable = 6, CkClass = 7, CkInterface = 8,
                      CkModule = 9, CkProperty = 10, CkKeyword = 14, CkEnum = 13, CkEnumMember = 20, CkConstant = 21 };
enum SymbolKind { SkModule = 2, SkClass = 5, SkMethod = 6, SkField = 8, SkConstructor = 9, SkEnum = 10, SkInterface = 11,
                  SkFunction = 12, SkVariable = 13, SkConstant = 14, SkEnumMember = 22 };

struct AnalysisDiag {
    bool error = true;
    int start = 0, end = 0;    // in the analyzed file
    std::string message;
};

struct AnalysisUse {           // hover / go to definition
    int start = 0, end = 0;
    std::string detail;
    std::string declPath;      // empty: built-in, no source location
    int declStart = 0, declEnd = 0;
};

struct AnalysisCompletion {
    std::string label;
    int kind = CkVariable;
    std::string detail;
};

struct AnalysisSymbol {        // document outline
    std::string name, detail;
    int kind = SkVariable;
    int start = 0, end = 0, selStart = 0, selEnd = 0;
    std::vector<AnalysisSymbol> children;
};

struct AnalysisResult {
    std::vector<AnalysisDiag> diags;
    std::vector<AnalysisUse> uses;
    std::vector<AnalysisSymbol> symbols;
    std::vector<AnalysisCompletion> completions;
};

AnalysisResult analyze(const AnalysisRequest& req);

} // namespace tp
