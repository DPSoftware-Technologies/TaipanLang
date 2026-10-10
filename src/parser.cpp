// parser.cpp - recursive-descent parser producing the AST.
//
// Newline handling: statements are newline-terminated. Inside (), [] and
// dict literals newlines are insignificant; a block `{ ... }` (including a
// lambda body nested inside a call) makes them significant again. This is
// tracked with a mode stack consulted by every token access.
#include "parser.h"

namespace tp {

namespace {

struct ParseError {};

class Parser {
public:
    Parser(const std::string& text, int begin, int end, int fileId, std::vector<Diagnostic>& diags)
        : text_(text), fileId_(fileId), diags_(diags) {
        toks_ = lex(text, begin, end);
        nl_.push_back(false);
    }

    std::vector<StmtP> program() {
        std::vector<StmtP> out;
        // Lexer errors are reported up front.
        for (auto& t : toks_) {
            if (t.t == Tok::Error) {
                report({t.start, t.end}, "syntax error", t.text);
                return out;
            }
        }
        skipSeparators();
        while (!check(Tok::End)) {
            try {
                out.push_back(statement());
                endStatement();
            } catch (ParseError&) {
                if (errors_ >= 10) break;
                synchronize();
            }
            skipSeparators();
        }
        return out;
    }

    // Parses a single expression covering the whole token range (f-string parts).
    ExprP wholeExpression() {
        nl_.back() = true;
        ExprP e = expression();
        if (!check(Tok::End)) error(cur(), "unexpected " + std::string(tokName(cur().t)) + " in f-string expression");
        return e;
    }

    bool hasLexError(Token& out) {
        for (auto& t : toks_) {
            if (t.t == Tok::Error) {
                out = t;
                return true;
            }
        }
        return false;
    }

    SigParse signature() {
        SigParse sp;
        try {
            nl_.back() = true;
            expect(Tok::LParen, "'('");
            while (!check(Tok::RParen)) {
                sp.params.push_back(type());
                bool opt = false;
                if (match(Tok::Assign)) opt = true;
                if (match(Tok::Ellipsis)) sp.variadic = true;
                sp.optional.push_back(opt);
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "')'");
            if (match(Tok::Arrow)) {
                if (check(Tok::Ident) && cur().text == "void") {
                    advance();
                } else {
                    sp.rets = returnTypes();
                }
            }
            sp.ok = check(Tok::End);
        } catch (ParseError&) {
            sp.ok = false;
        }
        return sp;
    }

    bool bindings(BindingFile& out) {
        for (auto& t : toks_) {
            if (t.t == Tok::Error) {
                report({t.start, t.end}, "syntax error", t.text);
                return false;
            }
        }
        skipSeparators();
        while (!check(Tok::End)) {
            try {
                bindingDecl(out, "");
            } catch (ParseError&) {
                if (errors_ >= 10) break;
                synchronize();
            }
            skipSeparators();
        }
        return errors_ == 0;
    }

private:
    bool isWord(const char* w) { return check(Tok::Ident) && cur().text == w; }

    std::string qualifiedName() {
        std::string n = expect(Tok::Ident, "a name").text;
        while (check(Tok::Dot) && peekAt(1).t == Tok::Ident) {
            advance();
            n += "." + advance().text;
        }
        return n;
    }

    BindingFunc bindingFunc(const std::string& prefix) {
        BindingFunc f;
        f.span.start = advance().start; // func / coroutine
        Token n = expect(Tok::Ident, "a function name");
        f.qualified = prefix.empty() ? n.text : prefix + "." + n.text;
        f.params = params();
        if (match(Tok::Arrow)) f.rets = returnTypes();
        f.span.end = lastEnd();
        return f;
    }

    void bindingDecl(BindingFile& out, const std::string& prefix) {
        if (isWord("namespace")) {
            advance();
            std::string name = qualifiedName();
            std::string full = prefix.empty() ? name : prefix + "." + name;
            out.namespaces.push_back(full);
            skipNewlines();
            expect(Tok::LBrace, "'{'");
            nl_.push_back(false);
            skipSeparators();
            while (!check(Tok::RBrace)) {
                if (check(Tok::End)) error(cur(), "expected '}' to close the namespace");
                bindingDecl(out, full);
                skipSeparators();
            }
            advance();
            nl_.pop_back();
            return;
        }
        if (check(Tok::KwClass)) {
            if (!prefix.empty()) error(cur(), "native classes must be declared at the top level of a binding file");
            advance();
            BindingClass c;
            c.name = expect(Tok::Ident, "a class name").text;
            if (match(Tok::KwAs)) {
                do {
                    c.aliases.push_back(expect(Tok::Ident, "an alias name").text);
                } while (match(Tok::Comma));
            }
            skipNewlines();
            expect(Tok::LBrace, "'{'");
            nl_.push_back(false);
            skipSeparators();
            while (!check(Tok::RBrace)) {
                if (check(Tok::End)) error(cur(), "expected '}' to close the class");
                if (check(Tok::KwFunc) || check(Tok::KwCoroutine)) {
                    c.methods.push_back(bindingFunc(""));
                } else {
                    BindingProp p;
                    if (isWord("readonly") || check(Tok::KwConst)) {
                        advance();
                        p.writable = false;
                    }
                    p.type = type();
                    p.name = expect(Tok::Ident, "a property name").text;
                    c.props.push_back(std::move(p));
                }
                endStatement();
                skipSeparators();
            }
            advance();
            nl_.pop_back();
            out.classes.push_back(std::move(c));
            return;
        }
        if (check(Tok::KwFunc) || check(Tok::KwCoroutine)) {
            out.funcs.push_back(bindingFunc(prefix));
            endStatement();
            return;
        }
        BindingValue v;
        match(Tok::KwConst);
        v.type = type();
        std::string n = expect(Tok::Ident, "a name").text;
        v.qualified = prefix.empty() ? n : prefix + "." + n;
        out.values.push_back(std::move(v));
        endStatement();
    }

