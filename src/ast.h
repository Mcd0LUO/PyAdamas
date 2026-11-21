// ast.h —— 抽象语法树
//
// 节点划分对齐 CPython 的 ast 模块，方便对照与后续扩展。
// 暂不支持的语法也有对应节点，parser 会在构造时报错，而不是让架构缺一块。
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

#include "token.h"

namespace pyadamas {

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ────────────────────────────── 表达式 ──────────────────────────────

enum class ExprKind : uint8_t {
    Constant, Name, FormattedValue,
    List, Tuple, Set, Dict,
    BinOp, UnaryOp, BoolOp, Compare,
    Call, Subscript, Attribute, Slice,
    ListComp, SetComp, DictComp, GeneratorExp, Comprehension,
    Lambda, IfExp, Starred, NamedExpr,
    Await, Yield, YieldFrom, JoinedStr,
    Go,  // go f(args)：spawn 虚拟线程，值是 handle（vthread 先验）
};

// 比较运算符。`is` / `is not` / `not in` 必须与 `==` / `!=` / `in` 分开：
// 三者语义完全不同（身份 vs 相等、成员测试 vs 全等），后端不能靠猜。
enum class CmpOp : uint8_t {
    Lt, Le, Gt, Ge, Eq, Ne, In, NotIn, Is, IsNot,
};

inline const char* cmp_name(CmpOp op) {
    switch (op) {
        case CmpOp::Lt: return "<";
        case CmpOp::Le: return "<=";
        case CmpOp::Gt: return ">";
        case CmpOp::Ge: return ">=";
        case CmpOp::Eq: return "==";
        case CmpOp::Ne: return "!=";
        case CmpOp::In: return "in";
        case CmpOp::NotIn: return "not in";
        case CmpOp::Is: return "is";
        case CmpOp::IsNot: return "is not";
    }
    return "?";
}

struct Expr {
    ExprKind kind;
    int line = 1;
    int col = 1;
    explicit Expr(ExprKind k) : kind(k) {}
    virtual ~Expr() = default;
    virtual void dump(std::ostream& os, int ind) const = 0;
};

inline void pad(std::ostream& os, int n) {
    for (int i = 0; i < n; ++i) os << "  ";
}

struct Constant : Expr {
    enum class CK : uint8_t { None, Bool, Int, Float, Str, Bytes, Complex, Ellipsis, Big } ck = CK::None;
    bool b = false;
    int64_t i = 0;
    // 十进制 u64 范围字面量（>INT64_MAX）：i 存无符号位模式，
    // 只有 u64 上下文能消费它（见 TypeChecker::accept_literal_into）。
    bool u64_lit = false;
    double f = 0.0;
    double im = 0.0;  // CK::Complex 的虚部，实部固定 0
    std::string s;

    Constant() : Expr(ExprKind::Constant) {}
    static ExprPtr none() {
        auto c = std::make_unique<Constant>();
        c->ck = CK::None;
        return c;
    }
    static ExprPtr boolean(bool v) {
        auto c = std::make_unique<Constant>();
        c->ck = CK::Bool;
        c->b = v;
        return c;
    }
    static ExprPtr integer(int64_t v) {
        auto c = std::make_unique<Constant>();
        c->ck = CK::Int;
        c->i = v;
        return c;
    }
    static ExprPtr floating(double v) {
        auto c = std::make_unique<Constant>();
        c->ck = CK::Float;
        c->f = v;
        return c;
    }
    static ExprPtr string(std::string v) {
        auto c = std::make_unique<Constant>();
        c->ck = CK::Str;
        c->s = std::move(v);
        return c;
    }
    static ExprPtr ellipsis() {
        auto c = std::make_unique<Constant>();
        c->ck = CK::Ellipsis;
        return c;
    }
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        switch (ck) {
            case CK::None: os << "Constant(None)\n"; break;
            case CK::Bool: os << "Constant(" << (b ? "True" : "False") << ")\n"; break;
            case CK::Int:
                if (u64_lit) os << "Constant(" << static_cast<uint64_t>(i) << "u64)\n";
                else os << "Constant(" << i << ")\n";
                break;
            case CK::Big: os << "Constant(big " << s << ")\n"; break;
            case CK::Float: os << "Constant(" << f << ")\n"; break;
            case CK::Complex: os << "Constant(" << im << "j)\n"; break;
            case CK::Str: os << "Constant(" << quote(s) << ")\n"; break;
            case CK::Bytes: os << "Constant(b" << quote(s) << ")\n"; break;
            case CK::Ellipsis: os << "Constant(Ellipsis)\n"; break;
        }
    }
    static std::string quote(const std::string& v) {
        std::string out = "\"";
        for (char c : v) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out + "\"";
    }
};

