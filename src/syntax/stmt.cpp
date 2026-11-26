// syntax/stmt.cpp —— 复合语句：if/while/for/try/with
#include "syntax/parser.h"

namespace pyadamas {

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
                // PEP 758（3.14）：`except A, B:` 合法（不带 as 时可省括号）。
                // 带 as 的 `except A, e` 仍是 Python 2 写法，但要报对错误。
                if (check(Tok::COMMA) && !check(Tok::KW_AS)) {
                    auto tup = node_at<TupleExpr>(ln, cl);
                    tup->elts.push_back(std::move(h.type));
                    while (check(Tok::COMMA)) {
                        advance();
                        tup->elts.push_back(parse_namedexpr());
                    }
                    if (check(Tok::KW_AS))
                        throw err("'except A, B as e' is not valid Python; "
                                  "write 'except (A, B) as e'")
                            ;
                    h.type = std::move(tup);
                } else if (check(Tok::KW_AS)) {
                    advance();
                    h.name = expect(Tok::NAME, "exception name").text;
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

bool Parser::looks_like_struct_def() const {
        return cur().kind == Tok::NAME && cur().text == "struct" &&
               peek_tok(1).kind == Tok::NAME &&
               peek_tok(2).kind == Tok::COLON &&
               peek_tok(3).kind == Tok::NEWLINE &&
               peek_tok(4).kind == Tok::INDENT;
    }

StmtPtr Parser::parse_structdef() {
        int ln = cur().line, cl = cur().col;
        advance();  // struct
        if (suite_depth_ != 0)
            throw err_at("'struct' is only allowed at module top level", ln, cl);
        auto node = node_at<StructDef>(ln, cl);
        node->name = expect(Tok::NAME, "struct name").text;
        if (check(Tok::LPAREN))
            throw err_at("struct does not support inheritance or methods", ln, cl);
        expect(Tok::COLON, "':' after struct name");
        expect(Tok::NEWLINE, "newline after struct header");
        if (!check(Tok::INDENT)) throw err("expected an indented block");
        advance();
        skip_newlines();
        while (!check(Tok::DEDENT) && !check(Tok::END)) {
            StructFieldDef f;
            f.line = cur().line;
            f.col = cur().col;
            f.name = expect(Tok::NAME, "field name").text;
            expect(Tok::COLON, "':' after field name");
            f.type = parse_namedexpr();
            if (check(Tok::NEWLINE) || check(Tok::SEMICOLON)) advance();
            else if (!check(Tok::DEDENT) && !check(Tok::END))
                throw err(std::format("unexpected '{}' after field declaration",
                                      describe(cur())));
            skip_newlines();
            node->fields.push_back(std::move(f));
        }
        if (check(Tok::DEDENT)) advance();
        else throw err("unexpected end of file, expected dedent");
        if (node->fields.empty()) throw err_at("struct must have at least one field", ln, cl);
        return node;
    }

StmtPtr Parser::parse_simple_statement() {
        int ln = cur().line, cl = cur().col;

        // match 是软关键字：只有 "match <subject>: NEWLINE INDENT case" 才是
        // match 语句。match[0]、match.attr、match, x = ... 都是普通标识符。
        if (check(Tok::NAME) && cur().text == "match" && looks_like_match_stmt())
            return parse_match();

        // struct 同样是软关键字：只有 "struct NAME: NEWLINE INDENT" 才是值类型
        // 定义；`struct = 1`、`import struct` 都不受影响。
        if (check(Tok::NAME) && cur().text == "struct" && looks_like_struct_def())
            return parse_structdef();

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
            case Tok::KW_CIMPORT: return parse_cimport();
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
            n->annotation = parse_annotation_expr();
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


}  // namespace pyadamas
