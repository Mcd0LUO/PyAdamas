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

bool stmt_binds(const Stmt& s, const std::string& n) {
    switch (s.kind) {
        case StmtKind::Assign: {
            auto& a = static_cast<const Assign&>(s);
            for (auto& t : a.targets)
                if (target_binds(t.get(), n)) return true;
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
            for (auto& c : static_cast<const Match&>(s).cases)
                if (body_binds(c.body, n)) return true;
            return false;
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
