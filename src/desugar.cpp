// desugar.cpp —— 把 lambda 和生成器表达式展开成合成的嵌套函数
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
// 每次执行都创建新的闭包。类体里的 lambda / 生成器表达式暂不展开
// （类体只有字段声明，降级层会拒绝 lambda，生成器表达式按 list 构造）。
#include "desugar.h"

#include <format>
#include <utility>
#include <vector>

namespace pyadamas {
namespace {

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
// range 作为值使用会被实体化成整数列表（runtime/rt_slice.c 的
// pa_range_new）：range(10**9) 就是当场造 10 亿个元素。源是 range 时
// 改成把边界值传进合成函数，用计数循环惰性迭代。
//
// 步长的方向决定比较用 < 还是 >：字面量步长在编译期定下来（快路径），
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

class Desugarer {
public:
    void run(Module& m) {
        allow_map_ = !body_binds(m.body, "map");
        allow_filter_ = !body_binds(m.body, "filter");
        allow_range_ = !body_binds(m.body, "range");
        allow_slice_ = !body_binds(m.body, "slice");
        desugar_body(m.body, false);
    }

private:
    int lambda_id_ = 0;
    int gen_id_ = 0;
    int map_arg_id_ = 0;
    bool allow_map_ = true;
    bool allow_filter_ = true;
    bool allow_slice_ = true;
    // range 被用户绑定过就不做惰性改写（宁可少改写也不误改）。
    bool allow_range_ = true;

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
        fn->name = std::format("__pyadamas$lambda_{}", ++lambda_id_);
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

