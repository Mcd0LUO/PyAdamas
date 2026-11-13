// parser.h —— 递归下降解析器
//
// 表达式用优先级爬升，语句用分派。Python 的几处语法歧义在这里处理：
//   (x)      → 分组
//   (x,)     → 单元组
//   (x for)  → 生成器表达式
//   [x for]  → 列表推导
//   {x: y}   → 字典；{x} → 集合
#pragma once

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast.h"
#include "lexer.h"
#include "token.h"

namespace pyadamas {

struct ParseError : std::runtime_error {
    int line, col;
    ParseError(std::string msg, int ln, int cl)
        : std::runtime_error(std::move(msg)), line(ln), col(cl) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::unique_ptr<Module> parse();

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    int loop_depth_ = 0;
    int func_depth_ = 0;
    int suite_depth_ = 0;   // 嵌套语句块深度；cimport 只允许模块顶层
    int async_depth_ = 0;   // 处于 async def 内时才允许 await / async for / async with
    int genexp_depth_ = 0;  // 生成器表达式体内允许 async for（延迟求值）
    int expr_depth_ = 0;    // 防止深层嵌套把栈打穿
    int unary_depth_ = 0;
    int not_depth_ = 0;

    // ────────────────────────── 基础操作 ──────────────────────────
    const Token& cur() const { return toks_[pos_]; }
    const Token& peek_tok(size_t n = 1) const {
        return toks_[std::min(pos_ + n, toks_.size() - 1)];
    }
    bool check(Tok k) const { return cur().kind == k; }
    bool check_any(std::initializer_list<Tok> ks) const {
        for (Tok k : ks) if (cur().kind == k) return true;
        return false;
    }
    Token advance() { return toks_[pos_++]; }
    bool match(Tok k) {
        if (check(k)) { ++pos_; return true; }
        return false;
    }
    Token expect(Tok k, std::string_view what) {
        if (!check(k)) throw err(std::format("expected {}, found '{}'", what, describe(cur())));
        return advance();
    }
    ParseError err(std::string msg) const { return ParseError(std::move(msg), cur().line, cur().col); }
    ParseError err_at(std::string msg, int line, int col) const { return ParseError(std::move(msg), line, col); }

    static std::string describe(const Token& t);

    void skip_newlines() {
        while (check(Tok::NEWLINE) || check(Tok::SEMICOLON)) advance();
    }

    template <typename T>
    std::unique_ptr<T> node_at(int line, int col) {
        auto n = std::make_unique<T>();
        n->line = line;
        n->col = col;
        return n;
    }

    // 递归下降的栈深保护：正常代码几百层都用不到，但生成代码或恶意
    // 输入可以用几千层括号把进程打崩，错误信息远比段错误好。
    class DepthGuard {
    public:
        DepthGuard(Parser& p, int& counter, int limit, const char* what)
            : p_(p), counter_(counter) {
            if (++counter_ > limit) throw p_.err(what);
        }
        ~DepthGuard() { --counter_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

    private:
        Parser& p_;
        int& counter_;
    };

    static constexpr int kMaxExprDepth = 200;
    static constexpr int kMaxUnaryDepth = 200;

    // ────────────────────────── 语句 ──────────────────────────
    StmtPtr parse_statement();

    StmtPtr parse_decorated();

    // suite：要么是换行 + 缩进块，要么是同一行的简单语句
    std::vector<StmtPtr> parse_suite();

    StmtPtr parse_if();

    StmtPtr parse_while();

    StmtPtr parse_for(bool is_async);

    StmtPtr parse_funcdef(std::vector<ExprPtr> decorators, bool is_async);

    Arguments parse_arguments();

    StmtPtr parse_classdef(std::vector<ExprPtr> decorators);

    StmtPtr parse_try();

    StmtPtr parse_with(bool is_async);

    bool paren_with_items() const;

    // ────────────────────────── 简单语句 ──────────────────────────
    StmtPtr parse_simple_statement();

    bool at_stmt_end() const {
        return check(Tok::NEWLINE) || check(Tok::SEMICOLON) || check(Tok::END) || check(Tok::DEDENT);
    }