struct Name : Expr {
    std::string id;
    explicit Name(std::string n) : Expr(ExprKind::Name), id(std::move(n)) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Name(" << id << ")\n";
    }
};

// f-string 的组成部分
struct JoinedStr;
struct FormattedValue : Expr {
    ExprPtr value;
    // 转换标志: -1 无, 's'/'r'/'a' 各对应 !s !r !a
    int conversion = -1;
    ExprPtr format_spec;  // 可空，对应 :>10 这类
    FormattedValue() : Expr(ExprKind::FormattedValue) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "FormattedValue(conv=" << conversion << ")\n";
        pad(os, ind + 1);
        os << "value:\n";
        value->dump(os, ind + 2);
        if (format_spec) {
            pad(os, ind + 1);
            os << "format_spec:\n";
            format_spec->dump(os, ind + 2);
        }
    }
};

// 既是 f-string 容器，也承载格式说明符里的字面量拼接
struct JoinedStr : Expr {
    std::vector<ExprPtr> values;  // Constant(str) 或 FormattedValue
    JoinedStr() : Expr(ExprKind::JoinedStr) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "JoinedStr\n";
        for (auto& v : values) v->dump(os, ind + 1);
    }
};

struct ListExpr : Expr {
    std::vector<ExprPtr> elts;
    ListExpr() : Expr(ExprKind::List) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "List\n";
        for (auto& e : elts) e->dump(os, ind + 1);
    }
};

struct TupleExpr : Expr {
    std::vector<ExprPtr> elts;
    TupleExpr() : Expr(ExprKind::Tuple) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Tuple\n";
        for (auto& e : elts) e->dump(os, ind + 1);
    }
};

struct SetExpr : Expr {
    std::vector<ExprPtr> elts;
    SetExpr() : Expr(ExprKind::Set) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Set\n";
        for (auto& e : elts) e->dump(os, ind + 1);
    }
};

struct DictExpr : Expr {
    std::vector<ExprPtr> keys;    // nullptr 表示 **unpack
    std::vector<ExprPtr> values;
    DictExpr() : Expr(ExprKind::Dict) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Dict\n";
        for (size_t i = 0; i < values.size(); ++i) {
            if (keys[i]) {
                pad(os, ind + 1);
                os << "key:\n";
                keys[i]->dump(os, ind + 2);
            } else {
                pad(os, ind + 1);
                os << "unpack:\n";
            }
            pad(os, ind + 1);
            os << "value:\n";
            values[i]->dump(os, ind + 2);
        }
    }
};

struct BinOp : Expr {
    Tok op = Tok::PLUS;
    ExprPtr left, right;
    BinOp() : Expr(ExprKind::BinOp) {}
    BinOp(Tok o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::BinOp), op(o), left(std::move(l)), right(std::move(r)) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "BinOp(" << tok_name(op) << ")\n";
        left->dump(os, ind + 1);
        right->dump(os, ind + 1);
    }
};

struct UnaryOp : Expr {
    Tok op = Tok::MINUS;
    ExprPtr operand;
    UnaryOp() : Expr(ExprKind::UnaryOp) {}
    UnaryOp(Tok o, ExprPtr v) : Expr(ExprKind::UnaryOp), op(o), operand(std::move(v)) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "UnaryOp(" << tok_name(op) << ")\n";
        operand->dump(os, ind + 1);
    }
};

struct BoolOp : Expr {
    Tok op;  // KW_AND / KW_OR
    std::vector<ExprPtr> values;
    BoolOp() : Expr(ExprKind::BoolOp), op(Tok::KW_AND) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "BoolOp(" << tok_name(op) << ")\n";
        for (auto& v : values) v->dump(os, ind + 1);
    }
};

