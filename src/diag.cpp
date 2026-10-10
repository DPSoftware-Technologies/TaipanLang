// diag.cpp - Rust-style diagnostics:
//
//   error: type mismatch
//     --> game.tp:3:11
//      |
//    3 |     int x = "hello"
//      |             ^^^^^^^ expected 'int', found 'str'
#include "internal.h"

#include <algorithm>

namespace tp {

void SourceFile::computeLines() {
    lineStarts.clear();
    lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') lineStarts.push_back((int)i + 1);
    }
}

void SourceFile::lineCol(int offset, int& line, int& col) const {
    if (offset < 0) offset = 0;
    if (offset > (int)text.size()) offset = (int)text.size();
    auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    int idx = (int)(it - lineStarts.begin()) - 1;
    line = idx + 1;
    col = offset - lineStarts[idx] + 1;
}

std::string SourceFile::lineText(int line) const {
    if (line < 1 || line > (int)lineStarts.size()) return "";
    int start = lineStarts[line - 1];
    int end = line < (int)lineStarts.size() ? lineStarts[line] - 1 : (int)text.size();
    std::string s = text.substr(start, end - start);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

std::string formatDiagnostic(const SourceFile* file, const Diagnostic& d) {
    std::string out = d.sev == Diagnostic::Error ? "error: " : "warning: ";
    out += d.title + "\n";
    if (!file) {
        if (!d.label.empty()) out += "  = " + d.label + "\n";
        for (auto& n : d.notes) out += "  = note: " + n + "\n";
        return out;
    }
    int line, col;
    file->lineCol(d.span.start, line, col);
    std::string num = std::to_string(line);
    size_t width = std::max<size_t>(2, num.size());
    std::string pad(width, ' ');
    out += pad + "--> " + file->display + ":" + num + ":" + std::to_string(col) + "\n";
    out += pad + " |\n";
    std::string text = file->lineText(line);
    out += std::string(width - num.size(), ' ') + num + " | " + text + "\n";
    // caret line: keep tabs so the carets line up with the source
    std::string prefix;
    for (int i = 0; i < col - 1 && i < (int)text.size(); i++) prefix += text[i] == '\t' ? '\t' : ' ';
    int len = d.span.end - d.span.start;
    int maxLen = (int)text.size() - (col - 1);
    if (len > maxLen) len = maxLen;
    if (len < 1) len = 1;
    out += pad + " | " + prefix + std::string(len, '^');
    if (!d.label.empty()) out += " " + d.label;
    out += "\n";
    for (auto& n : d.notes) out += pad + " = note: " + n + "\n";
    return out;
}

void VM::emitError(const std::string& text) {
    if (errorHandler) errorHandler(text);
    else std::fputs(text.c_str(), stderr);
}

void VM::report(const Diagnostic& d) {
    if (d.sev == Diagnostic::Warning && !warnings) return;
    const SourceFile* f = d.fileId >= 0 && d.fileId < (int)sources.size() ? &sources[d.fileId] : nullptr;
    std::string text = formatDiagnostic(f, d);
    if (d.sev == Diagnostic::Error) lastError += text;
    emitError(text);
}

void VM::reportAll(const std::vector<Diagnostic>& ds) {
    // warnings first, then errors, each in source order
    std::vector<const Diagnostic*> sorted;
    for (auto& d : ds) sorted.push_back(&d);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Diagnostic* a, const Diagnostic* b) {
        if (a->fileId != b->fileId) return a->fileId < b->fileId;
        return a->span.start < b->span.start;
    });
    for (int pass = 0; pass < 2; pass++) {
        for (auto* d : sorted) {
            if ((pass == 0) == (d->sev == Diagnostic::Warning)) report(*d);
        }
    }
}

} // namespace tp