    const std::string& text_;
    int fileId_;
    std::vector<Diagnostic>& diags_;
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::vector<bool> nl_;
    int speculating_ = 0;
    int errors_ = 0;

    // ----- token access -----
    bool ignoringNewlines() const { return nl_.back(); }

    const Token& cur() {
        if (ignoringNewlines()) {
            while (toks_[pos_].t == Tok::Newline) pos_++;
        }
        return toks_[pos_];
    }

    const Token& peekAt(int n) {
        size_t p = pos_;
        int seen = 0;
        while (true) {
            if (ignoringNewlines()) {
                while (toks_[p].t == Tok::Newline) p++;
            }
            if (seen == n || toks_[p].t == Tok::End) return toks_[p];
            p++;
            seen++;
        }
    }

    Token advance() {
        Token t = cur();
        if (toks_[pos_].t != Tok::End) pos_++;
        return t;
    }

    bool check(Tok t) { return cur().t == t; }

    bool match(Tok t) {
        if (check(t)) {
            advance();
            return true;
        }
        return false;
    }

    Token expect(Tok t, const std::string& what) {
        if (!check(t)) error(cur(), "expected " + what + ", found " + describe(cur()));
        return advance();
    }

    std::string describe(const Token& t) {
        if (t.t == Tok::Ident) return "'" + t.text + "'";
        if (t.t == Tok::Int || t.t == Tok::Float) return "number '" + text_.substr(t.start, t.end - t.start) + "'";
        if (t.t == Tok::String) return "string literal";
        return tokName(t.t);
    }

    Span spanOf(const Token& t) { return {t.start, t.end}; }
    int lastEnd() { return pos_ > 0 ? toks_[pos_ - 1].end : 0; }

    void skipNewlines() {
        while (toks_[pos_].t == Tok::Newline) pos_++;
    }

    void skipSeparators() {
        while (toks_[pos_].t == Tok::Newline || toks_[pos_].t == Tok::Semicolon) pos_++;
    }

    void report(Span sp, const std::string& title, const std::string& label) {
        Diagnostic d;
        d.sev = Diagnostic::Error;
        d.title = title;
        d.fileId = fileId_;
        d.span = sp;
        d.label = label;
        diags_.push_back(d);
        errors_++;
    }

    [[noreturn]] void error(const Token& t, const std::string& msg) {
        if (!speculating_) {
            Span sp = spanOf(t);
            if (t.t == Tok::Newline || t.t == Tok::End) sp = {t.start, t.start + 1};
            report(sp, "syntax error", msg);
        }
        throw ParseError();
    }

    [[noreturn]] void errorSpan(Span sp, const std::string& msg) {
        if (!speculating_) report(sp, "syntax error", msg);
        throw ParseError();
    }

    void synchronize() {
        nl_.resize(1);
        int depth = 0;
        while (toks_[pos_].t != Tok::End) {
            Tok t = toks_[pos_].t;
            if (t == Tok::LBrace) depth++;
            if (t == Tok::RBrace) {
                if (depth == 0) {
                    pos_++;
                    return;
                }
                depth--;
            }
            if (t == Tok::Newline && depth == 0) return;
            pos_++;
        }
    }

    void endStatement() {
        const Token& t = toks_[pos_];
        if (t.t == Tok::Newline || t.t == Tok::Semicolon) {
            pos_++;
            return;
        }
        if (t.t == Tok::RBrace || t.t == Tok::End) return;
        if (t.t == Tok::Ident && t.text == "elif") error(t, "Taipan uses 'else if', not 'elif'");
        if (t.t == Tok::Ident && (t.text == "then" || t.text == "do" || t.text == "end")) {
            error(t, "Taipan uses braces { } for blocks, not '" + t.text + "'");
        }
        error(t, "expected end of statement (newline or ';'), found " + describe(t));
    }

    // ----- types -----
    void splitShr() {
        // `list<list<int>>`: the lexer produced '>>'; split it into two '>'.
        Token& t = toks_[pos_];
        if (t.t == Tok::Shr) {
            t.t = Tok::Gt;
            Token second = t;
            t.end = t.start + 1;
            second.start = t.start + 1;
            toks_.insert(toks_.begin() + (long)pos_ + 1, second);
        }
    }

