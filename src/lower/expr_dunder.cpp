// lower/expr_dunder.cpp —— dunder 协议、类型转换与 f-string
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::dunder_str(VReg v, TypePtr ty, bool prefer_repr) {
    // repr 语义（容器元素、repr()、!r）只用 __repr__；str 语义先 __str__，
    // 没有再回退 __repr__。和 CPython 的默认关系一致。
    if (prefer_repr) return dunder_call(v, ty, "__repr__", {}, t_str());
    VReg s = dunder_call(v, ty, "__str__", {}, t_str());
    if (s != kNoReg) return s;
    return dunder_call(v, ty, "__repr__", {}, t_str());
}

VReg Lowerer::dunder_len(VReg v, TypePtr ty) {
    VReg ln = dunder_call(v, ty, "__len__", {}, t_int());
    if (ln != kNoReg) return ln;
    // __len__ 返回 bool 也合法（bool 是 int 的子类，check_dunder_returns
    // 同样放行）。以前这里只认 int，bool 版本被当成「没有 __len__」，
    // 于是 len() 直接报错、真值判断退回指针判空——静默算错。
    VReg bl = dunder_call(v, ty, "__len__", {}, t_bool());
    if (bl != kNoReg) return retype(bl, t_int());
    return kNoReg;
}

VReg Lowerer::dunder_bool(VReg v, TypePtr ty) {
    VReg b = dunder_call(v, ty, "__bool__", {}, t_bool());
    if (b != kNoReg) return b;
    // __bool__ 返回 int 时按非零判真（类型检查层把 bool/int 视为兼容）。
    // 以前只认 bool，int 版本被当成「没有 __bool__」，真值退回 __len__ /
    // 指针判空，bool(v) / not v 会给出相反的结果。
    VReg i = dunder_call(v, ty, "__bool__", {}, t_int());
    if (i != kNoReg) {
        Inst c;
        c.op = IrOp::Cmp;
        c.cop = CmpKind::Ne;
        c.a = i;
        c.b = const_int(0);
        return def(t_bool(), std::move(c));
    }
    return kNoReg;
}

VReg Lowerer::lower_dunder_eq(VReg l, TypePtr lt, VReg r, TypePtr rt, bool ne) {
    auto one = [&](VReg self, TypePtr st, VReg other, TypePtr ot) -> VReg {
        if (!st || st->kind != TyKind::Class || !st->cls) return kNoReg;
        std::vector<TypePtr> tys{ot};
        if (ne) {
            VReg v = dunder_call(self, st, "__ne__", {other}, t_bool(), &tys);
            if (v != kNoReg) return v;
        }
        VReg eq = dunder_call(self, st, "__eq__", {other}, t_bool(), &tys);
        if (eq == kNoReg) return kNoReg;
        if (!ne) return eq;
        Inst un;
        un.op = IrOp::Un;
        un.uop = UnKind::Not;
        un.a = eq;
        return def(t_bool(), std::move(un));
    };
    // 先左后右：左边类没有 __eq__ 时，右边的实现仍然可以接收它。
    VReg res = one(l, lt, r, rt);
    if (res != kNoReg) return res;
    return one(r, rt, l, lt);
}

VReg Lowerer::lower_dunder_cmp(VReg l, TypePtr lt, VReg r, TypePtr rt, CmpOp op) {
    struct Ref {
        CmpOp op;
        const char* fwd;
        const char* rev;
    };
    static const Ref tbl[] = {
        {CmpOp::Lt, "__lt__", "__gt__"},
        {CmpOp::Le, "__le__", "__ge__"},
        {CmpOp::Gt, "__gt__", "__lt__"},
        {CmpOp::Ge, "__ge__", "__le__"},
    };
    const Ref* ref = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            ref = &e;
            break;
        }
    if (!ref) return kNoReg;
    std::vector<TypePtr> ltys{rt};
    std::vector<TypePtr> rtys{lt};
    VReg v = dunder_call(l, lt, ref->fwd, {r}, t_bool(), &ltys);
    if (v != kNoReg) return v;
    v = dunder_call(r, rt, ref->rev, {l}, t_bool(), &rtys);
    if (v != kNoReg) return v;
    return kNoReg;
}

