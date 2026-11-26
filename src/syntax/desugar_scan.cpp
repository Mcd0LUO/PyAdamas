// syntax/desugar.cpp —— 把 lambda 和生成器表达式展开成合成的嵌套函数
//
// lambda 是表达式，但闭包/捕获/引用计数都建立在 FunctionDef 上。这里在
// 类型检查之前把它改写成：
//
//     __pyadamas$lambda_N = def (...) -> ...: return <body>
//     ... __pyadamas$lambda_N ...
//
// 生成器表达式同理，展开成生成器函数 + 一次调用，让惰性语义落在已有的
// 生成器状态机上：
//
//     def __pyadamas$gen_N(it):
//         for x in it: ... yield elt
//     __pyadamas$gen_N(<最外层 iterable>)
//
// 合成函数插在包含该表达式的那条语句之前，所以循环体里的 lambda 仍然
// 每次执行都创建新的闭包。类体里的 `f = lambda self, ...: ...` 在脱糖阶段
// 改写成方法定义；类体字段默认值里的 lambda 与类体生成器表达式仍不展开
// （后者按 list 构造）。
#include <unordered_set>

#include <format>
#include <memory>
#include <utility>
#include <vector>

#include "syntax/desugar_scan.h"

namespace pyadamas {

// map/filter 改写前先确认这两个名字没有被用户绑定过。绑定过就保留普通
// 调用语义（这里不区分作用域，宁可少改写也不误改）。
bool target_binds(const Expr* e, const std::string& n) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Name: return static_cast<const Name*>(e)->id == n;
        case ExprKind::Tuple:
            for (auto& x : static_cast<const TupleExpr*>(e)->elts)
                if (target_binds(x.get(), n)) return true;
            return false;
        case ExprKind::List:
            for (auto& x : static_cast<const ListExpr*>(e)->elts)
                if (target_binds(x.get(), n)) return true;
            return false;
        case ExprKind::Starred: return target_binds(static_cast<const Starred*>(e)->value.get(), n);
        default: return false;
    }
}

bool args_bind(const Arguments& a, const std::string& n) {
    auto hit = [&](const Arg& x) { return x.name == n; };
    for (auto& x : a.posonly)
        if (hit(x)) return true;
    for (auto& x : a.args)
        if (hit(x)) return true;
    for (auto& x : a.kwonly)
        if (hit(x)) return true;
    if (a.vararg && hit(*a.vararg)) return true;
    if (a.kwarg && hit(*a.kwarg)) return true;
    return false;
}

bool stmt_binds(const Stmt& s, const std::string& n);

bool body_binds(const std::vector<StmtPtr>& body, const std::string& n) {
    for (auto& s : body)
        if (s && stmt_binds(*s, n)) return true;
    return false;
}

static bool pattern_binds(const Pattern* p, const std::string& n) {
    if (!p) return false;
    if ((p->kind == PatternKind::Capture || p->kind == PatternKind::As) && p->name == n)
        return true;
    if (!p->rest.empty() && p->rest == n) return true;
    for (auto& it : p->items)
        if (pattern_binds(it.get(), n)) return true;
    for (auto& kv : p->pairs)
        if (pattern_binds(kv.second.get(), n)) return true;
    for (auto& sp : p->positional)
        if (pattern_binds(sp.get(), n)) return true;
    for (auto& a : p->attrs)
        if (pattern_binds(a.second.get(), n)) return true;
    return false;
}

