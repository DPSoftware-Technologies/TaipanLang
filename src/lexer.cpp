// lexer.cpp - turns Taipan source text into tokens.
//
// Statements end at newlines; the lexer emits Newline tokens (collapsed) and
// the parser decides where they are insignificant (inside (), [] and dict
// literals). A trailing backslash joins two lines.
#include "lexer.h"

#include <stdexcept>

namespace tp {

static const std::unordered_map<std::string, Tok>& keywords() {
    static const std::unordered_map<std::string, Tok> kw = {
        {"func", Tok::KwFunc}, {"coroutine", Tok::KwCoroutine}, {"start", Tok::KwStart},
        {"return", Tok::KwReturn}, {"if", Tok::KwIf}, {"else", Tok::KwElse}, {"for", Tok::KwFor},
        {"in", Tok::KwIn}, {"while", Tok::KwWhile}, {"match", Tok::KwMatch}, {"break", Tok::KwBreak},
        {"continue", Tok::KwContinue}, {"try", Tok::KwTry}, {"catch", Tok::KwCatch},
        {"throw", Tok::KwThrow}, {"class", Tok::KwClass}, {"interface", Tok::KwInterface},
        {"implements", Tok::KwImplements}, {"enum", Tok::KwEnum}, {"import", Tok::KwImport},
        {"as", Tok::KwAs}, {"enable", Tok::KwEnable}, {"const", Tok::KwConst},
        {"private", Tok::KwPrivate}, {"public", Tok::KwPublic}, {"true", Tok::KwTrue},
        {"false", Tok::KwFalse}, {"null", Tok::KwNull}, {"this", Tok::KwThis}, {"any", Tok::KwAny},
        {"int", Tok::KwInt}, {"float", Tok::KwFloat}, {"bool", Tok::KwBool}, {"str", Tok::KwStr},
        {"list", Tok::KwList}, {"dict", Tok::KwDict},
    };
    return kw;
}

const char* tokName(Tok t) {
    switch (t) {
    case Tok::End: return "end of file";
    case Tok::Newline: return "newline";
    case Tok::Ident: return "identifier";
    case Tok::Int: return "integer";
    case Tok::Float: return "float";
    case Tok::String: return "string";
    case Tok::FString: return "f-string";
    case Tok::Error: return "invalid token";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::Colon: return "':'";
    case Tok::Semicolon: return "';'";
    case Tok::Question: return "'?'";
    case Tok::QQ: return "'?\?'";
    case Tok::Ellipsis: return "'...'";
    case Tok::Arrow: return "'->'";
    case Tok::Assign: return "'='";
    case Tok::Gt: return "'>'";
    case Tok::Lt: return "'<'";
    default: break;
    }
    for (auto& kv : keywords()) {
        if (kv.second == t) {
            static thread_local std::string buf;
            buf = "'" + kv.first + "'";
            return buf.c_str();
        }
    }
    return "operator";
}

bool unescape(const std::string& raw, std::string& out, std::string& err) {
    out.clear();
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= raw.size()) {
            err = "unterminated escape sequence";
            return false;
        }
        switch (raw[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case '\'': out += '\''; break;
        case '{': out += '{'; break;
        case '}': out += '}'; break;
        case 'x': {
            std::string hex = raw.substr(i + 1, 2);
            if (hex.size() != 2 || !isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1])) {
                err = "bad \\x escape (expected two hex digits)";
                return false;
            }
            out += (char)std::stoi(hex, nullptr, 16);
            i += 2;
            break;
        }
        default:
            err = std::string("unknown escape sequence '\\") + raw[i] + "'";
            return false;
        }
    }
    return true;
}

namespace {

struct Lexer {
    const std::string& s;
    int pos;
    int end;
    std::vector<Token> out;

    Lexer(const std::string& src, int b, int e) : s(src), pos(b), end(e) {}

    char at(int i) const { return i < end ? s[i] : '\0'; }

    void push(Tok t, int start) {
        Token tk;
        tk.t = t;
        tk.start = start;
        tk.end = pos;
        out.push_back(tk);
    }

    void newline(int start) {
        if (!out.empty() && out.back().t != Tok::Newline) {
            Token tk;
            tk.t = Tok::Newline;
            tk.start = start;
            tk.end = start + 1;
            out.push_back(tk);
        }
    }