    // 生成器表达式 → 合成的生成器函数 + 一次调用：
    //
    //     def __pyadamas$gen_N(__pyadamas$it_N):
    //         for x in __pyadamas$it_N:
    //             if <ifs>: yield <elt>
    //     __pyadamas$gen_N(<最外层 iterable>)
    //
    // 最外层 iterable 在创建时求值（Python 语义），其余部分在 next() 时
    // 才执行，所以 sum(x*x for x in xs) 不再先建整个列表。合成函数体里的
    // 嵌套 lambda / 生成器表达式在合成函数作用域内展开，闭包捕获照旧。
    void desugar_genexpr(GeneratorExp& g, ExprPtr& slot, std::vector<StmtPtr>& pre) {
        if (g.generators.empty()) return;
        const int line = g.line;
        const int col = g.col;
        const int id = ++gen_id_;
        const std::string itname = std::format("__pyadamas$it_{}", id);
        const std::string sname = std::format("__pyadamas$rs_{}", id);
        const std::string ename = std::format("__pyadamas$re_{}", id);
        const std::string tname = std::format("__pyadamas$rt_{}", id);

        auto named = [&](const std::string& s) {
            auto n = std::make_unique<Name>(s);
            n->line = line;
            n->col = col;
            return n;
        };

        // 最外层 iterable 属于外层作用域：它内部的 lambda 提到 pre 里，
        // 之后整个表达式作为实参传进来。
        auto& first = static_cast<Comprehension&>(*g.generators[0]);

        // 源是 range(...) 时改成传边界值 + 计数循环：range 作为值使用会
        // 被实体化成整数列表（runtime/rt_slice.c 的 pa_range_new），
        // 那就没有惰性可言了。边界值在创建时求值（Python 语义），
        // 元素在 next() 时才产出。
        ExprPtr rstart, rstop, rstep_expr;
        int64_t rstep = 1;
        bool dyn_step = false;
        bool is_range = allow_range_ &&
                        parse_range(first.iter.get(), rstart, rstop, rstep, rstep_expr);
        dyn_step = static_cast<bool>(rstep_expr);
        if (is_range) {
            desugar_expr(rstart, pre);
            desugar_expr(rstop, pre);
            if (dyn_step) desugar_expr(rstep_expr, pre);
        } else {
            desugar_expr(first.iter, pre);
        }

        // 合成函数体里的 range(...)：边界值是形参，步长是字面量或形参。
        auto range_iter = [&]() {
            auto c = std::make_unique<Call>();
            c->func = named("range");
            c->args.push_back(named(sname));
            c->args.push_back(named(ename));
            if (dyn_step) {
                c->args.push_back(named(tname));
            } else {
                auto st = Constant::integer(rstep);
                st->line = line;
                st->col = col;
                c->args.push_back(std::move(st));
            }
            c->line = line;
            c->col = col;
            return c;
        };

        // 从最内层往外搭：yield elt，再逐层包 if / for。
        std::vector<StmtPtr> body;
        {
            auto y = std::make_unique<Yield>();
            y->value = std::move(g.elt);
            y->line = line;
            y->col = col;
            auto es = std::make_unique<ExprStmt>();
            es->value = std::move(y);
            es->line = line;
            es->col = col;
            body.push_back(std::move(es));
        }
        for (size_t i = g.generators.size(); i-- > 0;) {
            auto& c = static_cast<Comprehension&>(*g.generators[i]);
            for (size_t k = c.ifs.size(); k-- > 0;) {
                auto iff = std::make_unique<If>();
                If::Branch b;
                b.cond = std::move(c.ifs[k]);
                b.body = std::move(body);
                iff->branches.push_back(std::move(b));
                iff->line = line;
                iff->col = col;
                body.clear();
                body.push_back(std::move(iff));
            }
            auto fr = std::make_unique<For>();
            fr->target = std::move(c.target);
            if (i == 0 && is_range) fr->iter = range_iter();
            else fr->iter = i == 0 ? named(itname) : std::move(c.iter);
            fr->body = std::move(body);
            fr->line = line;
            fr->col = col;
            body.clear();
            body.push_back(std::move(fr));
        }

        auto fn = std::make_unique<FunctionDef>();
        fn->name = std::format("__pyadamas$gen_{}", id);
        if (is_range) {
            Arg s;
            s.name = sname;
            fn->args.args.push_back(std::move(s));
            Arg e;
            e.name = ename;
            fn->args.args.push_back(std::move(e));
            if (dyn_step) {
                Arg t;
                t.name = tname;
                fn->args.args.push_back(std::move(t));
            }
        } else {
            Arg a;
            a.name = itname;
            fn->args.args.push_back(std::move(a));
        }
        fn->body = std::move(body);
        fn->line = line;
        fn->col = col;

        // 体里的嵌套 lambda / 生成器表达式插在合成函数自己的作用域里。
        desugar_body(fn->body, false);

        auto call = std::make_unique<Call>();
        call->func = named(fn->name);
        if (is_range) {
            call->args.push_back(std::move(rstart));
            call->args.push_back(std::move(rstop));
            if (dyn_step) call->args.push_back(std::move(rstep_expr));
        } else {
            call->args.push_back(std::move(first.iter));
        }
        call->line = line;
        call->col = col;
        slot = std::move(call);
        pre.push_back(std::move(fn));
    }