    TypeExprP type() {
        auto t = std::make_unique<TypeExpr>();
        const Token& tk = cur();
        t->span.start = tk.start;
        switch (tk.t) {
        case Tok::KwInt: case Tok::KwFloat: case Tok::KwBool: case Tok::KwStr: case Tok::KwAny: case Tok::KwFunc:
            t->name = tk.text;
            advance();
            break;
        case Tok::KwList: {
            advance();
            t->kind = TypeExpr::List;
            t->name = "list";
            expect(Tok::Lt, "'<' after 'list'");
            t->args.push_back(type());
            splitShr();
            expect(Tok::Gt, "'>'");
            break;
        }
        case Tok::KwDict: {
            advance();
            t->kind = TypeExpr::Dict;
            t->name = "dict";
            expect(Tok::Lt, "'<' after 'dict'");
            t->args.push_back(type());
            expect(Tok::Comma, "',' between dict key and value types");
            t->args.push_back(type());
            splitShr();
            expect(Tok::Gt, "'>'");
            break;
        }
        case Tok::Ident: {
            t->name = tk.text;
            advance();
            while (check(Tok::Dot) && peekAt(1).t == Tok::Ident) {
                advance();
                t->name += "." + advance().text;
            }
            break;
        }
        default:
            error(tk, "expected a type, found " + describe(tk));
        }
        if (check(Tok::Question)) {
            advance();
            t->nullable = true;
        }
        t->span.end = lastEnd();
        return t;
    }

    std::vector<TypeExprP> returnTypes() {
        std::vector<TypeExprP> rets;
        if (check(Tok::LParen)) {
            nl_.push_back(true);
            advance();
            rets.push_back(type());
            while (match(Tok::Comma)) rets.push_back(type());
            expect(Tok::RParen, "')'");
            nl_.pop_back();
        } else {
            rets.push_back(type());
        }
        return rets;
    }

    // Speculatively checks for `Type ident` at the current position.
    bool looksLikeDecl() {
        size_t save = pos_;
        size_t nlSave = nl_.size();
        speculating_++;
        bool ok = false;
        try {
            type();
            ok = cur().t == Tok::Ident;
        } catch (ParseError&) {
            ok = false;
        }
        speculating_--;
        pos_ = save;
        nl_.resize(nlSave);
        return ok;
    }

    // ----- statements -----
    std::unique_ptr<Block> block() {
        skipNewlines();
        auto b = std::make_unique<Block>();
        Token open = expect(Tok::LBrace, "'{'");
        b->span.start = open.start;
        nl_.push_back(false);
        skipSeparators();
        while (!check(Tok::RBrace)) {
            if (check(Tok::End)) error(cur(), "expected '}' to close the block opened here");
            b->stmts.push_back(statement());
            endStatement();
            skipSeparators();
        }
        Token close = advance();
        nl_.pop_back();
        b->span.end = close.end;
        return b;
    }

    StmtP statement() {
        const Token& t = cur();
        switch (t.t) {
        case Tok::KwFunc:
            if (peekAt(1).t == Tok::Ident && peekAt(2).t == Tok::LParen) return funcStmt();
            break;
        case Tok::KwCoroutine: return funcStmt();
        case Tok::KwClass: return classStmt();
        case Tok::KwInterface: return interfaceStmt();
        case Tok::KwEnum: return enumStmt();
        case Tok::KwImport: return importStmt();
        case Tok::KwEnable: {
            auto s = std::make_unique<Stmt>(SK::Enable);
            s->span.start = advance().start;
            Token n = expect(Tok::Ident, "a feature name after 'enable'");
            s->name = n.text;
            s->nameSpan = spanOf(n);
            s->span.end = n.end;
            return s;
        }
        case Tok::KwIf: return ifStmt();
        case Tok::KwWhile: {
            auto s = std::make_unique<Stmt>(SK::While);
            s->span.start = advance().start;
            s->e = expression();
            s->body = block();
            s->span.end = s->body->span.end;
            return s;
        }
        case Tok::KwFor: return forStmt();
        case Tok::KwMatch: return matchStmt();
        case Tok::KwBreak: case Tok::KwContinue: {
            auto s = std::make_unique<Stmt>(t.t == Tok::KwBreak ? SK::Break : SK::Continue);
            s->span = spanOf(t);
            advance();
            return s;
        }
        case Tok::KwReturn: {
            auto s = std::make_unique<Stmt>(SK::Return);
            s->span = spanOf(t);
            advance();
            Tok n = toks_[pos_].t;
            if (n != Tok::Newline && n != Tok::Semicolon && n != Tok::RBrace && n != Tok::End) {
                s->values.push_back(expression());
                while (match(Tok::Comma)) s->values.push_back(expression());
                s->span.end = lastEnd();
            }
            return s;
        }
        case Tok::KwTry: {
            auto s = std::make_unique<Stmt>(SK::Try);
            s->span.start = advance().start;
            s->body = block();
            skipNewlines();
            expect(Tok::KwCatch, "'catch' after try block");
            if (check(Tok::Ident)) {
                Token n = advance();
                s->name = n.text;
                s->nameSpan = spanOf(n);
            }
            s->elseBody = block();
            s->span.end = s->elseBody->span.end;
            return s;
        }
        case Tok::KwThrow: {
            auto s = std::make_unique<Stmt>(SK::Throw);
            s->span.start = advance().start;
            s->e = expression();
            s->span.end = lastEnd();
            return s;
        }
        case Tok::LBrace: {
            auto s = std::make_unique<Stmt>(SK::Block);
            s->body = block();
            s->span = s->body->span;
            return s;
        }
        case Tok::KwPrivate: case Tok::KwPublic:
            error(t, "'" + t.text + "' is only allowed on class members");
        default:
            break;
        }
        return simpleStatement();
    }

