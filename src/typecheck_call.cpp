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
                    if (mod->builtin && mod->name == "itertools") {
                        if (auto r = itertools_call(a->attr, c)) return *r;
                    }
                    if (mod->builtin && mod->name == "collections") {
                        if (auto r = collections_call(a->attr, c)) return *r;
                    }
                    if (mod->builtin && mod->name == "json") {
                        if (auto r = json_call(a->attr, c)) return *r;
                    }
                    if (mod->builtin && mod->name == "functools") {
                        if (auto r = functools_call(a->attr, c)) return *r;
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
        if (fn && fn->name.starts_with("random.")) {
            if (auto r = random_call(fn->name.substr(7), c)) return *r;
        }
        if (fn && fn->name.starts_with("itertools.")) {
            if (auto r = itertools_call(fn->name.substr(10), c)) return *r;
        }
        if (fn && fn->name.starts_with("collections.")) {
            if (auto r = collections_call(fn->name.substr(12), c)) return *r;
        }
        if (fn && fn->name.starts_with("json.")) {
            if (auto r = json_call(fn->name.substr(5), c)) return *r;
        }
        if (fn && fn->name.starts_with("functools.")) {
            if (auto r = functools_call(fn->name.substr(10), c)) return *r;
        }
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

        // 未标注的形参用实参类型回填（单态化）。回填后函数体要按新签名
        // 重查；同一形参被多种类型调用则退回 unknown，不误杀 Python 的
        // 多态用法。
        auto bind_param = [&](Param& p, size_t idx, TypePtr ty, int line, int col) {
            // 方法调用走的是去掉 self 的 call_sig，回填要同步回含 self 的 sig，
            // 否则方法体重查时仍是旧类型。
            auto sync = [&]() {
                if (!fn->outer) return;
                size_t k = idx + fn->outer_off;
                if (k >= fn->outer->pos.size()) return;
                Param& sp = fn->outer->pos[k];
                sp.ty = p.ty;
                sp.inferred = p.inferred;
                sp.multi = p.multi;
                fn->outer->sig_dirty = true;
            };
            if (p.multi) return;
            if (!p.ty || p.ty->kind == TyKind::Unknown) {
                if (ty && ty->kind != TyKind::Unknown) {
                    p.ty = ty;
                    p.inferred = true;
                    fn->sig_dirty = true;
                    sync();
                }
                return;
            }
            if (assignable(p.ty, ty)) return;
            if (p.inferred) {
                p.ty = t_unknown();
                p.inferred = false;
                p.multi = true;
                fn->sig_dirty = true;
                sync();
                return;
            }
            err(line, col,
                std::format("argument '{}' of '{}' expects '{}', got '{}'", p.name, fn->name,
                            type_name(p.ty), type_name(ty)));
        };

        for (size_t i = 0; i < argtys.size(); ++i) {
            if (i < npos) {
                filled[i] = true;
                bind_param(fn->pos[i], i, argtys[i], c.args[i]->line, c.args[i]->col);
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
                bind_param(*pit, idx, ty, c.line, c.col);
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
                bind_param(*kit, idx, ty, c.line, c.col);
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

        if (fn->ret) {
            // 合成生成器函数的返回类型跟着形参回填走：第一次检查时形参
            // 还是 unknown，重查后才是 Generator[T]。调用点缓存的表达式
            // 类型要在重查结束后刷新一次，降级层才能按 T 选迭代路径。
            if (fn->name.rfind("__py23$gen_", 0) == 0) genexpr_call_sites_[&c] = fn;
            return fn->ret;
        }
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
            "QueueEmpty", "QueueFull",
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
            // subprocess 相关：py23 没有模块限定异常名，按内置名捕获
            "SubprocessError", "TimeoutExpired", "CalledProcessError",
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
            member != "wait_readable" && member != "wait_writable" &&
            member != "Event" && member != "Lock" && member != "Queue" &&
            member != "wait_for" && member != "timeout" && member != "TaskGroup" &&
            member != "Semaphore")
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

        if (member == "Event") {
            if (!a.empty()) err(c.line, c.col, "asyncio.Event() takes no arguments");
            return mk_async_event();
        }
        if (member == "Lock") {
            if (!a.empty()) err(c.line, c.col, "asyncio.Lock() takes no arguments");
            return mk_async_lock();
        }
        if (member == "Queue") {
            if (a.size() > 1)
                err(c.line, c.col, "asyncio.Queue() takes at most one argument");
            if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Int)
                err(c.line, c.col,
                    std::format("asyncio.Queue() maxsize must be an int, got '{}'",
                                type_name(a[0])));
            return mk_async_queue(t_unknown());
        }

        if (member == "Semaphore") {
            if (a.size() > 1)
                err(c.line, c.col, "asyncio.Semaphore() takes at most one argument");
            if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Int)
                err(c.line, c.col,
                    std::format("asyncio.Semaphore() value must be an int, got '{}'",
                                type_name(a[0])));
            return mk_async_semaphore();
        }

        if (member == "TaskGroup") {
            if (!a.empty()) err(c.line, c.col, "asyncio.TaskGroup() takes no arguments");
            return mk_async_taskgroup();
        }

        if (member == "timeout") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.timeout() expects 1 argument, got {}", a.size()));
            } else if (!is_unknown(a[0]) && !is_num(a[0])) {
                err(c.line, c.col,
                    std::format("asyncio.timeout() delay must be a number, got '{}'",
                                type_name(a[0])));
            }
            return mk_async_timeout();
        }

        if (member == "wait_for") {
            if (a.size() != 2) {
                err(c.line, c.col,
                    std::format("asyncio.wait_for() expects 2 arguments, got {}", a.size()));
                return t_unknown();
            }
            if (!is_unknown(a[1]) && !is_num(a[1]))
                err(c.line, c.col,
                    std::format("asyncio.wait_for() timeout must be a number, got '{}'",
                                type_name(a[1])));
            TypePtr res = t_unknown();
            if (a[0] && (a[0]->kind == TyKind::Coroutine || a[0]->kind == TyKind::Task ||
                         a[0]->kind == TyKind::Future) &&
                !a[0]->sub.empty())
                res = a[0]->sub[0];
            return mk_coroutine(res);
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


std::optional<TypePtr> TypeChecker::random_call(const std::string& member, Call& c) {
        if (member == "sample" || member == "choices") {
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            TypePtr kt = nullptr;
            for (auto& kw : c.keywords) {
                if (kw.arg == "k") {
                    kt = expr(*kw.value);
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("random.{}() got an unexpected keyword argument '{}'",
                                    member, kw.arg));
                }
            }
            // sample 用位置参数 (seq, k)；choices 跟 CPython 一样用 k= 关键字。
            size_t want = member == "sample" ? 2 : 1;
            if (a.size() != want) {
                err(c.line, c.col,
                    std::format("random.{}() takes {} positional argument{}, got {}", member, want,
                                want == 1 ? "" : "s", a.size()));
                return mk_list(t_unknown());
            }
            TypePtr kty = member == "sample" ? (a.size() > 1 ? a[1] : nullptr) : kt;
            if (kty && !is_unknown(kty) && kty->kind != TyKind::Int)
                err(c.line, c.col, std::format("random.{}() size must be an int", member));
            TypePtr et = t_unknown();
            TypePtr st = a[0];
            if (st && st->kind == TyKind::List) {
                if (!st->sub.empty()) et = st->sub[0];
            } else if (st && st->kind == TyKind::Tuple) {
                if (!st->sub.empty()) {
                    et = st->sub[0];
                    for (size_t i = 1; i < st->sub.size(); ++i) et = join(et, st->sub[i]);
                }
            } else if (st && st->kind == TyKind::Str) {
                et = t_str();
            } else if (st && st->kind != TyKind::Unknown) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("random.{}() expects a sequence, got '{}'", member,
                                type_name(st)));
            }
            return mk_list(et);
        }
        if (member != "choice" && member != "shuffle") return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("random.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        if (c.args.size() != 1) {
            for (auto& arg : c.args) expr(*arg);
            err(c.line, c.col,
                std::format("random.{}() expects 1 argument, got {}", member, c.args.size()));
            return t_unknown();
        }
        TypePtr st = expr(*c.args[0]);
        if (member == "shuffle") {
            if (st && st->kind != TyKind::List && st->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("random.shuffle() expects a list, got '{}'", type_name(st)));
            return t_none();
        }
        if (st && st->kind == TyKind::List)
            return st->sub.empty() ? t_unknown() : st->sub[0];
        if (st && st->kind == TyKind::Tuple) {
            if (st->sub.empty()) return t_unknown();
            TypePtr acc = st->sub[0];
            for (size_t i = 1; i < st->sub.size(); ++i) acc = join(acc, st->sub[i]);
            return acc;
        }
        if (st && st->kind == TyKind::Str) return t_str();
        if (st && st->kind != TyKind::Unknown)
            err(c.args[0]->line, c.args[0]->col,
                std::format("random.choice() expects a sequence, got '{}'", type_name(st)));
        return t_unknown();
}

