// 由 src/parser.h 拆出的实现。声明见 /tmp/pr.h。
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

StmtPtr Parser::parse_if() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<If>(ln, cl);
        If::Branch first;
        first.cond = parse_namedexpr();
        expect(Tok::COLON, "':' after if condition");
        first.body = parse_suite();
        node->branches.push_back(std::move(first));

        for (;;) {
            if (check(Tok::KW_ELIF)) {
                advance();
                If::Branch b;
                b.cond = parse_namedexpr();
                expect(Tok::COLON, "':' after elif condition");
                b.body = parse_suite();
                node->branches.push_back(std::move(b));
                continue;
            }
            if (check(Tok::KW_ELSE)) {
                advance();
                expect(Tok::COLON, "':' after else");
                node->orelse = parse_suite();
            }
            break;
        }
        return node;
    }

StmtPtr Parser::parse_while() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<While>(ln, cl);
        node->cond = parse_namedexpr();
        expect(Tok::COLON, "':' after while condition");
        ++loop_depth_;
        node->body = parse_suite();
        --loop_depth_;
        if (check(Tok::KW_ELSE)) {
            advance();
            expect(Tok::COLON, "':' after else");
            node->orelse = parse_suite();
        }
        return node;
    }

StmtPtr Parser::parse_for(bool is_async) {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<For>(ln, cl);
        node->is_async = is_async;
        node->target = parse_target_list();
        expect(Tok::KW_IN, "'in' in for statement");
        node->iter = parse_expr_list_no_in();
        expect(Tok::COLON, "':' after for clause");
        ++loop_depth_;
        node->body = parse_suite();
        --loop_depth_;
        if (check(Tok::KW_ELSE)) {
            advance();
            expect(Tok::COLON, "':' after else");
            node->orelse = parse_suite();
        }
        return node;
    }

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

StmtPtr Parser::parse_try() {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<Try>(ln, cl);
        expect(Tok::COLON, "':' after try");
        node->body = parse_suite();

        while (check(Tok::KW_EXCEPT)) {
            advance();
            ExceptHandler h;
            if (!check(Tok::COLON)) {
                h.type = parse_namedexpr();
                if (check(Tok::KW_AS)) {
                    advance();
                    h.name = expect(Tok::NAME, "exception name").text;
                } else if (check(Tok::COMMA)) {
                    // Python 2 风格，给出明确错误
                    throw err("Python 2 style 'except X, e' is not supported, use 'as'");
                }
            }
            expect(Tok::COLON, "':' after except");
            h.body = parse_suite();
            node->handlers.push_back(std::move(h));
        }
        if (check(Tok::KW_ELSE)) {
            advance();
            expect(Tok::COLON, "':' after else");
            node->orelse = parse_suite();
        }
        if (check(Tok::KW_FINALLY)) {
            advance();
            expect(Tok::COLON, "':' after finally");
            node->finalbody = parse_suite();
        }
        if (node->handlers.empty() && node->finalbody.empty())
            throw err_at("try must have at least one except or finally clause", ln, cl);
        return node;
    }

StmtPtr Parser::parse_with(bool is_async) {
        int ln = cur().line, cl = cur().col;
        advance();
        auto node = node_at<With>(ln, cl);
        node->is_async = is_async;

        // Python 3.10 的括号形式：with (a as x, b as y):
        // 只有括号内出现顶层 as、且右括号后紧跟冒号时才按「项目列表」解析，
        // 否则 with (a, b): 仍然是单个元组上下文。
        bool paren_items = check(Tok::LPAREN) && paren_with_items();
        if (paren_items) advance();

        while (true) {
            WithItem item;
            item.context = parse_namedexpr();
            if (check(Tok::KW_AS)) {
                advance();
                item.optional_vars = parse_target();
            }
            node->items.push_back(std::move(item));
            if (!match(Tok::COMMA)) break;
            if (paren_items && check(Tok::RPAREN)) break;
        }
        if (paren_items) expect(Tok::RPAREN, "')' after with items");
        expect(Tok::COLON, "':' after with clause");
        node->body = parse_suite();
        return node;
    }

bool Parser::paren_with_items() const {
        int depth = 0;
        bool has_as = false;
        for (size_t i = pos_; i < toks_.size(); ++i) {
            Tok k = toks_[i].kind;
            if (k == Tok::LPAREN || k == Tok::LBRACKET || k == Tok::LBRACE) {
                ++depth;
            } else if (k == Tok::RPAREN || k == Tok::RBRACKET || k == Tok::RBRACE) {
                --depth;
                if (depth == 0) {
                    if (!has_as) return false;
                    return i + 1 < toks_.size() && toks_[i + 1].kind == Tok::COLON;
                }
            } else if (k == Tok::KW_AS && depth == 1) {
                has_as = true;
            } else if (k == Tok::NEWLINE || k == Tok::END) {
                return false;
            }
        }
        return false;
    }