struct Compare : Expr {
    ExprPtr left;
    std::vector<CmpOp> ops;
    std::vector<ExprPtr> comparators;
    Compare() : Expr(ExprKind::Compare) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Compare\n";
        left->dump(os, ind + 1);
        for (size_t i = 0; i < ops.size(); ++i) {
            pad(os, ind + 1);
            os << "op: " << cmp_name(ops[i]) << "\n";
            comparators[i]->dump(os, ind + 1);
        }
    }
};

struct Keyword {
    std::string arg;   // 空字符串表示 **kwargs
    ExprPtr value;
};

struct Call : Expr {
    ExprPtr func;
    std::vector<ExprPtr> args;
    std::vector<Keyword> keywords;
    Call() : Expr(ExprKind::Call) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Call\n";
        pad(os, ind + 1);
        os << "func:\n";
        func->dump(os, ind + 2);
        if (!args.empty()) {
            pad(os, ind + 1);
            os << "args:\n";
            for (auto& a : args) a->dump(os, ind + 2);
        }
        for (auto& k : keywords) {
            pad(os, ind + 1);
            os << "keyword " << (k.arg.empty() ? "**" : k.arg) << ":\n";
            k.value->dump(os, ind + 2);
        }
    }
};

struct SliceExpr : Expr {
    ExprPtr lower, upper, step;  // 任一可空
    SliceExpr() : Expr(ExprKind::Slice) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Slice\n";
        if (lower) { pad(os, ind + 1); os << "lower:\n"; lower->dump(os, ind + 2); }
        if (upper) { pad(os, ind + 1); os << "upper:\n"; upper->dump(os, ind + 2); }
        if (step) { pad(os, ind + 1); os << "step:\n"; step->dump(os, ind + 2); }
    }
};

struct Subscript : Expr {
    ExprPtr value;
    ExprPtr slice;  // 普通表达式或 SliceExpr
    Subscript() : Expr(ExprKind::Subscript) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Subscript\n";
        value->dump(os, ind + 1);
        slice->dump(os, ind + 1);
    }
};

struct Attribute : Expr {
    ExprPtr value;
    std::string attr;
    Attribute() : Expr(ExprKind::Attribute) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Attribute(." << attr << ")\n";
        value->dump(os, ind + 1);
    }
};

// 推导式的 for 子句
struct Comprehension : Expr {
    ExprPtr target;
    ExprPtr iter;
    std::vector<ExprPtr> ifs;
    bool is_async = false;
    Comprehension() : Expr(ExprKind::Comprehension) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Comprehension" << (is_async ? " async" : "") << "\n";
        pad(os, ind + 1);
        os << "target:\n";
        target->dump(os, ind + 2);
        pad(os, ind + 1);
        os << "iter:\n";
        iter->dump(os, ind + 2);
        for (auto& c : ifs) {
            pad(os, ind + 1);
            os << "if:\n";
            c->dump(os, ind + 2);
        }
    }
};

struct ListComp : Expr {
    ExprPtr elt;
    std::vector<ExprPtr> generators;
    ListComp() : Expr(ExprKind::ListComp) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "ListComp\n";
        pad(os, ind + 1);
        os << "elt:\n";
        elt->dump(os, ind + 2);
        for (auto& g : generators) g->dump(os, ind + 1);
    }
};

struct SetComp : Expr {
    ExprPtr elt;
    std::vector<ExprPtr> generators;
    SetComp() : Expr(ExprKind::SetComp) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "SetComp\n";
        elt->dump(os, ind + 1);
        for (auto& g : generators) g->dump(os, ind + 1);
    }
};

struct DictComp : Expr {
    ExprPtr key, value;
    std::vector<ExprPtr> generators;
    DictComp() : Expr(ExprKind::DictComp) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "DictComp\n";
        pad(os, ind + 1); os << "key:\n"; key->dump(os, ind + 2);
        pad(os, ind + 1); os << "value:\n"; value->dump(os, ind + 2);
        for (auto& g : generators) g->dump(os, ind + 1);
    }
};