VReg Lowerer::lower_dunder_binop(VReg l, TypePtr lt, VReg r, TypePtr rt, Tok op, bool aug) {
    struct Ref {
        Tok op;
        const char* fwd;
        const char* rev;
        const char* iaug;
    };
    static const Ref tbl[] = {
        {Tok::PLUS, "__add__", "__radd__", "__iadd__"},
        {Tok::MINUS, "__sub__", "__rsub__", "__isub__"},
        {Tok::STAR, "__mul__", "__rmul__", "__imul__"},
        {Tok::SLASH, "__truediv__", "__rtruediv__", "__itruediv__"},
        {Tok::DOUBLESLASH, "__floordiv__", "__rfloordiv__", "__ifloordiv__"},
        {Tok::PERCENT, "__mod__", "__rmod__", "__imod__"},
        {Tok::DOUBLESTAR, "__pow__", "__rpow__", "__ipow__"},
    };
    if (aug) op = aug_to_bin(op);
    const Ref* ref = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            ref = &e;
            break;
        }
    if (!ref) return kNoReg;
    std::vector<TypePtr> ltys{rt};
    std::vector<TypePtr> rtys{lt};
    VReg v = kNoReg;
    if (aug) {
        v = dunder_call(l, lt, ref->iaug, {r}, nullptr, &ltys);
        if (v != kNoReg) return v;
    }
    v = dunder_call(l, lt, ref->fwd, {r}, nullptr, &ltys);
    if (v != kNoReg) return v;
    // 增量赋值同样可能落到右操作数的反射版（int += 类实例）。
    v = dunder_call(r, rt, ref->rev, {l}, nullptr, &rtys);
    if (v != kNoReg) return v;
    return kNoReg;
}

VReg Lowerer::truth_bool(VReg v, TypePtr ty) {
    // CPython：先 __bool__，没有再按 __len__ 是否非零；都没有时按指针判空。
    VReg b = dunder_bool(v, ty);
    if (b != kNoReg) return b;
    // P3 big：真值按值（nd != 0），不能按指针判空（永生字面量地址恒非零）。
    if (ty && ty->kind == TyKind::Big) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::BigTruth);
        in.args = {v};
        return def(t_bool(), std::move(in));
    }
    // **unknown 必须走运行期真值**：机器字可能是整数、空串、空容器、None。
    // 直接把它当布尔用（branch 只测非零）会让空串/空容器恒真 ——
    // filter(None, 异构列表) 保留空串就是这个洞（N7）。
    if (!ty || ty->kind == TyKind::Unknown)
        return call_runtime(RtFn::TruthAny, t_bool(), {v});
    VReg ln = dunder_len(v, ty);
    if (ln == kNoReg) return v;
    Inst c;
    c.op = IrOp::Cmp;
    c.cop = CmpKind::Gt;
    c.a = ln;
    c.b = const_int(0);
    return def(t_bool(), std::move(c));
}

VReg Lowerer::class_default_str(TypePtr ty) {
    std::string name = ty && ty->cls ? ty->cls->name : "object";
    return const_str("<" + name + " object>");
}

