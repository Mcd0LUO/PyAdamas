// syntax/func.cpp —— 函数与类定义、参数、lambda
#include "syntax/parser.h"

namespace pyadamas {

namespace {

// Follow type positions, not ordinary uses of a variable named big.
bool annotation_mentions_big(const Expr* e, int depth = 0) {
    if (depth > 8 || !e) return false;
    if (auto* n = dynamic_cast<const Name*>(e)) return n->id == "big";
    if (auto* c = dynamic_cast<const Constant*>(e); c && c->ck == Constant::CK::Str) {
        // The type checker treats quoted annotations as expressions too.
        try {
            Lexer lexer(c->s);
            Parser parser(lexer.run());
            return annotation_mentions_big(parser.parse_fstring_expr().get(), depth + 1);
        } catch (const LexError&) {
            return false;
        } catch (const ParseError&) {
            return false;
        }
    }
    if (auto* s = dynamic_cast<const Subscript*>(e)) {
        if (auto* t = dynamic_cast<const TupleExpr*>(s->slice.get()))
            return std::ranges::any_of(t->elts, [&](const auto& elt) {
                return annotation_mentions_big(elt.get(), depth);
            });
        return annotation_mentions_big(s->slice.get(), depth);
    }
    // Callable[[big], int] 这类注解：ListExpr 里的元素也要递归（0.640）。
    if (auto* l = dynamic_cast<const ListExpr*>(e))
        return std::ranges::any_of(l->elts, [&](const auto& elt) {
            return annotation_mentions_big(elt.get(), depth + 1);
        });
    if (auto* b = dynamic_cast<const BinOp*>(e); b && b->op == Tok::VBAR)
        return annotation_mentions_big(b->left.get(), depth) ||
               annotation_mentions_big(b->right.get(), depth);
    return false;
}

}  // namespace

ExprPtr Parser::parse_annotation_expr() {
    auto e = parse_namedexpr();
    mentions_big_ = mentions_big_ || annotation_mentions_big(e.get());
    return e;
}

void Parser::skip_type_params() {
    if (!check(Tok::LBRACKET)) return;
    int depth = 0;
    do {
        if (check(Tok::LBRACKET)) ++depth;
        else if (check(Tok::RBRACKET)) --depth;
        else if (check(Tok::NEWLINE) || check(Tok::END))
            throw err("unterminated type parameter list");
        advance();
    } while (depth > 0);
}

StmtPtr Parser::parse_funcdef(std::vector<ExprPtr> decorators, bool is_async) {
        int ln = cur().line, cl = cur().col;
        advance();  // def
        auto node = node_at<FunctionDef>(ln, cl);
        node->is_async = is_async;
        node->decorators = std::move(decorators);
        node->name = expect(Tok::NAME, "function name").text;
        skip_type_params();  // PEP 695：def f[T](...) —— 类型参数擦除
        expect(Tok::LPAREN, "'(' after function name");
        node->args = parse_arguments();
        expect(Tok::RPAREN, "')' after parameters");
        if (match(Tok::RARROW)) node->returns = parse_annotation_expr();
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
            if (match(Tok::COLON)) arg.annotation = parse_annotation_expr();
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
                    if (match(Tok::COLON)) va.annotation = parse_annotation_expr();
                    a.vararg = std::make_unique<Arg>(std::move(va));
                    kwonly_mode = true;
                }
            } else if (check(Tok::DOUBLESTAR)) {
                advance();
                Arg ka;
                ka.name = expect(Tok::NAME, "parameter name after '**'").text;
                if (match(Tok::COLON)) ka.annotation = parse_annotation_expr();
                a.kwarg = std::make_unique<Arg>(std::move(ka));
                match(Tok::COMMA);  // 允许 **kwargs, 的尾逗号
                if (!check(Tok::RPAREN)) throw err("'**' parameter must be last");
                break;
            } else if (check(Tok::SLASH)) {
                // `/` 的位置约束（0.640）：必须已有一个位置参数、之前没出现过、
                // 且必须在 `*` 之前。CPython 对 def f(/) / def f(a, /, b, /) /
                // def f(*, a, /) 都报 SyntaxError，原来全部放行。
                if (!a.kwonly.empty() || a.vararg)
                    throw err_at("'/' must be ahead of '*'", cur().line, cur().col);
                if (!a.posonly.empty()) throw err_at("'/' may appear only once", cur().line,
                                                    cur().col);
                if (a.args.empty()) throw err_at("'/' must follow at least one parameter",
                                                 cur().line, cur().col);
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
        skip_type_params();  // PEP 695：class C[T]: —— 类型参数擦除
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
        // 类体是独立作用域（0.640）：不能 break/continue 到外层循环，也不能在
        // 同步类体里 await（类体语句在函数帧内降级，但语义上不是循环体）。
        int saved_loop_c = loop_depth_;
        int saved_async_c = async_depth_;
        loop_depth_ = 0;
        async_depth_ = 0;
        node->body = parse_suite();
        async_depth_ = saved_async_c;
        loop_depth_ = saved_loop_c;
        return node;
    }