struct GeneratorExp : Expr {
    ExprPtr elt;
    std::vector<ExprPtr> generators;
    GeneratorExp() : Expr(ExprKind::GeneratorExp) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "GeneratorExp\n";
        elt->dump(os, ind + 1);
        for (auto& g : generators) g->dump(os, ind + 1);
    }
};

struct Arg {
    std::string name;
    ExprPtr annotation;  // 可空
};

struct Arguments {
    std::vector<Arg> posonly;
    std::vector<Arg> args;
    std::unique_ptr<Arg> vararg;   // *args
    std::vector<Arg> kwonly;
    std::vector<ExprPtr> kw_defaults;
    std::unique_ptr<Arg> kwarg;    // **kwargs
    std::vector<ExprPtr> defaults;
};

struct Lambda : Expr {
    Arguments args;
    ExprPtr body;
    Lambda() : Expr(ExprKind::Lambda) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Lambda\n";
        pad(os, ind + 1);
        os << "params:";
        for (auto& a : args.args) os << " " << a.name;
        os << "\n";
        body->dump(os, ind + 1);
    }
};

struct IfExp : Expr {
    ExprPtr test, body, orelse;
    IfExp() : Expr(ExprKind::IfExp) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "IfExp\n";
        pad(os, ind + 1); os << "test:\n"; test->dump(os, ind + 2);
        pad(os, ind + 1); os << "body:\n"; body->dump(os, ind + 2);
        pad(os, ind + 1); os << "orelse:\n"; orelse->dump(os, ind + 2);
    }
};

struct Starred : Expr {
    ExprPtr value;
    Starred() : Expr(ExprKind::Starred) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Starred\n";
        value->dump(os, ind + 1);
    }
};

struct NamedExpr : Expr {
    ExprPtr target, value;
    NamedExpr() : Expr(ExprKind::NamedExpr) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "NamedExpr\n";
        target->dump(os, ind + 1);
        value->dump(os, ind + 1);
    }
};

struct Await : Expr {
    ExprPtr value;
    Await() : Expr(ExprKind::Await) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Await\n";
        value->dump(os, ind + 1);
    }
};

struct GoExpr : Expr {
    ExprPtr call;  // 必须是 Call（typecheck 校验）
    GoExpr() : Expr(ExprKind::Go) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Go\n";
        call->dump(os, ind + 1);
    }
};

struct Yield : Expr {
    ExprPtr value;  // 可空
    Yield() : Expr(ExprKind::Yield) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Yield\n";
        if (value) value->dump(os, ind + 1);
    }
};

struct YieldFrom : Expr {
    ExprPtr value;
    YieldFrom() : Expr(ExprKind::YieldFrom) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "YieldFrom\n";
        value->dump(os, ind + 1);
    }
};

// ────────────────────────────── 语句 ──────────────────────────────

enum class StmtKind : uint8_t {
    Expr, Assign, AugAssign, AnnAssign,
    Return, Break, Continue, Pass,
    If, While, For,
    FunctionDef, ClassDef,
    Try, With, Raise, Assert,
    Import, ImportFrom, Global, Nonlocal, Delete, Match, CImport, StructDef,
};

struct Stmt {
    StmtKind kind;
    int line = 1;
    int col = 1;
    explicit Stmt(StmtKind k) : kind(k) {}
    virtual ~Stmt() = default;
    virtual void dump(std::ostream& os, int ind) const = 0;
};

struct ExprStmt : Stmt {
    ExprPtr value;
    ExprStmt() : Stmt(StmtKind::Expr) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "ExprStmt\n";
        value->dump(os, ind + 1);
    }
};

struct Assign : Stmt {
    std::vector<ExprPtr> targets;
    ExprPtr value;
    Assign() : Stmt(StmtKind::Assign) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Assign\n";
        for (auto& t : targets) {
            pad(os, ind + 1); os << "target:\n"; t->dump(os, ind + 2);
        }
        pad(os, ind + 1); os << "value:\n"; value->dump(os, ind + 2);
    }
};

struct AugAssign : Stmt {
    ExprPtr target;
    Tok op;
    ExprPtr value;
    AugAssign() : Stmt(StmtKind::AugAssign), op(Tok::PLUSEQUAL) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "AugAssign(" << tok_name(op) << ")\n";
        pad(os, ind + 1); os << "target:\n"; target->dump(os, ind + 2);
        pad(os, ind + 1); os << "value:\n"; value->dump(os, ind + 2);
    }
};

