// typecheck_call.cpp —— 调用入口、参数检查与 builtin 判定
#include "typecheck.h"

#include "module.h"

namespace pyadamas {


TypePtr TypeChecker::call(Call& c) {
        // 按被调表达式形态分流：名字、属性、任意值。
        if (auto* n = dynamic_cast<Name*>(c.func.get())) return call_name(*n, c);
        if (auto* a = dynamic_cast<Attribute*>(c.func.get())) return call_attr(*a, c);
        return call_value(c);
    }

TypePtr TypeChecker::check_args(const FuncInfoPtr& fn, Call& c) {
        if (fn && fn->name.starts_with("random.")) {
            if (auto r = random_call(fn->name.substr(7), c)) return *r;
        }
        if (fn && fn->name.starts_with("itertools.")) {
            if (auto r = itertools_call(fn->name.substr(10), c)) return *r;
        }
        if (fn && fn->name.starts_with("operator.")) {
            if (auto r = operator_call(fn->name.substr(9), c)) return *r;
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

        // cimport 的 ptr/ptr[T]：除原始地址外，还接受底层布局兼容的缓冲区。
        // 返回 0 表示不是缓冲区（交给 assignable），1 兼容，-1 元素不匹配。
        auto cptr_buffer = [](CKind elem, bool opaque, TypePtr ty) -> int {
            if (!ty) return 0;
            // addr(struct) 的机器表示就是结构体 payload 的地址，ptr/ptr[T]
            // 都能直接接收；C 结构体指针字段/返回值是 Optional[StructPtr]。
            if (ty->kind == TyKind::StructPtr) return 1;
            if (ty->kind == TyKind::Optional && !ty->sub.empty() &&
                ty->sub[0]->kind == TyKind::StructPtr)
                return 1;
            if (ty->kind == TyKind::List) {
                if (opaque) return -1;
                if (ty->sub.empty() || !ty->sub[0]) return 0;
                TyKind e = ty->sub[0]->kind;
                if (elem == CKind::None)
                    return (e == TyKind::Float || e == TyKind::Int) ? 1 : -1;
                if (elem == CKind::Float) return e == TyKind::Float ? 1 : -1;
                if (elem == CKind::Int) return e == TyKind::Int ? 1 : -1;
                return -1;
            }
            if (ty->kind == TyKind::Bytes) return (opaque || elem != CKind::None) ? -1 : 1;
            return 0;
        };

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
            if (p.cptr) {
                int r = cptr_buffer(p.cptr_elem, p.cptr_opaque, ty);
                if (r == 1) return;
                if (r == -1) {
                    std::string want;
                    if (p.cptr_opaque)
                        want = "an opaque C pointer";
                    else if (p.cptr_elem == CKind::Float)
                        want = "list[float] for 'ptr[float]'";
                    else if (p.cptr_elem == CKind::Int)
                        want = "list[int] for 'ptr[int]'";
                    else
                        want = "list[float], list[int] or bytes for 'ptr'";
                    err(line, col,
                        std::format("argument '{}' of '{}' expects {}, got '{}'", p.name,
                                    fn->name, want, type_name(ty)));
                    return;
                }
            }
            if (!p.ty || p.ty->kind == TyKind::Unknown) {
                // 实参是 None 时不回填：None 是「没传 / 清空」的哨兵，不是
                // 「这个形参就是 NoneType」。回填了会把 copy._reconstruct(x,
                // None, *rv) 这种调用点的 memo 钉成 None，随后 memo[k]=v
                // 就报「NoneType 不支持下标赋值」。留 unknown，等真正带
                // 类型的调用点（或干脆一直 unknown）来决定。
                if (ty && ty->kind != TyKind::Unknown && ty->kind != TyKind::None) {
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
            // 基类 __init__ 显式接收子类 self（Base.__init__(self,...)）合法。
            if (p.name == "self" && ty && ty->kind == TyKind::Class) return;
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
            if (fn->name.rfind("__pyadamas$gen_", 0) == 0) genexpr_call_sites_[&c] = fn;
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
            // subprocess 相关：PyAdamas 没有模块限定异常名，按内置名捕获
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
            // 装饰器/元编程名字：staticmethod / classmethod / property /
            // lru_cache / wraps 已实现；abstractmethod / dataclass 等其余名字
            // 只做解析，避免真实代码在第一行就报未定义、淹没真正的类型错误
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
            // 内部 intrinsics：stdlib/string.py 的 capwords 走单遍 C 实现
            "__pa_capwords",
            // 内部 intrinsics：stdlib/hashlib.py 的 sha256 走 C 实现
            "__pa_sha256",
            "__pa_sha256_hex",
            // 内部 intrinsics：stdlib/base64.py 的 b64 编解码走 C 实现
            "__pa_b64encode",
            "__pa_b64decode",
            // site 模块注入的内置名（交互式退出），真实代码里也会用到
            "quit", "exit", "copyright", "credits", "license",
            // 字节长度出口：len 返回码点数，len_bytes 返回 UTF-8 字节数（O(1)）
            "len_bytes",
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


}  // namespace pyadamas