VReg Lowerer::convert_to(VReg v, TypePtr from, TypePtr to) {
    if (!to) return v;
    if (from && type_eq(from, to)) return v;
    // ── unknown 域的进出（0.324）────────────────────────────────
    // float 进入 unknown 时装箱成 CPYFLOAT（8 字节 payload），这样
    // int / float / 堆指针在 unknown 里可以靠「堆探针 + kind」分开。
    // 反向（unknown → float/int/bool/str）解盒或走保守分派。
    // 这些分支只在静态类型是 unknown 的表达式上发射，热路径零调用。
    if (to->kind == TyKind::Unknown && from && from->kind == TyKind::Float) {
        VReg r = call_runtime(RtFn::AnyFromF64, t_unknown(), {v});
        push_temp_any_heap(r);
        return r;
    }
    if (from && from->kind == TyKind::Unknown) {
        if (to->kind == TyKind::Float)
            return call_runtime(RtFn::AnyToF64, t_float(), {v});
        if (to->kind == TyKind::Int)
            return call_runtime(RtFn::AnyToI64, t_int(), {v});
        if (to->kind == TyKind::Bool)
            return call_runtime(RtFn::TruthAny, t_bool(), {v});
        if (to->kind == TyKind::Str) {
            // str(unknown)：堆字符串原样、CPYFLOAT 按 double、容器按 repr、
            // 其余按整数。与 print(unknown) 的口径一致。
            VReg s = call_runtime(RtFn::StrAny, t_str(), {v});
            push_temp(s);
            st_->fresh_vregs.insert(s);
            return s;
        }
    }
    // str(类型对象)："<class 'name'>"（值本身就是类型名字符串）。
    if (from && from->kind == TyKind::TypeObj && to->kind == TyKind::Str) {
        VReg a = call_runtime(RtFn::StrConcat, t_str(), {const_str("<class '"), v});
        push_temp(a);
        VReg b = call_runtime(RtFn::StrConcat, t_str(), {a, const_str("'>")});
        push_temp(b);
        return b;
    }
    // Path → str：拿底层路径字符串
    if (from && from->kind == TyKind::Path && to->kind == TyKind::Str) {
        VReg s = call_runtime(RtFn::PathStr, t_str(), {v});
        push_temp(s);
        return s;
    }
    // 类实例的字符串/布尔转换走协议 dunder；没有实现就用默认行为。
    if (from && from->kind == TyKind::Class) {
        if (to->kind == TyKind::Str) {
            VReg s = dunder_str(v, from, false);
            return s != kNoReg ? s : class_default_str(from);
        }
        if (to->kind == TyKind::Bool) {
            VReg b = dunder_bool(v, from);
            if (b != kNoReg) return b;
            VReg ln = dunder_len(v, from);
            if (ln != kNoReg) {
                Inst c;
                c.op = IrOp::Cmp;
                c.cop = CmpKind::Gt;
                c.a = ln;
                c.b = const_int(0);
                return def(t_bool(), std::move(c));
            }
        }
    }
    if (to->kind == TyKind::Bool && from && from->kind == TyKind::Complex) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexTruth);
        in.args = {v};
        return def(t_bool(), std::move(in));
    }
    if (to->kind == TyKind::Complex) {
        if (from && from->kind == TyKind::Complex) return v;
        VReg f = (from && from->kind == TyKind::Float) ? v
                                                       : convert_to(v, from, t_float());
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexNew);
        in.args = {f, const_float(0.0)};
        return def_heap(t_complex(), std::move(in));
    }
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Complex) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexStr);
        in.args = {v};
        VReg s = def_heap(t_str(), std::move(in));
        return s;
    }
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Exception) {
        VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
        push_temp(s);
        return s;
    }
    // 原生对象没有 __str__ 协议，按和其他内置对象一致的默认文本处理。
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Random)
        return const_str("<random.Random object>");
    if (to->kind == TyKind::Str && from && from->kind == TyKind::CompletedProcess)
        return const_str("<subprocess.CompletedProcess object>");
    if (to->kind == TyKind::Str && from && from->kind == TyKind::StatResult)
        return const_str("<os.stat_result object>");
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Popen)
        return const_str("<subprocess.Popen object>");
    // ── P3 big 的转换（docs/numeric-types-design.md §1）─────────────
    // 进 big：int/定宽/bool 加宽（pa_big_from_i64）、float 按 int(float)
    // 截断（pa 格口径）、str 按十进制解析、unknown 运行期分派。
    // ── 元组元素级转换（0.359）───────────────────────────────────
    // 元组字面量按**推断**类型构造（`(1, 2)` 是 tuple[int, int]），边界上
    // 目标类型元素不同时必须逐元素重建。之前这里是机器字直通：
    //   def f() -> tuple[float, float]: return 1, 2   → (5e-324, 1e-323)
    //   def f() -> tuple[int, big, int]: return 0,0,0 → 取 b 即段错误
    // 重建的新元组自己持有元素引用；源值不动（它是调用方的，照旧由临时栈
    // 或槽释放），所以没有多减/漏减。
    if (from && to->kind == TyKind::Tuple && from->kind == TyKind::Tuple &&
        !from->variadic && !to->variadic && !from->sub.empty() &&
        from->sub.size() == to->sub.size()) {
        bool differs = false;
        for (size_t i = 0; i < from->sub.size(); ++i)
            if (!type_eq(from->sub[i], to->sub[i])) {
                differs = true;
                break;
            }
        if (differs) {
            std::vector<VReg> vals;
            std::vector<TypePtr> tys;
            vals.reserve(from->sub.size());
            tys.reserve(from->sub.size());
            for (size_t i = 0; i < from->sub.size(); ++i) {
                vals.push_back(seq_elem_at(v, from, static_cast<int64_t>(i)));
                tys.push_back(from->sub[i]);
            }
            return build_seq_vregs(std::move(vals), std::move(tys), to, true);
        }
    }
    if (to->kind == TyKind::Big) {
        if (from && from->kind == TyKind::Big) return v;
        if (from && from->kind == TyKind::Unknown) {
            VReg r = call_runtime(RtFn::AnyToBig, t_big(), {v});
            push_temp(r);
            st_->fresh_vregs.insert(r);
            return r;
        }
        RtFn fn = RtFn::BigFromI64;
        if (from && ir_is_float(from)) fn = RtFn::BigFromF64;
        if (from && is_unsigned_int(from)) fn = RtFn::BigFromU64;
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {v};
        if (from && from->kind == TyKind::Str) {
            in.imm = static_cast<int64_t>(RtFn::BigFromStr);
            in.args = {v, const_int(10)};
        }
        return def_heap(t_big(), std::move(in));
    }
    // 出 big：int/定宽是 cast 截断（低 64 位回绕 + 宽度正常化）、float
    // 正确舍入、str 十进制、bool 真值。big → unknown 直通（堆指针）。
    if (from && from->kind == TyKind::Big) {
        if (to->kind == TyKind::Unknown) return v;
        if (to->kind == TyKind::Int || is_fixed_int(to)) {
            VReg w = call_runtime(RtFn::BigToI64, t_int(), {v});
            if (is_fixed_int(to)) return convert_to(w, t_int(), to);
            return w;
        }
        if (to->kind == TyKind::Float) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::BigToF64);
            in.args = {v};
            return def(t_float(), std::move(in));
        }
        if (to->kind == TyKind::Str) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::BigStr);
            in.args = {v};
            return def_heap(t_str(), std::move(in));
        }
        if (to->kind == TyKind::Bool) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::BigTruth);
            in.args = {v};
            return def(t_bool(), std::move(in));
        }
    }
    Inst in;
    in.op = IrOp::Convert;
    in.a = v;
    VReg r = def(to, std::move(in));
    // 数值转换产出标量；转成字符串才是一个新的堆对象。
    //
    // **push_temp 必须配对 fresh_vregs.insert。** 漏登记时容器构造/推导式
    // 走 retain_elem 分支（多一次 pa_rc_inc + 语句水位上的 dec），持有
    // 方拿不到 take 快路径。40_map（`map(str, ...)`）每元素因此多一组
    // temp→inc→append→cleanup→dec，pa_rc_cleanup 单项就占 6.3%。
    if (to->kind == TyKind::Str && (!from || from->kind != TyKind::Str)) {
        push_temp(r);
        st_->fresh_vregs.insert(r);
    }
    return r;
}

