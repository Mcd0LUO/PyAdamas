// desugar.cpp —— 把 lambda 展开成合成的嵌套函数
//
// lambda 是表达式，但闭包/捕获/引用计数都建立在 FunctionDef 上。这里在
// 类型检查之前把它改写成：
//
//     __py23$lambda_N = def (...) -> ...: return <body>
//     ... __py23$lambda_N ...
//
// 合成函数插在包含该表达式的那条语句之前，所以循环体里的 lambda 仍然
// 每次执行都创建新的闭包。类体里的 lambda 暂不展开（类体只有字段声明，
// 降级层会拒绝）。
#include "desugar.h"

#include <format>
#include <utility>
#include <vector>

namespace py23 {
namespace {

class Desugarer {
public:
    void run(Module& m) { desugar_body(m.body, false); }

private:
    int lambda_id_ = 0;

    void desugar_body(std::vector<StmtPtr>& body, bool class_body) {
        std::vector<StmtPtr> out;
        out.reserve(body.size());
        for (auto& s : body) {
            std::vector<StmtPtr> pre;
            desugar_stmt(*s, pre, class_body);
            for (auto& d : pre) out.push_back(std::move(d));
            out.push_back(std::move(s));
        }
        body = std::move(out);
    }

    void desugar_arguments(Arguments& a, std::vector<StmtPtr>& pre) {
        for (auto& d : a.defaults) desugar_expr(d, pre);
        for (auto& d : a.kw_defaults)
            if (d) desugar_expr(d, pre);
    }

    void desugar_exprs(std::vector<ExprPtr>& es, std::vector<StmtPtr>& pre) {
        for (auto& e : es)
            if (e) desugar_expr(e, pre);
    }

    void desugar_lambda(Lambda& l, ExprPtr& slot, std::vector<StmtPtr>& pre) {
        const int line = l.line;
        const int col = l.col;
        Arguments args = std::move(l.args);
        ExprPtr body = std::move(l.body);

        // 默认值在 lambda 创建时求值，属于外层作用域；如果默认值里还有
        // lambda，它们要插在合成 def 之前。
        desugar_arguments(args, pre);

        auto ret = std::make_unique<Return>();
        ret->value = std::move(body);
        ret->line = line;
        ret->col = col;

        auto fn = std::make_unique<FunctionDef>();
        fn->name = std::format("__py23$lambda_{}", ++lambda_id_);
        fn->args = std::move(args);
        fn->line = line;
        fn->col = col;
        fn->body.push_back(std::move(ret));

        // lambda 体里的嵌套 lambda 也要展开，插在合成函数的 return 之前。
        desugar_body(fn->body, false);

        auto name = fn->name;
        pre.push_back(std::move(fn));

        auto n = std::make_unique<Name>(std::move(name));
        n->line = line;
        n->col = col;
        slot = std::move(n);
    }