std::optional<TypePtr> TypeChecker::itertools_call(const std::string& member, Call& c) {
        if (member != "chain" && member != "islice" && member != "repeat")
            return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("itertools.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        auto is_list = [](const TypePtr& t) { return t && t->kind == TyKind::List; };
        auto elem_of = [](const TypePtr& t) {
            return t && t->kind == TyKind::List && !t->sub.empty() ? t->sub[0] : t_unknown();
        };
        if (member == "chain") {
            if (a.size() != 2) {
                err(c.line, c.col,
                    std::format("itertools.chain() expects 2 arguments, got {}", a.size()));
                return mk_list(t_unknown());
            }
            for (size_t i = 0; i < 2; ++i)
                if (!is_list(a[i]) && !is_unknown(a[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("itertools.chain() expects lists, got '{}'",
                                    type_name(a[i])));
            TypePtr ea = elem_of(a[0]);
            TypePtr eb = elem_of(a[1]);
            return mk_list(join(ea, eb));
        }
        if (member == "islice") {
            if (a.size() != 2 && a.size() != 3) {
                err(c.line, c.col,
                    std::format("itertools.islice() expects 2 or 3 arguments, got {}", a.size()));
                return mk_list(t_unknown());
            }
            if (!is_list(a[0]) && !is_unknown(a[0]))
                err(c.args[0]->line, c.args[0]->col,
                    std::format("itertools.islice() expects a list, got '{}'",
                                type_name(a[0])));
            for (size_t i = 1; i < a.size(); ++i)
                if (a[i] && !is_unknown(a[i]) && a[i]->kind != TyKind::Int)
                    err(c.args[i]->line, c.args[i]->col,
                        "itertools.islice() indices must be int");
            return is_list(a[0]) ? a[0] : mk_list(t_unknown());
        }
        // repeat(x, n)
        if (a.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.repeat() expects 2 arguments, got {}", a.size()));
            return mk_list(t_unknown());
        }
        if (a[1] && !is_unknown(a[1]) && a[1]->kind != TyKind::Int)
            err(c.args[1]->line, c.args[1]->col, "itertools.repeat() count must be int");
        return mk_list(a[0] ? a[0] : t_unknown());
}

std::optional<TypePtr> TypeChecker::collections_call(const std::string& member, Call& c) {
        if (member != "Counter") return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col, "collections.Counter() does not take keyword arguments");
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        if (a.size() != 1) {
            err(c.line, c.col,
                std::format("collections.Counter() expects 1 argument, got {}", a.size()));
            return mk_dict(t_unknown(), t_int());
        }
        TypePtr et = t_unknown();
        if (a[0] && a[0]->kind == TyKind::List && !a[0]->sub.empty()) et = a[0]->sub[0];
        else if (a[0] && !is_unknown(a[0]))
            err(c.args[0]->line, c.args[0]->col,
                std::format("collections.Counter() expects a list, got '{}'",
                            type_name(a[0])));
        return mk_dict(et, t_int());
}

std::optional<TypePtr> TypeChecker::functools_call(const std::string& member, Call& c) {
        if (member == "reduce") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "functools.reduce() does not take keyword arguments");
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            if (a.size() < 2 || a.size() > 3) {
                err(c.line, c.col,
                    std::format("functools.reduce() expects 2 or 3 arguments, got {}",
                                a.size()));
                return t_unknown();
            }
            if (a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Func &&
                a[0]->kind != TyKind::BoundMethod) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("functools.reduce() expects a function, got '{}'",
                                type_name(a[0])));
            }
            TypePtr elem = iter_elem(a[1], c.args[1]->line, c.args[1]->col);
            // reduce 不直接调用 f，形参类型只能在这里回填：否则 lambda /
            // 具名函数的形参一直是 unknown，float 元素会按整数寄存器传参。
            // 同一个函数被不同类型的 reduce 用过就退回 unknown（和普通
            // 调用点的单态化规则一致），否则会按错误的值域编译。
            if (a[0] && a[0]->kind == TyKind::Func && a[0]->fn && elem && !is_unknown(elem)) {
                FuncInfoPtr fi = a[0]->fn;
                bool dirty = false;
                for (size_t i = 0; i < fi->pos.size() && i < 2; ++i) {
                    Param& p = fi->pos[i];
                    if (p.multi) continue;
                    if (is_unknown(p.ty)) {
                        p.ty = elem;
                        p.inferred = true;
                        dirty = true;
                    } else if (!assignable(p.ty, elem) && p.inferred) {
                        p.ty = t_unknown();
                        p.inferred = false;
                        p.multi = true;
                        dirty = true;
                    }
                }
                if (dirty) fi->sig_dirty = true;
            }
            // 有初始值时结果类型是元素类型与初始值的公共上界。
            if (a.size() == 3) return join(elem, a[2]);
            return elem;
        }
        if (member == "wraps") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "functools.wraps() does not take keyword arguments");
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("functools.wraps() expects 1 argument, got {}", a.size()));
            }
            // 只当装饰器用：返回 unknown，装饰器那一步会因为「看不出来」
            // 保留被装饰函数原有的签名——正是 wraps 的语义。
            return t_unknown();
        }
        if (member == "partial" || member == "lru_cache") {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            err(c.line, c.col, std::format("functools.{}() is not supported yet", member));
            return t_unknown();
        }
        return std::nullopt;
}

