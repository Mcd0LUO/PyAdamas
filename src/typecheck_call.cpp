// typecheck_call.cpp —— 调用与内置：参数检查、builtin、asyncio、方法表
#include "typecheck.h"

#include "module.h"

namespace py23 {

TypePtr TypeChecker::call(Call& c) {
        // f(...) 里 f 是 Name：可能是异常构造器、内置函数，也可能是用户函数
        if (auto* n = dynamic_cast<Name*>(c.func.get())) {
            if (n->id == "super" && !lookup("super")) {
                if (!cur_class_) {
                    err(c.line, c.col, "super() is only supported inside a class method");
                    return t_unknown();
                }
                if (!c.args.empty() || !c.keywords.empty()) {
                    err(c.line, c.col,
                        "only zero-argument super() is supported inside a method");
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    return t_unknown();
                }
                return t_unknown();
            }
            if (!lookup(n->id) && is_exception_name(n->id)) {
                if (!c.keywords.empty())
                    err(c.line, c.col, "exception constructor does not take keyword arguments");
                if (c.args.size() > 1)
                    err(c.line, c.col,
                        "exception constructor takes at most one message argument");
                for (auto& arg : c.args) expr(*arg);
                return mk_exception(n->id);
            }
            if (auto* sym = lookup(n->id);
                sym && sym->ty && sym->ty->kind == TyKind::Class && sym->ty->cls &&
                sym->ty->cls->is_exception_class) {
                if (!c.keywords.empty())
                    err(c.line, c.col, "exception constructor does not take keyword arguments");
                if (c.args.size() > 1)
                    err(c.line, c.col,
                        "exception constructor takes at most one message argument");
                for (auto& arg : c.args) expr(*arg);
                return mk_exception(sym->ty->cls->name);
            }
            if (!lookup(n->id) && is_builtin_callable(n->id)) return builtin_call(n->id, c);
        }
        if (auto* a = dynamic_cast<Attribute*>(c.func.get())) {
            if (auto* vn = dynamic_cast<Name*>(a->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
                    if (mod->builtin && mod->name == "asyncio") {
                        if (auto r = asyncio_call(a->attr, c)) return *r;
                    }
                    if (auto ci = mod->classes.find(a->attr); ci != mod->classes.end()) {
                        auto cls = ci->second.cls;
                        if (cls->has_init && ci->second.ctor_sig) {
                            check_args(ci->second.ctor_sig, c);
                        } else if (!c.args.empty() || !c.keywords.empty()) {
                            err(c.line, c.col,
                                std::format("'{}' takes no arguments", cls->name));
                            for (auto& arg : c.args) expr(*arg);
                            for (auto& kw : c.keywords) expr(*kw.value);
                        }
                        return mk_class(cls.get());
                    }
                    auto it = mod->globals.find(a->attr);
                    if (it == mod->globals.end()) {
                        err(c.line, c.col,
                            std::format("module '{}' has no attribute '{}'", vn->id, a->attr));
                        return t_unknown();
                    }
                    auto mt = it->second;
                    if (mt && mt->kind == TyKind::Func) return check_args(mt->fn, c);
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    err(c.line, c.col, "module attribute is not callable");
                    return t_unknown();
                }
            }
            // super().m(...)：按定义类的 MRO 找下一实现，至少能检查签名。
            if (auto* sc = dynamic_cast<Call*>(a->value.get())) {
                if (auto* sn = dynamic_cast<Name*>(sc->func.get());
                    sn && sn->id == "super" && !lookup("super") && sc->args.empty() &&
                    sc->keywords.empty()) {
                    if (!cur_class_) {
                        err(c.line, c.col,
                            "super() is only supported inside a class method");
                        return t_unknown();
                    }
                    const MethodInfo* m = super_method(a->attr);
                    if (!m) {
                        err(c.line, c.col,
                            std::format("super() has no method '{}' after class '{}'",
                                        a->attr, cur_class_->name));
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                        return t_unknown();
                    }
                    if (m->kind != MethodKind::Normal || !m->call_sig) {
                        err(c.line, c.col,
                            std::format("super().{} is not a plain method", a->attr));
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                        return t_unknown();
                    }
                    return check_args(m->call_sig, c);
                }
            }
            // int.bit_length / type.__new__ 这类「类型上的未绑定方法」：
            // 接收者不是值，没有可检查的方法表
            if (auto* vn = dynamic_cast<Name*>(a->value.get()); vn && is_type_name(vn->id)) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            auto ot = expr(*a->value);
            if (is_unknown(ot)) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            return method_call(ot, a->attr, c, is_self_attr(*a));
        }
        auto ft = expr(*c.func);
        if (is_unknown(ft)) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        if (ft->kind == TyKind::BoundMethod) return method_call(ft->sub[0], ft->name, c);
        if (ft->kind == TyKind::Class) {
            // 构造：分配 + 字段初值 + 调 __init__。参数按 __init__ 的
            // 调用签名检查（已去掉 self），返回值是实例。
            auto cls = ft->cls;
            if (!cls) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            if (!cls->complete && cls != cur_class_) {
                // 类体还没检查到：按未知放行，别把「先使用后定义」判死
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return mk_class(cls);
            }
            if (cls->has_init) {
                if (auto* im = cls->method("__init__"); im && im->call_sig)
                    check_args(im->call_sig, c);
                else
                    for (auto& arg : c.args) expr(*arg);
            } else {
                // 没有 __init__ 时只允许零实参；但如果父类在外部（比如
                // unittest.TestCase），__init__ 是那边定义的，管不了。
                if ((!c.args.empty() || !c.keywords.empty()) && !cls->is_open())
                    err(c.line, c.col,
                        std::format("'{}' takes no arguments (no __init__)", cls->name));
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            return mk_class(cls);
        }
        if (ft->kind != TyKind::Func) {
            err(c.line, c.col, std::format("'{}' object is not callable", type_name(ft)));
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        return check_args(ft->fn, c);
    }

TypePtr TypeChecker::check_args(const FuncInfoPtr& fn, Call& c) {
        // 实参和关键字实参都只求值一次。之前关键字值在循环里被求了两次，
        // 一次的类型错误会重复报两遍。
        std::vector<TypePtr> argtys;
        argtys.reserve(c.args.size());
        bool has_star = false;
        for (auto& arg : c.args) {
            if (arg->kind == ExprKind::Starred) {
                has_star = true;
                auto t = expr(*static_cast<Starred&>(*arg).value);
                argtys.push_back(iter_elem(t, arg->line, arg->col));
            } else {
                argtys.push_back(expr(*arg));
            }
        }
        std::vector<std::pair<std::string, TypePtr>> kws;
        kws.reserve(c.keywords.size());
        for (auto& kw : c.keywords) kws.emplace_back(kw.arg, expr(*kw.value));

        if (!fn || fn->unchecked) return t_unknown();

        const size_t npos = fn->pos.size();
        std::vector<bool> filled(npos, false);
        std::vector<bool> kw_filled(fn->kwonly.size(), false);
        // **mapping 调用：键在静态上不可知，缺参检查必须整体放弃
        bool has_dstar = false;
        for (auto& [name, ty] : kws)
            if (name.empty()) has_dstar = true;

        if (!has_star && argtys.size() > npos && !fn->varargs) {
            err(c.line, c.col,
                std::format("function '{}' takes {} positional argument{} but {} were given",
                            fn->name, npos, npos == 1 ? "" : "s", argtys.size()));
        }

        for (size_t i = 0; i < argtys.size(); ++i) {
            if (i < npos) {
                filled[i] = true;
                if (!assignable(fn->pos[i].ty, argtys[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'",
                                    fn->pos[i].name, fn->name, type_name(fn->pos[i].ty),
                                    type_name(argtys[i])));
            } else if (fn->varargs && !assignable(fn->vararg_ty, argtys[i])) {
                err(c.args[i]->line, c.args[i]->col,
                    std::format("'*{}' of '{}' expects '{}', got '{}'", fn->vararg_name,
                                fn->name, type_name(fn->vararg_ty), type_name(argtys[i])));
            }
        }

        for (auto& [name, ty] : kws) {
            if (name.empty()) continue;  // **mapping：键在静态上不可知
            auto pit = std::find_if(fn->pos.begin(), fn->pos.end(),
                                    [&](const Param& p) { return p.name == name; });
            if (pit != fn->pos.end()) {
                size_t idx = static_cast<size_t>(pit - fn->pos.begin());
                if (idx < fn->n_posonly) {
                    err(c.line, c.col, std::format("'{}' is a positional-only argument of '{}'",
                                                   name, fn->name));
                    continue;
                }
                if (filled[idx]) {
                    // 同一个参数被位置实参和关键字实参同时指定（或关键字重复）
                    err(c.line, c.col,
                        std::format("'{}' got multiple values for argument '{}'", fn->name, name));
                    continue;
                }
                filled[idx] = true;
                if (!assignable(pit->ty, ty))
                    err(c.line, c.col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'", name,
                                    fn->name, type_name(pit->ty), type_name(ty)));
                continue;
            }
            auto kit = std::find_if(fn->kwonly.begin(), fn->kwonly.end(),
                                    [&](const Param& p) { return p.name == name; });
            if (kit != fn->kwonly.end()) {
                size_t idx = static_cast<size_t>(kit - fn->kwonly.begin());
                if (kw_filled[idx]) {
                    err(c.line, c.col,
                        std::format("'{}' got multiple values for argument '{}'", fn->name, name));
                    continue;
                }
                kw_filled[idx] = true;
                if (!assignable(kit->ty, ty))
                    err(c.line, c.col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'", name,
                                    fn->name, type_name(kit->ty), type_name(ty)));
                continue;
            }
            if (!fn->kwargs)
                err(c.line, c.col,
                    std::format("'{}' got an unexpected keyword argument '{}'", fn->name, name));
        }

        // 缺参检查：位置参数可以被关键字补上，关键字参数只能靠关键字
        std::string missing;
        auto add_missing = [&](const std::string& n) {
            if (!missing.empty()) missing += ", ";
            missing += "'" + n + "'";
        };
        if (!has_star && !has_dstar) {
            for (size_t i = 0; i < npos; ++i)
                if (!filled[i] && !fn->pos[i].has_default) add_missing(fn->pos[i].name);
            for (size_t i = 0; i < fn->kwonly.size(); ++i)
                if (!kw_filled[i] && !fn->kwonly[i].has_default)
                    add_missing(fn->kwonly[i].name);
        }
        if (!missing.empty())
            err(c.line, c.col,
                std::format("function '{}' missing required argument{}: {}", fn->name,
                            missing.find(',') == std::string::npos ? "" : "s", missing));

        if (fn->ret) return fn->ret;
        return fn->ret_known ? t_none() : t_unknown();
    }

bool TypeChecker::is_exception_type(const std::string& n) {
        return is_exception_name(n);
}

bool TypeChecker::is_exception_name(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "BaseException", "Exception", "ArithmeticError", "AssertionError",
            "AttributeError",  "IndexError",  "KeyError",        "LookupError",
            "MemoryError",     "NameError",   "NotImplementedError", "OSError",
            "UnboundLocalError",
            "IOError",         "OverflowError", "RecursionError", "RuntimeError",
            "StopIteration",   "SyntaxError", "TypeError",       "ValueError",
            "ZeroDivisionError", "FileNotFoundError", "PermissionError",
            "ImportError",     "ModuleNotFoundError", "UnicodeDecodeError",
            "UnicodeEncodeError", "TimeoutError", "SystemExit",   "KeyboardInterrupt",
            "Warning", "DeprecationWarning", "UserWarning", "RuntimeWarning",
            "FutureWarning", "PendingDeprecationWarning", "ResourceWarning",
            "SyntaxWarning", "BytesWarning", "ImportWarning", "UnicodeWarning",
            "EncodingWarning", "ExceptionGroup", "BaseExceptionGroup",
            "EnvironmentError", "ConnectionError", "BrokenPipeError",
            "UnicodeError", "ReferenceError", "BufferError", "EOFError",
            // 文件系统与解释器相关的异常：标准库到处在用，漏一个就是一片未定义名
            "FileExistsError", "IsADirectoryError", "NotADirectoryError",
            "IndentationError", "TabError", "SystemError", "FloatingPointError",
            "BlockingIOError", "ChildProcessError", "ConnectionAbortedError",
            "ConnectionRefusedError", "ConnectionResetError", "InterruptedError",
            "ProcessLookupError", "UnicodeTranslateError", "GeneratorExit",
            "CancelledError",
            "StopAsyncIteration", "WindowsError",  // WindowsError 只在 Windows 上存在（OSError 别名）
        };
        return names.count(n) > 0;
    }

TypePtr TypeChecker::builtin_value_type(const std::string& n) {
        if (n == "__name__" || n == "__file__" || n == "__doc__") return t_str();
        if (n == "__debug__") return t_bool();
        if (n == "NotImplemented" || n == "Ellipsis") return t_unknown();
        return nullptr;
    }

bool TypeChecker::is_builtin_callable(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "print",  "len",    "range",  "abs",   "min",     "max",    "sum",
            "sorted", "reversed", "enumerate", "zip", "int",  "float",  "str",
            "bool",   "list",   "dict",   "tuple", "set",     "ord",    "chr",
            "repr",   "any",    "all",    "round", "isinstance", "input", "type",
            "map",    "filter", "divmod", "pow",   "hex",     "bin",    "oct",
            // 装饰器：语义尚未实现，但名字必须能解析，否则大量真实代码会在
            // 第一行就报未定义，淹没真正有价值的类型错误
            "staticmethod", "classmethod", "property", "abstractmethod",
            "dataclass", "override", "cached_property", "contextmanager",
            "lru_cache", "wraps", "total_ordering", "singledispatch",
            // 运行时依赖名字：类型无法静态表达，值一律为 unknown
            "open",  "getattr", "setattr", "hasattr", "delattr", "dir",   "vars",
            "id",    "hash",    "iter",    "next",    "slice",   "callable",
            "format", "super",  "exec",    "eval",    "globals", "locals",
            "compile", "__import__", "breakpoint", "help", "memoryview",
            "bytearray", "frozenset", "object", "complex", "bytes",
            "__build_class__",
            "issubclass", "ascii", "aiter", "anext",
            // site 模块注入的内置名（交互式退出），真实代码里也会用到
            "quit", "exit", "copyright", "credits", "license",
        };
        return names.count(n) > 0;
    }