StmtPtr Parser::parse_simple_statement() {
        int ln = cur().line, cl = cur().col;

        // match 是软关键字：只有 "match <subject>: NEWLINE INDENT case" 才是
        // match 语句。match[0]、match.attr、match, x = ... 都是普通标识符。
        if (check(Tok::NAME) && cur().text == "match" && looks_like_match_stmt()) {
            throw err_at("'match' statement is not supported yet (structural pattern matching)",
                         ln, cl);
        }

        switch (cur().kind) {
            case Tok::KW_RETURN: {
                advance();
                if (func_depth_ == 0) throw err_at("'return' outside function", ln, cl);
                auto n = node_at<Return>(ln, cl);
                if (!at_stmt_end()) n->value = parse_expr_list();
                return n;
            }
            case Tok::KW_BREAK: {
                advance();
                if (loop_depth_ == 0) throw err_at("'break' outside loop", ln, cl);
                return node_at<Break>(ln, cl);
            }
            case Tok::KW_CONTINUE: {
                advance();
                if (loop_depth_ == 0) throw err_at("'continue' not properly in loop", ln, cl);
                return node_at<Continue>(ln, cl);
            }
            case Tok::KW_PASS: advance(); return node_at<Pass>(ln, cl);
            case Tok::KW_RAISE: {
                advance();
                auto n = node_at<Raise>(ln, cl);
                if (!at_stmt_end()) {
                    n->exc = parse_namedexpr();
                    if (check(Tok::KW_FROM)) {
                        advance();
                        n->cause = parse_namedexpr();
                    }
                }
                return n;
            }
            case Tok::KW_ASSERT: {
                advance();
                auto n = node_at<Assert>(ln, cl);
                n->test = parse_namedexpr();
                if (match(Tok::COMMA)) n->msg = parse_namedexpr();
                return n;
            }
            case Tok::KW_GLOBAL: {
                advance();
                auto n = node_at<Global>(ln, cl);
                do { n->names.push_back(expect(Tok::NAME, "name").text); } while (match(Tok::COMMA));
                return n;
            }
            case Tok::KW_NONLOCAL: {
                advance();
                auto n = node_at<Nonlocal>(ln, cl);
                do { n->names.push_back(expect(Tok::NAME, "name").text); } while (match(Tok::COMMA));
                return n;
            }
            case Tok::KW_DEL: {
                advance();
                auto n = node_at<Delete>(ln, cl);
                do { n->targets.push_back(parse_target()); } while (match(Tok::COMMA));
                return n;
            }
            case Tok::KW_IMPORT: return parse_import();
            case Tok::KW_FROM: return parse_import_from();
            default: break;
        }

        auto first = parse_expr_list();

        // 赋值
        if (check(Tok::EQUAL)) {
            auto n = node_at<Assign>(ln, cl);
            n->targets.push_back(std::move(first));
            while (match(Tok::EQUAL)) {
                check_assignable(*n->targets.back());
                auto rhs = parse_expr_list();
                if (check(Tok::EQUAL)) {
                    n->targets.push_back(std::move(rhs));
                } else {
                    n->value = std::move(rhs);
                    break;
                }
            }
            if (!n->value) throw err("invalid assignment target");
            return n;
        }

        // 增强赋值
        if (is_augassign(cur().kind)) {
            auto n = node_at<AugAssign>(ln, cl);
            n->target = std::move(first);
            check_assignable(*n->target);
            n->op = advance().kind;
            n->value = parse_expr_list();
            return n;
        }

        // 类型标注赋值
        if (check(Tok::COLON)) {
            auto n = node_at<AnnAssign>(ln, cl);
            n->target = std::move(first);
            check_assignable(*n->target);
            advance();
            n->annotation = parse_namedexpr();
            if (match(Tok::EQUAL)) n->value = parse_expr_list();
            return n;
        }

        // 纯表达式
        auto n = node_at<ExprStmt>(ln, cl);
        n->value = std::move(first);
        return n;
    }

void Parser::check_assignable(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name:
            case ExprKind::Attribute:
            case ExprKind::Subscript:
                return;
            case ExprKind::Tuple:
            case ExprKind::List: {
                const auto& elts = (e.kind == ExprKind::Tuple)
                                       ? static_cast<const TupleExpr&>(e).elts
                                       : static_cast<const ListExpr&>(e).elts;
                for (auto& it : elts) check_assignable(*it);
                return;
            }
            case ExprKind::Starred:
                return;
            default:
                throw err_at("cannot assign to this expression", e.line, e.col);
        }
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

ExprPtr Parser::parse_expr_list() {
        int ln = cur().line, cl = cur().col;
        auto first = parse_namedexpr();
        if (!check(Tok::COMMA)) return first;

        auto t = node_at<TupleExpr>(ln, cl);
        t->elts.push_back(std::move(first));
        while (match(Tok::COMMA)) {
            if (at_expr_list_end()) break;
            t->elts.push_back(parse_namedexpr());
        }
        return t;
    }

ExprPtr Parser::parse_namedexpr() {
        DepthGuard guard(*this, expr_depth_, kMaxExprDepth, "expression nested too deeply");
        // 海象运算符：(n := f()) —— 优先级最低，右结合
        if (check(Tok::NAME) && peek_tok().kind == Tok::COLONEQUAL) {
            int ln = cur().line, cl = cur().col;
            auto n = node_at<NamedExpr>(ln, cl);
            n->target = std::make_unique<Name>(advance().text);
            advance();  // :=
            n->value = parse_namedexpr();
            return n;
        }
        return parse_lambda_or_ifexp();
    }

