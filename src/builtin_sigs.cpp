// builtin_sigs.cpp —— 内建模块的函数签名表
//
// 只放数据：模块名 → 全局名 → 类型。多模块构建时由 program_module
// 调用，类型检查直接复用同一份签名。
#include "driver_build.h"

#include <format>

namespace pyadamas {

namespace {

pyadamas::FuncInfoPtr builtin_fn(const std::string& name, const std::vector<pyadamas::TypePtr>& args,
                             pyadamas::TypePtr ret) {
    auto f = std::make_shared<pyadamas::FuncInfo>();
    f->name = name;
    for (size_t i = 0; i < args.size(); ++i) {
        pyadamas::Param p;
        p.name = (name == "fsum" && i == 0) ? "xs" : std::format("x{}", i);
        p.ty = args[i];
        f->pos.push_back(std::move(p));
    }
    f->ret = ret;
    f->ret_known = true;
    return f;
}

}  // namespace

pyadamas::ModuleInfoPtr builtin_module(const std::string& name) {
    static std::map<std::string, pyadamas::ModuleInfoPtr> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto info = std::make_shared<pyadamas::ModuleInfo>();
    info->name = name;
    info->builtin = true;
    // CPython 桥代理：根 "python" 与任意 "python.<module>" 子代理都没有
    // 签名表——属性/调用在 typecheck 里按 cpy_proxy 放行成 unknown。
    // 子代理按需生成（模块名是开集：stdlib / 第三方包都可能）。
    if (name == "python" || name.starts_with("python.")) {
        info->cpy_proxy = true;
        cache[name] = info;
        return info;
    }
    auto func = [&](const std::string& n, std::vector<pyadamas::TypePtr> args, pyadamas::TypePtr ret) {
        auto t = pyadamas::new_ty(pyadamas::TyKind::Func);
        t->fn = builtin_fn(n, args, ret);
        info->globals[n] = t;
    };
    auto lv_int = [](int64_t v) {
        pyadamas::LiteralValue lv;
        lv.kind = pyadamas::LiteralValue::Kind::Int;
        lv.i = v;
        return lv;
    };
    auto lv_bool = [](bool v) {
        pyadamas::LiteralValue lv;
        lv.kind = pyadamas::LiteralValue::Kind::Bool;
        lv.b = v;
        return lv;
    };
    auto lv_float = [](double v) {
        pyadamas::LiteralValue lv;
        lv.kind = pyadamas::LiteralValue::Kind::Float;
        lv.f = v;
        return lv;
    };
    auto lv_str = [](const std::string& v) {
        pyadamas::LiteralValue lv;
        lv.kind = pyadamas::LiteralValue::Kind::Str;
        lv.s = v;
        return lv;
    };
    auto func_with = [&](const std::string& n, const pyadamas::FuncInfoPtr& f) {
        auto t = pyadamas::new_ty(pyadamas::TyKind::Func);
        t->fn = f;
        info->globals[n] = t;
    };
    if (name == "math") {
        info->globals["pi"] = pyadamas::t_float();
        info->globals["e"] = pyadamas::t_float();
        info->globals["tau"] = pyadamas::t_float();
        info->globals["inf"] = pyadamas::t_float();
        info->globals["nan"] = pyadamas::t_float();
        for (auto& n : {"sqrt", "fabs", "sin", "cos", "tan", "log", "log2", "log10", "exp"})
            func(n, {pyadamas::t_float()}, pyadamas::t_float());
        func("floor", {pyadamas::t_float()}, pyadamas::t_int());
        func("ceil", {pyadamas::t_float()}, pyadamas::t_int());
        func("pow", {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        func("fmod", {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        for (auto& n : {"atan2", "hypot", "copysign"})
            func(n, {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        for (auto& n : {"degrees", "radians"})
            func(n, {pyadamas::t_float()}, pyadamas::t_float());
        func("trunc", {pyadamas::t_float()}, pyadamas::t_int());
        for (auto& n : {"isnan", "isinf", "isfinite"})
            func(n, {pyadamas::t_float()}, pyadamas::t_bool());
        func("factorial", {pyadamas::t_int()}, pyadamas::t_int());
        func("gcd", {pyadamas::t_int(), pyadamas::t_int()}, pyadamas::t_int());
        func("lcm", {pyadamas::t_int(), pyadamas::t_int()}, pyadamas::t_int());
        func("fsum", {pyadamas::mk_list(pyadamas::t_float())}, pyadamas::t_float());
        for (auto& n : {"acos", "asin", "atan", "acosh", "asinh", "atanh", "cbrt",
                        "cosh", "sinh", "tanh", "exp2", "expm1", "log1p", "erf",
                        "erfc", "gamma", "lgamma", "ulp"})
            func(n, {pyadamas::t_float()}, pyadamas::t_float());
        for (auto& n : {"nextafter", "remainder"})
            func(n, {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        func("ldexp", {pyadamas::t_float(), pyadamas::t_int()}, pyadamas::t_float());
        func("fma", {pyadamas::t_float(), pyadamas::t_float(), pyadamas::t_float()},
             pyadamas::t_float());
        func("isclose", {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_bool());
        func("frexp", {pyadamas::t_float()}, pyadamas::mk_tuple({pyadamas::t_float(), pyadamas::t_int()}));
        func("modf", {pyadamas::t_float()}, pyadamas::mk_tuple({pyadamas::t_float(), pyadamas::t_float()}));
        func("isqrt", {pyadamas::t_int()}, pyadamas::t_int());
        func("comb", {pyadamas::t_int(), pyadamas::t_int()}, pyadamas::t_int());
        func("perm", {pyadamas::t_int(), pyadamas::t_int()}, pyadamas::t_int());
        func("prod", {pyadamas::mk_list(pyadamas::t_int())}, pyadamas::t_int());
        func("sumprod", {pyadamas::mk_list(pyadamas::t_int()), pyadamas::mk_list(pyadamas::t_int())},
             pyadamas::t_int());
        func("dist", {pyadamas::mk_list(pyadamas::t_float()), pyadamas::mk_list(pyadamas::t_float())},
             pyadamas::t_float());
    } else if (name == "sys") {
        info->globals["argv"] = pyadamas::mk_list(pyadamas::t_str());
        info->globals["maxsize"] = pyadamas::t_int();
        info->globals["stdin"] = pyadamas::t_file();
        info->globals["stdout"] = pyadamas::t_file();
        info->globals["stderr"] = pyadamas::t_file();
        func("exit", {pyadamas::t_int()}, pyadamas::t_none());
    } else if (name == "asyncio") {
        // run/create_task 的结果类型按实参在 typecheck 里特判；这里的签名
        // 只用于 from-import 的缺省检查和错误提示。
        func("run", {pyadamas::mk_coroutine(pyadamas::t_unknown())}, pyadamas::t_unknown());
        func("sleep", {pyadamas::t_float()}, pyadamas::mk_coroutine(pyadamas::t_none()));
        func("create_task", {pyadamas::mk_coroutine(pyadamas::t_unknown())},
             pyadamas::mk_task(pyadamas::t_unknown()));
        func("Future", {}, pyadamas::mk_future(pyadamas::t_unknown()));
        func("Event", {}, pyadamas::mk_async_event());
        func("Lock", {}, pyadamas::mk_async_lock());
        {
            auto qf = std::make_shared<pyadamas::FuncInfo>();
            qf->name = "Queue";
            pyadamas::Param p;
            p.name = "maxsize";
            p.ty = pyadamas::t_int();
            p.has_default = true;
            pyadamas::LiteralValue lv;
            lv.kind = pyadamas::LiteralValue::Kind::Int;
            lv.i = 0;
            p.default_lit = lv;
            qf->pos.push_back(std::move(p));
            qf->ret = pyadamas::mk_async_queue(pyadamas::t_unknown());
            qf->ret_known = true;
            auto qt = pyadamas::new_ty(pyadamas::TyKind::Func);
            qt->fn = qf;
            info->globals["Queue"] = qt;
        }
        func("wait_for", {pyadamas::t_unknown(), pyadamas::t_float()},
             pyadamas::mk_coroutine(pyadamas::t_unknown()));
        func("timeout", {pyadamas::t_float()}, pyadamas::mk_async_timeout());
        func("TaskGroup", {}, pyadamas::mk_async_taskgroup());
        {
            auto sf = std::make_shared<pyadamas::FuncInfo>();
            sf->name = "Semaphore";
            pyadamas::Param p;
            p.name = "value";
            p.ty = pyadamas::t_int();
            p.has_default = true;
            pyadamas::LiteralValue lv;
            lv.kind = pyadamas::LiteralValue::Kind::Int;
            lv.i = 1;
            p.default_lit = lv;
            sf->pos.push_back(std::move(p));
            sf->ret = pyadamas::mk_async_semaphore();
            sf->ret_known = true;
            auto st = pyadamas::new_ty(pyadamas::TyKind::Func);
            st->fn = sf;
            info->globals["Semaphore"] = st;
        }
        func("wait_readable", {pyadamas::t_int()}, pyadamas::mk_coroutine(pyadamas::t_none()));
        func("wait_writable", {pyadamas::t_int()}, pyadamas::mk_coroutine(pyadamas::t_none()));
        auto gather = std::make_shared<pyadamas::FuncInfo>();
        gather->name = "gather";
        gather->varargs = true;
        gather->vararg_name = "aws";
        gather->vararg_ty = pyadamas::t_unknown();
        gather->ret = pyadamas::mk_coroutine(pyadamas::mk_list(pyadamas::t_unknown()));
        gather->ret_known = true;
        auto gt = pyadamas::new_ty(pyadamas::TyKind::Func);
        gt->fn = gather;
        info->globals["gather"] = gt;
    } else if (name == "time") {
        func("time", {}, pyadamas::t_float());
        func("monotonic", {}, pyadamas::t_float());
        func("perf_counter", {}, pyadamas::t_float());
        func("sleep", {pyadamas::t_float()}, pyadamas::t_none());
    } else if (name == "itertools") {
        // 没有泛型：签名只用于 from-import 的可调用标记，返回类型在
        // 调用点由 itertools_call 按实参特化。
        auto two_lists = [&](const std::string& n, bool second_int, pyadamas::TypePtr ret) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "itertools." + n;
            pyadamas::Param p;
            p.name = "a";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = second_int ? "b" : "b";
            q.ty = second_int ? pyadamas::t_int() : pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(q));
            f->ret = std::move(ret);
            f->ret_known = true;
            func_with(n, f);
        };
        two_lists("chain", false, pyadamas::mk_list(pyadamas::t_unknown()));
        two_lists("repeat", true, pyadamas::mk_list(pyadamas::t_unknown()));
        // product / zip_longest：可变个数的列表实参，类型在调用点特化。
        for (const char* n : {"product", "zip_longest"}) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = std::string("itertools.") + n;
            f->varargs = true;
            f->vararg_name = "iterables";
            f->vararg_ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->ret = pyadamas::mk_list(pyadamas::t_unknown());
            f->ret_known = true;
            func_with(n, f);
        }
        // permutations(p[, r]) / combinations(p, r) /
        // combinations_with_replacement(p, r)：签名只做可调用标记。
        for (const char* n : {"permutations", "combinations", "combinations_with_replacement"}) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = std::string("itertools.") + n;
            pyadamas::Param p;
            p.name = "iterable";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            pyadamas::Param r;
            r.name = "r";
            r.ty = pyadamas::t_int();
            // permutations 的 r 可以省略（默认取序列长度）。
            r.has_default = std::string(n) == "permutations";
            f->pos.push_back(std::move(r));
            f->ret = pyadamas::mk_list(pyadamas::t_unknown());
            f->ret_known = true;
            func_with(n, f);
        }
        // accumulate(xs)
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "itertools.accumulate";
            pyadamas::Param p;
            p.name = "xs";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::mk_list(pyadamas::t_unknown());
            f->ret_known = true;
            func_with("accumulate", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "itertools.islice";
            pyadamas::Param p;
            p.name = "x";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "stop";
            q.ty = pyadamas::t_int();
            f->pos.push_back(std::move(q));
            f->ret = pyadamas::mk_list(pyadamas::t_unknown());
            f->ret_known = true;
            func_with("islice", f);
        }
    } else if (name == "collections") {
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "collections.Counter";
            pyadamas::Param p;
            p.name = "items";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::mk_dict(pyadamas::t_unknown(), pyadamas::t_int());
            f->ret_known = true;
            func_with("Counter", f);
        }
        // OrderedDict：PyAdamas 的 dict 保序，直接当空 dict 用。
        func("OrderedDict", {}, pyadamas::mk_dict(pyadamas::t_str(), pyadamas::t_unknown()));
        // defaultdict：签名只用于 from-import；K/V 在 collections_call 里按工厂推。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "collections.defaultdict";
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = pyadamas::t_unknown();
            f->ret = pyadamas::mk_dict(pyadamas::t_unknown(), pyadamas::t_unknown());
            f->ret_known = true;
            func_with("defaultdict", f);
        }
        // deque(iterable=None, maxlen=None)：两个参数都可选。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "collections.deque";
            pyadamas::Param p;
            p.name = "iterable";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            p.has_default = true;
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "maxlen";
            q.ty = pyadamas::t_int();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            f->ret = pyadamas::new_ty(pyadamas::TyKind::Deque);
            f->ret_known = true;
            func_with("deque", f);
        }
    } else if (name == "datetime") {
        auto dt = pyadamas::new_ty(pyadamas::TyKind::DateTime);
        // date(y, m, d) / today()
        func("date", {pyadamas::t_int(), pyadamas::t_int(), pyadamas::t_int()}, dt);
        func("today", {}, dt);
        // now()：带时分秒的当前时间
        func("now", {}, dt);
        // datetime(...)：3 或 6 个位置参数，个数在降级阶段检查
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "datetime.datetime";
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = pyadamas::t_int();
            f->ret = dt;
            f->ret_known = true;
            func_with("datetime", f);
        }
        // timedelta(days=0, seconds=0, microseconds=0, milliseconds=0,
        //           minutes=0, hours=0, weeks=0)：全部是可选的 int 参数。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "datetime.timedelta";
            for (const char* pn : {"days", "seconds", "microseconds", "milliseconds",
                                   "minutes", "hours", "weeks"}) {
                pyadamas::Param p;
                p.name = pn;
                p.ty = pyadamas::t_int();
                p.has_default = true;
                f->pos.push_back(std::move(p));
            }
            f->ret = pyadamas::new_ty(pyadamas::TyKind::TimeDelta);
            f->ret_known = true;
            func_with("timedelta", f);
        }
    } else if (name == "operator") {
        // 包装函数按调用点特化：签名只用于名字解析与 from-import 标记。
        auto simple = [&](const std::string& n, size_t nargs, pyadamas::TypePtr ret) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "operator." + n;
            for (size_t i = 0; i < nargs; ++i) {
                pyadamas::Param p;
                p.name = i == 0 ? "a" : "b";
                p.ty = pyadamas::t_unknown();
                f->pos.push_back(std::move(p));
            }
            f->ret = std::move(ret);
            f->ret_known = true;
            func_with(n, f);
        };
        for (const char* n : {"add", "sub", "mul", "truediv", "floordiv", "mod", "pow",
                              "and_", "or_", "xor", "lshift", "rshift", "concat",
                              "getitem", "contains"})
            simple(n, 2, pyadamas::t_unknown());
        for (const char* n : {"eq", "ne", "lt", "le", "gt", "ge", "is_", "is_not"})
            simple(n, 2, pyadamas::t_bool());
        for (const char* n : {"neg", "pos", "abs", "invert"})
            simple(n, 1, pyadamas::t_unknown());
        for (const char* n : {"truth", "not_"})
            simple(n, 1, pyadamas::t_bool());
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "operator.itemgetter";
            f->varargs = true;
            f->vararg_name = "items";
            f->vararg_ty = pyadamas::t_unknown();
            f->ret = pyadamas::t_unknown();
            f->ret_known = true;
            func_with("itemgetter", f);
        }
        {
            // 目前只在名字解析层面存在；调用点会给出明确的不支持错误。
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "operator.attrgetter";
            f->varargs = true;
            f->vararg_name = "attrs";
            f->vararg_ty = pyadamas::t_str();
            f->ret = pyadamas::t_unknown();
            f->ret_known = true;
            func_with("attrgetter", f);
        }
    } else if (name == "pathlib") {
        auto p = pyadamas::new_ty(pyadamas::TyKind::Path);
        func("Path", {pyadamas::t_str()}, p);
    } else if (name == "functools") {
        // reduce 的返回类型按实参在调用点特化，签名只负责参数个数检查。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "functools.reduce";
            pyadamas::Param p;
            p.name = "function";
            p.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "iterable";
            q.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(q));
            pyadamas::Param r;
            r.name = "initial";
            r.ty = pyadamas::t_unknown();
            r.has_default = true;
            f->pos.push_back(std::move(r));
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with("reduce", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "functools.wraps";
            pyadamas::Param p;
            p.name = "wrapped";
            p.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with("wraps", f);
        }
        for (auto& n : {"partial", "lru_cache"}) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = std::string("functools.") + n;
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = pyadamas::t_unknown();
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with(n, f);
        }
    } else if (name == "json") {
        // 类型由调用点的静态类型决定；签名只用于 from-import 的可调用标记。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "json.dumps";
            pyadamas::Param p;
            p.name = "obj";
            p.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(p));
            // indent / sort_keys 可选；实际取值在降级阶段按关键字读。
            pyadamas::Param q;
            q.name = "indent";
            q.ty = pyadamas::t_int();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            pyadamas::Param r;
            r.name = "sort_keys";
            r.ty = pyadamas::t_bool();
            r.has_default = true;
            f->pos.push_back(std::move(r));
            f->ret = pyadamas::t_str();
            f->ret_known = true;
            func_with("dumps", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "json.loads";
            pyadamas::Param p;
            p.name = "s";
            p.ty = pyadamas::t_str();
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "sample";
            q.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(q));
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with("loads", f);
        }
    } else if (name == "re") {
        info->globals["I"] = pyadamas::t_int();
        info->globals["IGNORECASE"] = pyadamas::t_int();
        info->globals["M"] = pyadamas::t_int();
        info->globals["MULTILINE"] = pyadamas::t_int();
        info->globals["S"] = pyadamas::t_int();
        info->globals["DOTALL"] = pyadamas::t_int();
        info->globals["X"] = pyadamas::t_int();
        info->globals["VERBOSE"] = pyadamas::t_int();
        auto re_fn = [&](const std::string& n,
                         std::vector<std::pair<std::string, pyadamas::TypePtr>> ps,
                         size_t ndef, pyadamas::TypePtr ret) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = n;
            for (size_t i = 0; i < ps.size(); ++i) {
                pyadamas::Param p;
                p.name = ps[i].first;
                p.ty = ps[i].second;
                if (i + ndef >= ps.size()) {
                    p.has_default = true;
                    p.default_lit = lv_int(0);
                }
                f->pos.push_back(std::move(p));
            }
            f->ret = ret;
            f->ret_known = true;
            func_with(n, f);
        };
        re_fn("compile", {{"pattern", pyadamas::t_str()}, {"flags", pyadamas::t_int()}}, 1,
              pyadamas::t_regex());
        re_fn("search",
              {{"pattern", pyadamas::t_str()}, {"string", pyadamas::t_str()}, {"flags", pyadamas::t_int()}},
              1, pyadamas::mk_optional(pyadamas::t_match()));
        re_fn("match",
              {{"pattern", pyadamas::t_str()}, {"string", pyadamas::t_str()}, {"flags", pyadamas::t_int()}},
              1, pyadamas::mk_optional(pyadamas::t_match()));
        re_fn("fullmatch",
              {{"pattern", pyadamas::t_str()}, {"string", pyadamas::t_str()}, {"flags", pyadamas::t_int()}},
              1, pyadamas::mk_optional(pyadamas::t_match()));
        re_fn("findall",
              {{"pattern", pyadamas::t_str()}, {"string", pyadamas::t_str()}, {"flags", pyadamas::t_int()}},
              1, pyadamas::mk_list(pyadamas::t_str()));
        re_fn("finditer",
              {{"pattern", pyadamas::t_str()}, {"string", pyadamas::t_str()}, {"flags", pyadamas::t_int()}},
              1, pyadamas::mk_list(pyadamas::t_match()));
        re_fn("sub",
              {{"pattern", pyadamas::t_str()},
               {"repl", pyadamas::t_str()},
               {"string", pyadamas::t_str()},
               {"count", pyadamas::t_int()},
               {"flags", pyadamas::t_int()}},
              2, pyadamas::t_str());
        re_fn("split",
              {{"pattern", pyadamas::t_str()},
               {"string", pyadamas::t_str()},
               {"maxsplit", pyadamas::t_int()},
               {"flags", pyadamas::t_int()}},
              2, pyadamas::mk_list(pyadamas::t_str()));
        re_fn("escape", {{"pattern", pyadamas::t_str()}}, 0, pyadamas::t_str());
    } else if (name == "random") {
        // 不传种子时绑定哨兵 INT64_MIN，运行时用时间 + pid 播种。
        const int64_t kNoSeed = -9223372036854775807LL - 1;
        func("random", {}, pyadamas::t_float());
        func("randint", {pyadamas::t_int(), pyadamas::t_int()}, pyadamas::t_int());
        func("uniform", {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        func("getrandbits", {pyadamas::t_int()}, pyadamas::t_int());
        func("gauss", {pyadamas::t_float(), pyadamas::t_float()}, pyadamas::t_float());
        func("expovariate", {pyadamas::t_float()}, pyadamas::t_float());
        // triangular：CPython 里三个参数都有默认值，这里只把后两个标成可选，
        // 实际取值由降级阶段按实参个数决定（mode 缺省取中点）。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "random.triangular";
            const char* names[] = {"low", "high", "mode"};
            for (int i = 0; i < 3; ++i) {
                pyadamas::Param p;
                p.name = names[i];
                p.ty = pyadamas::t_float();
                if (i > 0) {
                    p.has_default = true;
                    p.default_lit = lv_float(0.0);
                }
                f->pos.push_back(std::move(p));
            }
            f->ret = pyadamas::t_float();
            f->ret_known = true;
            func_with("triangular", f);
        }
        // randrange：1~3 个位置参数，个数和方向在降级阶段检查
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "random.randrange";
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = pyadamas::t_int();
            f->ret = pyadamas::t_int();
            f->ret_known = true;
            func_with("randrange", f);
        }
        // sample / choices：返回 list[元素类型]，类型在 typecheck 里特化
        for (auto& n : {"sample", "choices"}) {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = std::string("random.") + n;
            pyadamas::Param p;
            p.name = "seq";
            p.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "k";
            q.ty = pyadamas::t_int();
            f->pos.push_back(std::move(q));
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with(n, f);
        }
        // choice/shuffle 的返回类型按实参在 typecheck 里特判（random_call）；
        // 这两个 FuncInfo 用带模块前缀的名字，from-import 调用也能识别。
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "random.choice";
            pyadamas::Param p;
            p.name = "seq";
            p.ty = pyadamas::t_unknown();
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::t_unknown();
            f->ret_known = false;
            func_with("choice", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "random.shuffle";
            pyadamas::Param p;
            p.name = "x";
            p.ty = pyadamas::mk_list(pyadamas::t_unknown());
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::t_none();
            f->ret_known = true;
            func_with("shuffle", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "seed";
            pyadamas::Param p;
            p.name = "a";
            p.ty = pyadamas::t_int();
            p.has_default = true;
            p.default_lit = lv_int(kNoSeed);
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::t_none();
            f->ret_known = true;
            func_with("seed", f);
        }
        {
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "Random";
            pyadamas::Param p;
            p.name = "a";
            p.ty = pyadamas::t_int();
            p.has_default = true;
            p.default_lit = lv_int(kNoSeed);
            f->pos.push_back(std::move(p));
            f->ret = pyadamas::t_random();
            f->ret_known = true;
            func_with("Random", f);
        }
    } else if (name == "os") {
        info->globals["sep"] = pyadamas::t_str();
        info->globals["linesep"] = pyadamas::t_str();
        for (auto& n : {"F_OK", "R_OK", "W_OK", "X_OK"}) info->globals[n] = pyadamas::t_int();
        func("mkdir", {pyadamas::t_str()}, pyadamas::t_none());
        func("rmdir", {pyadamas::t_str()}, pyadamas::t_none());
        func("remove", {pyadamas::t_str()}, pyadamas::t_none());
        func("rename", {pyadamas::t_str(), pyadamas::t_str()}, pyadamas::t_none());
        func("getcwd", {}, pyadamas::t_str());
        func("cpu_count", {}, pyadamas::t_int());
        func("listdir", {pyadamas::t_str()}, pyadamas::mk_list(pyadamas::t_str()));
        {
            // getenv(name, default="")：CPython 缺失返回 None，pyadamas 用空串默认值。
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "getenv";
            pyadamas::Param p;
            p.name = "name";
            p.ty = pyadamas::t_str();
            f->pos.push_back(std::move(p));
            pyadamas::Param d;
            d.name = "default";
            d.ty = pyadamas::t_str();
            d.has_default = true;
            pyadamas::LiteralValue lv;
            lv.kind = pyadamas::LiteralValue::Kind::Str;
            lv.s = "";
            d.default_lit = lv;
            f->pos.push_back(std::move(d));
            f->ret = pyadamas::t_str();
            f->ret_known = true;
            func_with("getenv", f);
        }
        func("getpid", {}, pyadamas::t_int());
        func("chdir", {pyadamas::t_str()}, pyadamas::t_none());
        func("chmod", {pyadamas::t_str(), pyadamas::t_int()}, pyadamas::t_none());
        func("access", {pyadamas::t_str(), pyadamas::t_int()}, pyadamas::t_bool());
        {
            // makedirs(name, exist_ok=False)：exist_ok 的参数在降级阶段
            // 手工装配（默认值没有 AST 节点可用）。
            auto f = std::make_shared<pyadamas::FuncInfo>();
            f->name = "os.makedirs";
            pyadamas::Param p;
            p.name = "name";
            p.ty = pyadamas::t_str();
            f->pos.push_back(std::move(p));
            pyadamas::Param q;
            q.name = "exist_ok";
            q.ty = pyadamas::t_bool();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            f->ret = pyadamas::t_none();
            f->ret_known = true;
            func_with("makedirs", f);
        }
    } else if (name == "os.path") {
        auto f = std::make_shared<pyadamas::FuncInfo>();
        f->name = "join";
        f->varargs = true;
        f->vararg_name = "parts";
        f->vararg_ty = pyadamas::t_str();
        f->ret = pyadamas::t_str();
        f->ret_known = true;
        func_with("join", f);
        func("exists", {pyadamas::t_str()}, pyadamas::t_bool());
        func("isdir", {pyadamas::t_str()}, pyadamas::t_bool());
        func("isfile", {pyadamas::t_str()}, pyadamas::t_bool());
        for (auto& n : {"basename", "dirname", "normpath", "abspath", "expanduser"})
            func(n, {pyadamas::t_str()}, pyadamas::t_str());
        func("splitext", {pyadamas::t_str()}, pyadamas::mk_tuple({pyadamas::t_str(), pyadamas::t_str()}));
        func("realpath", {pyadamas::t_str()}, pyadamas::t_str());
        func("getsize", {pyadamas::t_str()}, pyadamas::t_int());
        func("getmtime", {pyadamas::t_str()}, pyadamas::t_float());
        func("split", {pyadamas::t_str()}, pyadamas::mk_tuple({pyadamas::t_str(), pyadamas::t_str()}));
        func("samefile", {pyadamas::t_str(), pyadamas::t_str()}, pyadamas::t_bool());
    } else if (name == "subprocess") {
        auto f = std::make_shared<pyadamas::FuncInfo>();
        f->name = "run";
        auto add = [&](const std::string& n, pyadamas::TypePtr ty, bool has_def,
                       pyadamas::LiteralValue lv) {
            pyadamas::Param p;
            p.name = n;
            p.ty = std::move(ty);
            p.has_default = has_def;
            if (has_def) p.default_lit = lv;
            f->pos.push_back(std::move(p));
        };
        add("args", pyadamas::t_unknown(), false, lv_int(0));
        add("capture_output", pyadamas::t_bool(), true, lv_bool(false));
        add("text", pyadamas::t_bool(), true, lv_bool(false));
        add("timeout", pyadamas::t_float(), true, lv_float(-1.0));
        add("check", pyadamas::t_bool(), true, lv_bool(false));
        add("shell", pyadamas::t_bool(), true, lv_bool(false));
        add("cwd", pyadamas::t_str(), true, lv_str(""));
        f->ret = pyadamas::t_completed();
        f->ret_known = true;
        func_with("run", f);
        // Popen：真正降级在 lower_popen 里手工装配，这里只让类型检查通过。
        {
            auto g = std::make_shared<pyadamas::FuncInfo>();
            g->name = "subprocess.Popen";
            auto padd = [&](const std::string& n, pyadamas::TypePtr ty, bool has_def,
                            pyadamas::LiteralValue lv) {
                pyadamas::Param q;
                q.name = n;
                q.ty = std::move(ty);
                q.has_default = has_def;
                if (has_def) q.default_lit = lv;
                g->pos.push_back(std::move(q));
            };
            padd("args", pyadamas::t_unknown(), false, lv_int(0));
            padd("shell", pyadamas::t_bool(), true, lv_bool(false));
            padd("cwd", pyadamas::t_unknown(), true, lv_str(""));
            padd("stdin", pyadamas::t_unknown(), true, lv_int(0));
            padd("stdout", pyadamas::t_unknown(), true, lv_int(0));
            padd("stderr", pyadamas::t_unknown(), true, lv_int(0));
            padd("text", pyadamas::t_bool(), true, lv_bool(false));
            g->ret = pyadamas::t_popen();
            g->ret_known = true;
            func_with("Popen", g);
        }
        info->globals["PIPE"] = pyadamas::t_int();
        info->globals["STDOUT"] = pyadamas::t_int();
        info->globals["DEVNULL"] = pyadamas::t_int();
    }
    cache[name] = info;
    return info;
}

}  // namespace pyadamas