TypePtr TypeChecker::builtin_type(const std::string& n) {
        auto info = std::make_shared<FuncInfo>();
        info->name = n;
        info->unchecked = true;
        // 具体规则在 builtin_call 里按名字分派，这里只给出可调用标记
        auto t = new_ty(TyKind::Func);
        t->fn = info;
        return t;
    }

std::optional<TypePtr> TypeChecker::asyncio_call(const std::string& member, Call& c) {
        if (member != "run" && member != "sleep" && member != "gather" &&
            member != "create_task" && member != "Future" &&
            member != "wait_readable" && member != "wait_writable")
            return std::nullopt;

        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        for (auto& kw : c.keywords) expr(*kw.value);
        if (!c.keywords.empty())
            err(c.line, c.col, std::format("asyncio.{}() does not take keyword arguments", member));

        if (member == "run") {
            if (a.size() != 1) {
                err(c.line, c.col, std::format("asyncio.run() expects 1 argument, got {}", a.size()));
                return t_unknown();
            }
            if (!a[0] || a[0]->kind == TyKind::Unknown) return t_unknown();
            if (a[0]->kind == TyKind::Coroutine)
                return a[0]->sub.empty() ? t_unknown() : a[0]->sub[0];
            err(c.line, c.col,
                std::format("asyncio.run() expects a coroutine, got '{}'", type_name(a[0])));
            return t_unknown();
        }

        if (member == "sleep") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.sleep() expects 1 argument, got {}", a.size()));
            } else if (!is_unknown(a[0]) && !is_num(a[0])) {
                err(c.line, c.col,
                    std::format("asyncio.sleep() delay must be a number, got '{}'",
                                type_name(a[0])));
            }
            return mk_coroutine(t_none());
        }

        if (member == "Future") {
            if (!a.empty() || !c.keywords.empty())
                err(c.line, c.col, "asyncio.Future() takes no arguments");
            return mk_future(t_unknown());
        }

        if (member == "wait_readable" || member == "wait_writable") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.{}() expects 1 argument, got {}", member, a.size()));
            } else if (!is_unknown(a[0]) && a[0]->kind != TyKind::Int) {
                err(c.line, c.col,
                    std::format("asyncio.{}() expects an int fd, got '{}'", member,
                                type_name(a[0])));
            }
            return mk_coroutine(t_none());
        }

        if (member == "create_task") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.create_task() expects 1 argument, got {}", a.size()));
                return mk_task(t_unknown());
            }
            if (!a[0] || a[0]->kind == TyKind::Unknown) return mk_task(t_unknown());
            if (a[0]->kind == TyKind::Coroutine)
                return mk_task(a[0]->sub.empty() ? t_unknown() : a[0]->sub[0]);
            err(c.line, c.col,
                std::format("asyncio.create_task() expects a coroutine, got '{}'",
                            type_name(a[0])));
            return mk_task(t_unknown());
        }

        // gather(*awaitables) -> Coroutine[list[公共结果类型]]
        TypePtr elem = nullptr;
        for (size_t i = 0; i < a.size(); ++i) {
            TypePtr it = a[i];
            TypePtr r = t_unknown();
            if (it && (it->kind == TyKind::Coroutine || it->kind == TyKind::Task ||
                       it->kind == TyKind::Future))
                r = it->sub.empty() ? t_unknown() : it->sub[0];
            else if (!is_unknown(it)) {
                err(c.args[i]->line, c.args[i]->col,
                    std::format("asyncio.gather() argument is not awaitable: '{}'",
                                type_name(it)));
            }
            elem = elem ? join(elem, r) : r;
        }
        return mk_coroutine(mk_list(elem ? elem : t_unknown()));
    }