// 存储位置（槽 / 全局 / 形参 / 列表元素）有静态类型，值可能落在另一个
// 数值域上——典型是 `x: float = 1`。不补这条 convert，整数 1 的位模式
// 会被当成 double 读出来（5e-324），而且错得毫无声响。
// 只在数值域之间转：指针类之间的差异属于类型检查该报的错，这里不猜。
VReg Lowerer::coerce(VReg v, TypePtr from, TypePtr to) {
    if (!from || !to) return v;
    if (type_eq(from, to)) return v;
    // unknown 与 float 的互转不是机器字直通：float -> unknown 要装箱
    // （CPYFLOAT），unknown -> float 要解盒。放在数值域判断之前，因为
    // unknown 不属于 num 域。其余「unknown 与具体非数值类型」仍按原样
    // 直通（指针/整数机器字），保持旧行为。
    if (to->kind == TyKind::Unknown && from->kind == TyKind::Float)
        return convert_to(v, from, to);
    // **只做 float 的解盒**：unknown -> int/bool 保持裸机器字直通。
    // 这两类目标绝大多数是下标/计数/长度（unknown 实际上是 int），
    // 每次插一条解盒调用会把 fannkuch 这类热循环拖慢；显式 int(x)/
    // bool(x) 在各自的内置调用点单独处理。
    if (from->kind == TyKind::Unknown && to->kind == TyKind::Float)
        return convert_to(v, from, to);
    // 定宽整数（P1）：**目标**是定宽整数时必须发转换（按宽度截断 +
    // 符号/零扩展），不能像 int/bool 那样直通。这是「宽→窄」的唯一
    // 落地指令；类型检查已经保证它只出现在允许的位置（字面量、cast、
    // 复合赋值、按 §1.2 提升后的运算两侧）。
    if (is_fixed_int(to)) return convert_to(v, from, to);
    // 定宽 → float 需要 cvtsi2sd（u64 还要走无符号转换路径）。
    if (to->kind == TyKind::Float && is_fixed_int(from)) return convert_to(v, from, to);
    // P3 big：进出 big 都不是机器字直通（堆指针 ↔ 数值要调运行时）。
    // big → unknown 例外：unknown 的堆值表示就是裸指针，直通。
    if (to->kind == TyKind::Big || from->kind == TyKind::Big) {
        if (from->kind == TyKind::Big && to->kind == TyKind::Unknown) return v;
        return convert_to(v, from, to);
    }
    // complex 不是机器字直通：int/float → complex 要构造 16 字节复数对象
    // （pa_complex_new）。三元/if 赋值按 join 出来的 complex 落槽时若塞
    // 裸整数，打印时会把 1 当复数指针解引用（实测 c=True 的
    // 1 if c else 1j 直接段错误）。
    // 只处理「目标是 complex」：int/float/bool → complex 要构造 16 字节
    // 复数对象（pa_complex_new）。反过来 complex → unknown 必须保持机器字
    // 直通（复数就是堆指针），走 convert_to 的通用 Convert 会把它当数值
    // 转换，unknown 参数收到坏值（42_match_corners 的 case 3j 实测失效）。
    if (to->kind == TyKind::Complex) return convert_to(v, from, to);
    // 元组/列表的元素类型转换不是机器字直通（见 convert_to 的重建分支）。
    if (from->kind == TyKind::Tuple && to->kind == TyKind::Tuple &&
        !to->variadic && !from->variadic && from->sub.size() == to->sub.size())
        return convert_to(v, from, to);
    auto num = [](TyKind k) {
        return is_int_kind(k) || k == TyKind::Bool || k == TyKind::Float;
    };
    if (!num(from->kind) || !num(to->kind)) return v;
    bool from_int = from->kind != TyKind::Float;
    bool to_int = to->kind != TyKind::Float;
    if (from_int == to_int) return v;  // int/bool/定宽→i64：位模式已是规范值
    return convert_to(v, from, to);
}