struct AnnAssign : Stmt {
    ExprPtr target;
    ExprPtr annotation;
    ExprPtr value;  // 可空（仅声明）
    AnnAssign() : Stmt(StmtKind::AnnAssign) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "AnnAssign\n";
        pad(os, ind + 1); os << "target:\n"; target->dump(os, ind + 2);
        pad(os, ind + 1); os << "annotation:\n"; annotation->dump(os, ind + 2);
        if (value) { pad(os, ind + 1); os << "value:\n"; value->dump(os, ind + 2); }
    }
};

struct Return : Stmt {
    ExprPtr value;  // 可空
    Return() : Stmt(StmtKind::Return) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Return\n";
        if (value) value->dump(os, ind + 1);
    }
};

struct Break : Stmt {
    Break() : Stmt(StmtKind::Break) {}
    void dump(std::ostream& os, int ind) const override { pad(os, ind); os << "Break\n"; }
};

struct Continue : Stmt {
    Continue() : Stmt(StmtKind::Continue) {}
    void dump(std::ostream& os, int ind) const override { pad(os, ind); os << "Continue\n"; }
};

struct Pass : Stmt {
    Pass() : Stmt(StmtKind::Pass) {}
    void dump(std::ostream& os, int ind) const override { pad(os, ind); os << "Pass\n"; }
};

struct If : Stmt {
    struct Branch {
        ExprPtr cond;
        std::vector<StmtPtr> body;
    };
    std::vector<Branch> branches;
    std::vector<StmtPtr> orelse;
    If() : Stmt(StmtKind::If) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "If\n";
        for (auto& b : branches) {
            pad(os, ind + 1); os << "branch(test):\n"; b.cond->dump(os, ind + 2);
            pad(os, ind + 1); os << "body:\n";
            for (auto& s : b.body) s->dump(os, ind + 2);
        }
        if (!orelse.empty()) {
            pad(os, ind + 1); os << "orelse:\n";
            for (auto& s : orelse) s->dump(os, ind + 2);
        }
    }
};

struct While : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    std::vector<StmtPtr> orelse;
    While() : Stmt(StmtKind::While) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "While\n";
        pad(os, ind + 1); os << "cond:\n"; cond->dump(os, ind + 2);
        pad(os, ind + 1); os << "body:\n";
        for (auto& s : body) s->dump(os, ind + 2);
        if (!orelse.empty()) {
            pad(os, ind + 1); os << "orelse:\n";
            for (auto& s : orelse) s->dump(os, ind + 2);
        }
    }
};

struct For : Stmt {
    ExprPtr target;
    ExprPtr iter;
    std::vector<StmtPtr> body;
    std::vector<StmtPtr> orelse;
    bool is_async = false;
    For() : Stmt(StmtKind::For) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "For" << (is_async ? " async" : "") << "\n";
        pad(os, ind + 1); os << "target:\n"; target->dump(os, ind + 2);
        pad(os, ind + 1); os << "iter:\n"; iter->dump(os, ind + 2);
        pad(os, ind + 1); os << "body:\n";
        for (auto& s : body) s->dump(os, ind + 2);
        if (!orelse.empty()) {
            pad(os, ind + 1); os << "orelse:\n";
            for (auto& s : orelse) s->dump(os, ind + 2);
        }
    }
};

struct FunctionDef : Stmt {
    std::string name;
    Arguments args;
    ExprPtr returns;                    // 返回标注，可空
    std::vector<StmtPtr> body;
    std::vector<ExprPtr> decorators;
    bool is_async = false;
    FunctionDef() : Stmt(StmtKind::FunctionDef) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "FunctionDef(" << name << ")" << (is_async ? " async" : "") << "\n";
        for (auto& d : decorators) {
            pad(os, ind + 1); os << "decorator:\n"; d->dump(os, ind + 2);
        }
        pad(os, ind + 1);
        os << "params:";
        for (auto& a : args.posonly) os << " " << a.name << "/";
        for (auto& a : args.args) {
            os << " " << a.name;
            if (a.annotation) os << ":<annot>";
            if (!args.defaults.empty()) os << "";
        }
        if (args.vararg) os << " *" << args.vararg->name;
        for (auto& a : args.kwonly) os << " " << a.name;
        if (args.kwarg) os << " **" << args.kwarg->name;
        os << "\n";
        if (returns) {
            pad(os, ind + 1); os << "returns:\n"; returns->dump(os, ind + 2);
        }
        pad(os, ind + 1); os << "body:\n";
        for (auto& s : body) s->dump(os, ind + 2);
    }
};