    // 推导式里出现 lambda 时改成「生成器表达式 + 构造器」：
    // 列表/集合推导式目前是就地展开的，循环变量存在临时槽里，lambda
    // 脱糖后是个独立函数，看不到这些槽。生成器表达式本来就被脱糖成
    // 合成函数，循环变量是它的局部变量，闭包捕获按常规路径就能工作，
    // 语义（所有 lambda 共享同一个循环变量 cell）也和 CPython 一致。
    static bool has_lambda(const Expr* e) {
        if (!e) return false;
        if (e->kind == ExprKind::Lambda) return true;
        switch (e->kind) {
            case ExprKind::Constant:
            case ExprKind::Name:
                return false;
            case ExprKind::FormattedValue: {
                auto& f = static_cast<const FormattedValue&>(*e);
                return has_lambda(f.value.get()) || has_lambda(f.format_spec.get());
            }
            case ExprKind::JoinedStr: {
                for (auto& v : static_cast<const JoinedStr&>(*e).values)
                    if (has_lambda(v.get())) return true;
                return false;
            }
            case ExprKind::List:
                for (auto& v : static_cast<const ListExpr&>(*e).elts)
                    if (has_lambda(v.get())) return true;
                return false;
            case ExprKind::Tuple:
                for (auto& v : static_cast<const TupleExpr&>(*e).elts)
                    if (has_lambda(v.get())) return true;
                return false;
            case ExprKind::Set:
                for (auto& v : static_cast<const SetExpr&>(*e).elts)
                    if (has_lambda(v.get())) return true;
                return false;
            case ExprKind::Dict: {
                auto& d = static_cast<const DictExpr&>(*e);
                for (auto& v : d.keys)
                    if (has_lambda(v.get())) return true;
                for (auto& v : d.values)
                    if (has_lambda(v.get())) return true;
                return false;
            }
            case ExprKind::BinOp: {
                auto& b = static_cast<const BinOp&>(*e);
                return has_lambda(b.left.get()) || has_lambda(b.right.get());
            }
            case ExprKind::UnaryOp:
                return has_lambda(static_cast<const UnaryOp&>(*e).operand.get());
            case ExprKind::BoolOp: {
                for (auto& v : static_cast<const BoolOp&>(*e).values)
                    if (has_lambda(v.get())) return true;
                return false;
            }
            case ExprKind::Compare: {
                auto& c = static_cast<const Compare&>(*e);
                if (has_lambda(c.left.get())) return true;
                for (auto& v : c.comparators)
                    if (has_lambda(v.get())) return true;
                return false;
            }
            case ExprKind::Call: {
                auto& c = static_cast<const Call&>(*e);
                if (has_lambda(c.func.get())) return true;
                for (auto& v : c.args)
                    if (has_lambda(v.get())) return true;
                for (auto& k : c.keywords)
                    if (has_lambda(k.value.get())) return true;
                return false;
            }
            case ExprKind::Attribute:
                return has_lambda(static_cast<const Attribute&>(*e).value.get());
            case ExprKind::Subscript: {
                auto& sb = static_cast<const Subscript&>(*e);
                return has_lambda(sb.value.get()) || has_lambda(sb.slice.get());
            }
            case ExprKind::Slice: {
                auto& sl = static_cast<const SliceExpr&>(*e);
                return has_lambda(sl.lower.get()) || has_lambda(sl.upper.get()) ||
                       has_lambda(sl.step.get());
            }
            case ExprKind::IfExp: {
                auto& i = static_cast<const IfExp&>(*e);
                return has_lambda(i.test.get()) || has_lambda(i.body.get()) ||
                       has_lambda(i.orelse.get());
            }
            case ExprKind::Starred:
                return has_lambda(static_cast<const Starred&>(*e).value.get());
            case ExprKind::NamedExpr: {
                auto& n = static_cast<const NamedExpr&>(*e);
                return has_lambda(n.value.get());
            }
            case ExprKind::Await:
                return has_lambda(static_cast<const Await&>(*e).value.get());
            case ExprKind::Yield:
                return has_lambda(static_cast<const Yield&>(*e).value.get());
            case ExprKind::YieldFrom:
                return has_lambda(static_cast<const YieldFrom&>(*e).value.get());
            case ExprKind::GeneratorExp: {
                auto& g = static_cast<const GeneratorExp&>(*e);
                if (has_lambda(g.elt.get())) return true;
                for (auto& gen : g.generators) {
                    auto& c = static_cast<const Comprehension&>(*gen);
                    if (has_lambda(c.iter.get())) return true;
                    for (auto& cond : c.ifs)
                        if (has_lambda(cond.get())) return true;
                }
                return false;
            }
            case ExprKind::ListComp: {
                auto& g = static_cast<const ListComp&>(*e);
                if (has_lambda(g.elt.get())) return true;
                for (auto& gen : g.generators) {
                    auto& c = static_cast<const Comprehension&>(*gen);
                    if (has_lambda(c.iter.get())) return true;
                    for (auto& cond : c.ifs)
                        if (has_lambda(cond.get())) return true;
                }
                return false;
            }
            case ExprKind::SetComp: {
                auto& g = static_cast<const SetComp&>(*e);
                if (has_lambda(g.elt.get())) return true;
                for (auto& gen : g.generators) {
                    auto& c = static_cast<const Comprehension&>(*gen);
                    if (has_lambda(c.iter.get())) return true;
                    for (auto& cond : c.ifs)
                        if (has_lambda(cond.get())) return true;
                }
                return false;
            }
            case ExprKind::DictComp: {
                auto& g = static_cast<const DictComp&>(*e);
                if (has_lambda(g.key.get()) || has_lambda(g.value.get())) return true;
                for (auto& gen : g.generators) {
                    auto& c = static_cast<const Comprehension&>(*gen);
                    if (has_lambda(c.iter.get())) return true;
                    for (auto& cond : c.ifs)
                        if (has_lambda(cond.get())) return true;
                }
                return false;
            }
            case ExprKind::Comprehension:
                return false;
        }
        return false;
    }