VReg Lowerer::lower_joined(JoinedStr& j) {
    VReg acc = const_str("");
    bool first = true;
    for (auto& part : j.values) {
        VReg piece;
        if (part->kind == ExprKind::Constant) {
            auto& c = static_cast<Constant&>(*part);
            if (c.ck != Constant::CK::Str) {
                err(part->line, part->col, "non-string literal inside f-string");
                continue;
            }
            if (c.s.empty()) continue;
            piece = const_str(c.s);
        } else if (part->kind == ExprKind::FormattedValue) {
            auto& fv = static_cast<FormattedValue&>(*part);
            TypePtr vt = tc_.type_of(fv.value.get());
            VReg raw = expr(*fv.value);
            if (fv.conversion == 's') {
                if (!vt || vt->kind != TyKind::Str) raw = convert_to(raw, vt, t_str());
            } else if (fv.conversion == 'r' || fv.conversion == 'a') {
                if (vt && vt->kind == TyKind::Str) {
                    Inst in;
                    in.op = IrOp::CallRt;
                    in.imm = static_cast<int64_t>(RtFn::ReprStr);
                    in.args = {raw};
                    raw = def_heap(t_str(), std::move(in));
                } else if (vt && vt->kind == TyKind::Class) {
                    // !r 用 __repr__；没有就退回默认对象描述。
                    VReg s = dunder_str(raw, vt, true);
                    raw = s != kNoReg ? s : class_default_str(vt);
                } else {
                    raw = convert_to(raw, vt, t_str());
                }
            }
            VReg spec = const_str("");
            if (fv.format_spec)
                spec = lower_joined(static_cast<JoinedStr&>(*fv.format_spec));
            // __format__ 已经用同一个 spec 产出最终字符串：**不能再套一次**
            // 格式化。之前这里把 vt 改成 str 后仍走 FmtStr，等价于拿数值型
            // 说明符去格式化一个字符串 —— f"{d:.0f}" 直接 ValueError，
            // f"{d:<10}" 则重复补白（width 幂等所以看不出来）。
            bool already_formatted = false;
            // __format__ 已经用同一个 spec 产出最终字符串：**不能再套一次**
            // 格式化。之前这里把 vt 改成 str 后仍走 FmtStr，等价于拿数值型
            // 说明符去格式化一个字符串 —— f"{d:.0f}" 直接 ValueError，
            // f"{d:<10}" 则重复补白（width 幂等所以看不出来）。
            // 没有 !s/!r 的普通格式化：有 __format__ 就用它（和 format() 一致），
            // 否则按 __str__ 转成字符串再走字符串的格式说明。
            // （无转换时 conversion 是 -1。）
            if (fv.conversion < 0 && vt && vt->kind == TyKind::Class) {
                VReg r = kNoReg;
                if (vt->cls && vt->cls->method("__format__"))
                    r = dunder_call(raw, vt, "__format__", {spec}, t_str());
                if (r != kNoReg) {
                    raw = r;
                    already_formatted = true;
                } else {
                    VReg s = dunder_str(raw, vt, false);
                    raw = s != kNoReg ? s : class_default_str(vt);
                }
                vt = t_str();
            }

            auto fmt_call = [&](RtFn fn) {
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(fn);
                in.args = {raw, spec};
                return def_heap(t_str(), std::move(in));
            };
            TypePtr ct = vt;
            if (fv.conversion == 's' || fv.conversion == 'r' || fv.conversion == 'a')
                ct = t_str();
            if (already_formatted) {
                // __format__ 已产出最终字符串，直接拼进结果（不能再套 FmtStr）。
                piece = raw;
            } else if (!ct || ct->kind == TyKind::Unknown) {
                VReg s = convert_to(raw, ct, t_str());
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::FmtStr);
                in.args = {s, spec};
                piece = def_heap(t_str(), std::move(in));
            } else {
                switch (ct->kind) {
                    case TyKind::Bool: piece = fmt_call(RtFn::FmtBool); break;
                    case TyKind::Int:
                    // 定宽整数（P1）：窄宽度/无符号 8..32 的值在槽内恒非负
                    // 或已是正确有符号值，格式化走 i64 版本即可。
                    case TyKind::I8:
                    case TyKind::I16:
                    case TyKind::I32:
                    case TyKind::U8:
                    case TyKind::U16:
                    case TyKind::U32: piece = fmt_call(RtFn::FmtI64); break;
                    case TyKind::U64: piece = fmt_call(RtFn::FmtU64); break;
                    case TyKind::Big: piece = fmt_call(RtFn::BigFmt); break;
                    case TyKind::Float: piece = fmt_call(RtFn::FmtF64); break;
                    case TyKind::Str: piece = fmt_call(RtFn::FmtStr); break;
                    case TyKind::Complex: piece = fmt_call(RtFn::FmtComplex); break;
                    case TyKind::Exception: {
                        VReg ev = call_runtime(RtFn::ExcValueStr, t_str(), {raw});
                        push_temp(ev);
                        Inst in;
                        in.op = IrOp::CallRt;
                        in.imm = static_cast<int64_t>(RtFn::FmtStr);
                        in.args = {ev, spec};
                        piece = def_heap(t_str(), std::move(in));
                        break;
                    }
                    default:
                        err(part->line, part->col,
                            std::format("f-string cannot format '{}'", type_name(ct)));
                        continue;
                }
            }
        } else {
            err(part->line, part->col, "unsupported f-string part");
            continue;
        }

        if (first) {
            acc = piece;
            first = false;
        } else {
            acc = call_runtime(RtFn::StrConcat, t_str(), {acc, piece});
            push_temp(acc);
        }
    }
    return acc;
}

// 按签名重排实参：位置实参、关键字实参、缺省值、形参类型的数值转换。
// info 为空（签名未知）时按源码顺序直传，关键字参数直接报错。



}  // namespace pyadamas