ExprPtr Parser::parse_ifexp() {
        auto body = parse_or();
        if (check(Tok::KW_IF)) {
            int ln = cur().line, cl = cur().col;
            advance();
            auto n = node_at<IfExp>(ln, cl);
            n->body = std::move(body);
            n->test = parse_or();
            expect(Tok::KW_ELSE, "'else' in conditional expression");
            n->orelse = parse_ifexp();
            return n;
        }
        return body;
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

ExprPtr Parser::parse_or() {
        auto first = parse_and();
        if (!check(Tok::KW_OR)) return first;
        int ln = cur().line, cl = cur().col;
        auto n = node_at<BoolOp>(ln, cl);
        n->op = Tok::KW_OR;
        n->values.push_back(std::move(first));
        while (match(Tok::KW_OR)) n->values.push_back(parse_and());
        return n;
    }

ExprPtr Parser::parse_and() {
        auto first = parse_not();
        if (!check(Tok::KW_AND)) return first;
        int ln = cur().line, cl = cur().col;
        auto n = node_at<BoolOp>(ln, cl);
        n->op = Tok::KW_AND;
        n->values.push_back(std::move(first));
        while (match(Tok::KW_AND)) n->values.push_back(parse_not());
        return n;
    }

ExprPtr Parser::parse_not() {
        DepthGuard guard(*this, not_depth_, kMaxUnaryDepth, "expression nested too deeply");
        if (check(Tok::KW_NOT)) {
            int ln = cur().line, cl = cur().col;
            advance();
            return make_unary(Tok::KW_NOT, parse_not(), ln, cl);
        }
        return parse_comparison();
    }

ExprPtr Parser::parse_comparison() {
        auto left = parse_bitor();
        std::vector<CmpOp> ops;
        std::vector<ExprPtr> comps;

        for (;;) {
            CmpOp op;
            if (check(Tok::LESS)) { advance(); op = CmpOp::Lt; }
            else if (check(Tok::LESSEQUAL)) { advance(); op = CmpOp::Le; }
            else if (check(Tok::GREATER)) { advance(); op = CmpOp::Gt; }
            else if (check(Tok::GREATEREQUAL)) { advance(); op = CmpOp::Ge; }
            else if (check(Tok::EQUALEQUAL)) { advance(); op = CmpOp::Eq; }
            else if (check(Tok::NOTEQUAL)) { advance(); op = CmpOp::Ne; }
            else if (check(Tok::KW_IN)) { advance(); op = CmpOp::In; }
            else if (check(Tok::KW_NOT) && peek_tok().kind == Tok::KW_IN) {
                advance(); advance();
                op = CmpOp::NotIn;
            } else if (check(Tok::KW_IS)) {
                advance();
                if (check(Tok::KW_NOT)) { advance(); op = CmpOp::IsNot; }
                else op = CmpOp::Is;
            } else {
                break;
            }
            ops.push_back(op);
            comps.push_back(parse_bitor());
        }

        if (ops.empty()) return left;
        int ln = left->line, cl = left->col;
        auto n = node_at<Compare>(ln, cl);
        n->left = std::move(left);
        n->ops = std::move(ops);
        n->comparators = std::move(comps);
        return n;
    }

ExprPtr Parser::parse_bitor() {
        auto lhs = parse_bitxor();
        while (check(Tok::VBAR)) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_bitxor(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_bitxor() {
        auto lhs = parse_bitand();
        while (check(Tok::CIRCUMFLEX)) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_bitand(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_bitand() {
        auto lhs = parse_shift();
        while (check(Tok::AMPER)) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_shift(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_shift() {
        auto lhs = parse_arith();
        while (check_any({Tok::LSHIFT, Tok::RSHIFT})) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_arith(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_arith() {
        auto lhs = parse_term();
        while (check_any({Tok::PLUS, Tok::MINUS})) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_term(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_term() {
        auto lhs = parse_factor();
        while (check_any({Tok::STAR, Tok::SLASH, Tok::DOUBLESLASH, Tok::PERCENT, Tok::AT})) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            lhs = make_binop(op, std::move(lhs), parse_factor(), ln, cl);
        }
        return lhs;
    }

ExprPtr Parser::parse_factor() {
        DepthGuard guard(*this, unary_depth_, kMaxUnaryDepth, "expression nested too deeply");
        if (check_any({Tok::PLUS, Tok::MINUS, Tok::TILDE})) {
            int ln = cur().line, cl = cur().col;
            Tok op = advance().kind;
            return make_unary(op, parse_factor(), ln, cl);
        }
        return parse_power();
    }

ExprPtr Parser::parse_power() {
        auto base = parse_await();
        if (check(Tok::DOUBLESTAR)) {
            int ln = cur().line, cl = cur().col;
            advance();
            return make_binop(Tok::DOUBLESTAR, std::move(base), parse_factor(), ln, cl);
        }
        return base;
    }

ExprPtr Parser::parse_await() {
        if (check(Tok::KW_AWAIT)) {
            int ln = cur().line, cl = cur().col;
            // 生成器表达式是延迟求值的，await 写在里面任何地方都合法
            if (async_depth_ == 0 && genexp_depth_ == 0)
                throw err_at("'await' outside async function", ln, cl);
            advance();
            auto n = node_at<Await>(ln, cl);
            n->value = parse_primary();
            return n;
        }
        return parse_primary();
    }

ExprPtr Parser::parse_primary() {
        auto e = parse_atom();
        for (;;) {
            if (check(Tok::LPAREN)) {
                int ln = cur().line, cl = cur().col;
                advance();
                auto n = node_at<Call>(ln, cl);
                n->func = std::move(e);
                if (!check(Tok::RPAREN)) {
                    // 单个生成器表达式参数：f(x for x in y)
                    if (looks_like_genexp()) {
                        auto gen = std::make_unique<GeneratorExp>();
                        gen->line = cur().line;
                        gen->col = cur().col;
                        gen->elt = parse_namedexpr();
                        while (check(Tok::KW_FOR) || check(Tok::KW_ASYNC))
                            gen->generators.push_back(parse_comp_for());
                        n->args.push_back(std::move(gen));
                    } else {
                        while (true) {
                            if (check(Tok::STAR)) {
                                int sl = cur().line, sc = cur().col;
                                advance();
                                auto st = node_at<Starred>(sl, sc);
                                st->value = parse_namedexpr();
                                n->args.push_back(std::move(st));
                            } else if (check(Tok::DOUBLESTAR)) {
                                advance();
                                Keyword kw;
                                kw.value = parse_namedexpr();
                                n->keywords.push_back(std::move(kw));
                            } else if (check(Tok::NAME) && peek_tok().kind == Tok::EQUAL) {
                                Keyword kw;
                                kw.arg = advance().text;
                                advance();
                                kw.value = parse_namedexpr();
                                n->keywords.push_back(std::move(kw));
                            } else {
                                n->args.push_back(parse_namedexpr());
                            }
                            if (!match(Tok::COMMA)) break;
                            if (check(Tok::RPAREN)) break;
                        }
                    }
                }
                expect(Tok::RPAREN, "')'");
                e = std::move(n);
            } else if (check(Tok::LBRACKET)) {
                int ln = cur().line, cl = cur().col;
                advance();
                auto n = node_at<Subscript>(ln, cl);
                n->value = std::move(e);
                n->slice = parse_subscript_slice();
                expect(Tok::RBRACKET, "']'");
                e = std::move(n);
            } else if (check(Tok::DOT)) {
                int ln = cur().line, cl = cur().col;
                advance();
                auto n = node_at<Attribute>(ln, cl);
                n->value = std::move(e);
                n->attr = expect(Tok::NAME, "attribute name after '.'").text;
                e = std::move(n);
            } else {
                break;
            }
        }
        return e;
    }

ExprPtr Parser::parse_subscript_slice() {
        ExprPtr first;
        if (!check(Tok::COLON)) first = parse_namedexpr();

        if (check(Tok::COLON)) {
            int ln = cur().line, cl = cur().col;
            advance();
            auto s = node_at<SliceExpr>(ln, cl);
            s->lower = std::move(first);
            if (!check_any({Tok::RBRACKET, Tok::COLON, Tok::COMMA})) s->upper = parse_namedexpr();
            if (match(Tok::COLON)) {
                if (!check_any({Tok::RBRACKET, Tok::COMMA})) s->step = parse_namedexpr();
            }
            first = std::move(s);
        }

        if (!check(Tok::COMMA)) return first;

        // x[1:2, 3] → tuple of slices
        int ln = first->line, cl = first->col;
        auto t = node_at<TupleExpr>(ln, cl);
        t->elts.push_back(std::move(first));
        while (match(Tok::COMMA)) {
            if (check(Tok::RBRACKET)) break;
            ExprPtr item;
            if (!check(Tok::COLON)) item = parse_namedexpr();
            if (check(Tok::COLON)) {
                int sl = cur().line, sc = cur().col;
                advance();
                auto s = node_at<SliceExpr>(sl, sc);
                s->lower = std::move(item);
                if (!check_any({Tok::RBRACKET, Tok::COLON, Tok::COMMA})) s->upper = parse_namedexpr();
                if (match(Tok::COLON)) {
                    if (!check_any({Tok::RBRACKET, Tok::COMMA})) s->step = parse_namedexpr();
                }
                item = std::move(s);
            }
            t->elts.push_back(std::move(item));
        }
        return t;
    }

bool Parser::looks_like_match_stmt() const {
        // 从 'match' 之后开始，找顶层的冒号；冒号后必须是换行 + 缩进 + case。
        // 这样 match.foo: int = 1 这类标注赋值不会被误判成 match 语句。
        int depth = 0;
        for (size_t i = pos_ + 1; i < toks_.size(); ++i) {
            Tok k = toks_[i].kind;
            if (k == Tok::LPAREN || k == Tok::LBRACKET || k == Tok::LBRACE) {
                ++depth;
            } else if (k == Tok::RPAREN || k == Tok::RBRACKET || k == Tok::RBRACE) {
                if (depth == 0) return false;
                --depth;
            } else if (k == Tok::NEWLINE || k == Tok::END || k == Tok::SEMICOLON) {
                return false;  // 本行结束前没见到冒号
            } else if (k == Tok::COLON && depth == 0) {
                size_t j = i + 1;
                if (j >= toks_.size() || toks_[j].kind != Tok::NEWLINE) return false;
                ++j;
                if (j >= toks_.size() || toks_[j].kind != Tok::INDENT) return false;
                ++j;
                return j < toks_.size() && toks_[j].kind == Tok::NAME &&
                       toks_[j].text == "case";
            }
        }
        return false;
    }

bool Parser::looks_like_genexp() const {
        // 粗略前瞻：在下一个 RPAREN 之前出现顶层 for
        int depth = 0;
        for (size_t i = pos_; i < toks_.size(); ++i) {
            Tok k = toks_[i].kind;
            if (k == Tok::LPAREN || k == Tok::LBRACKET || k == Tok::LBRACE) ++depth;
            else if (k == Tok::RPAREN || k == Tok::RBRACKET || k == Tok::RBRACE) {
                if (depth == 0) return false;
                --depth;
            } else if (k == Tok::KW_FOR && depth == 0) {
                return true;
            } else if (k == Tok::END) {
                return false;
            }
        }
        return false;
    }

ExprPtr Parser::parse_atom() {
        int ln = cur().line, cl = cur().col;

        switch (cur().kind) {
            case Tok::INT: {
                auto t = advance();
                auto n = node_at<Constant>(ln, cl);
                n->ck = Constant::CK::Int;
                n->i = t.ival;
                return n;
            }
            case Tok::BYTES:
                throw err_at("bytes literals are not supported by this compiler", ln, cl);
            case Tok::FLOAT: {
                auto t = advance();
                auto n = node_at<Constant>(ln, cl);
                n->ck = Constant::CK::Float;
                n->f = t.fval;
                return n;
            }
            case Tok::STRING:
            case Tok::FSTRING: {
                // Python 允许相邻字面量拼接，且 string 与 f-string 可混用：
                //   ('a' f'{x}' 'b')  →  JoinedStr
                std::vector<Token> parts;
                while (check(Tok::STRING) || check(Tok::FSTRING)) parts.push_back(advance());

                bool has_f = std::ranges::any_of(parts, [](const Token& p) {
                    return p.kind == Tok::FSTRING;
                });

                if (!has_f) {
                    std::string merged;
                    for (auto& p : parts) merged += p.text;
                    auto n = node_at<Constant>(ln, cl);
                    n->ck = Constant::CK::Str;
                    n->s = std::move(merged);
                    return n;
                }

                auto j = node_at<JoinedStr>(ln, cl);
                for (auto& p : parts) {
                    if (p.kind == Tok::STRING) {
                        if (p.text.empty()) continue;
                        auto c = std::make_unique<Constant>();
                        c->ck = Constant::CK::Str;
                        c->s = p.text;
                        c->line = p.line;
                        c->col = p.col;
                        j->values.push_back(std::move(c));
                    } else {
                        auto sub = parse_fstring(p);
                        for (auto& v : sub->values) j->values.push_back(std::move(v));
                    }
                }
                return j;
            }
            // 这几个字面量走的是静态工厂，工厂不带位置信息，得在这里补上，
            // 否则 `x: int = True` 这类类型错误会指到 1:1。
            case Tok::KW_TRUE: {
                auto c = Constant::boolean(true);
                c->line = ln;
                c->col = cl;
                advance();
                return c;
            }
            case Tok::KW_FALSE: {
                auto c = Constant::boolean(false);
                c->line = ln;
                c->col = cl;
                advance();
                return c;
            }
            case Tok::KW_NONE: {
                auto c = Constant::none();
                c->line = ln;
                c->col = cl;
                advance();
                return c;
            }
            case Tok::ELLIPSIS: {
                auto c = Constant::ellipsis();
                c->line = ln;
                c->col = cl;
                advance();
                return c;
            }
            case Tok::NAME: {
                auto t = advance();
                auto n = std::make_unique<Name>(t.text);
                n->line = t.line;
                n->col = t.col;
                return n;
            }
            case Tok::LPAREN: return parse_paren();
            case Tok::LBRACKET: return parse_list_or_comp();
            case Tok::LBRACE: return parse_dict_or_set();
            case Tok::KW_LAMBDA: return parse_lambda();
            case Tok::KW_YIELD: {
                advance();
                if (func_depth_ == 0) throw err_at("'yield' outside function", ln, cl);
                if (check(Tok::KW_FROM)) {
                    advance();
                    auto n = node_at<YieldFrom>(ln, cl);
                    n->value = parse_namedexpr();
                    return n;
                }
                auto n = node_at<Yield>(ln, cl);
                if (!at_yield_end()) n->value = parse_expr_list();
                return n;
            }
            case Tok::STAR: {
                // 只在特定上下文合法，这里允许并让后续检查处理
                advance();
                auto n = node_at<Starred>(ln, cl);
                n->value = parse_namedexpr();
                return n;
            }
            case Tok::KW_NOT:
                return parse_not();
            default:
                throw err(std::format("unexpected token '{}'", describe(cur())));
        }
    }

ExprPtr Parser::parse_paren() {
        int ln = cur().line, cl = cur().col;
        advance();
        if (check(Tok::RPAREN)) {
            advance();
            return node_at<TupleExpr>(ln, cl);  // 空元组
        }

        // 生成器表达式
        if (looks_like_genexp()) {
            auto gen = node_at<GeneratorExp>(ln, cl);
            gen->elt = parse_namedexpr();
            // async for 在生成器表达式里任何位置都合法（PEP 530，3.7 起）：
            // 生成器体是延迟求值的，不需要当前就在协程里
            ++genexp_depth_;
            while (check(Tok::KW_FOR) || check(Tok::KW_ASYNC))
                gen->generators.push_back(parse_comp_for());
            --genexp_depth_;
            expect(Tok::RPAREN, "')'");
            return gen;
        }

        auto first = parse_namedexpr();
        if (check(Tok::COMMA)) {
            auto t = node_at<TupleExpr>(ln, cl);
            t->elts.push_back(std::move(first));
            while (match(Tok::COMMA)) {
                if (check(Tok::RPAREN)) break;
                t->elts.push_back(parse_namedexpr());
            }
            expect(Tok::RPAREN, "')'");
            return t;
        }
        expect(Tok::RPAREN, "')'");
        return first;
    }

ExprPtr Parser::parse_list_or_comp() {
        int ln = cur().line, cl = cur().col;
        advance();
        if (check(Tok::RBRACKET)) {
            advance();
            return node_at<ListExpr>(ln, cl);
        }

        auto first = parse_namedexpr();
        if (check(Tok::KW_FOR) || (check(Tok::KW_ASYNC) && peek_tok().kind == Tok::KW_FOR)) {
            auto n = node_at<ListComp>(ln, cl);
            n->elt = std::move(first);
            while (check(Tok::KW_FOR) || check(Tok::KW_ASYNC)) n->generators.push_back(parse_comp_for());
            expect(Tok::RBRACKET, "']'");
            return n;
        }

        auto n = node_at<ListExpr>(ln, cl);
        n->elts.push_back(std::move(first));
        while (match(Tok::COMMA)) {
            if (check(Tok::RBRACKET)) break;
            n->elts.push_back(parse_namedexpr());
        }
        expect(Tok::RBRACKET, "']'");
        return n;
    }

ExprPtr Parser::parse_dict_or_set() {
        int ln = cur().line, cl = cur().col;
        advance();
        if (check(Tok::RBRACE)) {
            advance();
            return node_at<DictExpr>(ln, cl);
        }

        // **unpack 开头 → 一定是 dict
        if (check(Tok::DOUBLESTAR)) {
            auto n = node_at<DictExpr>(ln, cl);
            while (true) {
                if (check(Tok::DOUBLESTAR)) {
                    advance();
                    n->keys.push_back(nullptr);
                    n->values.push_back(parse_namedexpr());
                } else {
                    auto k = parse_namedexpr();
                    expect(Tok::COLON, "':' in dict literal");
                    n->keys.push_back(std::move(k));
                    n->values.push_back(parse_namedexpr());
                }
                if (!match(Tok::COMMA)) break;
                if (check(Tok::RBRACE)) break;
            }
            expect(Tok::RBRACE, "'}'");
            return n;
        }

        auto first = parse_namedexpr();

        if (check(Tok::COLON)) {
            advance();
            auto first_val = parse_namedexpr();
            if (check(Tok::KW_FOR) || check(Tok::KW_ASYNC)) {
                auto n = node_at<DictComp>(ln, cl);
                n->key = std::move(first);
                n->value = std::move(first_val);
                while (check(Tok::KW_FOR) || check(Tok::KW_ASYNC)) n->generators.push_back(parse_comp_for());
                expect(Tok::RBRACE, "'}'");
                return n;
            }
            auto n = node_at<DictExpr>(ln, cl);
            n->keys.push_back(std::move(first));
            n->values.push_back(std::move(first_val));
            while (match(Tok::COMMA)) {
                if (check(Tok::RBRACE)) break;
                if (check(Tok::DOUBLESTAR)) {
                    advance();
                    n->keys.push_back(nullptr);
                    n->values.push_back(parse_namedexpr());
                    continue;
                }
                n->keys.push_back(parse_namedexpr());
                expect(Tok::COLON, "':' in dict literal");
                n->values.push_back(parse_namedexpr());
            }
            expect(Tok::RBRACE, "'}'");
            return n;
        }

        if (check(Tok::KW_FOR) || check(Tok::KW_ASYNC)) {
            auto n = node_at<SetComp>(ln, cl);
            n->elt = std::move(first);
            while (check(Tok::KW_FOR) || check(Tok::KW_ASYNC)) n->generators.push_back(parse_comp_for());
            expect(Tok::RBRACE, "'}'");
            return n;
        }

        auto n = node_at<SetExpr>(ln, cl);
        n->elts.push_back(std::move(first));
        while (match(Tok::COMMA)) {
            if (check(Tok::RBRACE)) break;
            n->elts.push_back(parse_namedexpr());
        }
        expect(Tok::RBRACE, "'}'");
        return n;
    }

ExprPtr Parser::parse_comp_for() {
        int ln = cur().line, cl = cur().col;
        auto n = node_at<Comprehension>(ln, cl);
        if (check(Tok::KW_ASYNC)) {
            if (async_depth_ == 0 && genexp_depth_ == 0)
                throw err_at("asynchronous comprehension outside of an async function", ln, cl);
            advance();
            n->is_async = true;
        }
        expect(Tok::KW_FOR, "'for' in comprehension");
        n->target = parse_target_list();
        expect(Tok::KW_IN, "'in' in comprehension");
        n->iter = parse_or_no_conditional();
        while (check(Tok::KW_IF)) {
            advance();
            n->ifs.push_back(parse_or_no_conditional());
        }
        return n;
    }

ExprPtr Parser::parse_target_list() {
        int ln = cur().line, cl = cur().col;
        auto first = parse_target();
        if (!check(Tok::COMMA)) return first;
        auto t = node_at<TupleExpr>(ln, cl);
        t->elts.push_back(std::move(first));
        while (match(Tok::COMMA)) {
            if (check(Tok::KW_IN) || check(Tok::COLON) || check(Tok::RBRACKET) || check(Tok::RPAREN))
                break;
            t->elts.push_back(parse_target());
        }
        return t;
    }

std::unique_ptr<JoinedStr> Parser::parse_fstring(const Token& tok) {
        auto out = std::make_unique<JoinedStr>();
        out->line = tok.line;
        out->col = tok.col;

        const std::string& s = tok.text;
        std::string lit;
        size_t i = 0;

        auto flush_lit = [&]() {
            if (!lit.empty()) {
                auto c = std::make_unique<Constant>();
                c->ck = Constant::CK::Str;
                c->s = std::move(lit);
                lit.clear();
                out->values.push_back(std::move(c));
            }
        };

        while (i < s.size()) {
            char c = s[i];
            if (c == '{' && i + 1 < s.size() && s[i + 1] == '{') {
                lit.push_back('{');
                i += 2;
                continue;
            }
            if (c == '}' && i + 1 < s.size() && s[i + 1] == '}') {
                lit.push_back('}');
                i += 2;
                continue;
            }
            if (c == '}') throw err_at("single '}' is not allowed in f-string", tok.line, tok.col);
            if (c != '{') {
                // 字面量部分的转义：raw f-string 原样保留，其余与普通字符串一致
                if (c == '\\' && !tok.raw) {
                    // f"\{x}" 里的反斜杠不是转义：花括号有语法含义，
                    // 原样留下再按表达式处理（与 CPython 一致）
                    if (i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '}')) {
                        lit.push_back(c);
                        ++i;
                    } else if (!decode_escape(s, i, lit)) {
                        throw err_at("invalid escape sequence in f-string", tok.line, tok.col);
                    }
                } else {
                    lit.push_back(c);
                    ++i;
                }
                continue;
            }

            // 表达式部分
            flush_lit();
            ++i;  // 跳过 '{'
            size_t expr_start = i;
            int depth = 0;
            char quote = 0;
            bool triple_q = false;   // 表达式里可以有三引号字符串（f"{'''x'''}"）
            size_t conv_pos = std::string::npos;
            size_t spec_pos = std::string::npos;

            while (i < s.size()) {
                char ch = s[i];
                if (quote) {
                    if (ch == '\\') { i = std::min(i + 2, s.size()); continue; }
                    if (ch == quote) {
                        if (triple_q && i + 2 < s.size() && s[i + 1] == quote && s[i + 2] == quote) {
                            quote = 0;
                            triple_q = false;
                            i += 3;
                            continue;
                        }
                        if (!triple_q) quote = 0;
                    }
                    ++i;
                    continue;
                }
                if (ch == '"' || ch == '\'') {
                    quote = ch;
                    triple_q = (i + 2 < s.size() && s[i + 1] == ch && s[i + 2] == ch);
                    i += triple_q ? 3 : 1;
                    continue;
                }
                if (ch == '(' || ch == '[' || ch == '{') { ++depth; ++i; continue; }
                if (ch == ')' || ch == ']') { --depth; ++i; continue; }
                if (ch == '}') {
                    if (depth == 0) break;
                    --depth;
                    ++i;
                    continue;
                }
                // '!' 只有在格式说明符之前才是转换标志：
                // f'{3.14:!<10}' 里的 '!' 是填充字符
                if (depth == 0 && spec_pos == std::string::npos && ch == '!' &&
                    i + 1 < s.size() && s[i + 1] != '=') {
                    if (conv_pos == std::string::npos) conv_pos = i;
                }
                if (depth == 0 && ch == ':' && spec_pos == std::string::npos) spec_pos = i;
                ++i;
            }
            if (i >= s.size()) throw err_at("unterminated f-string expression", tok.line, tok.col);

            size_t expr_end = i;
            if (conv_pos != std::string::npos && conv_pos < expr_end) expr_end = std::min(expr_end, conv_pos);
            if (spec_pos != std::string::npos && spec_pos < expr_end) expr_end = std::min(expr_end, spec_pos);

            std::string raw = s.substr(expr_start, expr_end - expr_start);

            // 自文档化表达式 f"{expr=}"（3.8+）：结尾的 '=' 表示把源码本身打出来
            bool self_doc = false;
            {
                size_t e = raw.size();
                while (e > 0 && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) --e;
                if (e > 0 && raw[e - 1] == '=') {
                    char prev = (e >= 2) ? raw[e - 2] : 0;
                    if (prev != '=' && prev != '!' && prev != '<' && prev != '>')
                        self_doc = true;
                }
            }

            std::string expr_src = self_doc ? raw.substr(0, raw.rfind('=')) : raw;
            if (expr_src.find_first_not_of(" \t\n") == std::string::npos)
                throw err_at("empty expression in f-string", tok.line, tok.col);
            // 去掉前导空白：否则子 lexer 会把它当成一级缩进，产出 INDENT
            if (size_t lead = expr_src.find_first_not_of(" \t\n"); lead != std::string::npos)
                expr_src.erase(0, lead);

            // 自文档化：先把 "expr=" 这段源码原文当字面量输出
            if (self_doc) {
                auto lit_part = std::make_unique<Constant>();
                lit_part->ck = Constant::CK::Str;
                lit_part->s = raw;
                lit_part->line = tok.line;
                lit_part->col = tok.col;
                out->values.push_back(std::move(lit_part));
            }

            auto fv = std::make_unique<FormattedValue>();
            fv->line = tok.line;
            fv->col = tok.col;
            if (self_doc) fv->conversion = 'r';

            // 解析内嵌表达式：重新走一遍 lex + parse。
            // 外面套一层括号：三引号 f-string 的表达式可以跨行
            // （f"""{3 +
            // 4}"""），括号内的换行会被隐式续行吃掉；顺便也让 `a, b`
            // 这种元组表达式原样成立。
            std::vector<Token> inner;
            std::string wrapped = "(" + expr_src + ")";  // Lexer 持有 view，必须活到 run() 之后
            try {
                Lexer sub(wrapped);
                inner = sub.run();
            } catch (const LexError& e) {
                throw err_at(std::format("in f-string: {}", e.what()), tok.line, tok.col);
            }
            Parser sub(std::move(inner));
            // 子解析器必须继承外层上下文：f-string 里的 {await x} 仍然
            // 在 async def 里，{yield} 仍然在函数里。否则 filemanager.py
            // 那种多行 f-string 里的 await 会被判成「不在 async 函数中」。
            sub.async_depth_ = async_depth_;
            sub.func_depth_ = func_depth_;
            try {
                fv->value = sub.parse_fstring_expr();
            } catch (const ParseError& e) {
                // 子解析器的行列是相对 f-string 内部源码的，直接冒泡会显示成
                // 1:1。位置只能给到 f-string 起点，至少行号是对的。
                throw err_at(std::format("in f-string expression: {}", e.what()), tok.line, tok.col);
            }

            // 转换标志
            if (conv_pos != std::string::npos && conv_pos < i) {
                size_t p = conv_pos + 1;
                if (p < s.size()) {
                    char cv = s[p];
                    if (cv == 's') fv->conversion = 's';
                    else if (cv == 'r') fv->conversion = 'r';
                    else if (cv == 'a') fv->conversion = 'a';
                    else throw err_at("invalid conversion in f-string", tok.line, tok.col);
                    ++p;
                }
                if (spec_pos != std::string::npos && spec_pos > conv_pos) p = spec_pos;
                if (p < i && s[p] != ':' && s[p] != '}')
                    throw err_at("unexpected characters after f-string conversion", tok.line, tok.col);
            }

            // 格式说明符：当作嵌套的 f-string 处理（Python 语义）
            if (spec_pos != std::string::npos && spec_pos < i) {
                std::string spec_src = s.substr(spec_pos + 1, i - spec_pos - 1);
                Token spec_tok;
                spec_tok.kind = Tok::FSTRING;
                spec_tok.text = spec_src;
                spec_tok.raw = tok.raw;
                spec_tok.line = tok.line;
                spec_tok.col = tok.col;
                auto spec = parse_fstring(spec_tok);
                if (!spec->values.empty()) fv->format_spec = std::move(spec);
            }

            out->values.push_back(std::move(fv));
            ++i;  // 跳过 '}'
        }
        flush_lit();
        return out;
    }

ExprPtr Parser::parse_fstring_expr() {
        skip_newlines();
        // 替换字段里可以是元组：f"{x,}" 合法（Python 的 star_expressions）。
        // 以前按 namedexpr 解析，逗号留在原地就报 trailing tokens。
        auto e = parse_expr_list();
        skip_newlines();
        if (!check(Tok::END)) throw err("unexpected trailing tokens in f-string expression");
        return e;
    }

}  // namespace py23
