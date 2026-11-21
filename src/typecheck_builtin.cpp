// typecheck_builtin.cpp —— 内置函数调用的类型分派
#include "typecheck.h"

namespace pyadamas {

TypePtr TypeChecker::builtin_call(const std::string& n, Call& c) {
        // make_chan 的第 0 实参是类型名（编译期特殊形式），不能按值求类型：
        // unknown/Any 作为名字并不存在。单独分支处理。
        if (n == "make_chan") {
            if (c.args.size() != 2 || !c.keywords.empty()) {
                err(c.line, c.col, "make_chan() expects (elem_type, capacity)");
                return mk_chan(t_unknown());
            }
            TypePtr elem = t_unknown();
            auto* nm = dynamic_cast<Name*>(c.args[0].get());
            std::string tn = nm ? nm->id : "";
            if (tn == "int" || tn == "bool") elem = t_int();
            else if (tn == "float") elem = t_float();
            else if (tn == "str") elem = t_str();
            else if (tn == "unknown" || tn == "Any") elem = t_unknown();
            else err(c.args[0]->line, c.args[0]->col,
                     "make_chan() element type must be int/float/str/unknown");
            TypePtr ct = expr(*c.args[1]);
            if (!is_unknown(ct) && ct && !is_int_like(ct))
                err(c.args[1]->line, c.args[1]->col, "make_chan() capacity must be int");
            return mk_chan(elem);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        // 关键字的值在这里求一次就够了，下面按名字分派时只查类型。
        // 再求一次会把同一个未定义名报两遍。
        std::vector<TypePtr> kwt;
        for (auto& kw : c.keywords) kwt.push_back(expr(*kw.value));

        // ── 定宽整数显式转换（P1）──────────────────────────────────────
        //
        // cast_i8(x) / cast_u8(x) / ...：显式窄化/换符号的唯一途径
        // （设计 §1.2：宽→窄必须显式，静默截断是最难查的算错）。
        // 语义按 C 的转换 + pa 的确定回绕：取低 N 位，有符号再符号扩展，
        // 无符号再零扩展。超范围不是错误（这正是 cast 的意义），
        // 与字面量 coerce（超范围编译报错）刻意不同。
        if (n.starts_with("cast_")) {
            TypePtr to = cast_target_of(n);
            if (!to) {
                err(c.line, c.col, std::format("unknown cast function '{}'", n));
                return t_unknown();
            }
            if (c.args.size() != 1 || !c.keywords.empty()) {
                err(c.line, c.col, std::format("'{}' expects exactly 1 argument", n));
                return to;
            }
            TypePtr from = a.empty() ? t_unknown() : a[0];
            if (to->kind == TyKind::Big) consume_u64_literal(to, c.args[0].get());
            if (from && !is_unknown(from) && !is_num(from) &&
                !(to->kind == TyKind::Big && from->kind == TyKind::Str)) {
                // v1：只支持数值域之间的显式转换。str → 整数请先 int()。
                err(c.args[0]->line, c.args[0]->col,
                    std::format("cannot cast '{}' to '{}'", type_name(from), type_name(to)));
            }
            return to;
        }
        auto need = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) {
                err(c.line, c.col,
                    std::format("'{}' expects {} argument{}, got {}", n,
                                lo == hi ? std::to_string(lo)
                                         : std::format("{}..{}", lo, hi),
                                hi == 1 ? "" : "s", a.size()));
                return false;
            }
            return true;
        };