    StmtP simpleStatement() {
        if (check(Tok::KwConst) || looksLikeDecl()) return varDecl();
        int start = cur().start;
        ExprP e = expression();
        Tok t = cur().t;
        if (t == Tok::Assign || t == Tok::PlusEq || t == Tok::MinusEq || t == Tok::StarEq || t == Tok::SlashEq ||
            t == Tok::PercentEq) {
            auto s = std::make_unique<Stmt>(SK::Assign);
            s->op = t;
            advance();
            s->target = std::move(e);
            s->e = expression();
            s->span = {start, lastEnd()};
            return s;
        }
        if (t == Tok::PlusPlus || t == Tok::MinusMinus) {
            auto s = std::make_unique<Stmt>(SK::IncDec);
            s->op = t;
            advance();
            s->target = std::move(e);
            s->span = {start, lastEnd()};
            return s;
        }
        if (t == Tok::Comma && e->k == EK::Ident) {
            error(cur(), "multiple assignment needs types, e.g. 'int a, int b = f()'");
        }
        auto s = std::make_unique<Stmt>(SK::Expr);
        s->span = {start, lastEnd()};
        s->e = std::move(e);
        return s;
    }

    StmtP varDecl() {
        auto s = std::make_unique<Stmt>(SK::VarDecl);
        s->span.start = cur().start;
        if (match(Tok::KwConst)) s->isConst = true;
        do {
            VarTarget v;
            v.type = type();
            Token n = expect(Tok::Ident, "a variable name");
            v.name = n.text;
            v.span = spanOf(n);
            s->vars.push_back(std::move(v));
        } while (match(Tok::Comma));
        if (match(Tok::Assign)) {
            s->e = expression();
        } else if (s->vars.size() > 1) {
            error(cur(), "a multiple declaration needs a value, e.g. 'int a, int b = f()'");
        } else if (s->isConst) {
            error(cur(), "a constant needs a value");
        }
        s->span.end = lastEnd();
        return s;
    }

    StmtP ifStmt() {
        auto s = std::make_unique<Stmt>(SK::If);
        s->span.start = advance().start;
        s->e = expression();
        s->body = block();
        s->span.end = s->body->span.end;
        size_t save = pos_;
        skipNewlines();
        if (toks_[pos_].t == Tok::KwElse) {
            advance();
            if (check(Tok::KwIf)) {
                auto b = std::make_unique<Block>();
                StmtP inner = ifStmt();
                b->span = inner->span;
                b->stmts.push_back(std::move(inner));
                s->elseBody = std::move(b);
            } else {
                s->elseBody = block();
            }
            s->span.end = s->elseBody->span.end;
        } else {
            pos_ = save;
        }
        return s;
    }

    StmtP forStmt() {
        int start = advance().start;
        if (check(Tok::LParen)) {
            // Destructuring `for (T a, U b) in xs` or C-style `for (init; cond; step)`.
            size_t save = pos_;
            nl_.push_back(true);
            advance();
            bool destructure = false;
            {
                size_t s2 = pos_;
                speculating_++;
                try {
                    type();
                    if (check(Tok::Ident)) {
                        advance();
                        destructure = check(Tok::Comma);
                    }
                } catch (ParseError&) {
                }
                speculating_--;
                pos_ = s2;
            }
            if (destructure) {
                auto s = std::make_unique<Stmt>(SK::ForIn);
                do {
                    VarTarget v;
                    v.type = type();
                    Token n = expect(Tok::Ident, "a loop variable name");
                    v.name = n.text;
                    v.span = spanOf(n);
                    s->vars.push_back(std::move(v));
                } while (match(Tok::Comma));
                expect(Tok::RParen, "')'");
                nl_.pop_back();
                if (s->vars.size() > 2) errorSpan(s->vars[2].span, "at most two loop variables can be destructured");
                expect(Tok::KwIn, "'in'");
                s->e = expression();
                s->body = block();
                s->span = {start, s->body->span.end};
                return s;
            }
            (void)save;
            auto s = std::make_unique<Stmt>(SK::ForC);
            if (!check(Tok::Semicolon)) s->init = simpleStatement();
            expect(Tok::Semicolon, "';' after the loop initializer");
            if (!check(Tok::Semicolon)) s->e = expression();
            expect(Tok::Semicolon, "';' after the loop condition");
            if (!check(Tok::RParen)) s->step = simpleStatement();
            expect(Tok::RParen, "')'");
            nl_.pop_back();
            s->body = block();
            s->span = {start, s->body->span.end};
            return s;
        }
        auto s = std::make_unique<Stmt>(SK::ForIn);
        VarTarget v;
        if (check(Tok::Ident) && peekAt(1).t == Tok::KwIn) {
            errorSpan(spanOf(cur()), "loop variable needs a type, e.g. 'for int i in range(10)'");
        }
        v.type = type();
        Token n = expect(Tok::Ident, "a loop variable name");
        v.name = n.text;
        v.span = spanOf(n);
        s->vars.push_back(std::move(v));
        if (check(Tok::Comma)) error(cur(), "use parentheses to destructure: 'for (int i, T x) in ...'");
        expect(Tok::KwIn, "'in'");
        s->e = expression();
        s->body = block();
        s->span = {start, s->body->span.end};
        return s;
    }