    void error(int start, const std::string& msg) {
        Token tk;
        tk.t = Tok::Error;
        tk.start = start;
        tk.end = pos > start ? pos : start + 1;
        tk.text = msg;
        out.push_back(tk);
    }

    // Scans a quoted body starting after the opening quote; returns the raw body.
    bool scanQuoted(char q, int start, std::string& raw) {
        while (pos < end && s[pos] != q) {
            if (s[pos] == '\n') {
                error(start, "unterminated string literal");
                return false;
            }
            if (s[pos] == '\\' && pos + 1 < end) pos++;
            pos++;
        }
        if (pos >= end) {
            error(start, "unterminated string literal");
            return false;
        }
        raw = s.substr(start, pos - start);
        pos++; // closing quote
        return true;
    }

    void number(int start) {
        bool isFloat = false;
        std::string digits;
        if (at(pos) == '0' && (at(pos + 1) == 'x' || at(pos + 1) == 'X' || at(pos + 1) == 'b' || at(pos + 1) == 'B')) {
            int base = (at(pos + 1) == 'x' || at(pos + 1) == 'X') ? 16 : 2;
            pos += 2;
            while (isxdigit((unsigned char)at(pos)) || at(pos) == '_') {
                if (at(pos) != '_') digits += at(pos);
                pos++;
            }
            if (digits.empty()) {
                error(start, "malformed number literal");
                return;
            }
            Token tk;
            tk.t = Tok::Int;
            tk.start = start;
            tk.end = pos;
            try {
                tk.ival = (int64_t)std::stoull(digits, nullptr, base);
            } catch (...) {
                error(start, "integer literal out of range");
                return;
            }
            out.push_back(tk);
            return;
        }
        while (isdigit((unsigned char)at(pos)) || at(pos) == '_') {
            if (at(pos) != '_') digits += at(pos);
            pos++;
        }
        if (at(pos) == '.' && isdigit((unsigned char)at(pos + 1))) {
            isFloat = true;
            digits += '.';
            pos++;
            while (isdigit((unsigned char)at(pos)) || at(pos) == '_') {
                if (at(pos) != '_') digits += at(pos);
                pos++;
            }
        }
        if (at(pos) == 'e' || at(pos) == 'E') {
            int save = pos;
            std::string exp = "e";
            pos++;
            if (at(pos) == '+' || at(pos) == '-') exp += s[pos++];
            if (isdigit((unsigned char)at(pos))) {
                while (isdigit((unsigned char)at(pos))) exp += s[pos++];
                digits += exp;
                isFloat = true;
            } else {
                pos = save;
            }
        }
        Token tk;
        tk.start = start;
        tk.end = pos;
        if (isFloat) {
            tk.t = Tok::Float;
            tk.fval = std::strtod(digits.c_str(), nullptr);
        } else {
            tk.t = Tok::Int;
            try {
                unsigned long long v = std::stoull(digits);
                if (v > 9223372036854775807ULL) throw std::out_of_range("int");
                tk.ival = (int64_t)v;
            } catch (...) {
                error(start, "integer literal out of range");
                return;
            }
        }
        out.push_back(tk);
    }

