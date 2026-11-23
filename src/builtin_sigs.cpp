// builtin_sigs.cpp —— 内建模块的函数签名表
//
// 只放数据：模块名 → 全局名 → 类型。多模块构建时由 program_module
// 调用，类型检查直接复用同一份签名。
#include "driver_build.h"

#include "native_sigs.h"  // 声明式原生模块签名（P0）

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
    if (name == "c") {
        // 免写直调：符号名集合在编译期不可知（from c import 任意名字），
        // globals 留空；绑定与调用检查在 stmt_import_from / check_c_lazy。
    } else if (name == "dylib") {
        func("load", {pyadamas::t_str()}, pyadamas::t_dylib());
        func("close", {pyadamas::t_dylib()}, pyadamas::t_int());
    } else if (name == "sys") {
        // sys 里**无法用 .pysig 声明**的 6 个运行期成员（其余如 maxsize/exit
        // 已迁到 stdlib/sigs/sys.pysig，靠末尾的默认 merge 合并进来）：
        //   argv/executable/标准流：运行期由 builtin_value 生成 call_runtime，
        //     静态类型必须是 list[str]/str/file，声明成 object 会让方法调用失效；
        //   exc_info()：返回类型含 ClassObj，.pysig 注解语法表达不了。
        info->globals["argv"] = pyadamas::mk_list(pyadamas::t_str());
        info->globals["executable"] = pyadamas::t_str();
        info->globals["stdin"] = pyadamas::t_file();
        info->globals["stdout"] = pyadamas::t_file();
        info->globals["stderr"] = pyadamas::t_file();
        func("exc_info", {},
             pyadamas::mk_tuple({pyadamas::mk_class_obj(pyadamas::mk_exception("Exception")),
                                 pyadamas::t_unknown(), pyadamas::t_unknown()}));
        // exit 的正常来源是 .pysig 的声明；这里保留硬编码 fallback，半声明
        // 状态（临时 pysig 只写一部分）下它仍然可用——merge 语义的直接体现。
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
        // ── 动态 JSON（0.447）────────────────────────────────────────
        // 签名只给类型检查/from-import 用；具体语义在 lower_json/json_call。
        // parse 的返回是 unknown：JSON 形状由对端决定，静态类型拿不到。
        func("parse", {pyadamas::t_str()}, pyadamas::t_unknown());
        func("stringify", {pyadamas::t_unknown()}, pyadamas::t_str());
        // 访问辅助：get/get_or/has 的 key 是 str（对象）或 int（数组下标），
        // 静态上收成 unknown 让两种实参都过（运行期按容器种类选）。
        func("get", {pyadamas::t_unknown(), pyadamas::t_unknown()},
             pyadamas::t_unknown());
        func("get_or",
             {pyadamas::t_unknown(), pyadamas::t_unknown(), pyadamas::t_unknown()},
             pyadamas::t_unknown());
        func("has", {pyadamas::t_unknown(), pyadamas::t_unknown()},
             pyadamas::t_bool());
        func("path", {pyadamas::t_unknown(), pyadamas::mk_list(pyadamas::t_str())},
             pyadamas::t_unknown());
        func("len", {pyadamas::t_unknown()}, pyadamas::t_int());
        func("keys", {pyadamas::t_unknown()}, pyadamas::mk_list(pyadamas::t_str()));
        func("type", {pyadamas::t_unknown()}, pyadamas::t_str());
        func("is_null", {pyadamas::t_unknown()}, pyadamas::t_bool());
        func("as_str", {pyadamas::t_unknown()}, pyadamas::t_str());
        func("as_int", {pyadamas::t_unknown()}, pyadamas::t_int());
        func("as_float", {pyadamas::t_unknown()}, pyadamas::t_float());
        func("as_bool", {pyadamas::t_unknown()}, pyadamas::t_bool());
        func("as_dict", {pyadamas::t_unknown()},
             pyadamas::mk_dict(pyadamas::t_str(), pyadamas::t_unknown()));
        func("as_list", {pyadamas::t_unknown()},
             pyadamas::mk_list(pyadamas::t_unknown()));
        // 构造辅助：unknown 位置上的 JSON null/true/false 要显式拿盒，
        // 否则语言既有的 None/bool→unknown 表示是裸 0/1，stringify 会写成 0/1。
        func("null", {}, pyadamas::t_unknown());
        func("bool", {pyadamas::t_bool()}, pyadamas::t_unknown());
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
    } else if (name == "os") {
        // 其余 os 成员已迁到 stdlib/sigs/os.pysig（按名字 merge）。
        // environ/walk 无法用字面量/标量签名表达，保留在硬编码。
        // os.environ：只读快照 dict[str,str]（写入在降级阶段显式报错）
        info->globals["environ"] = pyadamas::mk_dict(pyadamas::t_str(), pyadamas::t_str());
        // walk：立即求值（非生成器），返回 list[tuple[str, list[str], list[str]]]
        func("walk", {pyadamas::t_str()},
             pyadamas::mk_list(
                 pyadamas::mk_tuple({pyadamas::t_str(), pyadamas::mk_list(pyadamas::t_str()),
                                     pyadamas::mk_list(pyadamas::t_str())})));
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
    // 声明式签名与硬编码**按名字合并**（P0 merge 语义）：stdlib/sigs/<module>.pysig
    // 里的 func/const 优先（同名覆盖硬编码），硬编码独有的成员原样保留。
    // 例如 sys：maxsize/exit 等在 .pysig 里声明（覆盖），而 argv/stdout/
    // exc_info 这类只能由 builtin_value 生成运行期值的成员留在硬编码里。
    // 因此任何模块都不需要 C++ 侧的专用 merge 分支：能声明的声明，不能声明
    // 的继续用硬编码，两者共存。
    if (auto declared = pyadamas::nativesig::module_info(name)) {
        for (auto& [gname, gty] : declared->globals) info->globals[gname] = gty;
    }
    cache[name] = info;
    return info;
}

}  // namespace pyadamas