    void check_assignable(const Expr& e);

    StmtPtr parse_import();

    StmtPtr parse_cimport();

    StmtPtr parse_import_from();

    std::string parse_dotted_name();

    // ────────────────────────── 表达式 ──────────────────────────

    // 逗号列表：多个时折叠成 tuple
    ExprPtr parse_expr_list();

    // for 的 iterable 不能把顶层的 in 吃掉
    ExprPtr parse_expr_list_no_in() {
        return parse_expr_list();
    }

    bool at_expr_list_end() const {
        return check_any({Tok::NEWLINE, Tok::END, Tok::EQUAL, Tok::RPAREN, Tok::RBRACKET,
                          Tok::RBRACE, Tok::COLON, Tok::SEMICOLON, Tok::KW_IN, Tok::DEDENT});
    }

    ExprPtr parse_namedexpr();

    ExprPtr parse_lambda_or_ifexp() {
        if (check(Tok::KW_LAMBDA)) return parse_lambda();
        return parse_ifexp();
    }

    ExprPtr parse_ifexp();

    ExprPtr parse_lambda();

    Arguments parse_lambda_params();

    ExprPtr parse_or();

    ExprPtr parse_and();

    ExprPtr parse_not();

    ExprPtr parse_comparison();

    ExprPtr parse_bitor();

    ExprPtr parse_bitxor();

    ExprPtr parse_bitand();

    ExprPtr parse_shift();

    ExprPtr parse_arith();

    ExprPtr parse_term();

    ExprPtr parse_factor();

    ExprPtr parse_power();

    ExprPtr parse_await();

    // 后缀链：调用、下标、属性
    ExprPtr parse_primary();

    ExprPtr parse_subscript_slice();

    bool looks_like_genexp() const;

    bool looks_like_match_stmt() const;

    // struct 是软关键字：只有 "struct NAME: NEWLINE INDENT" 才是定义。
    bool looks_like_struct_def() const;
    StmtPtr parse_structdef();

    // match/case 模式
    StmtPtr parse_match();
    PatternPtr parse_pattern();
    PatternPtr parse_or_pattern();
    PatternPtr parse_closed_pattern();
    PatternPtr parse_class_pattern(std::string name, int ln, int cl);
    PatternPtr parse_sequence_pattern(Tok close);
    MatchCase parse_case();

    // ────────────────────────── 原子 ──────────────────────────
    ExprPtr parse_atom();

    bool at_yield_end() const {
        return check_any({Tok::NEWLINE, Tok::RPAREN, Tok::RBRACKET, Tok::RBRACE,
                          Tok::END, Tok::SEMICOLON, Tok::KW_IN, Tok::DEDENT});
    }

    ExprPtr parse_paren();

    ExprPtr parse_list_or_comp();

    ExprPtr parse_dict_or_set();

    ExprPtr parse_comp_for();

    // 推导式里的 iterable 后面可能跟 if/for，必须避免把条件表达式吃掉
    ExprPtr parse_or_no_conditional() { return parse_or(); }

    // for/with 的目标：支持 a, b 与 (a, b) 与 [a, b] 与 *rest。
    // 注意不能走 parse_namedexpr —— 它会把 "in" 当成比较运算符吃掉，
    // 于是 `for a, *b in xs` 会被解析成 `for a, *(b in xs)`。
    ExprPtr parse_target() {
        if (check(Tok::STAR)) {
            auto n = node_at<Starred>(cur().line, cur().col);
            advance();
            n->value = parse_target();
            return n;
        }
        return parse_primary();
    }

    // for/with 的目标：支持 a, b 与 (a, b) 与 [a, b]
    ExprPtr parse_target_list();

    // ────────────────────────── f-string ──────────────────────────
    // 输入是引号内原文（lexer 保留），这里拆成 字面量 / 表达式 交替
    std::unique_ptr<JoinedStr> parse_fstring(const Token& tok);

public:
    // 供 f-string 内嵌表达式解析使用
    ExprPtr parse_fstring_expr();
};


}  // namespace pyadamas
