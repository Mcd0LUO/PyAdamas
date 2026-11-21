// parser_expr.cpp —— 表达式：优先级爬升、原子、推导式、f-string
#include "parser.h"

namespace pyadamas {

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
                if (auto* name = dynamic_cast<Name*>(e.get()); name && name->id == "cast_big")
                    mentions_big_ = true;
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
                if (t.is_big) {
                    mentions_big_ = true;
                    n->ck = Constant::CK::Big;
                    n->s = t.text;   // 去下划线的十进制串
                } else {
                    n->ck = Constant::CK::Int;
                    n->i = t.ival;
                    n->u64_lit = t.u64_lit;
                }
                return n;
            }
            case Tok::BYTES: {
                // 相邻 bytes 字面量隐式拼接（CPython 语义）：b"a" b"b" -> b"ab"。
                // 词法层已经把前缀和转义处理掉了，token.text 就是字节内容，
                // 所以拼接就是拼内容。bytes 与 str 不能混拼。
                auto t = advance();
                std::string merged = t.text;
                while (check(Tok::BYTES)) merged += advance().text;
                if (check(Tok::STRING) || check(Tok::FSTRING))
                    throw err("cannot mix bytes and nonbytes literals");
                auto n = node_at<Constant>(ln, cl);
                n->ck = Constant::CK::Bytes;
                n->s = std::move(merged);
                return n;
            }
            case Tok::FLOAT: {
                auto t = advance();
                auto n = node_at<Constant>(ln, cl);
                n->ck = Constant::CK::Float;
                n->f = t.fval;
                return n;
            }
            case Tok::COMPLEX: {
                auto t = advance();
                auto n = node_at<Constant>(ln, cl);
                n->ck = Constant::CK::Complex;
                n->im = t.fval;
                return n;
            }
            case Tok::STRING:
            case Tok::FSTRING: {
                // Python 允许相邻字面量拼接，且 string 与 f-string 可混用：
                //   ('a' f'{x}' 'b')  →  JoinedStr
                std::vector<Token> parts;
                while (check(Tok::STRING) || check(Tok::FSTRING)) parts.push_back(advance());
                // str / f-string 与 bytes 不能混拼（CPython 同款报错）。
                if (check(Tok::BYTES))
                    throw err("cannot mix bytes and nonbytes literals");

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
                // go 是软关键字：只有「go NAME…」才按 spawn 表达式解析，
                // go = 1 / go() / go[0] 里的 go 仍是普通名字。
                if (t.text == "go" && check(Tok::NAME)) {
                    auto g = node_at<GoExpr>(ln, cl);
                    g->call = parse_primary();
                    return g;
                }
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
                mentions_big_ |= sub.mentions_big();
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


}  // namespace pyadamas