// 表达式里是否出现了绑定 n 的海象运算符（0.637）。
// stmt_binds 只看语句结构，walrus 藏在任意表达式位置，这里递归扫常见的
// 容器节点；扫不到的形态最多是"漏判成内建"，不会误判。
static bool expr_binds(const Expr* e, const std::string& n) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::NamedExpr: {
            auto& x = static_cast<const NamedExpr&>(*e);
            if (x.target && x.target->kind == ExprKind::Name &&
                static_cast<const Name&>(*x.target).id == n)
                return true;
            return expr_binds(x.value.get(), n);
        }
        case ExprKind::Call: {
            auto& c = static_cast<const Call&>(*e);
            if (expr_binds(c.func.get(), n)) return true;
            for (auto& a : c.args)
                if (expr_binds(a.get(), n)) return true;
            for (auto& k : c.keywords)
                if (expr_binds(k.value.get(), n)) return true;
            return false;
        }
        case ExprKind::BinOp: {
            auto& b = static_cast<const BinOp&>(*e);
            return expr_binds(b.left.get(), n) || expr_binds(b.right.get(), n);
        }
        case ExprKind::UnaryOp:
            return expr_binds(static_cast<const UnaryOp&>(*e).operand.get(), n);
        case ExprKind::BoolOp: {
            for (auto& v : static_cast<const BoolOp&>(*e).values)
                if (expr_binds(v.get(), n)) return true;
            return false;
        }
        case ExprKind::Compare: {
            auto& c = static_cast<const Compare&>(*e);
            if (expr_binds(c.left.get(), n)) return true;
            for (auto& r : c.comparators)
                if (expr_binds(r.get(), n)) return true;
            return false;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<const Subscript&>(*e);
            return expr_binds(s.value.get(), n) || expr_binds(s.slice.get(), n);
        }
        case ExprKind::Attribute:
            return expr_binds(static_cast<const Attribute&>(*e).value.get(), n);
        case ExprKind::IfExp: {
            auto& i = static_cast<const IfExp&>(*e);
            return expr_binds(i.test.get(), n) || expr_binds(i.body.get(), n) ||
                   expr_binds(i.orelse.get(), n);
        }
        case ExprKind::Lambda:
            return expr_binds(static_cast<const Lambda&>(*e).body.get(), n);
        case ExprKind::Starred:
            return expr_binds(static_cast<const Starred&>(*e).value.get(), n);
        default: return false;
    }
}

bool stmt_binds(const Stmt& s, const std::string& n) {
    switch (s.kind) {
        case StmtKind::Assign: {
            auto& a = static_cast<const Assign&>(s);
            for (auto& t : a.targets)
                if (target_binds(t.get(), n)) return true;
            if (expr_binds(static_cast<const Assign&>(s).value.get(), n)) return true;
            return false;
        }
        case StmtKind::AnnAssign:
            return target_binds(static_cast<const AnnAssign&>(s).target.get(), n);
        case StmtKind::AugAssign:
            return target_binds(static_cast<const AugAssign&>(s).target.get(), n);
        case StmtKind::For: {
            auto& f = static_cast<const For&>(s);
            return target_binds(f.target.get(), n) || body_binds(f.body, n) ||
                   body_binds(f.orelse, n);
        }
        case StmtKind::While: {
            auto& w = static_cast<const While&>(s);
            if (expr_binds(w.cond.get(), n)) return true;
            return body_binds(w.body, n) || body_binds(w.orelse, n);
        }
        case StmtKind::If: {
            auto& f = static_cast<const If&>(s);
            for (auto& b : f.branches)
                if (body_binds(b.body, n)) return true;
            return body_binds(f.orelse, n);
        }
        case StmtKind::With: {
            auto& w = static_cast<const With&>(s);
            for (auto& it : w.items)
                if (target_binds(it.optional_vars.get(), n)) return true;
            return body_binds(w.body, n);
        }
        case StmtKind::Try: {
            auto& t = static_cast<const Try&>(s);
            if (body_binds(t.body, n) || body_binds(t.orelse, n) ||
                body_binds(t.finalbody, n))
                return true;
            for (auto& h : t.handlers)
                if (h.name == n || body_binds(h.body, n)) return true;
            return false;
        }
        case StmtKind::FunctionDef: {
            auto& f = static_cast<const FunctionDef&>(s);
            if (f.name == n || args_bind(f.args, n)) return true;
            return body_binds(f.body, n);
        }
        case StmtKind::ClassDef: {
            auto& c = static_cast<const ClassDef&>(s);
            if (c.name == n) return true;
            return body_binds(c.body, n);
        }
        case StmtKind::Import: {
            for (auto& a : static_cast<const Import&>(s).names)
                if ((a.asname.empty() ? a.name : a.asname) == n) return true;
            return false;
        }
        case StmtKind::ImportFrom: {
            for (auto& a : static_cast<const ImportFrom&>(s).names)
                if ((a.asname.empty() ? a.name : a.asname) == n) return true;
            return false;
        }
        case StmtKind::Global: {
            for (auto& g : static_cast<const Global&>(s).names)
                if (g == n) return true;
            return false;
        }
        case StmtKind::Nonlocal: {
            for (auto& g : static_cast<const Nonlocal&>(s).names)
                if (g == n) return true;
            return false;
        }
        case StmtKind::Match: {
            auto& m = static_cast<const Match&>(s);
            if (expr_binds(m.subject.get(), n)) return true;
            for (auto& c : m.cases) {
                // 模式里的捕获（case [a, *rest] / case _ as filter）也是当前作用域的绑定。
                if (pattern_binds(c.pattern.get(), n)) return true;
                if (expr_binds(c.guard.get(), n)) return true;
                if (body_binds(c.body, n)) return true;
            }
            return false;
        }
        case StmtKind::Delete: {
            // 函数里 del x 同样让 x 成为局部名。
            for (auto& t : static_cast<const Delete&>(s).targets)
                if (target_binds(t.get(), n)) return true;
            return false;
        }
        case StmtKind::Expr:
            return expr_binds(static_cast<const ExprStmt&>(s).value.get(), n);
        case StmtKind::Return: {
            auto* rv = static_cast<const Return&>(s).value.get();
            return rv ? expr_binds(rv, n) : false;
        }
        default: return false;
    }
}