    void desugar_expr(ExprPtr& e, std::vector<StmtPtr>& pre) {
        if (!e) return;
        if (e->kind == ExprKind::Lambda) {
            desugar_lambda(static_cast<Lambda&>(*e), e, pre);
            return;
        }
        switch (e->kind) {
            case ExprKind::Constant:
            case ExprKind::Name:
                break;
            case ExprKind::FormattedValue: {
                auto& f = static_cast<FormattedValue&>(*e);
                desugar_expr(f.value, pre);
                if (f.format_spec) desugar_expr(f.format_spec, pre);
                break;
            }
            case ExprKind::JoinedStr:
                desugar_exprs(static_cast<JoinedStr&>(*e).values, pre);
                break;
            case ExprKind::List:
                desugar_exprs(static_cast<ListExpr&>(*e).elts, pre);
                break;
            case ExprKind::Tuple:
                desugar_exprs(static_cast<TupleExpr&>(*e).elts, pre);
                break;
            case ExprKind::Set:
                desugar_exprs(static_cast<SetExpr&>(*e).elts, pre);
                break;
            case ExprKind::Dict: {
                auto& d = static_cast<DictExpr&>(*e);
                desugar_exprs(d.keys, pre);
                desugar_exprs(d.values, pre);
                break;
            }
            case ExprKind::BinOp: {
                auto& b = static_cast<BinOp&>(*e);
                desugar_expr(b.left, pre);
                desugar_expr(b.right, pre);
                break;
            }
            case ExprKind::UnaryOp:
                desugar_expr(static_cast<UnaryOp&>(*e).operand, pre);
                break;
            case ExprKind::BoolOp:
                desugar_exprs(static_cast<BoolOp&>(*e).values, pre);
                break;
            case ExprKind::Compare: {
                auto& c = static_cast<Compare&>(*e);
                desugar_expr(c.left, pre);
                desugar_exprs(c.comparators, pre);
                break;
            }
            case ExprKind::Call: {
                auto& c = static_cast<Call&>(*e);
                desugar_expr(c.func, pre);
                desugar_exprs(c.args, pre);
                for (auto& k : c.keywords) desugar_expr(k.value, pre);
                break;
            }
            case ExprKind::Slice: {
                auto& s = static_cast<SliceExpr&>(*e);
                if (s.lower) desugar_expr(s.lower, pre);
                if (s.upper) desugar_expr(s.upper, pre);
                if (s.step) desugar_expr(s.step, pre);
                break;
            }
            case ExprKind::Subscript: {
                auto& s = static_cast<Subscript&>(*e);
                desugar_expr(s.value, pre);
                desugar_expr(s.slice, pre);
                break;
            }
            case ExprKind::Attribute:
                desugar_expr(static_cast<Attribute&>(*e).value, pre);
                break;
            case ExprKind::Comprehension: {
                auto& c = static_cast<Comprehension&>(*e);
                desugar_expr(c.target, pre);
                desugar_expr(c.iter, pre);
                desugar_exprs(c.ifs, pre);
                break;
            }
            case ExprKind::ListComp: {
                auto& c = static_cast<ListComp&>(*e);
                desugar_expr(c.elt, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::SetComp: {
                auto& c = static_cast<SetComp&>(*e);
                desugar_expr(c.elt, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::GeneratorExp: {
                auto& c = static_cast<GeneratorExp&>(*e);
                desugar_expr(c.elt, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::DictComp: {
                auto& c = static_cast<DictComp&>(*e);
                desugar_expr(c.key, pre);
                desugar_expr(c.value, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::IfExp: {
                auto& i = static_cast<IfExp&>(*e);
                desugar_expr(i.test, pre);
                desugar_expr(i.body, pre);
                desugar_expr(i.orelse, pre);
                break;
            }
            case ExprKind::Starred:
                desugar_expr(static_cast<Starred&>(*e).value, pre);
                break;
            case ExprKind::NamedExpr: {
                auto& n = static_cast<NamedExpr&>(*e);
                desugar_expr(n.target, pre);
                desugar_expr(n.value, pre);
                break;
            }
            case ExprKind::Await:
                desugar_expr(static_cast<Await&>(*e).value, pre);
                break;
            case ExprKind::Yield: {
                auto& y = static_cast<Yield&>(*e);
                if (y.value) desugar_expr(y.value, pre);
                break;
            }
            case ExprKind::YieldFrom:
                desugar_expr(static_cast<YieldFrom&>(*e).value, pre);
                break;
            case ExprKind::Lambda:
                break;  // 上面已经处理
        }
    }

    void desugar_stmt(Stmt& s, std::vector<StmtPtr>& pre, bool class_body) {
        switch (s.kind) {
            case StmtKind::Expr:
                if (!class_body) desugar_expr(static_cast<ExprStmt&>(s).value, pre);
                break;
            case StmtKind::Assign: {
                auto& a = static_cast<Assign&>(s);
                if (!class_body) {
                    desugar_exprs(a.targets, pre);
                    desugar_expr(a.value, pre);
                }
                break;
            }
            case StmtKind::AugAssign: {
                auto& a = static_cast<AugAssign&>(s);
                if (!class_body) {
                    desugar_expr(a.target, pre);
                    desugar_expr(a.value, pre);
                }
                break;
            }
            case StmtKind::AnnAssign: {
                auto& a = static_cast<AnnAssign&>(s);
                if (!class_body && a.value) desugar_expr(a.value, pre);
                break;
            }
            case StmtKind::Return: {
                auto& r = static_cast<Return&>(s);
                if (r.value) desugar_expr(r.value, pre);
                break;
            }
            case StmtKind::If: {
                auto& f = static_cast<If&>(s);
                for (auto& b : f.branches) {
                    if (!class_body) desugar_expr(b.cond, pre);
                    desugar_body(b.body, class_body);
                }
                desugar_body(f.orelse, class_body);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(s);
                if (!class_body) desugar_expr(w.cond, pre);
                desugar_body(w.body, class_body);
                desugar_body(w.orelse, class_body);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(s);
                if (!class_body) {
                    desugar_expr(f.target, pre);
                    desugar_expr(f.iter, pre);
                }
                desugar_body(f.body, class_body);
                desugar_body(f.orelse, class_body);
                break;
            }
            case StmtKind::FunctionDef: {
                auto& f = static_cast<FunctionDef&>(s);
                if (!class_body) {
                    desugar_exprs(f.decorators, pre);
                    desugar_arguments(f.args, pre);
                    if (f.returns) desugar_expr(f.returns, pre);
                }
                desugar_body(f.body, false);
                break;
            }
            case StmtKind::ClassDef: {
                auto& c = static_cast<ClassDef&>(s);
                desugar_exprs(c.bases, pre);
                for (auto& k : c.keywords) desugar_expr(k.value, pre);
                desugar_exprs(c.decorators, pre);
                desugar_body(c.body, true);
                break;
            }
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(s);
                desugar_body(t.body, class_body);
                for (auto& h : t.handlers) {
                    if (h.type) desugar_expr(h.type, pre);
                    desugar_body(h.body, class_body);
                }
                desugar_body(t.orelse, class_body);
                desugar_body(t.finalbody, class_body);
                break;
            }
            case StmtKind::With: {
                auto& w = static_cast<With&>(s);
                for (auto& it : w.items) {
                    desugar_expr(it.context, pre);
                    if (it.optional_vars) desugar_expr(it.optional_vars, pre);
                }
                desugar_body(w.body, class_body);
                break;
            }
            case StmtKind::Raise: {
                auto& r = static_cast<Raise&>(s);
                if (r.exc) desugar_expr(r.exc, pre);
                if (r.cause) desugar_expr(r.cause, pre);
                break;
            }
            case StmtKind::Assert: {
                auto& a = static_cast<Assert&>(s);
                desugar_expr(a.test, pre);
                if (a.msg) desugar_expr(a.msg, pre);
                break;
            }
            case StmtKind::Delete:
                desugar_exprs(static_cast<Delete&>(s).targets, pre);
                break;
            case StmtKind::Match: {
                auto& m = static_cast<Match&>(s);
                desugar_expr(m.subject, pre);
                for (auto& c : m.cases) {
                    if (c.guard) desugar_expr(c.guard, pre);
                    desugar_body(c.body, class_body);
                }
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Pass:
            case StmtKind::Import:
            case StmtKind::ImportFrom:
            case StmtKind::Global:
            case StmtKind::Nonlocal:
                break;
        }
    }
};

}  // namespace

void desugar_lambdas(Module& m) {
    Desugarer d;
    d.run(m);
}

}  // namespace py23
