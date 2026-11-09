// parser_match.cpp —— match / case 结构化模式匹配（语法层）
#include "parser.h"

namespace py23 {

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
                    star->kind = PatternKind::Wildcard;
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
            auto base = std::make_unique<Name>(std::move(text));
            base->line = ln;
            base->col = cl;
            ExprPtr dotted = std::move(base);
            while (match(Tok::DOT)) {
                if (!check(Tok::NAME)) throw err("expected attribute name after '.'");
                auto at = std::make_unique<Attribute>();
                at->value = std::move(dotted);
                at->attr = advance().text;
                at->line = ln;
                at->col = cl;
                dotted = std::move(at);
            }
            if (check(Tok::LPAREN))
                throw err_at("class patterns with dotted names are not supported yet", ln, cl);
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
    if (check_any({Tok::INT, Tok::FLOAT, Tok::COMPLEX, Tok::STRING, Tok::KW_TRUE,
                   Tok::KW_FALSE, Tok::KW_NONE})) {
        ExprPtr lit = parse_atom();
        if (lit->kind == ExprKind::Constant) {
            auto& c = static_cast<Constant&>(*lit);
            if (neg) {
                if (c.ck == Constant::CK::Int) c.i = -c.i;
                else if (c.ck == Constant::CK::Float) c.f = -c.f;
                else if (c.ck == Constant::CK::Complex) c.im = -c.im;
                else throw err_at("invalid negative literal pattern", ln, cl);
            }
        } else {
            throw err_at("literal pattern must be a constant", ln, cl);
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

}  // namespace py23