    StmtP matchStmt() {
        auto s = std::make_unique<Stmt>(SK::Match);
        s->span.start = advance().start;
        s->e = expression();
        skipNewlines();
        expect(Tok::LBrace, "'{' after match subject");
        nl_.push_back(false);
        skipSeparators();
        while (!check(Tok::RBrace)) {
            if (check(Tok::End)) error(cur(), "expected '}' to close the match");
            MatchArm arm;
            arm.span.start = cur().start;
            if (match(Tok::KwElse)) {
                arm.isElse = true;
            } else {
                arm.patterns.push_back(expression());
                while (match(Tok::Comma)) arm.patterns.push_back(expression());
            }
            expect(Tok::Arrow, "'->' after match pattern");
            if (check(Tok::LBrace)) {
                arm.body = block();
            } else {
                arm.body = std::make_unique<Block>();
                StmtP st = statement();
                arm.body->span = st->span;
                arm.body->stmts.push_back(std::move(st));
            }
            arm.span.end = arm.body->span.end;
            s->arms.push_back(std::move(arm));
            endStatement();
            skipSeparators();
        }
        s->span.end = advance().end;
        nl_.pop_back();
        return s;
    }

    std::vector<Param> params() {
        std::vector<Param> ps;
        nl_.push_back(true);
        expect(Tok::LParen, "'('");
        while (!check(Tok::RParen)) {
            Param p;
            p.span.start = cur().start;
            p.type = type();
            if (match(Tok::Ellipsis)) p.variadic = true;
            Token n = expect(Tok::Ident, "a parameter name");
            p.name = n.text;
            if (match(Tok::Assign)) p.def = expression();
            p.span.end = lastEnd();
            ps.push_back(std::move(p));
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RParen, "')'");
        nl_.pop_back();
        return ps;
    }

    std::unique_ptr<FuncDecl> funcDecl(bool needBody) {
        auto f = std::make_unique<FuncDecl>();
        f->span.start = cur().start;
        Token kw = advance(); // func / coroutine
        f->isCoroutine = kw.t == Tok::KwCoroutine;
        Token n = expect(Tok::Ident, "a function name");
        f->name = n.text;
        f->nameSpan = spanOf(n);
        f->params = params();
        if (match(Tok::Arrow)) {
            if (f->isCoroutine) error(toks_[pos_ - 1], "coroutines cannot declare a return type");
            f->rets = returnTypes();
        }
        if (needBody) {
            f->body = block();
            f->span.end = f->body->span.end;
        } else {
            f->span.end = lastEnd();
        }
        return f;
    }

    StmtP funcStmt() {
        auto s = std::make_unique<Stmt>(SK::Func);
        s->func = funcDecl(true);
        s->span = s->func->span;
        return s;
    }

    StmtP classStmt() {
        auto s = std::make_unique<Stmt>(SK::Class);
        s->span.start = advance().start;
        auto c = std::make_unique<ClassDecl>();
        Token n = expect(Tok::Ident, "a class name");
        c->name = n.text;
        c->nameSpan = spanOf(n);
        if (check(Tok::Ident) && cur().text == "extends") {
            error(cur(), "Taipan classes have no inheritance; use 'implements' with interfaces, or composition");
        }
        if (match(Tok::KwImplements)) {
            do {
                Token i = expect(Tok::Ident, "an interface name");
                std::string nm = i.text;
                while (check(Tok::Dot) && peekAt(1).t == Tok::Ident) {
                    advance();
                    nm += "." + advance().text;
                }
                c->implements.push_back(nm);
                c->implSpans.push_back({i.start, lastEnd()});
            } while (match(Tok::Comma));
        }
        skipNewlines();
        expect(Tok::LBrace, "'{' to start the class body");
        nl_.push_back(false);
        skipSeparators();
        while (!check(Tok::RBrace)) {
            if (check(Tok::End)) error(cur(), "expected '}' to close the class");
            bool isPrivate = false;
            if (match(Tok::KwPrivate)) isPrivate = true;
            else match(Tok::KwPublic);
            if (check(Tok::KwFunc) && peekAt(1).t == Tok::Ident && peekAt(2).t == Tok::LParen) {
                auto f = funcDecl(true);
                f->isPrivate = isPrivate;
                c->methods.push_back(std::move(f));
            } else if (check(Tok::KwCoroutine)) {
                auto f = funcDecl(true);
                f->isPrivate = isPrivate;
                c->methods.push_back(std::move(f));
            } else {
                FieldDecl fd;
                fd.span.start = cur().start;
                fd.isPrivate = isPrivate;
                if (match(Tok::KwConst)) fd.isConst = true;
                fd.type = type();
                Token fn = expect(Tok::Ident, "a field name");
                fd.name = fn.text;
                if (match(Tok::Assign)) fd.init = expression();
                fd.span.end = lastEnd();
                fd.span.start = fn.start;
                fd.span.end = fn.end;
                c->fields.push_back(std::move(fd));
            }
            endStatement();
            skipSeparators();
        }
        s->span.end = advance().end;
        nl_.pop_back();
        s->cls = std::move(c);
        return s;
    }