        if (n == "print") {
            // struct 是值类型，没有通用的文本表示；直接打印会把 payload
            // 地址当成整数输出，必须明确拒绝。
            for (size_t i = 0; i < c.args.size() && i < a.size(); ++i) {
                if (a[i] && (a[i]->kind == TyKind::Struct || a[i]->kind == TyKind::StructPtr))
                    err(c.args[i]->line, c.args[i]->col,
                        "cannot print a struct value; print its fields instead");
            }
            for (size_t i = 0; i < c.keywords.size(); ++i) {
                auto& kw = c.keywords[i];
                if (kw.arg.empty()) {
                    // print(*args, **kwargs) 是转发包装的常见写法，键名静态不可知。
                    // 放行——后端展开时自然会拒。
                    continue;
                }
                if (kw.arg != "sep" && kw.arg != "end" && kw.arg != "file" && kw.arg != "flush") {
                    err(kw.value->line, kw.value->col,
                        std::format("print() got an unexpected keyword argument '{}'", kw.arg));
                    continue;
                }
                TypePtr kt = kwt[i];
                if (kw.arg == "file") {
                    // 任意 file-like 对象（Devnull、自定义 writer）都接受：
                    // 静态只认 TyKind::File 会误杀，duck typing 放行。
                    continue;
                }
                if (kw.arg == "flush") {
                    if (!is_unknown(kt) && kt && kt->kind != TyKind::Bool &&
                        kt->kind != TyKind::Int)
                        err(kw.value->line, kw.value->col,
                            "print() flush must be a bool");
                    continue;
                }
                if (!is_unknown(kt) && (!kt || kt->kind != TyKind::Str))
                    err(kw.value->line, kw.value->col,
                        std::format("print() '{}' must be a string, got '{}'", kw.arg,
                                    type_name(kt)));
            }
            return t_none();
        }
        if (n == "len_bytes") {
            if (a.size() != 1) return t_int();
            if (!is_unknown(a[0]) && a[0]->kind != TyKind::Str && a[0]->kind != TyKind::Bytes)
                err(c.line, c.col,
                    std::format("len_bytes() expects a str or bytes, got '{}'",
                                type_name(a[0])));
            return t_int();
        }
        if (n == "join") {
            if (!need(1, 1)) return t_none();
            if (!is_unknown(a[0]) && a[0] && a[0]->kind != TyKind::VThread)
                err(c.line, c.col,
                    std::format("join() expects a vthread handle, got '{}'", type_name(a[0])));
            return t_none();
        }
        if (n == "sleep") {
            if (!need(1, 1)) return t_none();
            if (!is_unknown(a[0]) && a[0] && !is_int_like(a[0]))
                err(c.line, c.col,
                    std::format("sleep() expects milliseconds (int), got '{}'", type_name(a[0])));
            return t_none();
        }
        if (n == "len") {
            if (a.size() != 1) return t_int();
            if (!is_unknown(a[0]) && a[0]->kind != TyKind::List && a[0]->kind != TyKind::Str &&
                a[0]->kind != TyKind::Bytes && a[0]->kind != TyKind::Dict && a[0]->kind != TyKind::Tuple &&
                a[0]->kind != TyKind::Set && a[0]->kind != TyKind::Range &&
                a[0]->kind != TyKind::Class && a[0]->kind != TyKind::Deque)
                err(c.line, c.col,
                    std::format("object of type '{}' has no len()", type_name(a[0])));
            return t_int();
        }
        if (n == "range") {
            for (auto& t : a)
                if (!is_unknown(t) && !is_int_like(t))
                    err(c.line, c.col,
                        std::format("'range' argument must be int, got '{}'", type_name(t)));
            // range 是惰性对象，不是列表：range(10**9) 不造十个亿元素。
            return mk_range(t_int());
        }
        if (n == "abs") {
            if (a.empty()) return t_unknown();
            // 用户类：__abs__ 的声明返回类型就是 abs() 的结果类型。
            if (a[0] && a[0]->kind == TyKind::Class) {
                if (TypePtr r = dunder_unary_ret("__abs__", a[0])) return r;
                if (a[0]->cls && a[0]->cls->is_open()) return t_unknown();
                err(c.line, c.col,
                    std::format("bad operand type for abs(): '{}'", type_name(a[0])));
                return t_unknown();
            }
            if (!is_unknown(a[0]) && !is_num(a[0]) &&
                (!a[0] || a[0]->kind != TyKind::Complex))
                err(c.line, c.col,
                    std::format("bad operand type for abs(): '{}'", type_name(a[0])));
            if (a[0] && a[0]->kind == TyKind::Complex) return t_float();
            // 定宽整数按 C 的整数提升返回 int：abs(i8 的 -128) = 128，
            // 而不是在 i8 里回绕成 -128（abs 是函数，不是定宽运算）。
            if (is_fixed_int(a[0])) return t_int();
            return a[0];
        }
        if (n == "min" || n == "max") {
            if (a.empty()) return t_unknown();
            // big 是支持的：比较走运行时 pa_big_cmp（见 lower_minmax）。
            bool any_big = std::any_of(a.begin(), a.end(), has_big_value);
            if (a.size() == 1) {
                TypePtr e = iter_elem(a[0], c.line, c.col);
                return has_big_value(e) ? t_big() : e;
            }
            TypePtr acc = a[0];
            for (size_t i = 1; i < a.size(); ++i) acc = join(acc, a[i]);
            // 混了 big 就整体按 big（big 吸收其它整数/浮点域）。
            if (any_big) return t_big();
            return acc;
        }
        if (n == "sum") {
            if (!a.empty()) {
                // big 元素/起始值是支持的：累加走运行时 BigAdd（不是裸 Bin）。
                auto e = iter_elem(a[0], c.line, c.col);
                if (!is_unknown(e) && !is_num(e))
                    err(c.line, c.col,
                        std::format("sum() over non-numeric '{}'", type_name(e)));
                // 定宽元素按 C 的整数提升累加到 int（避免 u8 的 255+1 静默回绕）。
                TypePtr res = is_num(e) ? (is_fixed_int(e) ? t_int() : e) : t_int();
                // start 参数参与数值域提升：sum([], 0.5) 是 float。
                if (a.size() > 1 && is_num(a[1])) res = promote(res, a[1]);
                // 任一侧是 big 就整体按 big 累加（精确，不回绕）。
                if (has_big_value(e) || (a.size() > 1 && has_big_value(a[1]))) res = t_big();
                return res;
            }
            return t_int();
        }
        if (n == "sorted" || n == "reversed") {
            if (a.empty()) return mk_list(t_unknown());
            if (n == "reversed" && a[0] && a[0]->kind == TyKind::Class && a[0]->cls) {
                if (auto* m = a[0]->cls->method("__reversed__")) {
                    TypePtr rt = m->call_sig ? m->call_sig->ret : nullptr;
                    if (m->decorated && m->decorated_ty &&
                        m->decorated_ty->kind == TyKind::Func && m->decorated_ty->fn)
                        rt = m->decorated_ty->fn->ret;
                    return mk_list(iter_elem(rt ? rt : t_unknown(), c.line, c.col));
                }
            }
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(e);
        }
        if (n == "enumerate") {
            if (a.empty()) return mk_list(mk_tuple({t_int(), t_unknown()}));
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(mk_tuple({t_int(), e}));
        }
        if (n == "zip") {
            std::vector<TypePtr> elems;
            for (auto& t : a) elems.push_back(iter_elem(t, c.line, c.col));
            return mk_list(mk_tuple(std::move(elems)));
        }
        // 容器转标量没有定义（CPython 抛 TypeError）。以前不管，
        // 降级层会把容器指针当数字用，静默给出垃圾值——这里直接拦。
        // str(元组) / str(range) 是合法的（走 repr 形式）。
        if (a.size() == 1 && !is_unknown(a[0]) && a[0] &&
            (n == "int" || n == "float" || n == "str")) {
            bool container = a[0]->kind == TyKind::List ||
                              a[0]->kind == TyKind::Dict ||
                              a[0]->kind == TyKind::Set;
            bool seq = a[0]->kind == TyKind::Tuple || a[0]->kind == TyKind::Range;
            bool str_ok = n == "str" && (a[0]->kind == TyKind::Tuple ||
                                        a[0]->kind == TyKind::Range);
            if (container || (seq && !str_ok))
                err(c.line, c.col,
                    std::format("{}() argument must be a number or a string, not '{}'",
                                n, type_name(a[0])));
        }
        if (n == "int") {
            if (a.size() > 2)
                err(c.line, c.col, std::format("int() takes at most 2 arguments, got {}", a.size()));
            else if (a.size() == 2 && !is_unknown(a[0]) && (!a[0] || a[0]->kind != TyKind::Str))
                err(c.line, c.col,
                    std::format("int() can't convert non-string with explicit base: '{}'",
                                type_name(a[0])));
            if (a.size() == 1 && a[0] && a[0]->kind == TyKind::Big) return t_big();
            return t_int();
        }
        if (n == "float") return t_float();
        if (n == "str") return t_str();
        if (n == "bool") return t_bool();