    void run() {
        while (pos < end) {
            char c = s[pos];
            int start = pos;
            if (c == '\n') {
                pos++;
                newline(start);
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                pos++;
                continue;
            }
            if (c == '\\') { // explicit line continuation
                int p = pos + 1;
                while (p < end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
                if (p < end && s[p] == '\n') {
                    pos = p + 1;
                    continue;
                }
                pos++;
                error(start, "unexpected '\\' (line continuation must end the line)");
                continue;
            }
            if (c == '/' && at(pos + 1) == '/') {
                while (pos < end && s[pos] != '\n') pos++;
                continue;
            }
            if (c == '/' && at(pos + 1) == '*') {
                pos += 2;
                bool hadNewline = false;
                while (pos < end && !(s[pos] == '*' && at(pos + 1) == '/')) {
                    if (s[pos] == '\n') hadNewline = true;
                    pos++;
                }
                if (pos >= end) {
                    error(start, "unterminated block comment");
                    return;
                }
                pos += 2;
                if (hadNewline) newline(start);
                continue;
            }
            if (isdigit((unsigned char)c)) {
                number(start);
                continue;
            }
            if (c == 'f' && (at(pos + 1) == '"' || at(pos + 1) == '\'')) {
                char q = at(pos + 1);
                pos += 2;
                std::string raw;
                int contentStart = pos;
                if (!scanQuoted(q, contentStart, raw)) continue;
                Token tk;
                tk.t = Tok::FString;
                tk.start = start;
                tk.end = pos;
                tk.text = raw;
                tk.contentStart = contentStart;
                out.push_back(tk);
                continue;
            }
            if (isalpha((unsigned char)c) || c == '_') {
                while (isalnum((unsigned char)at(pos)) || at(pos) == '_') pos++;
                std::string word = s.substr(start, pos - start);
                auto it = keywords().find(word);
                Token tk;
                tk.t = it != keywords().end() ? it->second : Tok::Ident;
                tk.start = start;
                tk.end = pos;
                tk.text = word;
                out.push_back(tk);
                continue;
            }
            if (c == '"' || c == '\'') {
                pos++;
                std::string raw;
                if (!scanQuoted(c, pos, raw)) continue;
                Token tk;
                tk.t = Tok::String;
                tk.start = start;
                tk.end = pos;
                std::string err;
                if (!unescape(raw, tk.text, err)) {
                    error(start, err);
                    continue;
                }
                out.push_back(tk);
                continue;
            }
            pos++;
            char n = at(pos);
            switch (c) {
            case '(': push(Tok::LParen, start); break;
            case ')': push(Tok::RParen, start); break;
            case '[': push(Tok::LBracket, start); break;
            case ']': push(Tok::RBracket, start); break;
            case '{': push(Tok::LBrace, start); break;
            case '}': push(Tok::RBrace, start); break;
            case ',': push(Tok::Comma, start); break;
            case ':': push(Tok::Colon, start); break;
            case ';': push(Tok::Semicolon, start); break;
            case '~': push(Tok::Tilde, start); break;
            case '^': push(Tok::Caret, start); break;
            case '.':
                if (n == '.' && at(pos + 1) == '.') {
                    pos += 2;
                    push(Tok::Ellipsis, start);
                } else {
                    push(Tok::Dot, start);
                }
                break;
            case '?':
                if (n == '?') { pos++; push(Tok::QQ, start); } else push(Tok::Question, start);
                break;
            case '+':
                if (n == '+') { pos++; push(Tok::PlusPlus, start); }
                else if (n == '=') { pos++; push(Tok::PlusEq, start); }
                else push(Tok::Plus, start);
                break;
            case '-':
                if (n == '-') { pos++; push(Tok::MinusMinus, start); }
                else if (n == '=') { pos++; push(Tok::MinusEq, start); }
                else if (n == '>') { pos++; push(Tok::Arrow, start); }
                else push(Tok::Minus, start);
                break;
            case '*':
                if (n == '=') { pos++; push(Tok::StarEq, start); } else push(Tok::Star, start);
                break;
            case '/':
                if (n == '=') { pos++; push(Tok::SlashEq, start); } else push(Tok::Slash, start);
                break;
            case '%':
                if (n == '=') { pos++; push(Tok::PercentEq, start); } else push(Tok::Percent, start);
                break;
            case '=':
                if (n == '=') { pos++; push(Tok::Eq, start); } else push(Tok::Assign, start);
                break;
            case '!':
                if (n == '=') { pos++; push(Tok::Ne, start); } else push(Tok::Bang, start);
                break;
            case '<':
                if (n == '=') { pos++; push(Tok::Le, start); }
                else if (n == '<') { pos++; push(Tok::Shl, start); }
                else push(Tok::Lt, start);
                break;
            case '>':
                if (n == '=') { pos++; push(Tok::Ge, start); }
                else if (n == '>') { pos++; push(Tok::Shr, start); }
                else push(Tok::Gt, start);
                break;
            case '&':
                if (n == '&') { pos++; push(Tok::AndAnd, start); } else push(Tok::Amp, start);
                break;
            case '|':
                if (n == '|') { pos++; push(Tok::OrOr, start); } else push(Tok::Pipe, start);
                break;
            case '#':
                error(start, "'#' is reserved for future directives; use // for comments");
                break;
            default:
                error(start, std::string("unexpected character '") + c + "'");
                break;
            }
        }
        newline(pos);
        Token e;
        e.t = Tok::End;
        e.start = pos;
        e.end = pos;
        out.push_back(e);
    }
};

} // namespace

std::vector<Token> lex(const std::string& source, int begin, int end) {
    Lexer lx(source, begin, end);
    lx.run();
    return std::move(lx.out);
}

} // namespace tp