struct ClassDef : Stmt {
    std::string name;
    std::vector<ExprPtr> bases;
    std::vector<Keyword> keywords;
    std::vector<StmtPtr> body;
    std::vector<ExprPtr> decorators;
    ClassDef() : Stmt(StmtKind::ClassDef) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "ClassDef(" << name << ")\n";
        for (auto& b : bases) b->dump(os, ind + 1);
        pad(os, ind + 1); os << "body:\n";
        for (auto& s : body) s->dump(os, ind + 2);
    }
};

// 值类型 struct 的字段声明：`x: float`。
struct StructFieldDef {
    std::string name;
    ExprPtr type;  // 只允许 int / float / bool 的 Name
    int line = 1;
    int col = 1;
};

// `struct Point:` 块。布局与 C 结构体一致，字段按声明顺序排列。
struct StructDef : Stmt {
    std::string name;
    std::vector<StructFieldDef> fields;
    StructDef() : Stmt(StmtKind::StructDef) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "StructDef(" << name << ")\n";
        for (auto& f : fields) {
            pad(os, ind + 1);
            os << "field " << f.name << ":\n";
            f.type->dump(os, ind + 2);
        }
    }
};

struct ExceptHandler {
    ExprPtr type;      // 可空
    std::string name;  // 可空
    std::vector<StmtPtr> body;
};

struct Try : Stmt {
    std::vector<StmtPtr> body;
    std::vector<ExceptHandler> handlers;
    std::vector<StmtPtr> orelse;
    std::vector<StmtPtr> finalbody;
    Try() : Stmt(StmtKind::Try) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Try\n";
        pad(os, ind + 1); os << "body:\n";
        for (auto& s : body) s->dump(os, ind + 2);
        for (auto& h : handlers) {
            pad(os, ind + 1); os << "except " << h.name << ":\n";
            for (auto& s : h.body) s->dump(os, ind + 2);
        }
        if (!finalbody.empty()) {
            pad(os, ind + 1); os << "finally:\n";
            for (auto& s : finalbody) s->dump(os, ind + 2);
        }
    }
};

struct WithItem {
    ExprPtr context;
    ExprPtr optional_vars;  // 可空
};

struct With : Stmt {
    std::vector<WithItem> items;
    std::vector<StmtPtr> body;
    bool is_async = false;
    With() : Stmt(StmtKind::With) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "With" << (is_async ? " async" : "") << "\n";
        for (auto& it : items) it.context->dump(os, ind + 1);
        for (auto& s : body) s->dump(os, ind + 2);
    }
};

struct Raise : Stmt {
    ExprPtr exc;   // 可空
    ExprPtr cause; // 可空
    Raise() : Stmt(StmtKind::Raise) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Raise\n";
        if (exc) exc->dump(os, ind + 1);
        if (cause) cause->dump(os, ind + 1);
    }
};

struct Assert : Stmt {
    ExprPtr test;
    ExprPtr msg;  // 可空
    Assert() : Stmt(StmtKind::Assert) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Assert\n";
        test->dump(os, ind + 1);
    }
};

struct Alias {
    std::string name;
    std::string asname;  // 可空
};

struct Import : Stmt {
    std::vector<Alias> names;
    Import() : Stmt(StmtKind::Import) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Import";
        for (auto& a : names) os << " " << a.name << (a.asname.empty() ? "" : " as " + a.asname);
        os << "\n";
    }
};

struct CImport : Stmt {
    std::vector<Alias> names;
    CImport() : Stmt(StmtKind::CImport) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "CImport";
        for (auto& a : names)
            os << " " << a.name << (a.asname.empty() ? "" : " as " + a.asname);
        os << "\n";
    }
};