        if (n == "list") {
            if (a.empty()) return mk_list(fresh_hole());
            return mk_list(iter_elem(a[0], c.line, c.col));
        }
        if (n == "tuple") {
            if (a.empty()) return mk_tuple({});
            return mk_tuple_rest(iter_elem(a[0], c.line, c.col));
        }
        if (n == "set") {
            if (a.empty()) return mk_set(fresh_hole());
            return mk_set(iter_elem(a[0], c.line, c.col));
        }
        if (n == "dict") {
            if (a.size() == 1) {
                TypePtr et = iter_elem(a[0], c.line, c.col);
                if (et && (et->kind == TyKind::Tuple || et->kind == TyKind::List) &&
                    et->sub.size() == 2)
                    return mk_dict(et->sub[0], et->sub[1]);
                if (et && et->kind == TyKind::Str) return mk_dict(t_str(), t_str());
            }
            // dict(x=1, y=2)：关键字构造的键恒为 str，值类型按关键字值求
            // 并集。原来一律退 unknown 双参 —— 静态类型看不见，打印只剩
            // "<dict object>"，下标读回也只能走 unknown 路径（N6）。
            if (a.empty() && !c.keywords.empty()) {
                TypePtr vt = kwt.empty() ? t_unknown() : kwt[0];
                for (size_t i = 1; i < kwt.size(); ++i) {
                    if (!vt || !kwt[i] || type_eq(vt, kwt[i])) continue;
                    vt = join(vt, kwt[i]);
                }
                return mk_dict(t_str(), vt ? vt : t_unknown());
            }
            return mk_dict(t_unknown(), t_unknown());
        }
        if (n == "ord") return t_int();
        if (n == "chr") return t_str();
        if (n == "__pa_capwords") {
            if (!need(1, 1)) return t_str();
            if (a[0] && a[0]->kind != TyKind::Str && a[0]->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("capwords() expects a string, got '{}'", type_name(a[0])));
            return t_str();
        }
        if (n == "__pa_sha256") {
            if (!need(1, 1)) return t_bytes();
            if (a[0] && a[0]->kind != TyKind::Bytes && a[0]->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("sha256() expects bytes, got '{}'", type_name(a[0])));
            return t_bytes();
        }
        if (n == "__pa_sha256_hex") {
            if (!need(1, 1)) return t_str();
            if (a[0] && a[0]->kind != TyKind::Bytes && a[0]->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("sha256() expects bytes, got '{}'", type_name(a[0])));
            return t_str();
        }
        if (n == "__pa_b64encode") {
            if (!need(2, 2)) return t_bytes();
            if (a[0] && a[0]->kind != TyKind::Bytes && a[0]->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("b64encode() expects bytes, got '{}'", type_name(a[0])));
            return t_bytes();
        }
        if (n == "__pa_any_items") {
            // P2 内部 intrinsic（stdlib/pprint.py）：unknown dict 的
            // (key,value) 对列表；未知值不支持方法调用，只能由运行时取表。
            if (!need(1, 1)) return mk_list(t_unknown());
            return mk_list(t_unknown());
        }
        if (n == "__pa_exc_is_a") {
            // P4 内部 intrinsic（stdlib/contextlib.py suppress）：
            // (异常类型名 str, 异常类对象 unknown) -> bool。with 协议的
            // et 是名字串，类对象过 unknown 形参，运行期按名字判父子。
            if (!need(2, 2)) return t_bool();
            return t_bool();
        }
        if (n == "__pa_close") {
            // P4 内部 intrinsic（stdlib/contextlib.py closing）：
            // unknown 值上的零参 .close()（file / 带 close 方法的实例）。
            if (!need(1, 1)) return t_none();
            return t_none();
        }
        if (n == "__pa_tb_frames") {
            // P4 块3 内部 intrinsic（stdlib/traceback.py）：tb 对象 →
            // list[(file, line, func)]，外→内。tb 过 unknown（sys.exc_info
            // 的第三个位置）；None 运行期按空表处理。
            if (!need(1, 1)) return mk_list(mk_tuple({t_str(), t_int(), t_str()}));
            return mk_list(mk_tuple({t_str(), t_int(), t_str()}));
        }
        if (n == "__pa_b64decode") {
            if (!need(1, 1)) return t_bytes();
            if (a[0] && a[0]->kind != TyKind::Bytes && a[0]->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("b64decode() expects bytes, got '{}'", type_name(a[0])));
            return t_bytes();
        }
        if (n == "repr") return t_str();
        if (n == "hex" || n == "bin" || n == "oct") return t_str();
        if (n == "isinstance") {
            if (!need(2, 2)) return t_bool();
            TypePtr tt = a[1];
            std::function<bool(const TypePtr&)> valid =
                [&](const TypePtr& t) -> bool {
                    if (!t || t->kind == TyKind::Unknown) return true;
                    if (t->kind == TyKind::Class || t->kind == TyKind::ClassObj ||
                        t->kind == TyKind::TypeObj)
                        return true;
                    if (t->kind == TyKind::Func && t->fn) {
                        static const std::unordered_set<std::string> types = {
                            "int", "float", "str", "bool", "list", "tuple",
                            "dict", "set", "complex", "object", "range",
                            "bytes", "bytearray", "memoryview", "type",
                            "slice", "frozenset", "property", "classmethod",
                            "staticmethod",
                        };
                        return types.count(t->fn->name) > 0;
                    }
                    if (t->kind == TyKind::Tuple || t->kind == TyKind::List) {
                        for (auto& e : t->sub)
                            if (!valid(e)) return false;
                        return true;
                    }
                    if (t->kind == TyKind::Optional)
                        return valid(t->sub.empty() ? nullptr : t->sub[0]);
                    return false;
                };
            if (!valid(tt))
                err(c.args[1]->line, c.args[1]->col,
                    "isinstance() second argument must be a class or builtin type");
            return t_bool();
        }
        if (n == "callable") {
            if (!need(1, 1)) return t_bool();
            return t_bool();
        }
        if (n == "hash") {
            if (!need(1, 1)) return t_int();
            return t_int();
        }
        if (n == "type") {
            if (!need(1, 3)) return t_unknown();
            // 3 参 type(name, bases, dict)：动态造类，结果类型未知。
            if (a.size() >= 3) return t_unknown();
            return mk_typeobj(a.empty() ? t_unknown() : a[0]);
        }
        if (n == "frozenset") {
            if (a.size() > 1)
                err(c.line, c.col, "frozenset() takes at most one argument");
            return a.empty() ? mk_set(t_unknown()) : mk_set(iter_elem(a[0], c.line, c.col));
        }
        if (n == "id") {
            if (!need(1, 1)) return t_int();
            return t_int();
        }
        if (n == "issubclass") {
            if (!need(2, 2)) return t_bool();
            return t_bool();
        }
        if (n == "hasattr" || n == "getattr" || n == "setattr") {
            // getattr 允许第三个参数（缺省值）。静态布局里属性集合是固定的，
            // 「没有这个字段」时的结果就是缺省值。
            size_t lo = n == "setattr" ? 3 : 2;
            size_t hi = n == "getattr" ? 3 : lo;
            if (!need(lo, hi))
                return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
            auto* ne = c.args[1].get();
            const std::string* an = nullptr;
            if (ne->kind == ExprKind::Constant) {
                auto& cc = static_cast<Constant&>(*ne);
                if (cc.ck == Constant::CK::Str) an = &cc.s;
            }
            TypePtr ot0 = a[0];
            TypePtr ot = ot0;
            if (ot && ot->kind == TyKind::Optional && !ot->sub.empty()) ot = ot->sub[0];
            // 属性名不是字面量：只能查运行期动态属性表（类实例才有槽）。
            // 非类接收者（模块/函数/unknown）静态证明不了有没有这个属性，
            // CPython 最常见的动态判别写法，check 一律放过返回 unknown，
            // 交给 build 阶段决定是否支持。
            if (!an) {
                if (ot && ot->kind == TyKind::Class && ot->cls) ot->cls->needs_dict = true;
                if (ot && ot->kind != TyKind::Unknown && ot->kind != TyKind::Class)
                    return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
                return n == "hasattr" ? t_bool()
                                      : n == "getattr" ? t_unknown() : t_none();
            }
            if (!ot || ot->kind != TyKind::Class || !ot->cls) {
                // hasattr 对已知的内建/容器类型可以静态判定。
                if (n == "hasattr" && ot && ot->kind != TyKind::Unknown) return t_bool();
                // getattr 第三参：已知类型上没有这个成员，结果就是缺省值。
                if (n == "getattr" && a.size() == 3 && ot &&
                    ot->kind != TyKind::Unknown && !type_has_member(ot, *an))
                    return a[2];
                // 模块 / 函数 / 其它内建的反射：静态上证明不了有还是无。
                // 这是 CPython 里最常见的动态判别（hasattr(os,'fork') 之类），
                // check 阶段必须放过——否则标准库一半文件都会判死。运行的
                // 细节留给 build（未知的返回 unknown，已知但没成员的 getattr
                // 给缺省值）。
                unsupported.push_back("reflection on a non-class instance");
                return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
            }
            // 静态布局里没有这个字段：hasattr 结果是「运行时查动态属性表」
            //（类带槽时），不再静态判死。
            if (n == "hasattr") {
                ot->cls->needs_dict = true;

                return t_bool();
            }
            if (!ot->cls->complete && ot->cls != cur_class_) return t_unknown();
            if (const PropertyInfo* p = ot->cls->property(*an)) {
                if (n == "setattr") {
                    // 有 setter 就允许，降级层走 setter 的虚调用。
                    if (!p->owner || p->setter < 0 ||
                        p->setter >= static_cast<int>(p->owner->methods.size()))
                        err(c.line, c.col, std::format("property '{}' has no setter", *an));
                    return t_none();
                }
                if (!p->owner || p->getter < 0 ||
                    p->getter >= static_cast<int>(p->owner->methods.size()))
                    return t_unknown();
                const MethodInfo& gm = p->owner->methods[p->getter];
                return gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret : t_unknown();
            }
            if (const FieldInfo* f = ot->cls->field(*an)) {
                if (n == "getattr") return f->ty;
                return t_none();
            }
            // 静态没有这个字段：setattr 落到运行期动态属性表。
            if (n == "setattr") {
                ot->cls->needs_dict = true;

                return t_none();
            }
            if (ot->cls->method(*an) && n == "getattr") {
                err(c.line, c.col,
                    std::format("getattr() of method '{}' must be called with ()", *an));
                return t_unknown();
            }
            if (ot->cls->is_open()) return t_unknown();
            if (n == "getattr" && a.size() == 3) {
                ot->cls->needs_dict = true;

                return a[2];
            }
            // 类已开启动态属性（本模块或定义模块 setattr 过）：字面量名
            // 的 getattr 也落运行期 dict，与直接读 args.x 同口径。
            if (n == "getattr" && ot->cls->needs_dict) return t_unknown();
            err(c.line, c.col,
                std::format("'{}' object has no field '{}'", ot->cls->name, *an));
            return n == "getattr" ? t_unknown() : t_none();
        }
        if (n == "any" || n == "all") return t_bool();
        if (n == "round") {
            if (a.empty()) return t_unknown();
            if (a.size() > 1 && has_big_value(a[0]))
                err(c.line, c.col, "round(big, ndigits) is not supported");
            if (a[0] && a[0]->kind == TyKind::Float)
                return a.size() >= 2 ? t_float() : t_int();
            return a[0];
        }
        if (n == "divmod") {
            if (std::any_of(a.begin(), a.end(), has_big_value))
                err(c.line, c.col, "divmod() with big is not supported");
            if (a.size() >= 2 && is_num(a[0]) && is_num(a[1])) {
                auto q = promote(a[0], a[1]);
                return mk_tuple({q, q});
            }
            return mk_tuple({t_unknown(), t_unknown()});
        }
        if (n == "pow") {
            if (a.size() == 3 && std::any_of(a.begin(), a.end(), has_big_value))
                err(c.line, c.col, "three-argument pow() with big is not supported");
            return a.size() >= 2 ? promote(a[0], a[1]) : t_unknown();
        }
        if (n == "input") return t_str();
        if (n == "type") return t_unknown();
        if (n == "next") {
            return a.empty() ? t_unknown() : iter_elem(a[0], c.line, c.col);
        }
        if (n == "iter") {
            if (a.empty()) return t_unknown();
            if (a[0] && a[0]->kind == TyKind::Class && a[0]->cls) {
                if (auto* m = a[0]->cls->method("__iter__")) {
                    TypePtr rt = m->call_sig ? m->call_sig->ret : nullptr;
                    if (m->decorated && m->decorated_ty &&
                        m->decorated_ty->kind == TyKind::Func && m->decorated_ty->fn)
                        rt = m->decorated_ty->fn->ret;
                    if (rt) return rt;
                }
            }
            return a[0];
        }
        if (n == "complex") {
            if (a.size() > 2)
                err(c.line, c.col, std::format("complex() takes at most 2 arguments, got {}", a.size()));
            return t_complex();
        }
        if (n == "open") {
            static const char* kParams[] = {"file",      "mode",    "buffering", "encoding",
                                            "errors",    "newline", "closefd",   "opener"};
            constexpr size_t kNP = 8;
            std::vector<TypePtr> bound(kNP, nullptr);
            std::vector<Expr*> bexpr(kNP, nullptr);
            if (a.size() > kNP) {
                err(c.line, c.col, "open() takes at most 8 arguments");
                return t_file();
            }
            for (size_t i = 0; i < a.size(); ++i) {
                bound[i] = a[i];
                bexpr[i] = c.args[i].get();
            }
            for (size_t i = 0; i < c.keywords.size(); ++i) {
                const std::string& k = c.keywords[i].arg;
                Expr* ve = c.keywords[i].value.get();
                if (k.empty()) {
                    // open(**kw) 动态关键字：conservative 忽略。
                    continue;
                }
                int idx = -1;
                for (int p = 0; p < static_cast<int>(kNP); ++p)
                    if (k == kParams[p]) idx = p;
                if (idx < 0) {
                    err(ve->line, ve->col,
                        std::format("open() got an unexpected keyword argument '{}'", k));
                    continue;
                }
                if (bound[static_cast<size_t>(idx)]) {
                    err(ve->line, ve->col,
                        std::format("open() got multiple values for argument '{}'", k));
                    continue;
                }
                bound[static_cast<size_t>(idx)] = kwt[i];
                bexpr[static_cast<size_t>(idx)] = ve;
            }
            if (!bound[0]) err(c.line, c.col, "open() missing required argument 'file'");
            auto str_or_none = [](const TypePtr& t) {
                return !t || is_unknown(t) || t->kind == TyKind::Str || t->kind == TyKind::None;
            };
            auto int_like = [](const TypePtr& t) {
                return !t || is_unknown(t) || t->kind == TyKind::Int || t->kind == TyKind::Bool;
            };
            if (bound[0] && !is_unknown(bound[0]) && bound[0]->kind != TyKind::Str &&
                !is_int_like(bound[0]))
                err(c.line, c.col,
                    std::format("open() file must be a path string or file descriptor, got '{}'",
                                type_name(bound[0])));
            if (!str_or_none(bound[1])) err(c.line, c.col, "open() mode must be a string");
            if (!int_like(bound[2]))
                err(c.line, c.col, "open() buffering must be an integer");
            if (!str_or_none(bound[3]))
                err(c.line, c.col, "open() encoding must be a string or None");
            if (!str_or_none(bound[4]))
                err(c.line, c.col, "open() errors must be a string or None");
            if (!str_or_none(bound[5]))
                err(c.line, c.col, "open() newline must be a string or None");
            if (!int_like(bound[6])) err(c.line, c.col, "open() closefd must be a bool");
            if (bound[7]) {
                err(c.line, c.col, "open() opener= is not supported by the code generator");
                return t_file();
            }
            return t_file();
        }
        if (n == "format") {
            if (a.size() != 2 && !(a.size() == 1 && a[0] && a[0]->kind == TyKind::Big))
                err(c.line, c.col, "format() takes exactly two arguments");
            return t_str();
        }
        if (n == "slice") {
            // 独立 slice() 构造：脱糖后 xs[slice(a,b)] 已折成切片语法，
            // 走到这里说明当独立值用。返回 unknown 让 check 通过。
            return t_unknown();
        }        if (n == "vars") {
            if (!need(1, 1)) return mk_dict(t_str(), t_unknown());
            TypePtr ot = a[0];
            if (ot && ot->kind == TyKind::Class && ot->cls)
                return mk_dict(t_str(), t_unknown());
            if (!is_unknown(ot))
                err(c.args[0]->line, c.args[0]->col,
                    std::format("vars() argument must be a class instance, got '{}'",
                                type_name(ot)));
            return mk_dict(t_str(), t_unknown());
        }
        if (n == "dir") {
            if (a.size() > 1) err(c.line, c.col, "dir() takes at most one argument");
            return mk_list(t_str());
        }
        if (n == "super" ||
            n == "bytearray" || n == "object" || n == "complex" || n == "memoryview") {
            unsupported.push_back(std::format("runtime builtin '{}'", n));
            if (n == "bytearray") return a.empty() ? t_unknown() : mk_list(t_unknown());
            if (n == "format") return t_str();
            return t_unknown();
        }
        if (n == "exec" || n == "eval" || n == "globals" || n == "locals" || n == "compile" ||
            n == "__import__") {
            // 只登记，不报错：能否支持由代码生成阶段决定，类型层面只能返回 unknown
            unsupported.push_back(std::format("dynamic builtin '{}'", n));
            return t_unknown();
        }
        if (n == "map" || n == "filter") {
            if (a.size() >= 2) return mk_list(iter_elem(a[1], c.line, c.col));
            return mk_list(t_unknown());
        }
        if (n == "bytes") {
            if (a.empty()) return t_bytes();
            if (a.size() > 2) {
                err(c.line, c.col,
                    std::format("bytes() expects at most 2 arguments, got {}", a.size()));
                return t_bytes();
            }
            // 第二参是编码/错误（str/None）只影响语义，不影响类型；
            // 第一参支持 bytes/int/list[int]/tuple[int]/生成器/unknown。
            TypePtr at = a[0];
            bool ok = at && (at->kind == TyKind::Bytes || at->kind == TyKind::Int ||
                             at->kind == TyKind::Unknown || at->kind == TyKind::None);
            if (!ok && at && (at->kind == TyKind::List || at->kind == TyKind::Tuple ||
                              at->kind == TyKind::Generator || at->kind == TyKind::Range)) {
                auto e = at->sub.empty() ? TypePtr() : at->sub[0];
                ok = e && (e->kind == TyKind::Int || e->kind == TyKind::Unknown);
            }
            if (!ok && a.size() == 2 && at && at->kind == TyKind::Str)
                ok = true;  // bytes(str, encoding)：编码为 bytes
            if (!ok)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("bytes() argument must be a bytes-like or iterable of int, got '{}'",
                                type_name(at)));
            return t_bytes();
        }
        (void)need;
        return t_unknown();
    }

}  // namespace pyadamas