std::optional<TypePtr> TypeChecker::json_call(const std::string& member, Call& c) {
        if (member != "dumps" && member != "loads") return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col, std::format("json.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        if (member == "dumps") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("json.dumps() expects 1 argument, got {}", a.size()));
            }
            return t_str();
        }
        // loads(text, sample)：样例值只提供静态类型，运行时不求值。
        if (a.size() != 2) {
            err(c.line, c.col,
                std::format("json.loads() expects 2 arguments (text, sample), got {}",
                            a.size()));
            return t_unknown();
        }
        if (a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str)
            err(c.args[0]->line, c.args[0]->col,
                std::format("json.loads() expects a str, got '{}'", type_name(a[0])));
        TypePtr sample = a[1];
        if (!sample || sample->kind == TyKind::Unknown) {
            err(c.args[1]->line, c.args[1]->col,
                "json.loads() needs a sample value with a static type");
            return t_unknown();
        }
        return sample;
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
                    "setdefault", "clear", "copy", "popitem", "most_common",
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
            case TyKind::Random: {
                static const std::unordered_set<std::string> ms = {
                    "random", "seed", "randint", "uniform", "choice", "shuffle",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Regex: {
                static const std::unordered_set<std::string> ms = {
                    "search", "match", "fullmatch", "findall", "sub", "split",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Match: {
                static const std::unordered_set<std::string> ms = {
                    "group", "groups", "start", "end", "span",
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
            (recv->sub[0]->kind == TyKind::Class || recv->sub[0]->kind == TyKind::Match))
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

        if (recv->kind == TyKind::Random) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("random.Random.{}() does not take keyword arguments", name));
            if (name == "random") {
                argc(0, 0);
                return t_float();
            }
            if (name == "seed") {
                argc(0, 1);
                if (!a.empty() && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Int)
                    err(c.line, c.col, "seed() expects an int");
                return t_none();
            }
            if (name == "randint") {
                argc(2, 2);
                for (auto& t : a)
                    if (t && !is_unknown(t) && t->kind != TyKind::Int)
                        err(c.line, c.col, "randint() expects int arguments");
                return t_int();
            }
            if (name == "uniform") {
                argc(2, 2);
                for (auto& t : a)
                    if (t && !is_unknown(t) && !is_num(t))
                        err(c.line, c.col, "uniform() expects number arguments");
                return t_float();
            }
            if (name == "choice") {
                argc(1, 1);
                TypePtr st = a.empty() ? nullptr : a[0];
                if (st && st->kind == TyKind::List)
                    return st->sub.empty() ? t_unknown() : st->sub[0];
                if (st && st->kind == TyKind::Tuple) {
                    if (st->sub.empty()) return t_unknown();
                    TypePtr acc = st->sub[0];
                    for (size_t i = 1; i < st->sub.size(); ++i) acc = join(acc, st->sub[i]);
                    return acc;
                }
                if (st && st->kind == TyKind::Str) return t_str();
                if (st && !is_unknown(st))
                    err(c.line, c.col, "choice() expects a sequence");
                return t_unknown();
            }
            if (name == "shuffle") {
                argc(1, 1);
                if (!a.empty() && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::List)
                    err(c.line, c.col, "shuffle() expects a list");
                return t_none();
            }
            return t_unknown();
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

        if (recv->kind == TyKind::AsyncEvent) {
            if (name == "wait") {
                argc(0, 0);
                return mk_future(t_bool());
            }
            if (name == "set") {
                argc(0, 0);
                return t_none();
            }
            if (name == "is_set") {
                argc(0, 0);
                return t_bool();
            }
            err(c.line, c.col, std::format("'asyncio.Event' has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncLock) {
            if (name == "acquire") {
                argc(0, 0);
                return mk_future(t_bool());
            }
            if (name == "release") {
                argc(0, 0);
                return t_none();
            }
            if (name == "locked") {
                argc(0, 0);
                return t_bool();
            }
            err(c.line, c.col, std::format("'asyncio.Lock' has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncQueue) {
            if (name == "put") {
                argc(1, 1);
                return mk_future(t_none());
            }
            if (name == "get") {
                argc(0, 0);
                return mk_future(elem());
            }
            if (name == "qsize") {
                argc(0, 0);
                return t_int();
            }
            if (name == "empty") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "full") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "join") {
                argc(0, 0);
                return mk_future(t_none());
            }
            if (name == "task_done") {
                argc(0, 0);
                return t_none();
            }
            if (name == "put_nowait") {
                argc(1, 1);
                return t_none();
            }
            if (name == "get_nowait") {
                argc(0, 0);
                return elem();
            }
            err(c.line, c.col, std::format("'asyncio.Queue' has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncTimeout) {
            if (name == "expired") {
                argc(0, 0);
                return t_bool();
            }
            err(c.line, c.col, std::format("'asyncio.Timeout' has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncSemaphore) {
            if (name == "acquire") {
                argc(0, 0);
                return mk_future(t_bool());
            }
            if (name == "release") {
                argc(0, 0);
                return t_none();
            }
            if (name == "locked") {
                argc(0, 0);
                return t_bool();
            }
            err(c.line, c.col,
                std::format("'asyncio.Semaphore' has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::AsyncTaskGroup) {
            if (name == "create_task") {
                argc(1, 1);
                if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Coroutine)
                    err(c.line, c.col,
                        std::format("TaskGroup.create_task() expects a coroutine, got '{}'",
                                    type_name(a[0])));
                return mk_task(t_unknown());
            }
            err(c.line, c.col,
                std::format("'asyncio.TaskGroup' has no method '{}'", name));
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

        if (recv->kind == TyKind::Bytes) {
            if (!c.keywords.empty())
                err(c.line, c.col, std::format("bytes.{}() does not take keyword arguments", name));
            if (name == "decode") {
                argc(0, 1);
                return t_str();
            }
            if (name == "upper" || name == "lower" || name == "strip" || name == "lstrip" ||
                name == "rstrip") {
                argc(0, 1);
                return t_bytes();
            }
            if (name == "startswith" || name == "endswith") {
                argc(1, 1);
                return t_bool();
            }
            if (name == "find" || name == "rfind") {
                argc(1, 3);
                return t_int();
            }
            if (name == "count") {
                argc(1, 1);
                return t_int();
            }
            if (name == "join") {
                argc(1, 1);
                return t_bytes();
            }
            if (name == "replace") {
                argc(2, 3);
                return t_bytes();
            }
            if (name == "split") {
                argc(0, 2);
                return mk_list(t_bytes());
            }
            err(c.line, c.col, std::format("bytes has no method '{}'", name));
            return t_unknown();
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
            if (name == "encode") return t_bytes();
        }

        if (recv->kind == TyKind::DateTime) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("datetime.{}() does not take keyword arguments", name));
            if (name == "isoformat") {
                argc(0, 0);
                return t_str();
            }
            if (name == "strftime") {
                argc(1, 1);
                return t_str();
            }
            if (name == "weekday") {
                argc(0, 0);
                return t_int();
            }
            err(c.line, c.col, std::format("datetime has no method '{}'", name));
            return t_unknown();
        }
        if (recv->kind == TyKind::Path) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("pathlib.Path.{}() does not take keyword arguments", name));
            if (name == "exists" || name == "is_file" || name == "is_dir") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "read_text") {
                argc(0, 0);
                return t_str();
            }
            if (name == "write_text") {
                argc(1, 1);
                return t_int();
            }
            err(c.line, c.col, std::format("pathlib.Path has no method '{}'", name));
            return t_unknown();
        }
        if (recv->kind == TyKind::Regex) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("re.Pattern.{}() does not take keyword arguments", name));
            if (name == "search" || name == "match" || name == "fullmatch") {
                argc(1, 1);
                if (!a.empty() && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str)
                    err(c.line, c.col, "re.Pattern method expects a str");
                return mk_optional(t_match());
            }
            if (name == "findall") {
                argc(1, 1);
                return mk_list(t_str());
            }
            if (name == "finditer") {
                argc(1, 1);
                if (!a.empty() && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str)
                    err(c.line, c.col, "re.Pattern.finditer() expects a str");
                return mk_list(t_match());
            }
            if (name == "sub") {
                argc(2, 3);
                if (a.size() >= 2 && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str &&
                    a[0]->kind != TyKind::Func)
                    err(c.line, c.col, "sub() replacement must be a str or a function");
                if (a.size() >= 2 && a[1] && !is_unknown(a[1]) && a[1]->kind != TyKind::Str)
                    err(c.line, c.col, "sub() text must be a str");
                // 回调形参的类型由这里定：re.sub 的 repl 一定吃一个 Match。
                if (a.size() >= 2 && a[0] && a[0]->kind == TyKind::Func && a[0]->fn &&
                    a[0]->fn->pos.size() == 1) {
                    Param& cp = a[0]->fn->pos[0];
                    if (!cp.ty || cp.ty->kind == TyKind::Unknown) {
                        cp.ty = t_match();
                        cp.inferred = true;
                        a[0]->fn->sig_dirty = true;
                    }
                }
                return t_str();
            }
            if (name == "split") {
                argc(1, 2);
                return mk_list(t_str());
            }
            err(c.line, c.col, std::format("re.Pattern has no method '{}'", name));
            return t_unknown();
        }

        if (recv->kind == TyKind::Match) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("re.Match.{}() does not take keyword arguments", name));
            if (name == "group") {
                if (c.args.size() > 1) return mk_tuple_rest(t_str());
                return t_str();
            }
            if (name == "groups") {
                argc(0, 0);
                return mk_list(t_str());
            }
            if (name == "groupdict") {
                argc(0, 0);
                return mk_dict(t_str(), t_str());
            }
            if (name == "start" || name == "end") {
                argc(0, 1);
                return t_int();
            }
            if (name == "span") {
                argc(0, 1);
                return mk_tuple({t_int(), t_int()});
            }
            if (name == "expand") {
                argc(1, 1);
                return t_str();
            }
            err(c.line, c.col, std::format("re.Match has no method '{}'", name));
            return t_unknown();
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
                // 只有一个参数时可能缺失，返回类型是 Optional[值类型]
                return a.size() == 1 ? mk_optional(vt) : vt;
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
            if (name == "most_common") {
                // py23 扩展：Counter 返回普通 dict，计数排序走这个方法。
                argc(0, 1);
                if (a.size() == 1 && a[0] && !is_unknown(a[0]) &&
                    a[0]->kind != TyKind::Int)
                    err(c.line, c.col, "most_common() count must be int");
                if (vt && !is_unknown(vt) && vt->kind != TyKind::Int)
                    err(c.line, c.col, "most_common() expects a dict with int values");
                return mk_list(mk_tuple({kt, t_int()}));
            }
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
