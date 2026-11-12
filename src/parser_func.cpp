// parser_func.cpp —— 函数与类定义、参数、lambda
#include "parser.h"

namespace pyadamas {

StmtPtr Parser::parse_funcdef(std::vector<ExprPtr> decorators, bool is_async) {
        int ln = cur().line, cl = cur().col;
        advance();  // def
        auto node = node_at<FunctionDef>(ln, cl);
        node->is_async = is_async;
        node->decorators = std::move(decorators);
        node->name = expect(Tok::NAME, "function name").text;
        expect(Tok::LPAREN, "'(' after function name");
        node->args = parse_arguments();
        expect(Tok::RPAREN, "')' after parameters");
        if (match(Tok::RARROW)) node->returns = parse_namedexpr();
        expect(Tok::COLON, "':' after function signature");
        ++func_depth_;
        int saved_loop = loop_depth_;
        int saved_async = async_depth_;
        loop_depth_ = 0;
        // 同步函数体内的 await 非法；嵌套的 async def 仍然合法
        async_depth_ = is_async ? saved_async + 1 : 0;
        node->body = parse_suite();
        async_depth_ = saved_async;
        loop_depth_ = saved_loop;
        --func_depth_;
        return node;
    }

Arguments Parser::parse_arguments() {
        Arguments a;
        bool kwonly_mode = false;
        size_t default_start = 0;
        bool seen_default = false;

        auto parse_one = [&](std::vector<Arg>& dest) {
            Arg arg;
            arg.name = expect(Tok::NAME, "parameter name").text;
            if (match(Tok::COLON)) arg.annotation = parse_namedexpr();
            if (check(Tok::EQUAL)) {
                if (kwonly_mode) {
                    advance();
                    a.kw_defaults.push_back(parse_namedexpr());
                    dest.push_back(std::move(arg));
                } else {
                    advance();
                    a.defaults.push_back(parse_namedexpr());
                    seen_default = true;
                    dest.push_back(std::move(arg));
                }
            } else {
                if (kwonly_mode) a.kw_defaults.push_back(nullptr);
                if (seen_default && !kwonly_mode)
                    throw err("non-default argument follows default argument");
                dest.push_back(std::move(arg));
            }
        };

        if (check(Tok::RPAREN)) return a;

        while (true) {
            if (check(Tok::STAR)) {
                advance();
                if (check(Tok::COMMA) || check(Tok::RPAREN)) {
                    kwonly_mode = true;  // 单独的 * 表示后面的都是 kw-only
                } else {
                    Arg va;
                    va.name = expect(Tok::NAME, "parameter name after '*'").text;
                    if (match(Tok::COLON)) va.annotation = parse_namedexpr();
                    a.vararg = std::make_unique<Arg>(std::move(va));
                    kwonly_mode = true;
                }
            } else if (check(Tok::DOUBLESTAR)) {
                advance();
                Arg ka;
                ka.name = expect(Tok::NAME, "parameter name after '**'").text;
                if (match(Tok::COLON)) ka.annotation = parse_namedexpr();
                a.kwarg = std::make_unique<Arg>(std::move(ka));
                match(Tok::COMMA);  // 允许 **kwargs, 的尾逗号
                if (!check(Tok::RPAREN)) throw err("'**' parameter must be last");
                break;
            } else if (check(Tok::SLASH)) {
                advance();
                for (auto& p : a.args) a.posonly.push_back(std::move(p));
                a.args.clear();
            } else {
                if (kwonly_mode) parse_one(a.kwonly);
                else parse_one(a.args);
            }

            if (!match(Tok::COMMA)) break;
            // lambda 的终止符是 ':'，不是 ')'
            if (check(Tok::RPAREN) || check(Tok::COLON)) break;
        }
        (void)default_start;
        return a;
    }

StmtPtr Parser::parse_classdef(std::vector<ExprPtr> decorators) {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<ClassDef>(ln, cl);
        node->decorators = std::move(decorators);
        node->name = expect(Tok::NAME, "class name").text;
        if (match(Tok::LPAREN)) {
            if (!check(Tok::RPAREN)) {
                while (true) {
                    if (check(Tok::DOUBLESTAR)) {
                        advance();
                        Keyword kw;
                        kw.value = parse_namedexpr();
                        node->keywords.push_back(std::move(kw));
                    } else if (check(Tok::NAME) && peek_tok().kind == Tok::EQUAL) {
                        Keyword kw;
                        kw.arg = advance().text;
                        advance();
                        kw.value = parse_namedexpr();
                        node->keywords.push_back(std::move(kw));
                    } else {
                        node->bases.push_back(parse_namedexpr());
                    }
                    if (!match(Tok::COMMA)) break;
                    if (check(Tok::RPAREN)) break;
                }
            }
            expect(Tok::RPAREN, "')' after class bases");
        }
        expect(Tok::COLON, "':' after class header");
        node->body = parse_suite();
        return node;
    }

ExprPtr Parser::parse_lambda() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto n = node_at<Lambda>(ln, cl);
        if (!check(Tok::COLON)) n->args = parse_lambda_params();
        expect(Tok::COLON, "':' in lambda");
        // lambda 体是一个函数体：里面可以有 yield（生成器 lambda）
        ++func_depth_;
        n->body = parse_namedexpr();
        --func_depth_;
        return n;
    }

Arguments Parser::parse_lambda_params() {
        Arguments a;
        bool kwonly = false;
        while (true) {
            if (check(Tok::STAR)) {
                advance();
                if (check(Tok::COMMA)) kwonly = true;
                else {
                    Arg va;
                    va.name = expect(Tok::NAME, "parameter name").text;
                    a.vararg = std::make_unique<Arg>(std::move(va));
                    kwonly = true;
                }
            } else if (check(Tok::DOUBLESTAR)) {
                advance();
                Arg ka;
                ka.name = expect(Tok::NAME, "parameter name").text;
                a.kwarg = std::make_unique<Arg>(std::move(ka));
                break;
            } else if (check(Tok::SLASH)) {
                advance();
                for (auto& p : a.args) a.posonly.push_back(std::move(p));
                a.args.clear();
            } else {
                Arg arg;
                arg.name = expect(Tok::NAME, "parameter name").text;
                if (check(Tok::EQUAL)) {
                    advance();
                    a.defaults.push_back(parse_namedexpr());
                }
                if (kwonly) a.kwonly.push_back(std::move(arg));
                else a.args.push_back(std::move(arg));
            }
            if (!match(Tok::COMMA)) break;
            // 尾逗号：lambda a, b, /, : ... 是合法的；'/' 后面的逗号
            // 以前会被当成「还要再来一个参数」，于是把 ':' 当参数名报错
            if (check(Tok::COLON)) break;
        }
        return a;
    }


}  // namespace pyadamas