    StmtP interfaceStmt() {
        auto s = std::make_unique<Stmt>(SK::Interface);
        s->span.start = advance().start;
        auto it = std::make_unique<InterfaceDecl>();
        Token n = expect(Tok::Ident, "an interface name");
        it->name = n.text;
        it->nameSpan = spanOf(n);
        skipNewlines();
        expect(Tok::LBrace, "'{'");
        nl_.push_back(false);
        skipSeparators();
        while (!check(Tok::RBrace)) {
            if (!check(Tok::KwFunc) && !check(Tok::KwCoroutine)) {
                error(cur(), "interfaces may only contain method signatures ('func name(...)')");
            }
            it->methods.push_back(funcDecl(false));
            endStatement();
            skipSeparators();
        }
        s->span.end = advance().end;
        nl_.pop_back();
        s->iface = std::move(it);
        return s;
    }

    StmtP enumStmt() {
        auto s = std::make_unique<Stmt>(SK::Enum);
        s->span.start = advance().start;
        auto e = std::make_unique<EnumDecl>();
        Token n = expect(Tok::Ident, "an enum name");
        e->name = n.text;
        e->nameSpan = spanOf(n);
        skipNewlines();
        expect(Tok::LBrace, "'{'");
        nl_.push_back(true);
        while (!check(Tok::RBrace)) {
            Token m = expect(Tok::Ident, "an enum member name");
            e->members.push_back(m.text);
            e->memberSpans.push_back(spanOf(m));
            if (!match(Tok::Comma)) break;
        }
        s->span.end = expect(Tok::RBrace, "'}'").end;
        nl_.pop_back();
        s->en = std::move(e);
        return s;
    }

    StmtP importStmt() {
        auto s = std::make_unique<Stmt>(SK::Import);
        s->span.start = advance().start;
        Token n = expect(Tok::Ident, "a module path after 'import'");
        s->name = n.text;
        s->nameSpan.start = n.start;
        while (match(Tok::Dot)) {
            Token p = cur();
            if (p.t != Tok::Ident && p.text.empty()) error(p, "expected a module name after '.'");
            advance();
            s->name += "." + p.text;
        }
        s->nameSpan.end = lastEnd();
        if (match(Tok::KwAs)) {
            Token a = expect(Tok::Ident, "an alias name after 'as'");
            s->alias = a.text;
        }
        s->span.end = lastEnd();
        return s;
    }

    // ----- expressions -----
    ExprP expression() { return binary(0); }

    static int precedence(Tok t) {
        switch (t) {
        case Tok::QQ: return 1;
        case Tok::OrOr: return 2;
        case Tok::AndAnd: return 3;
        case Tok::Eq: case Tok::Ne: return 4;
        case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 5;
        case Tok::Pipe: return 6;
        case Tok::Caret: return 7;
        case Tok::Amp: return 8;
        case Tok::Shl: case Tok::Shr: return 9;
        case Tok::Plus: case Tok::Minus: return 10;
        case Tok::Star: case Tok::Slash: case Tok::Percent: return 11;
        default: return -1;
        }
    }

    ExprP binary(int minPrec) {
        ExprP left = unary();
        while (true) {
            Tok op = cur().t;
            int prec = precedence(op);
            if (prec < 0 || prec < minPrec) break;
            advance();
            ExprP right = binary(prec + 1);
            EK kind = EK::Binary;
            if (op == Tok::AndAnd || op == Tok::OrOr) kind = EK::Logical;
            if (op == Tok::QQ) kind = EK::Coalesce;
            auto e = std::make_unique<Expr>(kind);
            e->op = op;
            e->span = {left->span.start, right->span.end};
            e->a = std::move(left);
            e->b = std::move(right);
            left = std::move(e);
        }
        return left;
    }

    ExprP unary() {
        Tok t = cur().t;
        if (t == Tok::Minus || t == Tok::Bang || t == Tok::Tilde) {
            Token op = advance();
            ExprP operand = unary();
            // Fold negative numeric literals.
            if (t == Tok::Minus && operand->k == EK::Int) {
                operand->ival = (int64_t)(0ULL - (uint64_t)operand->ival);
                operand->span.start = op.start;
                return operand;
            }
            if (t == Tok::Minus && operand->k == EK::Float) {
                operand->fval = -operand->fval;
                operand->span.start = op.start;
                return operand;
            }
            auto e = std::make_unique<Expr>(EK::Unary);
            e->op = t;
            e->span = {op.start, operand->span.end};
            e->a = std::move(operand);
            return e;
        }
        return postfix(primary());
    }

