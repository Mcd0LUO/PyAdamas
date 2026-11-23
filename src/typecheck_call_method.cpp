// typecheck_call_method.cpp —— 方法表与 method_call
#include "typecheck.h"

#include "module.h"

#include "native_sigs.h"

namespace pyadamas {

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
                return m == "send" || m == "throw" || m == "close" || m == "__next__";
            case TyKind::AsyncGenerator:
                return m == "asend" || m == "athrow" || m == "aclose" || m == "__anext__";
            case TyKind::Task:
                return m == "done" || m == "cancel" || m == "cancelled" ||
                       m == "result" || m == "exception";
            case TyKind::Future:
                return m == "done" || m == "cancel" || m == "cancelled" ||
                       m == "result" || m == "exception" || m == "set_result" ||
                       m == "set_exception" || m == "add_done_callback";
            case TyKind::Chan:
                return m == "send" || m == "recv";
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
            case TyKind::Random:
                // 方法表来自 .pysig 的 class Random（不再硬编码）。
                return nativesig::class_has_method("random", "Random", m);
            case TyKind::Regex:
                // 方法表来自 .pysig 的 class Pattern（不再硬编码）。
                return nativesig::class_has_method("re", "Pattern", m);
            case TyKind::Match:
                // 方法表来自 .pysig 的 class Match（不再硬编码）。
                return nativesig::class_has_method("re", "Match", m);
            case TyKind::DateTime:
                // date/datetime 共用 TyKind::DateTime，方法表取两个声明类的并集。
                return nativesig::class_has_method("datetime", "date", m) ||
                       nativesig::class_has_method("datetime", "datetime", m);
            case TyKind::TimeDelta:
                return nativesig::class_has_method("datetime", "timedelta", m);
            case TyKind::Deque:
                return nativesig::class_has_method("collections", "deque", m);
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

bool TypeChecker::imported_base_in_mro(const ClassInfo* c) const {
    if (!c) return false;
    for (const ClassInfo* k : c->mro) {
        if (k == c) continue;
        if (k->imported() || k->is_open()) return true;
    }
    // 骨架/导入类没有 C3 表时，退回逐层看基类
    for (const ClassInfo* p = c->base; p; p = p->base)
        if (p->imported() || p->is_open()) return true;
    for (const ClassInfo* e : c->extra_bases)
        if (e->imported() || e->is_open()) return true;
    return false;
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
            // 方法表来自 .pysig 的 `class Random`（nativesig::class_info），不再硬编码。
            const auto* cls = nativesig::class_info("random", "Random");
            if (!cls) return t_unknown();
            auto it = cls->methods.find(name);
            if (it == cls->methods.end()) {
                err(c.line, c.col,
                    std::format("class 'Random' has no attribute '{}'", name));
                return t_unknown();
            }
            const FuncInfoPtr& fn = it->second;
            const size_t npos = fn->pos.size();
            size_t ndef = 0;
            for (auto& p : fn->pos)
                if (p.has_default) ++ndef;
            argc(npos - ndef, npos);
            for (size_t i = 0; i < a.size() && i < npos; ++i) {
                TypePtr want = fn->pos[i].ty;
                TypePtr got = a[i];
                if (!want || !got || is_unknown(got)) continue;
                // 数值参数放宽（int 实参进 float 形参是 Python 常规用法）。
                if (want->kind == TyKind::Float && is_num(got)) continue;
                if (assignable(want, got)) continue;
                err(c.args[i]->line, c.args[i]->col,
                    std::format("argument '{}' of 'random.Random.{}' expects '{}', got '{}'",
                                fn->pos[i].name, name, type_name(want), type_name(got)));
            }
            if (fn->ret_specialized) {
                // @ret_specialized：与硬编码 choice 同口径——序列的元素类型。
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
                return t_unknown();
            }
            return fn->ret_known ? fn->ret : t_unknown();
        }
        if (recv->kind == TyKind::Generator) {
            if (name == "send") {
                argc(1, 1);
                // Generator[Y, S, R] 标注了 send 值类型时按 S 检查；
                // 返回的仍是下一次 yield 的值（Y）。
                TypePtr st = recv->sub.size() > 1 ? recv->sub[1] : nullptr;
                if (st && !is_unknown(st) && a.size() == 1 && a[0] && !is_unknown(a[0]) &&
                    !assignable(st, a[0]))
                    err(c.line, c.col,
                        std::format("send() expects {}, got {}", type_name(st), type_name(a[0])));
                return elem();
            }
            if (name == "throw") {
                argc(1, 1);
                TypePtr a0t = a.empty() ? nullptr : a[0];
                if (a0t && a0t->kind == TyKind::ClassObj && !a0t->sub.empty()) a0t = a0t->sub[0];
                // 异常类（内置或自定义）等价于无消息实例。
                bool exc_class = a0t && a0t->kind == TyKind::Class &&
                                 a0t->cls && a0t->cls->is_exception_class;
                if (a0t && !is_unknown(a0t) &&
                    a0t->kind != TyKind::Exception && !exc_class)
                    err(c.line, c.col, "throw() expects an exception instance");
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
                TypePtr a0t = a.empty() ? nullptr : a[0];
                if (a0t && a0t->kind == TyKind::ClassObj && !a0t->sub.empty()) a0t = a0t->sub[0];
                // 异常类（内置或自定义）等价于无消息实例。
                bool exc_class = a.size() == 1 && a0t && a0t->kind == TyKind::Class &&
                                 a0t->cls && a0t->cls->is_exception_class;
                if (a.size() == 1 && a0t && !is_unknown(a0t) &&
                    a0t->kind != TyKind::Exception && !exc_class)
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

        if (recv->kind == TyKind::Chan) {
            if (name == "send") {
                argc(1, 1);
                TypePtr et = elem();
                if (a.size() == 1 && a[0] && !is_unknown(a[0]) && !is_unknown(et) &&
                    !assignable(et, a[0]))
                    err(c.line, c.col,
                        std::format("send() expects {}, got {}", type_name(et), type_name(a[0])));
                return t_none();
            }
            if (name == "recv") {
                argc(0, 0);
                return elem();
            }
            err(c.line, c.col, std::format("'chan' object has no method '{}'", name));
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
                if (a.size() == 1) {
                    // 定宽元素 + 字面量实参：范围内隐式 coerce（与声明/赋值
                    // 同口径），超范围 accept_literal_into 已报错。非字面量
                    // 仍走 merge_elem（窄化要 cast，锁住就报错）。
                    if (is_fixed_int(e) && accept_literal_into(e, c.args[0].get()))
                        a[0] = e;
                    if (!merge_elem(recv->sub[0], recv, a[0], recv->locked))
                        err(c.line, c.col,
                            std::format("list element type is '{}', got '{}'", type_name(e),
                                        type_name(a[0])));
                }
                return t_none();
            }
            if (name == "extend") {
                argc(1, 1);
                if (a.size() == 1) {
                    // 定宽元素 + 字面量列表实参：逐元素 coerce（同口径）。
                    if (is_fixed_int(e) && c.args[0]->kind == ExprKind::List &&
                        coerce_container_literal(recv, c.args[0].get()))
                        a[0] = recv;
                    auto ie = iter_elem(a[0], c.line, c.col);
                    if (!merge_elem(recv->sub[0], recv, ie, recv->locked))
                        err(c.line, c.col,
                            std::format("list element type is '{}', got '{}'", type_name(e),
                                        type_name(ie)));
                }
                return t_none();
            }
            if (name == "insert") {
                argc(2, 2);
                // 元素检查与 append 同口径（此前 insert 完全没有元素检查，
                // 定宽列表会被静默窄化；现在字面量 coerce、非字面量要 cast）。
                if (a.size() == 2) {
                    if (is_fixed_int(e) && accept_literal_into(e, c.args[1].get()))
                        a[1] = e;
                    if (!merge_elem(recv->sub[0], recv, a[1], recv->locked))
                        err(c.line, c.col,
                            std::format("list element type is '{}', got '{}'", type_name(e),
                                        type_name(a[1])));
                }
                return t_none();
            }
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
                argc(0, 2);
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
            if (name == "translate") {
                argc(1, 1);
                return t_bytes();
            }
            if (name == "split") {
                argc(0, 2);
                return mk_list(t_bytes());
            }
            if (name == "partition" || name == "rpartition") {
                argc(1, 1);
                return mk_tuple({t_bytes(), t_bytes(), t_bytes()});
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
            if (name == "decode") { argc(0, 2); return t_str(); }  // bytes 误推成 str 时的 decode
            if (name == "maketrans") { argc(1, 3); return t_unknown(); }
        }

        if (recv->kind == TyKind::DateTime || recv->kind == TyKind::TimeDelta) {
            const bool is_dt = recv->kind == TyKind::DateTime;
            const char* cls = is_dt ? "datetime" : "timedelta";
            // replace 是唯一允许关键字参数的方法（原有口径）。
            if (!c.keywords.empty() && !(is_dt && name == "replace"))
                err(c.line, c.col,
                    std::format("datetime.{}() does not take keyword arguments", name));
            const auto* info = nativesig::class_info("datetime", cls);
            if (!info) return t_unknown();
            auto it = info->methods.find(name);
            if (it == info->methods.end()) {
                err(c.line, c.col,
                    std::format("class '{}' has no attribute '{}'", cls, name));
                return t_unknown();
            }
            const FuncInfoPtr& fn = it->second;
            // 位置参数个数按声明检查（replace 只收关键字，0 位置参）。
            if (!fn->varargs) {
                size_t ndef = 0;
                for (auto& pp : fn->pos) if (pp.has_default) ++ndef;
                argc(fn->pos.size() - ndef, fn->pos.size());
            }
            for (size_t i = 0; i < a.size() && i < fn->pos.size(); ++i) {
                TypePtr want = fn->pos[i].ty;
                TypePtr got = a[i];
                if (!want || !got || is_unknown(got)) continue;
                if (assignable(want, got)) continue;
                err(c.args[i]->line, c.args[i]->col,
                    std::format("argument '{}' of 'datetime.{}.{}' expects '{}', got '{}'",
                                fn->pos[i].name, cls, name, type_name(want), type_name(got)));
            }
            // replace 的关键字集合校验保留（原有口径）。
            if (is_dt && name == "replace") {
                for (auto& kw : c.keywords) {
                    if (kw.arg == "year" || kw.arg == "month" || kw.arg == "day" ||
                        kw.arg == "hour" || kw.arg == "minute" || kw.arg == "second") {
                        expr(*kw.value);
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("datetime.replace() got an unexpected keyword "
                                        "argument '{}'",
                                        kw.arg));
                    }
                }
                return new_ty(TyKind::DateTime);
            }
            // 返回类型用 pysig 声明（都是 str/int/float，可表达）。
            return fn->ret_known ? fn->ret : t_unknown();
        }
        if (recv->kind == TyKind::Path) {
            if (name == "mkdir") {
                // mkdir(mode=0o777, parents=False, exist_ok=False)：只收关键字。
                argc(0, 0);
                for (auto& kw : c.keywords) {
                    if (kw.arg == "mode") {
                        expr(*kw.value);
                    } else if (kw.arg == "parents" || kw.arg == "exist_ok") {
                        expr(*kw.value);
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("pathlib.Path.mkdir() got an unexpected keyword "
                                        "argument '{}'",
                                        kw.arg));
                    }
                }
                return t_none();
            }
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
            if (name == "iterdir") {
                argc(0, 0);
                return mk_list(new_ty(TyKind::Path));
            }
            if (name == "glob") {
                argc(1, 1);
                return mk_list(new_ty(TyKind::Path));
            }
            if (name == "unlink" || name == "rmdir" || name == "touch") {
                argc(0, 0);
                return t_none();
            }
            if (name == "with_suffix") {
                argc(1, 1);
                return new_ty(TyKind::Path);
            }
            if (name == "read_bytes") {
                argc(0, 0);
                return t_bytes();
            }
            if (name == "write_bytes") {
                argc(1, 1);
                if (a.size() == 1 && a[0] && !is_unknown(a[0]) &&
                    a[0]->kind != TyKind::Bytes)
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("write_bytes() expects bytes, got '{}'", type_name(a[0])));
                return t_int();
            }
            if (name == "is_absolute") {
                argc(0, 0);
                return t_bool();
            }
            if (name == "joinpath" || name == "relative_to") {
                argc(1, 1);
                return new_ty(TyKind::Path);
            }
            if (name == "resolve" || name == "absolute") {
                argc(0, 0);
                return new_ty(TyKind::Path);
            }
            err(c.line, c.col, std::format("pathlib.Path has no method '{}'", name));
            return t_unknown();
        }
        if (recv->kind == TyKind::Deque) {
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("collections.deque.{}() does not take keyword arguments", name));
            const auto* info = nativesig::class_info("collections", "deque");
            if (!info) return t_unknown();
            auto it = info->methods.find(name);
            if (it == info->methods.end()) {
                err(c.line, c.col,
                    std::format("class 'deque' has no attribute '{}'", name));
                return t_unknown();
            }
            const FuncInfoPtr& fn = it->second;
            if (!fn->varargs) {
                size_t ndef = 0;
                for (auto& pp : fn->pos) if (pp.has_default) ++ndef;
                argc(fn->pos.size() - ndef, fn->pos.size());
            }
            for (size_t i = 0; i < a.size() && i < fn->pos.size(); ++i) {
                TypePtr want = fn->pos[i].ty;
                TypePtr got = a[i];
                if (!want || !got || is_unknown(want) || is_unknown(got)) continue;
                if (assignable(want, got)) continue;
                err(c.args[i]->line, c.args[i]->col,
                    std::format("argument '{}' of 'collections.deque.{}' expects '{}', got '{}'",
                                fn->pos[i].name, name, type_name(want), type_name(got)));
            }
            // pop/popleft/copy 返回接收者的元素类型（pysig 用 object 占位）。
            if (name == "pop" || name == "popleft") return elem();
            if (name == "copy") return mk_deque(elem());
            return fn->ret_known ? fn->ret : t_unknown();
        }
        if (recv->kind == TyKind::Popen) {
            if (name == "wait") {
                argc(0, 0);
                for (auto& kw : c.keywords) {
                    if (kw.arg == "timeout") {
                        expr(*kw.value);
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("Popen.wait() got an unexpected keyword argument '{}'",
                                        kw.arg));
                    }
                }
                return t_int();
            }
            if (name == "communicate") {
                argc(0, 0);
                for (auto& kw : c.keywords) {
                    if (kw.arg == "input") {
                        expr(*kw.value);
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("Popen.communicate() got an unexpected keyword "
                                        "argument '{}'",
                                        kw.arg));
                    }
                }
                return mk_tuple({t_str(), t_str()});
            }
            if (name == "kill" || name == "terminate") {
                if (!c.keywords.empty())
                    err(c.line, c.col,
                        std::format("Popen.{}() does not take keyword arguments", name));
                argc(0, 0);
                return t_none();
            }
            if (name == "send_signal") {
                if (!c.keywords.empty())
                    err(c.line, c.col, "Popen.send_signal() does not take keyword arguments");
                argc(1, 1);
                return t_none();
            }
            err(c.line, c.col, std::format("Popen has no method '{}'", name));
            return t_unknown();
        }
        if (recv->kind == TyKind::Regex || recv->kind == TyKind::Match) {
            const bool is_pattern = recv->kind == TyKind::Regex;
            const char* mod = "re";
            const char* cls = is_pattern ? "Pattern" : "Match";
            if (!c.keywords.empty())
                err(c.line, c.col,
                    std::format("re.{}.{}() does not take keyword arguments", cls, name));
            const auto* info = nativesig::class_info(mod, cls);
            if (!info) return t_unknown();
            auto it = info->methods.find(name);
            if (it == info->methods.end()) {
                err(c.line, c.col,
                    std::format("class '{}' has no attribute '{}'", cls, name));
                return t_unknown();
            }
            const FuncInfoPtr& fn = it->second;
            // argc 按声明的位置参数/默认值个数检查（可变参方法用 varargs）。
            if (!fn->varargs) {
                size_t ndef = 0;
                for (auto& pp : fn->pos) if (pp.has_default) ++ndef;
                argc(fn->pos.size() - ndef, fn->pos.size());
            }
            for (size_t i = 0; i < a.size() && i < fn->pos.size(); ++i) {
                TypePtr want = fn->pos[i].ty;
                TypePtr got = a[i];
                if (!want || !got || is_unknown(got)) continue;
                if (assignable(want, got)) continue;
                err(c.args[i]->line, c.args[i]->col,
                    std::format("argument '{}' of 're.{}.{}' expects '{}', got '{}'",
                                fn->pos[i].name, cls, name, type_name(want), type_name(got)));
            }
            // sub 的 repl 回调形参：re.sub 的 repl 一定吃一个 Match。
            if (is_pattern && name == "sub" && a.size() >= 1 && a[0] &&
                a[0]->kind == TyKind::Func && a[0]->fn && a[0]->fn->pos.size() == 1) {
                Param& cp = a[0]->fn->pos[0];
                if (!cp.ty || cp.ty->kind == TyKind::Unknown) {
                    cp.ty = t_match();
                    cp.inferred = true;
                    a[0]->fn->sig_dirty = true;
                }
            }
            // 返回类型：pysig 无法表达 Optional[Match] / 变长 group 的元组返回，
            // 这些保留专门口径（与硬编码时代逐字一致）。
            if (is_pattern) {
                if (name == "search" || name == "match" || name == "fullmatch")
                    return mk_optional(t_match());
                if (name == "findall") return mk_list(t_str());
                if (name == "finditer") return mk_list(t_match());
                if (name == "sub") return t_str();
                if (name == "split") return mk_list(t_str());
            } else {
                if (name == "group") {
                    if (c.args.size() > 1) return mk_tuple_rest(t_str());
                    return t_str();
                }
                if (name == "groups") return mk_list(t_str());
                if (name == "groupdict") return mk_dict(t_str(), t_str());
                if (name == "start" || name == "end") return t_int();
                if (name == "span") return mk_tuple({t_int(), t_int()});
                if (name == "expand") return t_str();
            }
            return fn->ret_known ? fn->ret : t_unknown();
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
                // 只有一个参数时可能缺失，返回类型是 Optional[值类型]。
                // 两参数形式的缺省值本身就是返回类型的一部分：空 dict 的值
                // 类型还是「洞」时，用 join(值类型, 缺省值类型) 把洞填成
                // 缺省值的域（counts.get(w, 0) + 1 这类计数器惯用法因此
                // 留在 int 快路径，不必落到 unknown 的动态算术）。
                if (a.size() == 1) return mk_optional(vt);
                return join(vt, a[1]);
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
                // pyadamas 扩展：Counter 返回普通 dict，计数排序走这个方法。
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
            if (name == "add") {
                argc(1, 1);
                // 定宽元素 + 字面量实参：范围内 coerce、超范围报错（与
                // list.append 同口径）。非字面量的既有路径不动。
                if (a.size() == 1 && is_fixed_int(e)) accept_literal_into(e, c.args[0].get());
                return t_none();
            }
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

        if (is_int_kind(recv->kind) || recv->kind == TyKind::Float ||
            recv->kind == TyKind::Bool) {
            if (name == "bit_length") { argc(0, 0); return t_int(); }
            if (name == "as_integer_ratio") { argc(0, 0); return mk_tuple({t_int(), t_int()}); }
            if (name == "is_integer") { argc(0, 0); return t_bool(); }
            if (name == "to_bytes") { argc(1, 3); return t_unknown(); }
            if (name == "conjugate" || name == "real" || name == "imag" || name == "numerator" ||
                name == "denominator")
                return recv;
            // hex/fromhex/from_bytes：参数形态与结果类型不一致，暂按 unknown
            if (name == "hex" || name == "fromhex" || name == "from_bytes") return t_unknown();
        }

        if (recv->kind == TyKind::Func && recv->fn && recv->fn->unchecked) {
            static const std::unordered_set<std::string> type_ctor = {
                "int", "float", "str", "bool", "bytes", "bytearray", "complex", "set",
                "frozenset", "list", "tuple", "dict",
            };
            static const std::unordered_set<std::string> type_methods = {
                "from_bytes", "fromhex", "to_bytes", "bit_length", "as_integer_ratio",
                "is_integer", "decode", "encode",
            };
            if (c.args.size() <= 4 && type_ctor.count(recv->fn->name) && type_methods.count(name)) {
                (void)argc(0, 4);
                return t_unknown();
            }
        }

        if (recv->kind == TyKind::Exception) {
            // BaseException 的方法：with_traceback 返回自身，args 是元组，
            // __str__ 返回消息；其余保守放行（异常类常有自定义协议）。
            if (name == "with_traceback") { argc(1, 1); return recv; }
            if (name == "args") { argc(0, 0); return mk_tuple({t_unknown()}); }
            if (name == "__str__" || name == "add_note" || name == "__reduce__")
                return t_unknown();
            unsupported.push_back("method on a builtin exception value");
            return t_unknown();
        }

        if (recv->kind == TyKind::None) {
            // None 接收者多为 flow-narrowing 丢失（is not None 守卫没
            // 更新类型）；保守求值实参后放行，避免误杀。
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        err(c.line, c.col,
            std::format("'{}' object has no method '{}'", type_name(recv), name));
        return t_unknown();
    }



}  // namespace pyadamas
