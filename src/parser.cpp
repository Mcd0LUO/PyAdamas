// parser.cpp —— 语法驱动：语句分派、简单语句、import
#include "parser.h"

namespace py23 {

std::unique_ptr<Module> Parser::parse() {
        auto mod = std::make_unique<Module>();
        skip_newlines();
        while (!check(Tok::END)) {
            mod->body.push_back(parse_statement());
            skip_newlines();
        }
        return mod;
    }

std::string Parser::describe(const Token& t) {
        if (t.kind == Tok::END) return "end of file";
        if (t.kind == Tok::NEWLINE) return "newline";
        if (t.kind == Tok::INDENT) return "indent";
        if (t.kind == Tok::DEDENT) return "dedent";
        if (!t.text.empty()) return t.text;
        return tok_name(t.kind);
    }


StmtPtr Parser::parse_statement() {
        if (check(Tok::INDENT))
            throw err_at("unexpected indent", cur().line, cur().col);
        if (check(Tok::DEDENT))
            throw err_at("unexpected dedent", cur().line, cur().col);
        switch (cur().kind) {
            case Tok::KW_IF: return parse_if();
            case Tok::KW_WHILE: return parse_while();
            case Tok::KW_FOR: return parse_for(false);
            case Tok::KW_DEF: return parse_funcdef({}, false);
            case Tok::KW_CLASS: return parse_classdef({});
            case Tok::KW_TRY: return parse_try();
            case Tok::KW_WITH: return parse_with(false);
            case Tok::AT: return parse_decorated();
            case Tok::KW_ASYNC: {
                int ln = cur().line, cl = cur().col;
                advance();
                if (check(Tok::KW_DEF)) return parse_funcdef({}, true);
                if (check(Tok::KW_FOR)) {
                    if (async_depth_ == 0)
                        throw err_at("'async for' outside async function", ln, cl);
                    return parse_for(true);
                }
                if (check(Tok::KW_WITH)) {
                    if (async_depth_ == 0)
                        throw err_at("'async with' outside async function", ln, cl);
                    return parse_with(true);
                }
                throw err_at("expected 'def', 'for' or 'with' after 'async'", ln, cl);
            }
            default: return parse_simple_statement();
        }
    }

StmtPtr Parser::parse_decorated() {
        std::vector<ExprPtr> decorators;
        while (check(Tok::AT)) {
            advance();
            decorators.push_back(parse_namedexpr());
            expect(Tok::NEWLINE, "newline after decorator");
        }
        skip_newlines();
        bool is_async = false;
        if (check(Tok::KW_ASYNC)) { advance(); is_async = true; }
        if (check(Tok::KW_DEF)) return parse_funcdef(std::move(decorators), is_async);
        if (check(Tok::KW_CLASS)) return parse_classdef(std::move(decorators));
        throw err("expected 'def' or 'class' after decorator");
    }

std::vector<StmtPtr> Parser::parse_suite() {
        std::vector<StmtPtr> body;
        if (check(Tok::NEWLINE)) {
            advance();
            if (!check(Tok::INDENT))
                throw err("expected an indented block");
            advance();
            skip_newlines();
            while (!check(Tok::DEDENT) && !check(Tok::END)) {
                body.push_back(parse_statement());
                skip_newlines();
            }
            if (check(Tok::DEDENT)) advance();
            else throw err("unexpected end of file, expected dedent");
        } else {
            // 单行：if x: a = 1; b = 2
            for (;;) {
                body.push_back(parse_simple_statement());
                if (match(Tok::SEMICOLON)) {
                    if (check(Tok::NEWLINE) || check(Tok::END)) break;
                    continue;
                }
                break;
            }
            if (!check(Tok::NEWLINE) && !check(Tok::END) && !check(Tok::DEDENT))
                throw err(std::format("unexpected '{}' after statement", describe(cur())));
            match(Tok::NEWLINE);
        }
        if (body.empty()) throw err("expected at least one statement in block");
        return body;
    }

StmtPtr Parser::parse_import() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto n = node_at<Import>(ln, cl);
        do {
            Alias a;
            a.name = parse_dotted_name();
            if (check(Tok::KW_AS)) { advance(); a.asname = expect(Tok::NAME, "alias").text; }
            n->names.push_back(std::move(a));
        } while (match(Tok::COMMA));
        return n;
    }

StmtPtr Parser::parse_import_from() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto n = node_at<ImportFrom>(ln, cl);
        while (check(Tok::DOT) || check(Tok::ELLIPSIS)) {
            if (check(Tok::ELLIPSIS)) { advance(); n->level += 3; }
            else { advance(); n->level += 1; }
        }
        if (check(Tok::NAME)) n->module = parse_dotted_name();
        expect(Tok::KW_IMPORT, "'import'");
        if (check(Tok::STAR)) { advance(); n->names.push_back(Alias{"*", ""}); }
        else if (check(Tok::LPAREN)) {
            advance();
            while (!check(Tok::RPAREN)) {
                Alias a;
                a.name = expect(Tok::NAME, "imported name").text;
                if (check(Tok::KW_AS)) { advance(); a.asname = expect(Tok::NAME, "alias").text; }
                n->names.push_back(std::move(a));
                if (!match(Tok::COMMA)) break;
            }
            expect(Tok::RPAREN, "')'");
        } else {
            do {
                Alias a;
                a.name = expect(Tok::NAME, "imported name").text;
                if (check(Tok::KW_AS)) { advance(); a.asname = expect(Tok::NAME, "alias").text; }
                n->names.push_back(std::move(a));
            } while (match(Tok::COMMA));
        }
        return n;
    }

std::string Parser::parse_dotted_name() {
        std::string name = expect(Tok::NAME, "module name").text;
        while (check(Tok::DOT)) {
            advance();
            name += ".";
            name += expect(Tok::NAME, "name after '.'").text;
        }
        return name;
    }


}  // namespace py23
