// syntax/match.cpp —— match / case 结构化模式匹配（语法层）
#include "syntax/parser.h"

#include <optional>
#include <unordered_set>

namespace pyadamas {

// 映射模式里做静态重复 key 检查：只处理常量键（含 -1 这种一元负号）。
static std::optional<std::string> pattern_key_id(Expr& e) {
    bool neg = false;
    Expr* base = &e;
    if (base->kind == ExprKind::UnaryOp) {
        auto& u = static_cast<UnaryOp&>(*base);
        if (u.op != Tok::MINUS) return std::nullopt;
        neg = true;
        base = u.operand.get();
    }
    if (base->kind != ExprKind::Constant) return std::nullopt;
    auto& c = static_cast<Constant&>(*base);
    // 键归一化（0.645）：
    //  - 整数必须**精确**比较：原来一律转 double，2**53 以上会误判（
    //    {9007199254740993: a, 9007199254740992: b} 被当成重复键），
    //  - -0.0 与 0.0 是同一个键（CPython 也报重复），
    //  - Big 字面量也参与判重。
    auto num = [&](double v) {
        double x = neg ? -v : v;
        // 归一化必须在**取负之后**：-0.0 与 0.0 是同一个键（否则 {:.17g}
        // 会给出 "-0" 与 "0"，判重失效）。
        if (x == 0.0) x = 0.0;
        return std::format("n:{:.17g}", x);
    };
    auto i64key = [&](int64_t v) { return std::format("n:{}", neg ? -v : v); };
    auto bigkey = [&](const std::string& v) {
        // 十进制串加减号；Big 与 float 的等值（1e20 vs 10**20）不判重。
        if (neg) return v.empty() || v[0] == '-' ? std::string("n:") + v.substr(v[0] == '-')
                                                  : std::string("n:-") + v;
        return std::string("n:") + v;
    };
    switch (c.ck) {
        case Constant::CK::None: return neg ? std::nullopt : std::optional<std::string>("N");
        case Constant::CK::Bool: return num(c.b ? 1.0 : 0.0);
        case Constant::CK::Int: return i64key(c.i);
        case Constant::CK::Big: return bigkey(c.s);
        case Constant::CK::Float: return num(c.f);
        case Constant::CK::Str: return neg ? std::nullopt : std::optional<std::string>("s:" + c.s);
        case Constant::CK::Complex:
            if (!neg) return std::format("c:{:.17g},{:.17g}", 0.0, c.im);
            return std::format("c:{:.17g},{:.17g}", 0.0, -c.im);
        default: return std::nullopt;
    }
}

StmtPtr Parser::parse_match() {
    int ln = cur().line, cl = cur().col;
    advance();  // match
    auto node = node_at<Match>(ln, cl);
    node->subject = parse_expr_list();
    expect(Tok::COLON, "':' after match subject");
    if (!check(Tok::NEWLINE)) throw err("expected newline after match statement");
    advance();
    if (!check(Tok::INDENT)) throw err("expected an indented case block");
    advance();
    skip_newlines();
    while (check(Tok::NAME) && cur().text == "case") {
        node->cases.push_back(parse_case());
        skip_newlines();
    }
    if (check(Tok::DEDENT)) advance();
    else throw err("unexpected end of file, expected dedent after match");
    if (node->cases.empty()) throw err_at("match statement requires at least one case", ln, cl);
    return node;
}

MatchCase Parser::parse_case() {
    advance();  // case
    MatchCase c;
    c.pattern = parse_pattern();
    if (check(Tok::KW_IF)) {
        advance();
        c.guard = parse_namedexpr();
    }
    expect(Tok::COLON, "':' after case pattern");
    c.body = parse_suite();
    return c;
}

PatternPtr Parser::parse_pattern() {
    // 顶层允许多个模式（隐式元组），case a, b: 等价于 case (a, b):
    int ln = cur().line, cl = cur().col;
    auto first = parse_or_pattern();
    if (!check(Tok::COMMA)) return first;
    auto seq = std::make_shared<Pattern>();
    seq->kind = PatternKind::Sequence;
    seq->line = ln;
    seq->col = cl;
    seq->items.push_back(std::move(first));
    while (match(Tok::COMMA)) {
        if (check(Tok::COLON) || check(Tok::KW_IF)) break;
        seq->items.push_back(parse_or_pattern());
    }
    return seq;
}

PatternPtr Parser::parse_or_pattern() {
    int ln = cur().line, cl = cur().col;
    auto first = parse_closed_pattern();
    PatternPtr pat = std::move(first);
    if (check(Tok::VBAR)) {
        auto node = std::make_shared<Pattern>();
        node->kind = PatternKind::Or;
        node->line = ln;
        node->col = cl;
        node->items.push_back(std::move(pat));
        while (match(Tok::VBAR)) node->items.push_back(parse_closed_pattern());
        pat = std::move(node);
    }
    if (check(Tok::KW_AS)) {
        advance();
        if (!check(Tok::NAME))
            throw err("expected a capture name after 'as'");
        auto text = advance().text;
        if (text == "_") throw err_at("cannot use '_' as a capture name", ln, cl);
        auto node = std::make_shared<Pattern>();
        node->kind = PatternKind::As;
        node->line = ln;
        node->col = cl;
        node->name = std::move(text);
        node->items.push_back(std::move(pat));
        return node;
    }
    return pat;
}

PatternPtr Parser::parse_class_pattern(std::string name, int ln, int cl) {
    advance();  // (
    auto p = std::make_shared<Pattern>();
    p->kind = PatternKind::Class;
    p->line = ln;
    p->col = cl;
    p->cls = std::move(name);
    bool seen_kw = false;
    if (!check(Tok::RPAREN)) {
        while (true) {
            if (check(Tok::NAME) && peek_tok().kind == Tok::EQUAL) {
                std::string attr = advance().text;
                advance();  // =
                seen_kw = true;
                p->attrs.emplace_back(std::move(attr), parse_or_pattern());
            } else {
                if (seen_kw)
                    throw err("positional pattern follows keyword pattern");
                p->positional.push_back(parse_or_pattern());
            }
            if (!match(Tok::COMMA)) break;
            if (check(Tok::RPAREN)) break;
        }
    }
    expect(Tok::RPAREN, "')'");
    return p;
}

PatternPtr Parser::parse_sequence_pattern(Tok close) {
    int ln = cur().line, cl = cur().col;
    advance();  // [ 或 (
    auto node = std::make_shared<Pattern>();
    node->kind = PatternKind::Sequence;
    node->line = ln;
    node->col = cl;
    bool trailing = false;
    if (!check(close)) {
        while (true) {
            if (check(Tok::STAR)) {
                int sl = cur().line, sc = cur().col;
                advance();
                auto star = std::make_shared<Pattern>();
                star->line = sl;
                star->col = sc;
                if (check(Tok::NAME)) {
                    auto text = cur().text;
                    if (text == "_") {
                        star->kind = PatternKind::Wildcard;
                        advance();
                    } else {
                        star->kind = PatternKind::Capture;
                        star->name = text;
                        advance();
                    }
                } else {
                    // PEP 634：star_pattern = '*' (pattern_capture_target | '_')。
                    // 裸 `*` 非法（case [*]:），原来被静默当成 `*_`。
                    throw err_at("expected a name or '_' after '*' in sequence pattern", sl,
                                 sc);
                }
                if (node->star >= 0) throw err_at("multiple starred patterns", sl, sc);
                node->star = static_cast<int>(node->items.size());
                node->items.push_back(std::move(star));
            } else {
                node->items.push_back(parse_or_pattern());
            }
            if (match(Tok::COMMA)) {
                trailing = true;
                if (check(close)) break;
                continue;
            }
            trailing = false;
            break;
        }
    }
    expect(close, close == Tok::RPAREN ? "')'" : "']'");
    if (close == Tok::RPAREN && node->items.size() == 1 && node->star < 0 && !trailing)
        return std::move(node->items[0]);
    return node;
}

PatternPtr Parser::parse_closed_pattern() {
    int ln = cur().line, cl = cur().col;

    if (check(Tok::LBRACKET)) return parse_sequence_pattern(Tok::RBRACKET);
    if (check(Tok::LPAREN)) return parse_sequence_pattern(Tok::RPAREN);

    if (check(Tok::LBRACE)) {
        advance();
        auto node = std::make_shared<Pattern>();
        node->kind = PatternKind::Mapping;
        node->line = ln;
        node->col = cl;
        std::unordered_set<std::string> seen_keys;
        if (!check(Tok::RBRACE)) {
            while (true) {
                if (check(Tok::DOUBLESTAR)) {
                    advance();
                    auto rest_tok = expect(Tok::NAME, "name after '**'");
                    if (!node->rest.empty())
                        throw err_at("multiple '**rest' in mapping pattern", ln, cl);
                    node->rest = rest_tok.text;
                    if (match(Tok::COMMA) && check(Tok::RBRACE)) break;
                    if (!check(Tok::RBRACE))
                        throw err_at("'**rest' must be the last entry in a mapping pattern",
                                     ln, cl);
                    break;
                }
                auto key = parse_namedexpr();
                if (auto id = pattern_key_id(*key)) {
                    if (!seen_keys.insert(*id).second)
                        throw err_at("mapping pattern checks duplicate key", key->line,
                                     key->col);
                }
                expect(Tok::COLON, "':' in mapping pattern");
                node->pairs.emplace_back(std::move(key), parse_or_pattern());
                if (!match(Tok::COMMA)) break;
                if (check(Tok::RBRACE)) break;
            }
        }
        expect(Tok::RBRACE, "'}'");
        return node;
    }

    if (check(Tok::NAME)) {
        auto text = cur().text;
        if (text == "_") {
            advance();
            auto p = std::make_shared<Pattern>();
            p->kind = PatternKind::Wildcard;
            p->line = ln;
            p->col = cl;
            return p;
        }
        advance();
        if (check(Tok::LPAREN)) return parse_class_pattern(std::move(text), ln, cl);
        if (check(Tok::DOT)) {
            std::string path = std::move(text);
            auto base = std::make_unique<Name>(path);
            base->line = ln;
            base->col = cl;
            ExprPtr dotted = std::move(base);
            while (match(Tok::DOT)) {
                if (!check(Tok::NAME)) throw err("expected attribute name after '.'");
                std::string attr = advance().text;
                path += "." + attr;
                auto at = std::make_unique<Attribute>();
                at->value = std::move(dotted);
                at->attr = std::move(attr);
                at->line = ln;
                at->col = cl;
                dotted = std::move(at);
            }
            if (check(Tok::LPAREN))
                return parse_class_pattern(std::move(path), ln, cl);
            auto p = std::make_shared<Pattern>();
            p->kind = PatternKind::Value;
            p->lit = std::move(dotted);
            p->line = ln;
            p->col = cl;
            return p;
        }
        auto p = std::make_shared<Pattern>();
        p->kind = PatternKind::Capture;
        p->name = std::move(text);
        p->line = ln;
        p->col = cl;
        return p;
    }

    bool neg = false;
    if (match(Tok::MINUS)) neg = true;
    if (check_any({Tok::INT, Tok::FLOAT, Tok::COMPLEX, Tok::STRING, Tok::BYTES,
                   Tok::KW_TRUE, Tok::KW_FALSE, Tok::KW_NONE})) {
        ExprPtr lit = parse_atom();
        if (lit->kind == ExprKind::Constant) {
            auto& c = static_cast<Constant&>(*lit);
            if (neg) {
                if (c.ck == Constant::CK::Int) c.i = -c.i;
                else if (c.ck == Constant::CK::Float) c.f = -c.f;
                else if (c.ck == Constant::CK::Complex) c.im = -c.im;
                else if (c.ck == Constant::CK::Big) {
                    // 负大整数字面量（0.641）：CK::Big 的正文是十进制串，
                    // 求负只是在串上加减号——原来落到 else 直接报错。
                    if (!c.s.empty() && c.s[0] == '-') c.s.erase(0, 1);
                    else c.s.insert(c.s.begin(), '-');
                }
                else throw err_at("invalid negative literal pattern", ln, cl);
            }
        } else {
            throw err_at("literal pattern must be a constant", ln, cl);
        }
        // PEP 634：signed_real_number ('+'|'-') imaginary_number（0.641）。
        // Constant 的 Complex 只能表示纯虚数（实部固定 0），所以这里保留成
        // BinOp 表达式，降级时按普通表达式求值即可得到 1+2j。
        if (!neg && lit->kind == ExprKind::Constant &&
            (check(Tok::PLUS) || check(Tok::MINUS)) && peek_tok().kind == Tok::COMPLEX) {
            auto& c0 = static_cast<Constant&>(*lit);
            if (c0.ck == Constant::CK::Int || c0.ck == Constant::CK::Float) {
                Tok op = check(Tok::PLUS) ? Tok::PLUS : Tok::MINUS;
                advance();
                ExprPtr imag = parse_atom();
                auto b = std::make_unique<BinOp>(op, std::move(lit), std::move(imag));
                b->line = ln;
                b->col = cl;
                lit = std::move(b);
            }
        }
        auto p = std::make_shared<Pattern>();
        p->kind = PatternKind::Literal;
        p->lit = std::move(lit);
        p->line = ln;
        p->col = cl;
        return p;
    }

    throw err_at("unsupported pattern", ln, cl);
}

}  // namespace pyadamas