// 生成器表达式的最外层 iterable 是 range(...) 时的边界解析。
//
// range 本身是惰性对象（pa_range_new 只存边界），但生成器表达式仍把边界
// 值传进合成函数、用计数循环逐个产出，省掉每次 next() 对范围对象的下标
// 调用。步长的方向决定比较用 < 还是 >：字面量步长在编译期定下来（快路径），
// 动态步长走运行期分支。
bool parse_range(Expr* e, ExprPtr& start, ExprPtr& stop, int64_t& step_val,
                 ExprPtr& step_expr) {
    if (!e || e->kind != ExprKind::Call) return false;
    auto& c = static_cast<Call&>(*e);
    if (!c.keywords.empty()) return false;
    auto* fn = dynamic_cast<Name*>(c.func.get());
    if (!fn || fn->id != "range") return false;
    if (c.args.empty() || c.args.size() > 3) return false;
    // 解包实参的个数静态不可知，合成函数的形参对不上，别改写。
    for (auto& a : c.args)
        if (a && a->kind == ExprKind::Starred) return false;

    step_val = 1;
    step_expr = nullptr;
    if (c.args.size() == 3) {
        auto* cv = dynamic_cast<Constant*>(c.args[2].get());
        if (cv && cv->ck == Constant::CK::Int) {
            if (cv->i == 0) return false;  // 步长 0 是 ValueError，保持原样
            step_val = cv->i;
        } else if (cv && cv->ck == Constant::CK::Bool) {
            if (!cv->b) return false;
            step_val = 1;
        } else {
            step_val = 0;
            step_expr = std::move(c.args[2]);
        }
    }
    if (c.args.size() == 1) {
        start = Constant::integer(0);
        stop = std::move(c.args[0]);
    } else {
        start = std::move(c.args[0]);
        stop = std::move(c.args[1]);
    }
    start->line = e->line;
    start->col = e->col;
    stop->line = e->line;
    stop->col = e->col;
    if (step_expr) {
        step_expr->line = e->line;
        step_expr->col = e->col;
    }
    return true;
}
}  // namespace pyadamas
