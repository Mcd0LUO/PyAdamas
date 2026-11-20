// typecheck_call.cpp —— 调用入口、参数检查与 builtin 判定
#include "typecheck.h"

#include "module.h"

namespace pyadamas {
namespace {

TypePtr literal_type(const LiteralValue& lit) {
    switch (lit.kind) {
        case LiteralValue::Kind::None: return t_none();
        case LiteralValue::Kind::Bool: return t_bool();
        case LiteralValue::Kind::Int: return t_int();
        case LiteralValue::Kind::Float: return t_float();
        case LiteralValue::Kind::Str: return t_str();
        case LiteralValue::Kind::Bytes: return t_bytes();
    }
    return t_unknown();
}

}  // namespace

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
        if (fn && fn->c_lazy)
            return check_c_lazy(c, fn->c_symbol.empty() ? fn->name : fn->c_symbol);
        // 实参和关键字实参都只求值一次。之前关键字值在循环里被求了两次，
        // 一次的类型错误会重复报两遍。
        struct PosArg {
            TypePtr ty;
            Expr* source;
            bool dynamic_star = false;
        };
        std::vector<PosArg> positional;
        positional.reserve(c.args.size());
        bool has_dynamic_star = false;
        for (auto& arg : c.args) {
            if (arg->kind != ExprKind::Starred) {
                positional.push_back({expr(*arg), arg.get()});
                continue;
            }
            auto t = expr(*static_cast<Starred&>(*arg).value);
            if (t && t->kind == TyKind::Tuple && !t->variadic) {
                // Lowering expands a fixed tuple slot by slot. A joined
                // element type loses both the arity and numeric ABI facts.
                for (auto& elem : t->sub) positional.push_back({elem, arg.get()});
            } else {
                has_dynamic_star = true;
                positional.push_back({iter_elem(t, arg->line, arg->col), arg.get(), true});
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

        if (!has_dynamic_star && positional.size() > npos && !fn->varargs) {
            err(c.line, c.col,
                std::format("function '{}' takes {} positional argument{} but {} were given",
                            fn->name, npos, npos == 1 ? "" : "s", positional.size()));
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
        auto bind_param = [&](Param& p, size_t idx, TypePtr ty, int line, int col,
                              bool is_default = false, Expr* arg_expr = nullptr) {
            // Bound and unbound calls to a method must use the same evidence.
            Param* source = nullptr;
            if (fn->outer) {
                auto& params = p.kwonly ? fn->outer->kwonly : fn->outer->pos;
                const size_t k = p.kwonly ? idx : idx + fn->outer_off;
                if (k < params.size() && params[k].name == p.name) source = &params[k];
            }
            auto copy_inference = [](Param& to, const Param& from) {
                to.ty = from.ty;
                to.inferred = from.inferred;
                to.multi = from.multi;
                to.omitted_none = from.omitted_none;
            };
            if (source) copy_inference(p, *source);
            auto sync = [&]() {
                if (!source) return;
                copy_inference(*source, p);
                fn->outer->sig_dirty = true;
            };
            auto mixed_float = [&]() {
                err(line, col,
                    std::format("argument '{}' of '{}' is used with both float and "
                                "non-float values; annotate the parameter type",
                                p.name, fn->name));
            };
            // 显式 object/Any 标注：故意保持 unknown 域，不回填、不报
            // float 混用——调用点对 float 实参装箱（AnyFromF64），callee
            // 按 unknown 动态分派。这是 argparse.add_argument(default=...)
            // 这类异构 API 的前提。
            if (p.ann_unknown && !p.inferred) return;
            if (is_default && ty && ty->kind == TyKind::None) {
                if (p.ty && p.ty->kind == TyKind::Float) mixed_float();
                if (is_unknown(p.ty) || p.inferred) {
                    p.omitted_none = true;
                    if (source) source->omitted_none = true;
                }
                return;
            }
            if (ty && ty->kind == TyKind::Float && (p.multi || p.omitted_none)) {
                mixed_float();
                return;
            }
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
            // **标注里含类型洞（裸 `list` / `set` / `dict` / `tuple`）时，
            // 也走调用点回填，而不是当成「已标注」直接比对。**
            //
            // 裸 `list` 解析成 `list[fresh_hole]`，kind 是 List 不是 Unknown，
            // 原来会跳过下面这个回填分支、直接落到 assignable(p.ty, ty) ——
            // assignable 对 `list[hole]` vs `list[int]` 返回 true（unknown 与任何
            // 类型兼容），但**不回填**，于是洞永远是空的，参数停在 list[unknown]。
            //
            // 结果：`def f(xs: list)` 比 `def f(xs)` 还差 —— 后者 p.ty 是 Unknown，
            // 能走回填分支拿到 list[int]。这是结构性缺陷。
            //
            // 修法：含洞时先递归填洞，再继续原有的 assignable 检查（填完就
            // 等价于用户写了 `list[int]`，后续逻辑不变）。
            const bool ty_has_hole = p.ty && p.ty->kind != TyKind::Unknown &&
                                     ty_contains_hole(p.ty, 0);
            if (ty_has_hole && ty && ty->kind != TyKind::Unknown) {
                fill_holes_recursive(p.ty, ty, 0);
                fn->sig_dirty = true;
                sync();
            }
            // 实参是带洞容器、形参具体（f({}) 传 dict[str,str]）：把形参
            // 类型填进实参的洞，空字面量才按正确的键/值种类建表。
            if (ty && p.ty && ty_contains_hole(ty, 0) && !ty_contains_hole(p.ty, 0))
                fill_holes_recursive(ty, p.ty, 0);
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
            // Inferred float is not permission to convert an integer: float
            // and non-float calls need distinct representations. Check both
            // orders before assignable(), which allows int -> float.
            if (p.inferred && !is_unknown(ty) &&
                ((p.ty->kind == TyKind::Float) != (ty->kind == TyKind::Float))) {
                mixed_float();
                return;
            }
            // 定宽形参 + 字面量实参：范围内隐式 coerce（超范围在此报错）。
            // 非字面量 int 实参窄化到定宽形参仍然要求显式 cast。
            if (arg_expr && is_fixed_int(p.ty) && accept_literal_into(p.ty, arg_expr))
                return;
            if (assignable(p.ty, ty)) return;
            if (p.inferred) {
                // Keep the existing fallback for other incompatible types.
                // Sharing a register class is not general dynamic dispatch.
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

        size_t pos_i = 0;
        bool uncertain_pos = false;
        for (const auto& a : positional) {
            if (a.dynamic_star && pos_i < npos) uncertain_pos = true;
            if (uncertain_pos) continue;
            if (pos_i < npos) {
                filled[pos_i] = true;
                bind_param(fn->pos[pos_i], pos_i, a.ty, a.source->line, a.source->col,
                           false, a.source);
                ++pos_i;
            } else if (fn->varargs && !assignable(fn->vararg_ty, a.ty)) {
                err(a.source->line, a.source->col,
                    std::format("'*{}' of '{}' expects '{}', got '{}'", fn->vararg_name,
                                fn->name, type_name(fn->vararg_ty), type_name(a.ty)));
            }
        }

        for (auto& [name, ty] : kws) {
            if (name.empty()) continue;  // **mapping：键在静态上不可知
            auto pit = std::find_if(fn->pos.begin(), fn->pos.end(),
                                    [&](const Param& p) { return p.name == name; });
            if (pit != fn->pos.end()) {
                size_t idx = static_cast<size_t>(pit - fn->pos.begin());
                if (idx < fn->n_posonly) {
                    if (fn->kwargs) continue;  // The name belongs to **kwargs, not this slot.
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
        auto bind_default = [&](Param& p, size_t idx) {
            if (!p.has_default) {
                add_missing(p.name);
                return;
            }
            // Only a default actually used by this call is type evidence.
            // Nonliteral defaults are not supported by the call lowerer.
            if (!p.default_lit) return;
            const auto* impl = fn->outer ? fn->outer : fn.get();
            const auto it = signature_defs_.find(impl);
            const bool can_recheck = it != signature_defs_.end() &&
                (!func_owners_.contains(it->second) ||
                 std::ranges::find(func_defs_, it->second) != func_defs_.end());
            TypePtr ty = literal_type(*p.default_lit);
            // 定宽形参的整数字面量默认值：范围内的常量直接兼容
            // （default_lit 里是编译期常量，降级按形参类型生成常量）。
            if (p.ty && is_fixed_int(p.ty) &&
                p.default_lit->kind == LiteralValue::Kind::Int &&
                int_literal_fits(p.ty, p.default_lit->i))
                return;
            if (can_recheck) {
                bind_param(p, idx, ty, c.line, c.col, true);
                // The invocation may go through a vtable. Validate any newly
                // inferred FP argument against the module's possible overrides.
                if (ty->kind == TyKind::Float && p.inferred) {
                    if (auto owner = func_owners_.find(it->second);
                        owner != func_owners_.end() && owner->second)
                        methods_with_float_defaults_.insert(it->second);
                }
            } else if (!p.ann_unknown &&
                       ((ty->kind == TyKind::Float && (!p.ty || p.ty->kind != TyKind::Float)) ||
                        (ty->kind == TyKind::None && p.ty && p.ty->kind == TyKind::Float))) {
                err(c.line, c.col, std::format(
                    "default for '{}' of '{}' has no compatible compiled parameter type",
                    p.name, fn->name));
            }
        };
        if (!has_dstar) {
            if (!uncertain_pos)
                for (size_t i = 0; i < npos; ++i)
                    if (!filled[i]) bind_default(fn->pos[i], i);
            for (size_t i = 0; i < fn->kwonly.size(); ++i)
                if (!kw_filled[i]) bind_default(fn->kwonly[i], i);
        }
        if (!missing.empty())
            err(c.line, c.col,
                std::format("function '{}' missing required argument{}: {}", fn->name,
                            missing.find(',') == std::string::npos ? "" : "s", missing));

        if (fn->ret) {
            // 合成生成器函数的返回类型跟着形参回填走：第一次检查时形参
            // 还是 unknown，重查后才是 Generator[T]。
            if (fn->name.rfind("__pyadamas$gen_", 0) == 0) {
                genexpr_call_sites_[&c] = fn;
                // **同步重查**：`for v in (x for x in xs)` 的循环变量类型
                // 取 iter_elem(调用结果)；如果等到 worklist 末尾再刷新，
                // 调用点早已按 Generator[unknown] 给 v 定过型（0.322 P0：
                // 生成器表达式丢浮点元素类型，打印 double 位模式）。
                recheck_synth_gen(fn);
            }
        }
        return observe_call_result(c, fn, fn->ret ? fn->ret
                                      : fn->ret_known ? t_none() : t_unknown());
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
            // 内部 intrinsics：stdlib/pprint.py 取 unknown dict 的键值对
            "__pa_any_items",
            // 内部 intrinsics（P4）：stdlib/contextlib.py 的 suppress/closing。
            // 异常类当值后按名字判父子；unknown 值上的零参方法调用（close）。
            "__pa_exc_is_a", "__pa_close",
            // site 模块注入的内置名（交互式退出），真实代码里也会用到
            "quit", "exit", "copyright", "credits", "license",
            // 字节长度出口：len 返回码点数，len_bytes 返回 UTF-8 字节数（O(1)）
            "len_bytes",
            // 虚拟线程先验（docs/vthread-design.md）：join/sleep/make_chan
            "join", "sleep", "make_chan",
            // 定宽整数显式转换（P1）：cast_i8/.../cast_u64。宽→窄的唯一
            // 隐式途径之外的手段；语义是 C 式截断 + 回绕（见设计 §1.2/§1.3）。
            "cast_i8", "cast_i16", "cast_i32", "cast_i64", "cast_int",
            "cast_u8", "cast_u16", "cast_u32", "cast_u64",
        };
        return names.count(n) > 0;
    }

// 免写直调的调用点检查：没有声明可查，只求实参类型并推断返回 ABI。
// 规则（ctypes 式契约，调用方负责）：有 float 实参 → 返回按 double；
// 否则按 int。标量标注（x: float = f(...) / -> float 的 return）覆盖推断。
TypePtr TypeChecker::check_c_lazy(Call& c, const std::string& sym) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("C function '{}' without a declaration does not take keyword "
                            "arguments", sym));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        bool any_float = false;
        for (auto& arg : c.args) {
            if (arg->kind == ExprKind::Starred) {
                err(arg->line, arg->col,
                    std::format("'*args' is not supported for C function '{}' without a "
                                "declaration", sym));
                expr(*static_cast<Starred&>(*arg).value);
                continue;
            }
            TypePtr t = expr(*arg);
            if (t && t->kind == TyKind::Float) any_float = true;
        }
        return any_float ? t_float() : t_int();
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