    // 把含 lambda 的 list/set 推导式改写成 list()/set() + 生成器表达式。
    static ExprPtr as_ctor_genexp(ExprPtr& src, const char* ctor_name,
                                  ExprPtr elt, std::vector<ExprPtr> gens) {
        auto call = std::make_unique<Call>();
        call->line = src->line;
        call->col = src->col;
        auto fn = std::make_unique<Name>(ctor_name);
        fn->line = src->line;
        fn->col = src->col;
        call->func = std::move(fn);
        auto gen = std::make_unique<GeneratorExp>();
        gen->line = src->line;
        gen->col = src->col;
        gen->elt = std::move(elt);
        gen->generators = std::move(gens);
        call->args.push_back(std::move(gen));
        return call;
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
                // map(f, xs) → [f(x) for x in xs]
                // filter(f, xs) → [x for x in xs if f(x)]
                // filter(None, xs) → [x for x in xs if x]
                // f 只接受名字（含脱糖后的 lambda），否则逐个元素求值会改变
                // 求值次数；这类写法请直接写推导式。
                if (!c.keywords.empty() || c.args.size() != 2) break;
                bool is_map = false;
                bool is_filter = false;
                if (auto* fn = dynamic_cast<Name*>(c.func.get())) {
                    is_map = allow_map_ && fn->id == "map";
                    is_filter = allow_filter_ && fn->id == "filter";
                }
                if (!is_map && !is_filter) break;
                if (c.args[0]->kind == ExprKind::Starred ||
                    c.args[1]->kind == ExprKind::Starred)
                    break;
                bool func_none =
                    is_filter && c.args[0]->kind == ExprKind::Constant &&
                    static_cast<Constant*>(c.args[0].get())->ck == Constant::CK::None;
                if (!func_none && c.args[0]->kind != ExprKind::Name) break;

                const int line = e->line;
                const int col = e->col;
                const std::string tmp = std::format("__pyadamas$map_arg_{}", ++map_arg_id_);
                auto named = [&](const std::string& id) {
                    auto n = std::make_unique<Name>(id);
                    n->line = line;
                    n->col = col;
                    return n;
                };
                auto guarded_call = [&](ExprPtr fn) {
                    auto call = std::make_unique<Call>();
                    call->line = line;
                    call->col = col;
                    call->func = std::move(fn);
                    call->args.push_back(named(tmp));
                    return call;
                };

                auto gen = std::make_unique<Comprehension>();
                gen->line = line;
                gen->col = col;
                gen->target = named(tmp);
                gen->iter = std::move(c.args[1]);
                auto lc = std::make_unique<ListComp>();
                lc->line = line;
                lc->col = col;
                if (is_map) {
                    lc->elt = guarded_call(std::move(c.args[0]));
                } else {
                    lc->elt = named(tmp);
                    if (func_none)
                        gen->ifs.push_back(named(tmp));
                    else
                        gen->ifs.push_back(guarded_call(std::move(c.args[0])));
                }
                lc->generators.push_back(std::move(gen));
                e = std::move(lc);
                break;
            }
            case ExprKind::Slice: {
                auto& s = static_cast<SliceExpr&>(*e);
                if (s.lower) desugar_expr(s.lower, pre);
                if (s.upper) desugar_expr(s.upper, pre);
                if (s.step) desugar_expr(s.step, pre);
                // 切片里的 None 表示「这一端不设限」：xs[None:3] / xs[-2:None]。
                // 留成常量会被降级层当成 0。
                auto drop_none = [](ExprPtr& x) {
                    if (x && x->kind == ExprKind::Constant &&
                        static_cast<Constant*>(x.get())->ck == Constant::CK::None)
                        x.reset();
                };
                drop_none(s.lower);
                drop_none(s.upper);
                drop_none(s.step);
                break;
            }
            case ExprKind::Subscript: {
                auto& s = static_cast<Subscript&>(*e);
                desugar_expr(s.value, pre);
                desugar_expr(s.slice, pre);
                // xs[slice(a, b, c)] → xs[a:b:c]：切片本身在 pyadamas 里是
                // 语法层的，没有运行期 slice 对象；这里把直接调用的写法
                // 折成同样的形状，typecheck/lower 完全不用改。
                // slice() 当独立值用仍然不支持（降级层会明确报错）。
                if (allow_slice_ && s.slice && s.slice->kind == ExprKind::Call) {
                    auto& call = static_cast<Call&>(*s.slice);
                    auto* fn = dynamic_cast<Name*>(call.func.get());
                    bool plain = fn && fn->id == "slice" && call.keywords.empty() &&
                                 call.args.size() >= 1 && call.args.size() <= 3;
                    for (auto& a : call.args)
                        if (a->kind == ExprKind::Starred) plain = false;
                    if (plain) {
                        auto sl = std::make_unique<SliceExpr>();
                        sl->line = call.line;
                        sl->col = call.col;
                        auto take = [](ExprPtr& x) -> ExprPtr {
                            if (!x) return nullptr;
                            if (x->kind == ExprKind::Constant &&
                                static_cast<Constant*>(x.get())->ck == Constant::CK::None)
                                return nullptr;
                            return std::move(x);
                        };
                        if (call.args.size() == 1) {
                            sl->upper = take(call.args[0]);
                        } else {
                            sl->lower = take(call.args[0]);
                            sl->upper = take(call.args[1]);
                            if (call.args.size() == 3) sl->step = take(call.args[2]);
                        }
                        s.slice = std::move(sl);
                    }
                }
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
                if (has_lambda(e.get())) {
                    e = as_ctor_genexp(e, "list", std::move(c.elt),
                                       std::move(c.generators));
                    desugar_expr(e, pre);
                    break;
                }
                desugar_expr(c.elt, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::SetComp: {
                auto& c = static_cast<SetComp&>(*e);
                if (has_lambda(e.get())) {
                    e = as_ctor_genexp(e, "set", std::move(c.elt),
                                       std::move(c.generators));
                    desugar_expr(e, pre);
                    break;
                }
                desugar_expr(c.elt, pre);
                desugar_exprs(c.generators, pre);
                break;
            }
            case ExprKind::GeneratorExp:
                desugar_genexpr(static_cast<GeneratorExp&>(*e), e, pre);
                break;
            case ExprKind::DictComp: {
                auto& c = static_cast<DictComp&>(*e);
                if (has_lambda(e.get())) {
                    auto tup = std::make_unique<TupleExpr>();
                    tup->line = c.key->line;
                    tup->col = c.key->col;
                    tup->elts.push_back(std::move(c.key));
                    tup->elts.push_back(std::move(c.value));
                    e = as_ctor_genexp(e, "dict", std::move(tup),
                                       std::move(c.generators));
                    desugar_expr(e, pre);
                    break;
                }
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
            case StmtKind::CImport:
                break;
            case StmtKind::StructDef:
                break;  // 值类型声明，无需脱糖
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
                // 函数体里绑定了 range 的话，体里的生成器表达式看到的可能
                // 不是内建 range：这一层先记下来，宁可少改写也不误改。
                bool saved_range = allow_range_;
                allow_range_ = allow_range_ && !body_binds(f.body, "range");
                desugar_body(f.body, false);
                allow_range_ = saved_range;
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

}  // namespace pyadamas