ExprPtr Parser::parse_lambda() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto n = node_at<Lambda>(ln, cl);
        if (!check(Tok::COLON)) n->args = parse_lambda_params();
        expect(Tok::COLON, "':' in lambda");
        // lambda 体是一个函数体：里面可以有 yield（生成器 lambda）。
        // 与 def 同口径（0.640）：lambda 是同步函数，体内不能 await、
        // 也不能 break/continue 到外层循环。
        ++func_depth_;
        int saved_loop = loop_depth_;
        int saved_async = async_depth_;
        loop_depth_ = 0;
        async_depth_ = 0;
        n->body = parse_namedexpr();
        async_depth_ = saved_async;
        loop_depth_ = saved_loop;
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
                match(Tok::COMMA);  // lambda **kw,: kw —— 尾逗号合法
                break;
            } else if (check(Tok::SLASH)) {
                // `/` 的位置约束（0.640）：必须已有一个位置参数、之前没出现过、
                // 且必须在 `*` 之前。CPython 对 def f(/) / def f(a, /, b, /) /
                // def f(*, a, /) 都报 SyntaxError，原来全部放行。
                if (kwonly || a.vararg)
                    throw err_at("'/' must be ahead of '*'", cur().line, cur().col);
                if (!a.posonly.empty()) throw err_at("'/' may appear only once", cur().line,
                                                    cur().col);
                if (a.args.empty()) throw err_at("'/' must follow at least one parameter",
                                                 cur().line, cur().col);
                advance();
                for (auto& p : a.args) a.posonly.push_back(std::move(p));
                a.args.clear();
            } else {
                Arg arg;
                arg.name = expect(Tok::NAME, "parameter name").text;
                if (kwonly) {
                    // keyword-only 的默认值必须进 kw_defaults（与 parse_arguments 同口径）：
                    // 塞进 a.defaults 会被按位置从末尾对齐，`lambda a, *, b=2` 里 b 的 2
                    // 会被当成 a 的默认值，b 反而变成必填（0.640）。
                    if (match(Tok::EQUAL)) a.kw_defaults.push_back(parse_namedexpr());
                    else a.kw_defaults.push_back(nullptr);
                    a.kwonly.push_back(std::move(arg));
                } else {
                    if (match(Tok::EQUAL)) a.defaults.push_back(parse_namedexpr());
                    a.args.push_back(std::move(arg));
                }
            }
            if (!match(Tok::COMMA)) break;
            // 尾逗号：lambda a, b, /, : ... 是合法的；'/' 后面的逗号
            // 以前会被当成「还要再来一个参数」，于是把 ':' 当参数名报错
            if (check(Tok::COLON)) break;
        }
        return a;
    }


}  // namespace pyadamas