struct ImportFrom : Stmt {
    std::string module;
    std::vector<Alias> names;
    int level = 0;  // 相对导入的点数
    ImportFrom() : Stmt(StmtKind::ImportFrom) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "ImportFrom(" << module << ")\n";
    }
};

struct Global : Stmt {
    std::vector<std::string> names;
    Global() : Stmt(StmtKind::Global) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Global";
        for (auto& n : names) os << " " << n;
        os << "\n";
    }
};

struct Nonlocal : Stmt {
    std::vector<std::string> names;
    Nonlocal() : Stmt(StmtKind::Nonlocal) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Nonlocal";
        for (auto& n : names) os << " " << n;
        os << "\n";
    }
};

enum class PatternKind : uint8_t {
    Literal, Capture, Wildcard, Sequence, Or, Mapping, Class, Value, As,
};

struct Pattern;
using PatternPtr = std::shared_ptr<Pattern>;

struct Pattern {
    PatternKind kind = PatternKind::Wildcard;
    int line = 1;
    int col = 1;
    ExprPtr lit;                 // Literal / Value（点分名字）
    std::string name;            // Capture / As
    std::vector<PatternPtr> items;  // Sequence / Or / As（items[0]）
    int star = -1;               // Sequence：* 模式的下标
    std::vector<std::pair<ExprPtr, PatternPtr>> pairs;  // Mapping：key → pattern
    std::string rest;            // Mapping：**rest 的绑定名（空 = 无）
    std::string cls;             // Class：类名（可能带点）
    std::string qualname;        // Class：解析后的模块限定名（跨模块模式用）
    std::vector<PatternPtr> positional;                    // Class：位置子模式
    std::vector<std::pair<std::string, PatternPtr>> attrs; // Class：属性子模式
};

struct MatchCase {
    PatternPtr pattern;
    ExprPtr guard;
    std::vector<StmtPtr> body;
};

struct Match : Stmt {
    ExprPtr subject;
    std::vector<MatchCase> cases;
    Match() : Stmt(StmtKind::Match) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind);
        os << "Match\n";
        subject->dump(os, ind + 1);
        for (auto& c : cases) {
            pad(os, ind + 1);
            os << "case guard=" << (c.guard ? "yes" : "no") << "\n";
            for (auto& s : c.body) s->dump(os, ind + 2);
        }
    }
};

struct Delete : Stmt {
    std::vector<ExprPtr> targets;
    Delete() : Stmt(StmtKind::Delete) {}
    void dump(std::ostream& os, int ind) const override {
        pad(os, ind); os << "Delete\n";
        for (auto& t : targets) t->dump(os, ind + 1);
    }
};

// 一个源文件
struct Module {
    std::vector<StmtPtr> body;
    void dump(std::ostream& os) const {
        os << "Module\n";
        for (auto& s : body) s->dump(os, 1);
    }
};

// ────────────────────────── 构造辅助 ──────────────────────────
// unique_ptr 的所有权在这些函数里完整转移，避免在 parser 里手写 make_unique

inline ExprPtr make_binop(Tok op, ExprPtr lhs, ExprPtr rhs, int line, int col) {
    auto n = std::make_unique<BinOp>(op, std::move(lhs), std::move(rhs));
    n->line = line;
    n->col = col;
    return n;
}

inline ExprPtr make_unary(Tok op, ExprPtr operand, int line, int col) {
    // P3：±big 字面量折叠成带符号常量（-18446744073709551616 是一个 big
    // 常量，不是运行期 neg）。只影响新的 CK::Big，既有常量路径逐字不变。
    if (operand && operand->kind == ExprKind::Constant &&
        (op == Tok::MINUS || op == Tok::PLUS)) {
        auto& c = static_cast<Constant&>(*operand);
        if (c.ck == Constant::CK::Big) {
            if (op == Tok::MINUS) {
                if (!c.s.empty() && c.s[0] == '-') c.s.erase(0, 1);
                else c.s.insert(c.s.begin(), '-');
            }
            return operand;
        }
    }
    auto n = std::make_unique<UnaryOp>(op, std::move(operand));
    n->line = line;
    n->col = col;
    return n;
}

}  // namespace pyadamas