    ExprP postfix(ExprP e) {
        while (true) {
            if (check(Tok::LParen)) {
                nl_.push_back(true);
                advance();
                auto call = std::make_unique<Expr>(EK::Call);
                while (!check(Tok::RParen)) {
                    call->args.push_back(expression());
                    if (!match(Tok::Comma)) break;
                }
                Token close = expect(Tok::RParen, "')' to close the argument list");
                nl_.pop_back();
                call->span = {e->span.start, close.end};
                call->a = std::move(e);
                e = std::move(call);
            } else if (check(Tok::LBracket)) {
                nl_.push_back(true);
                advance();
                auto ix = std::make_unique<Expr>(EK::Index);
                ix->b = expression();
                Token close = expect(Tok::RBracket, "']'");
                nl_.pop_back();
                ix->span = {e->span.start, close.end};
                ix->a = std::move(e);
                e = std::move(ix);
            } else if (check(Tok::Dot)) {
                advance();
                Token n = cur();
                if (n.t != Tok::Ident && n.text.empty()) error(n, "expected a member name after '.'");
                advance();
                auto m = std::make_unique<Expr>(EK::Member);
                m->name = n.text;
                m->nameSpan = spanOf(n);
                m->span = {e->span.start, n.end};
                m->a = std::move(e);
                e = std::move(m);
            } else {
                break;
            }
        }
        return e;
    }

    bool tryLambdaParams(std::vector<Param>& out) {
        size_t save = pos_;
        size_t nlSave = nl_.size();
        speculating_++;
        bool ok = false;
        try {
            out = params();
            ok = check(Tok::Arrow);
        } catch (ParseError&) {
            ok = false;
        }
        speculating_--;
        if (!ok) {
            pos_ = save;
            nl_.resize(nlSave);
            out.clear();
        }
        return ok;
    }

    ExprP lambda(int start, std::vector<Param> ps) {
        auto e = std::make_unique<Expr>(EK::Lambda);
        e->params = std::move(ps);
        expect(Tok::Arrow, "'->'");
        if (check(Tok::LBrace)) {
            e->body = block();
            e->span = {start, e->body->span.end};
            return e;
        }
        // Typed block body `-> int { ... }` or an expression body.
        size_t save = pos_;
        speculating_++;
        bool typed = false;
        try {
            type();
            typed = check(Tok::LBrace);
        } catch (ParseError&) {
        }
        speculating_--;
        pos_ = save;
        if (typed) {
            e->hasArrowType = true;
            e->rets.push_back(type());
            e->body = block();
            e->span = {start, e->body->span.end};
            return e;
        }
        e->exprBody = expression();
        e->span = {start, e->exprBody->span.end};
        return e;
    }

    ExprP fstring(const Token& t) {
        auto e = std::make_unique<Expr>(EK::FStr);
        e->span = spanOf(t);
        const std::string& raw = t.text;
        std::string lit;
        size_t i = 0;
        auto flush = [&]() {
            std::string out, err;
            if (!unescape(lit, out, err)) errorSpan(spanOf(t), err);
            e->strParts.push_back(out);
            lit.clear();
        };
        while (i < raw.size()) {
            char c = raw[i];
            if (c == '\\' && i + 1 < raw.size()) {
                lit += raw.substr(i, 2);
                i += 2;
                continue;
            }
            if (c == '{' && i + 1 < raw.size() && raw[i + 1] == '{') {
                lit += '{';
                i += 2;
                continue;
            }
            if (c == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
                lit += '}';
                i += 2;
                continue;
            }
            if (c == '}') {
                int off = t.contentStart + (int)i;
                errorSpan({off, off + 1}, "single '}' in f-string (write '}}' for a literal brace)");
            }
            if (c == '{') {
                size_t j = i + 1;
                int depth = 1;
                char quote = 0;
                while (j < raw.size()) {
                    char d = raw[j];
                    if (quote) {
                        if (d == '\\') j++;
                        else if (d == quote) quote = 0;
                    } else if (d == '"' || d == '\'') {
                        quote = d;
                    } else if (d == '{') {
                        depth++;
                    } else if (d == '}') {
                        if (--depth == 0) break;
                    }
                    j++;
                }
                int off = t.contentStart + (int)i;
                if (j >= raw.size()) errorSpan({off, off + 1}, "unclosed '{' in f-string");
                flush();
                int b = t.contentStart + (int)i + 1;
                int en = t.contentStart + (int)j;
                bool blank = true;
                for (int k = b; k < en; k++) {
                    if (!isspace((unsigned char)text_[k])) blank = false;
                }
                if (blank) errorSpan({off, en + 1}, "empty expression in f-string");
                Parser sub(text_, b, en, fileId_, diags_);
                Token lexErr;
                if (sub.hasLexError(lexErr)) errorSpan({lexErr.start, lexErr.end}, lexErr.text);
                ExprP part;
                try {
                    part = sub.wholeExpression();
                } catch (ParseError&) {
                    errors_++;
                    throw;
                }
                e->args.push_back(std::move(part));
                i = j + 1;
                continue;
            }
            lit += c;
            i++;
        }
        flush();
        return e;
    }