bool TypeChecker::has_method(const TypePtr& t, const std::string& m) {
        if (!t) return true;
        switch (t->kind) {
            case TyKind::List: {
                static const std::unordered_set<std::string> ms = {
                    "append", "extend", "insert", "remove", "pop",    "clear",
                    "index",  "count",  "sort",   "reverse", "copy", "sort",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Generator:
                return m == "send" || m == "close" || m == "__next__";
            case TyKind::AsyncGenerator:
                return m == "asend" || m == "athrow" || m == "aclose" || m == "__anext__";
            case TyKind::Task:
                return m == "done" || m == "cancel" || m == "cancelled" ||
                       m == "result" || m == "exception";
            case TyKind::Future:
                return m == "done" || m == "cancel" || m == "cancelled" ||
                       m == "result" || m == "exception" || m == "set_result" ||
                       m == "set_exception" || m == "add_done_callback";
            case TyKind::Str: {
                static const std::unordered_set<std::string> ms = {
                    "upper",    "lower",     "strip",  "lstrip", "rstrip", "split",
                    "rsplit",   "join",      "replace", "find",  "rfind",  "startswith",
                    "endswith", "format",    "isdigit", "isalpha", "isspace", "isupper",
                    "islower",  "title",     "capitalize", "count", "index", "zfill",
                    "ljust",    "rjust",     "center", "encode", "removeprefix",
                    "removesuffix", "splitlines", "casefold", "swapcase", "partition",
                    "rpartition",   "isnumeric", "isalnum",  "isdecimal", "isidentifier",
                    "isprintable",  "istitle",   "expandtabs", "translate", "maketrans",
                    "format_map",   "rsplit",    "ljust",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Dict: {
                static const std::unordered_set<std::string> ms = {
                    "get",   "keys",   "values", "items",  "pop",    "update",
                    "setdefault", "clear", "copy", "popitem",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Set: {
                static const std::unordered_set<std::string> ms = {
                    "add", "remove", "discard", "pop", "clear", "copy", "update",
                    "union", "intersection", "difference", "issubset", "issuperset",
                    "difference_update", "intersection_update", "symmetric_difference",
                    "symmetric_difference_update", "isdisjoint",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Tuple: {
                static const std::unordered_set<std::string> ms = {"count", "index"};
                return ms.count(m) > 0;
            }
            case TyKind::File: {
                static const std::unordered_set<std::string> ms = {
                    "read",    "readline", "readlines", "write",    "writelines",
                    "close",   "flush",    "seek",      "tell",     "truncate",
                    "fileno",  "isatty",   "readable",  "writable", "seekable",
                    "__enter__", "__exit__",
                };
                return ms.count(m) > 0;
            }
            // 数值类型没有真正的「方法表」，但这些名字确实存在；
            // 缺了就是标准库里的假错误（statistics 用 int.bit_length）。
            case TyKind::Int:
            case TyKind::Bool:
            case TyKind::Float: {
                static const std::unordered_set<std::string> ms = {
                    "bit_length", "to_bytes", "from_bytes", "as_integer_ratio",
                    "conjugate",  "hex",      "fromhex",    "is_integer",
                    "numerator",  "denominator", "real",    "imag",
                };
                return ms.count(m) > 0;
            }
            default:
                return false;
        }
    }

const MethodInfo* TypeChecker::super_method(const std::string& name) const {
        if (!cur_class_) return nullptr;
        bool after = false;
        for (const ClassInfo* k : cur_class_->mro) {
            if (!after) {
                if (k == cur_class_) after = true;
                continue;
            }
            if (auto* m = k->own_method(name)) return m;
        }
        return nullptr;
    }

TypePtr TypeChecker::method_call(TypePtr recv, const std::string& name, Call& c,
                                 bool self_recv) {
        // Optional[Class] 上的方法：内层类的方法表是确定的，运行时有
        // None 检查兜底，静态上直接按内层查，避免收窄不到位就误杀。
        if (recv && recv->kind == TyKind::Optional && !recv->sub.empty() &&
            recv->sub[0]->kind == TyKind::Class)
            recv = recv->sub[0];
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        for (auto& kw : c.keywords) expr(*kw.value);

        auto argc = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) {
                err(c.line, c.col,
                    std::format("'{}.{}' expects {} argument{}, got {}", type_name(recv), name,
                                lo == hi ? std::to_string(lo) : std::format("{}..{}", lo, hi),
                                hi == 1 ? "" : "s", a.size()));
            }
        };
        auto elem = [&]() { return recv->sub.empty() ? t_unknown() : recv->sub[0]; };

        if (recv->kind == TyKind::Class) {
            if (!recv->cls) return t_unknown();
            // 骨架（还没看到类体）才放宽；本类方法体里成员表是齐的
            if (!recv->cls->complete && recv->cls != cur_class_) return t_unknown();
            auto* m = recv->cls->method(name);
            if (!m) {
                // 父类是外部类型：方法可能在那边，实参也无从检查。
                // self 上调不到的方法同理——mixin 组合出来的成员静态看不见。
                if (self_recv || recv->cls->is_open()) return t_unknown();
                err(c.line, c.col,
                    std::format("'{}' object has no method '{}'", recv->cls->name, name));
                return t_unknown();
            }
            auto r = m->call_sig ? check_args(m->call_sig, c) : t_unknown();
            return r;
        }

        if (recv->kind == TyKind::Generator) {
            if (name == "send") {
                argc(1, 1);
                return t_unknown();
            }
            if (name == "__next__" || name == "close") {
                argc(0, 0);
                return name == "close" ? t_none() : elem();
            }
            err(c.line, c.col, std::format("'generator' object has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncGenerator) {
            TypePtr et = elem();
            if (name == "asend") {
                argc(1, 1);
                return mk_coroutine(et);
            }
            if (name == "athrow") {
                argc(1, 1);
                if (a.size() == 1 && !is_unknown(a[0]) &&
                    a[0]->kind != TyKind::Exception)
                    err(c.line, c.col, "athrow() expects an exception instance");
                return mk_coroutine(et);
            }
            if (name == "__anext__") {
                argc(0, 0);
                return mk_coroutine(et);
            }
            if (name == "aclose") {
                argc(0, 0);
                return mk_coroutine(t_none());
            }
            err(c.line, c.col,
                std::format("'async generator' object has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::Task) {
            if (name == "done" || name == "cancel" || name == "cancelled") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "result") {
                argc(0, 0);
                return elem();
            }
            if (name == "exception") {
                argc(0, 0);
                return mk_optional(mk_exception("Exception"));
            }
            err(c.line, c.col, std::format("'task' object has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::Future) {
            if (name == "done" || name == "cancel" || name == "cancelled") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "set_result") {
                argc(1, 1);
                return t_none();
            }
            if (name == "set_exception") {
                argc(1, 1);
                if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Str &&
                    a[0]->kind != TyKind::Exception)
                    err(c.line, c.col,
                        "set_exception() expects an exception instance or message string");
                return t_none();
            }
            if (name == "add_done_callback") {
                argc(1, 1);
                if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Func)
                    err(c.line, c.col,
                        "add_done_callback() expects a function taking the future");
                return t_none();
            }
            if (name == "result") {
                argc(0, 0);
                return elem();
            }
            if (name == "exception") {
                argc(0, 0);
                return mk_optional(mk_exception("Exception"));
            }
            err(c.line, c.col, std::format("'future' object has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::List) {
            auto e = elem();
            if (name == "append") {
                argc(1, 1);
                if (a.size() == 1 && !merge_elem(recv->sub[0], recv, a[0], recv->locked))
                    err(c.line, c.col,
                        std::format("list element type is '{}', got '{}'", type_name(e),
                                    type_name(a[0])));
                return t_none();
            }
            if (name == "extend") {
                argc(1, 1);
                if (a.size() == 1) {
                    auto ie = iter_elem(a[0], c.line, c.col);
                    if (!merge_elem(recv->sub[0], recv, ie, recv->locked))
                        err(c.line, c.col,
                            std::format("list element type is '{}', got '{}'", type_name(e),
                                        type_name(ie)));
                }
                return t_none();
            }
            if (name == "insert") { argc(2, 2); return t_none(); }
            if (name == "remove") { argc(1, 1); return t_none(); }
            if (name == "pop") { argc(0, 1); return e; }
            if (name == "index") { argc(1, 3); return t_int(); }
            if (name == "count") { argc(1, 1); return t_int(); }
            if (name == "sort" || name == "reverse") { argc(0, 0); return t_none(); }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy") { argc(0, 0); return recv; }
        }

        if (recv->kind == TyKind::Str) {
            if (name == "upper" || name == "lower" || name == "strip" || name == "lstrip" ||
                name == "rstrip" || name == "title" || name == "capitalize" ||
                name == "casefold" || name == "swapcase" || name == "removeprefix" ||
                name == "removesuffix")
                return t_str();
            if (name == "split" || name == "rsplit" || name == "splitlines")
                return mk_list(t_str());
            if (name == "join") {
                argc(1, 1);
                if (a.size() == 1) iter_elem(a[0], c.line, c.col);
                return t_str();
            }
            if (name == "replace") { argc(2, 3); return t_str(); }
            if (name == "find" || name == "rfind" || name == "count" || name == "index" ||
                name == "rindex")
                return t_int();
            if (name == "startswith" || name == "endswith")
                return t_bool();
            if (name == "isdigit" || name == "isalpha" || name == "isspace" ||
                name == "isupper" || name == "islower" || name == "isnumeric" ||
                name == "isalnum" || name == "isdecimal" || name == "isidentifier" ||
                name == "isprintable" || name == "istitle")
                return t_bool();
            if (name == "format") return t_str();
            if (name == "zfill" || name == "ljust" || name == "rjust" || name == "center" ||
                name == "expandtabs" || name == "translate" || name == "format_map")
                return t_str();
            if (name == "partition" || name == "rpartition")
                return mk_tuple({t_str(), t_str(), t_str()});
            if (name == "encode") return t_unknown();
        }

        if (recv->kind == TyKind::File) {
            if (name == "read" || name == "readline") {
                argc(0, 1);
                return t_str();
            }
            if (name == "readlines") {
                argc(0, 1);
                return mk_list(t_str());
            }
            if (name == "write") {
                argc(1, 1);
                if (!a.empty() && !is_unknown(a[0]) && (!a[0] || a[0]->kind != TyKind::Str))
                    err(c.line, c.col, "write() expects a string argument");
                return t_int();
            }
            if (name == "writelines") {
                argc(1, 1);
                if (!a.empty() && !is_unknown(a[0]) &&
                    (!a[0] || (a[0]->kind != TyKind::List && a[0]->kind != TyKind::Tuple)))
                    err(c.line, c.col, "writelines() expects a list of strings");
                return t_none();
            }
            if (name == "seek") {
                argc(1, 2);
                return t_int();
            }
            if (name == "tell" || name == "fileno") {
                argc(0, 0);
                return t_int();
            }
            if (name == "truncate") {
                argc(0, 1);
                return t_int();
            }
            if (name == "isatty" || name == "readable" || name == "writable" ||
                name == "seekable") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "close" || name == "flush" || name == "__exit__") {
                argc(0, name == "__exit__" ? 3 : 0);
                return t_none();
            }
            if (name == "__enter__") {
                argc(0, 0);
                return recv;
            }
        }

        if (recv->kind == TyKind::Dict) {
            auto kt = recv->sub.size() > 0 ? recv->sub[0] : t_unknown();
            auto vt = recv->sub.size() > 1 ? recv->sub[1] : t_unknown();
            if (name == "get") {
                argc(1, 2);
                if (!a.empty() && !assignable(kt, a[0]))
                    err(c.line, c.col,
                        std::format("dict key type is '{}', got '{}'", type_name(kt),
                                    type_name(a[0])));
                return vt;
            }
            if (name == "keys") { argc(0, 0); return mk_list(kt); }
            if (name == "values") { argc(0, 0); return mk_list(vt); }
            if (name == "items") { argc(0, 0); return mk_list(mk_tuple({kt, vt})); }
            if (name == "pop") { argc(1, 2); return vt; }
            if (name == "setdefault") { argc(1, 2); return vt; }
            if (name == "update") { argc(0, 1); return t_none(); }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy") { argc(0, 0); return recv; }
            if (name == "popitem") { return mk_tuple({kt, vt}); }
        }

        if (recv->kind == TyKind::Tuple) {
            if (name == "count") { argc(1, 1); return t_int(); }
            if (name == "index") { argc(1, 3); return t_int(); }
        }

        if (recv->kind == TyKind::Set) {
            auto e = elem();
            if (name == "add") { argc(1, 1); return t_none(); }
            if (name == "remove" || name == "discard") { argc(1, 1); return t_none(); }
            if (name == "pop") { argc(0, 0); return e; }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy" || name == "union" || name == "intersection" ||
                name == "difference" || name == "symmetric_difference")
                return recv;
            if (name == "update" || name == "difference_update" ||
                name == "intersection_update" || name == "symmetric_difference_update")
                return t_none();
            if (name == "issubset" || name == "issuperset" || name == "isdisjoint")
                return t_bool();
        }

        if (recv->kind == TyKind::Int || recv->kind == TyKind::Float || recv->kind == TyKind::Bool) {
            if (name == "bit_length") { argc(0, 0); return t_int(); }
            if (name == "as_integer_ratio") { argc(0, 0); return mk_tuple({t_int(), t_int()}); }
            if (name == "is_integer") { argc(0, 0); return t_bool(); }
            if (name == "to_bytes") { argc(1, 3); return t_unknown(); }
            if (name == "conjugate" || name == "real" || name == "imag" || name == "numerator" ||
                name == "denominator")
                return recv;
            // hex/fromhex/from_bytes：结果类型（str/bytes）尚未建模
            if (name == "hex" || name == "fromhex" || name == "from_bytes") return t_unknown();
        }

        err(c.line, c.col,
            std::format("'{}' object has no method '{}'", type_name(recv), name));
        return t_unknown();
    }


}  // namespace py23
