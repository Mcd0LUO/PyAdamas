// pydecl.h —— .pysig / .cpya 两个声明文件加载器的共用前端
//
// 声明文件统一是 **Python 语法**（stub 风格）：没有实现，只有 def / 注解赋值 /
// class / 装饰器。两个加载器（native_sigs.h 的原生模块签名、cimport.cpp 的 C
// 声明）只差「AST → 什么结构」，**读文件、词法/语法、顶层语句白名单、装饰器、
// 字面量、诊断**必须共用这一份，绝不写第二套（否则又会出现两套语法）。
//
// 设计要点：
//   * 解析复用编译器自己的 Lexer + Parser（不裁剪、不分叉）；
//   * 错误用 pydecl::Error{line, msg} 抛出，由调用方决定怎么报（.pysig 直接
//     fatal 退出；.cpya 把「路径:行: 消息」填进 err 返回）；
//   * 这里只做 **AST 级** 的事，不认识 pyadamas 的 Type 或 CKind——注解语义
//     由各自的加载器决定（.pysig 走 TypeChecker::parse_annotation_for_decl，
//     .cpya 走自己的 CKind 映射）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

namespace pyadamas {
namespace pydecl {

// ── 诊断 ──────────────────────────────────────────────────────────────

struct Error {
    int line = 0;
    std::string msg;
};

[[noreturn]] inline void fail(int line, std::string msg) {
    throw Error{line, std::move(msg)};
}

// ── 读文件 + 解析（抛出 Error；行号来自 Lexer/ParseError）──────────────

inline std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) fail(0, "cannot open declaration file");
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::unique_ptr<Module> parse_text(std::string_view src) {
    std::vector<Token> toks;
    try {
        toks = Lexer(src).run();
    } catch (const LexError& e) {
        fail(e.line, e.what());
    }
    try {
        Parser parser(std::move(toks));
        return parser.parse();
    } catch (const ParseError& e) {
        fail(e.line, e.what());
    }
}

inline std::unique_ptr<Module> parse_file(const std::string& path) {
    return parse_text(read_file(path));
}

// ── AST 小工具（两个加载器都要用）────────────────────────────────────

// 模块/函数体里的字符串常量语句 = docstring。
inline bool is_docstring(const Stmt& s) {
    if (s.kind != StmtKind::Expr) return false;
    const auto& e = static_cast<const ExprStmt&>(s);
    const auto* c = dynamic_cast<const Constant*>(e.value.get());
    return c && c->ck == Constant::CK::Str;
}

inline const char* stmt_kind_name(StmtKind k) {
    switch (k) {
        case StmtKind::Expr: return "expression statement";
        case StmtKind::Assign: return "assignment";
        case StmtKind::AugAssign: return "augmented assignment";
        case StmtKind::AnnAssign: return "annotated assignment";
        case StmtKind::Return: return "return";
        case StmtKind::Break: return "break";
        case StmtKind::Continue: return "continue";
        case StmtKind::Pass: return "pass";
        case StmtKind::If: return "if";
        case StmtKind::While: return "while";
        case StmtKind::For: return "for";
        case StmtKind::FunctionDef: return "def";
        case StmtKind::ClassDef: return "class";
        case StmtKind::Try: return "try";
        case StmtKind::With: return "with";
        case StmtKind::Raise: return "raise";
        case StmtKind::Assert: return "assert";
        case StmtKind::Import: return "import";
        case StmtKind::ImportFrom: return "from-import";
        case StmtKind::Global: return "global";
        case StmtKind::Nonlocal: return "nonlocal";
        case StmtKind::Delete: return "del";
        case StmtKind::Match: return "match";
        case StmtKind::CImport: return "cimport";
        case StmtKind::StructDef: return "struct";
    }
    return "statement";
}

// 注解表达式的「类型名」（Name / Attribute 的最后一段 / Subscript 的基）。
inline std::string annotation_base_name(const Expr* e) {
    if (!e) return {};
    if (const auto* n = dynamic_cast<const Name*>(e)) return n->id;
    if (const auto* a = dynamic_cast<const Attribute*>(e)) return a->attr;
    if (const auto* s = dynamic_cast<const Subscript*>(e)) return annotation_base_name(s->value.get());
    if (const auto* c = dynamic_cast<const Constant*>(e))
        return c->ck == Constant::CK::Str ? "string-annotation" : "constant";
    return {};
}

// ── 装饰器 ────────────────────────────────────────────────────────────
// 只支持 @name 与 @name("字符串") 两种形态（本项目声明文件够用）。
struct Decorator {
    std::string name;      // native / extern / extern_var
    bool has_string_arg = false;
    std::string arg;
    int line = 0;
};

inline Decorator decorator_of(const Expr& d, const std::string& owner) {
    Decorator out;
    out.line = d.line;
    const auto* call = dynamic_cast<const Call*>(&d);
    const auto* name = call ? dynamic_cast<const Name*>(call->func.get())
                            : dynamic_cast<const Name*>(&d);
    if (!name)
        fail(d.line,
             "unknown decorator on '" + owner + "' (only @name or @name(\"...\") is allowed)");
    out.name = name->id;
    if (!call) return out;  // @name 裸装饰器
    if (call->args.size() != 1 || !call->keywords.empty())
        fail(d.line, "@" + out.name + " on '" + owner +
                         "' expects exactly one string literal argument");
    const auto* c = dynamic_cast<const Constant*>(call->args[0].get());
    if (!c || c->ck != Constant::CK::Str || c->s.empty())
        fail(d.line, "@" + out.name + " on '" + owner +
                         "' expects a non-empty string literal argument");
    out.has_string_arg = true;
    out.arg = c->s;
    return out;
}

// 函数体：只允许 pass（可带前置 docstring）。
inline void check_pass_body(const FunctionDef& fd) {
    size_t i = 0;
    if (i < fd.body.size() && is_docstring(*fd.body[i])) ++i;
    if (i >= fd.body.size())
        fail(fd.line, "function '" + fd.name + "' has no body (declaration bodies must be pass)");
    const Stmt& s = *fd.body[i];
    if (s.kind != StmtKind::Pass)
        fail(s.line, "function '" + fd.name + "' body must be pass (optionally preceded by a "
                     "docstring); found " + std::string(stmt_kind_name(s.kind)));
    if (i + 1 != fd.body.size())
        fail(fd.body[i + 1]->line, "function '" + fd.name +
                                       "' body must contain only pass; found an extra " +
                                       stmt_kind_name(fd.body[i + 1]->kind));
}

// ── 字面量 ────────────────────────────────────────────────────────────
// 声明文件里的默认值/常量初值：只允许字面量（含一元负号折叠）。
struct Literal {
    enum class Kind : uint8_t { None, Bool, Int, Float, Str } kind = Kind::None;
    int64_t i = 0;
    bool u64 = false;  // 十进制 u64 范围字面量（i 存无符号位模式）
    double f = 0.0;
    bool b = false;
    std::string s;
    int line = 0;
};

inline std::optional<Literal> literal_of(const Expr* e) {
    if (!e) return std::nullopt;
    Literal v;
    v.line = e->line;
    if (e->kind == ExprKind::UnaryOp) {
        const auto& u = static_cast<const UnaryOp&>(*e);
        if (u.op != Tok::MINUS && u.op != Tok::PLUS) return std::nullopt;
        auto inner = literal_of(u.operand.get());
        if (!inner) return std::nullopt;
        if (u.op == Tok::PLUS) return inner;
        switch (inner->kind) {
            case Literal::Kind::Int:
                if (inner->u64) return std::nullopt;
                inner->i = -inner->i;
                return inner;
            case Literal::Kind::Float:
                inner->f = -inner->f;
                return inner;
            default: return std::nullopt;
        }
    }
    if (e->kind != ExprKind::Constant) return std::nullopt;
    const auto& c = static_cast<const Constant&>(*e);
    switch (c.ck) {
        case Constant::CK::None: v.kind = Literal::Kind::None; break;
        case Constant::CK::Bool: v.kind = Literal::Kind::Bool; v.b = c.b; break;
        case Constant::CK::Int: v.kind = Literal::Kind::Int; v.i = c.i; v.u64 = c.u64_lit; break;
        case Constant::CK::Float: v.kind = Literal::Kind::Float; v.f = c.f; break;
        case Constant::CK::Str: v.kind = Literal::Kind::Str; v.s = c.s; break;
        default: return std::nullopt;
    }
    return v;
}

}  // namespace pydecl
}  // namespace pyadamas