    ExprP primary() {
        Token t = cur();
        switch (t.t) {
        case Tok::Int: {
            advance();
            auto e = std::make_unique<Expr>(EK::Int);
            e->ival = t.ival;
            e->span = spanOf(t);
            return e;
        }
        case Tok::Float: {
            advance();
            auto e = std::make_unique<Expr>(EK::Float);
            e->fval = t.fval;
            e->span = spanOf(t);
            return e;
        }
        case Tok::String: {
            advance();
            auto e = std::make_unique<Expr>(EK::Str);
            e->name = t.text;
            e->span = spanOf(t);
            return e;
        }
        case Tok::FString:
            advance();
            return fstring(t);
        case Tok::KwTrue: case Tok::KwFalse: {
            advance();
            auto e = std::make_unique<Expr>(EK::Bool);
            e->bval = t.t == Tok::KwTrue;
            e->span = spanOf(t);
            return e;
        }
        case Tok::KwNull: {
            advance();
            auto e = std::make_unique<Expr>(EK::Null);
            e->span = spanOf(t);
            return e;
        }
        case Tok::KwThis: {
            advance();
            auto e = std::make_unique<Expr>(EK::This);
            e->span = spanOf(t);
            return e;
        }
        case Tok::Ident: case Tok::KwStr: case Tok::KwInt: case Tok::KwFloat: case Tok::KwBool:
        case Tok::KwAny: {
            advance();
            auto e = std::make_unique<Expr>(EK::Ident);
            e->name = t.text;
            e->span = spanOf(t);
            return e;
        }
        case Tok::KwStart: {
            advance();
            ExprP call = postfix(primary());
            if (call->k != EK::Call) errorSpan(call->span, "'start' must be followed by a call, e.g. 'start patrol(goblin)'");
            auto e = std::make_unique<Expr>(EK::Start);
            e->span = {t.start, call->span.end};
            e->a = std::move(call);
            return e;
        }
        case Tok::LParen: {
            std::vector<Param> ps;
            if (tryLambdaParams(ps)) return lambda(t.start, std::move(ps));
            nl_.push_back(true);
            advance();
            ExprP inner = expression();
            Token close = expect(Tok::RParen, "')'");
            nl_.pop_back();
            inner->span = {t.start, close.end};
            return inner;
        }
        case Tok::LBracket: {
            nl_.push_back(true);
            advance();
            auto e = std::make_unique<Expr>(EK::List);
            while (!check(Tok::RBracket)) {
                e->args.push_back(expression());
                if (!match(Tok::Comma)) break;
            }
            Token close = expect(Tok::RBracket, "']' to close the list");
            nl_.pop_back();
            e->span = {t.start, close.end};
            return e;
        }
        case Tok::LBrace: {
            nl_.push_back(true);
            advance();
            auto e = std::make_unique<Expr>(EK::Dict);
            while (!check(Tok::RBrace)) {
                e->keys.push_back(expression());
                expect(Tok::Colon, "':' between dict key and value");
                e->args.push_back(expression());
                if (!match(Tok::Comma)) break;
            }
            Token close = expect(Tok::RBrace, "'}' to close the dict");
            nl_.pop_back();
            e->span = {t.start, close.end};
            return e;
        }
        default:
            error(t, "expected an expression, found " + describe(t));
        }
    }
};

} // namespace

std::vector<StmtP> parseProgram(const SourceFile& src, int fileId, std::vector<Diagnostic>& diags) {
    Parser p(src.text, 0, (int)src.text.size(), fileId, diags);
    return p.program();
}

bool parseBindings(const SourceFile& src, int fileId, BindingFile& out, std::vector<Diagnostic>& diags) {
    Parser p(src.text, 0, (int)src.text.size(), fileId, diags);
    return p.bindings(out);
}

std::string typeExprText(const TypeExpr* t) {
    if (!t) return "any";
    std::string s;
    if (t->kind == TypeExpr::List) s = "list<" + typeExprText(t->args[0].get()) + ">";
    else if (t->kind == TypeExpr::Dict) s = "dict<" + typeExprText(t->args[0].get()) + ", " + typeExprText(t->args[1].get()) + ">";
    else s = t->name;
    return t->nullable ? s + "?" : s;
}

std::string bindingSignature(const BindingFunc& f) {
    std::string s = "(";
    for (size_t i = 0; i < f.params.size(); i++) {
        if (i) s += ", ";
        s += typeExprText(f.params[i].type.get());
        if (f.params[i].def) s += "=";
        if (f.params[i].variadic) s += "...";
    }
    s += ")";
    if (f.rets.size() == 1) {
        s += " -> " + typeExprText(f.rets[0].get());
    } else if (f.rets.size() > 1) {
        s += " -> (";
        for (size_t i = 0; i < f.rets.size(); i++) s += (i ? ", " : "") + typeExprText(f.rets[i].get());
        s += ")";
    }
    return s;
}

SigParse parseSignature(const std::string& text) {
    std::vector<Diagnostic> ignored;
    Parser p(text, 0, (int)text.size(), -1, ignored);
    Token err;
    if (p.hasLexError(err)) return SigParse();
    return p.signature();
}

} // namespace tp
