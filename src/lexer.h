// lexer.h - tokens and the Taipan lexer.
#pragma once

#include "internal.h"

namespace tp {

enum class Tok {
    End, Newline, Ident, Int, Float, String, FString, Error,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Dot, Colon, Semicolon, Question, QQ, Ellipsis, Arrow,
    Plus, Minus, Star, Slash, Percent, PlusPlus, MinusMinus,
    Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    Eq, Ne, Lt, Le, Gt, Ge, AndAnd, OrOr, Bang,
    Amp, Pipe, Caret, Tilde, Shl, Shr,
    // keywords
    KwFunc, KwCoroutine, KwStart, KwReturn, KwIf, KwElse, KwFor, KwIn, KwWhile, KwMatch,
    KwBreak, KwContinue, KwTry, KwCatch, KwThrow, KwClass, KwInterface, KwImplements,
    KwEnum, KwImport, KwAs, KwEnable, KwConst, KwPrivate, KwPublic, KwTrue, KwFalse,
    KwNull, KwThis, KwAny, KwInt, KwFloat, KwBool, KwStr, KwList, KwDict
};

struct Token {
    Tok t = Tok::End;
    int start = 0;
    int end = 0;
    std::string text;   // identifier name, processed string literal, raw f-string body, error message
    int64_t ival = 0;
    double fval = 0;
    int contentStart = 0; // f-strings: offset of the raw body in the source
};

const char* tokName(Tok t);

// Tokenizes source[begin, end). Offsets in tokens are absolute.
std::vector<Token> lex(const std::string& source, int begin, int end);

// Processes escape sequences of a raw literal body. Returns false on error.
bool unescape(const std::string& raw, std::string& out, std::string& err);

} // namespace tp
