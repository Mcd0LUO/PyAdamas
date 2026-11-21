// lower_expr_ops.cpp —— 常量、格式化、算术/比较与下标
#include "lower_impl.h"

namespace pyadamas {

// unknown 域动态算术/比较的操作码：与 runtime/rt_any.c 的 PA_ANY_* 逐字对应。
static int any_arith_op(Tok op) {
    switch (op) {
        case Tok::PLUS: return 0;
        case Tok::MINUS: return 1;
        case Tok::STAR: return 2;
        case Tok::SLASH: return 3;
        case Tok::DOUBLESLASH: return 4;
        case Tok::PERCENT: return 5;
        default: return -1;
    }
}

static int any_cmp_op(CmpOp op) {
    switch (op) {
        case CmpOp::Eq: return 0;
        case CmpOp::Ne: return 1;
        case CmpOp::Lt: return 2;
        case CmpOp::Le: return 3;
        case CmpOp::Gt: return 4;
        case CmpOp::Ge: return 5;
        default: return -1;
    }
}

// 动态算术/比较的实参静态域描述符（与 rt_any.c 的 any_decode_typed 对应）。
// 静态域已知的实参让运行时跳过堆探针；float 以位模式放进整型实参寄存器
// （movq xmm->gpr），不改变任何代码生成形态。
static int64_t any_desc(const TypePtr& t) {
    if (!t || t->kind == TyKind::Unknown) return 'x';
    switch (t->kind) {
        case TyKind::Float: return 'f';
        case TyKind::Str: return 's';
        case TyKind::Int:
        case TyKind::Bool:
        case TyKind::None: return 'i';
        default: return ir_is_heap(t) ? 'o' : 'i';
    }
}

VReg Lowerer::lower_constant(Constant& c) {
    switch (c.ck) {
        case Constant::CK::None: return const_none();
        case Constant::CK::Bool: return const_bool(c.b);
        case Constant::CK::Int: {
            // 定宽标注 / cast 把字面量的静态类型定成了 i8/u8/...（类型检查
            // 已验过值域），常量必须按那个类型生成：codegen 的正常化与
            // 无符号判定都看 vreg 类型。默认 int 路径逐字不变。
            TypePtr ct = tc_.type_of(&c);
            if (ct && is_fixed_int(ct)) return const_int_ty(c.i, ct);
            // P3：big 标注上下文里的普通整数字面量直接发永生 big 常量。
            if (ct && ct->kind == TyKind::Big) return const_big(std::to_string(c.i));
            return const_int(c.i);
        }
        case Constant::CK::Big: return const_big(c.s);   // P3：超 u64 字面量
        case Constant::CK::Float: return const_float(c.f);
        case Constant::CK::Complex: {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexNew);
            in.args = {const_float(0.0), const_float(c.im)};
            return def_heap(t_complex(), std::move(in));
        }
        case Constant::CK::Str: return const_str(c.s);
        case Constant::CK::Bytes: return const_bytes(c.s);
        case Constant::CK::Ellipsis:
            err(c.line, c.col, "Ellipsis is not supported by the code generator");
            return const_none();
    }
    return const_none();
}

VReg Lowerer::lower_percent_format(BinOp& b, TypePtr lt, TypePtr ty) {
    TypePtr rt = tc_.type_of(b.right.get());
    std::vector<TypePtr> argtys;
    // CPython 把固定长度 tuple 当作参数列表（含 0/1 个元素）；
    // list 仍按单个值处理，暂不支持。
    bool tuple_args = rt && rt->kind == TyKind::Tuple && !rt->variadic;
    if (tuple_args) argtys = rt->sub;
    else argtys.push_back(rt);
    std::string desc;
    for (auto& t : argtys) {
        // 定宽整数（P1）：u64 用无符号描述符 'u'（≥ 2^63 的 %d 不能带负号），
        // 其余定宽按 'i'（槽内值已是正确有符号/非负值）。
        if (t && t->kind == TyKind::U64) desc.push_back('u');
        else if (t && is_int_kind(t->kind)) desc.push_back('i');
        else if (t && t->kind == TyKind::Bool) desc.push_back('b');
        else if (t && t->kind == TyKind::Float) desc.push_back('f');
        else if (t && (t->kind == TyKind::Str || t->kind == TyKind::Bytes)) desc.push_back('s');
        else if (t && t->kind == TyKind::None) desc.push_back('n');
        else desc.push_back('x');
    }
    VReg pat = expr(*b.left);
    VReg args;
    if (tuple_args) {
        args = expr(*b.right);
    } else {
        VReg v = expr(*b.right);
        TypePtr vt = rt ? rt : t_unknown();
        TypePtr list_ty = mk_list(vt);
        int kind = list_kind_filled(list_ty);
        retain_elem(v, kind);
        Inst mk;
        mk.op = IrOp::MakeList;
        mk.imm = kind;
        mk.args.push_back(v);
        args = def_heap(list_ty, std::move(mk));
    }
    TypePtr out_ty =
        (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::Bytes)) ? ty : t_str();
    VReg r = call_runtime(RtFn::StrPercentFormat, out_ty,
                          {pat, args, const_str(desc),
                           const_int(lt->kind == TyKind::Bytes ? 1 : 0)});
    push_temp(r);
    return r;
}

VReg Lowerer::lower_binop(BinOp& b) {
    TypePtr ty = tc_.type_of(&b);
    TypePtr lt = tc_.type_of(b.left.get());
    TypePtr rt = tc_.type_of(b.right.get());
    // str/bytes 的 % 格式化（printf 风格）。
    if (b.op == Tok::PERCENT && lt &&
        (lt->kind == TyKind::Str || lt->kind == TyKind::Bytes))
        return lower_percent_format(b, lt, ty);
    // Path / str：路径拼接（对应 CPython 的 Path.__truediv__）
    if (b.op == Tok::SLASH && lt && lt->kind == TyKind::Path) {
        VReg p = expr(*b.left);
        VReg s = expr(*b.right);
        s = coerce(s, rt, t_str());
        VReg v = call_runtime(RtFn::PathJoin, new_ty(TyKind::Path), {p, s});
        push_temp(v);
        return v;
    }
    // datetime / timedelta 的算术：按运算符和两侧类型直接调运行时函数。
    // 不能落到下面按机器整数运算的通用路径（指针加减不会报错，只会算出野值）。
    if ((lt && (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta)) ||
        (rt && (rt->kind == TyKind::DateTime || rt->kind == TyKind::TimeDelta))) {
        auto is_dt = [](const TypePtr& t) { return t && t->kind == TyKind::DateTime; };
        auto is_td = [](const TypePtr& t) { return t && t->kind == TyKind::TimeDelta; };
        VReg l = expr(*b.left);
        VReg r = expr(*b.right);
        if (b.op == Tok::MINUS && is_dt(lt) && is_dt(rt)) {
            VReg v = call_runtime(RtFn::DateTimeDiff, new_ty(TyKind::TimeDelta), {l, r});
            push_temp(v);
            return v;
        }
        if ((b.op == Tok::PLUS || b.op == Tok::MINUS) && is_dt(lt) && is_td(rt)) {
            VReg v = call_runtime(RtFn::DateTimeAddDelta, new_ty(TyKind::DateTime),
                                  {l, r, const_int(b.op == Tok::MINUS ? 1 : 0)});
            push_temp(v);
            return v;
        }
        if (b.op == Tok::PLUS && is_td(lt) && is_dt(rt)) {
            VReg v = call_runtime(RtFn::DateTimeAddDelta, new_ty(TyKind::DateTime),
                                  {r, l, const_int(0)});
            push_temp(v);
            return v;
        }
        if ((b.op == Tok::PLUS || b.op == Tok::MINUS) && is_td(lt) && is_td(rt)) {
            RtFn fn = b.op == Tok::PLUS ? RtFn::TimeDeltaAdd : RtFn::TimeDeltaSub;
            VReg v = call_runtime(fn, new_ty(TyKind::TimeDelta), {l, r});
            push_temp(v);
            return v;
        }
        if (b.op == Tok::STAR && (is_td(lt) || is_td(rt))) {
            VReg td = is_td(lt) ? l : r;
            VReg k = is_td(lt) ? r : l;
            VReg v = call_runtime(RtFn::TimeDeltaMul, new_ty(TyKind::TimeDelta), {td, k});
            push_temp(v);
            return v;
        }
        if ((b.op == Tok::SLASH || b.op == Tok::DOUBLESLASH) && is_td(lt)) {
            if (is_td(rt)) {
                RtFn fn = b.op == Tok::SLASH ? RtFn::TimeDeltaTrueDivTD
                                             : RtFn::TimeDeltaFloorDivTD;
                return call_runtime(fn, b.op == Tok::SLASH ? t_float() : t_int(), {l, r});
            }
            if (rt && rt->kind == TyKind::Int) {
                RtFn fn = b.op == Tok::SLASH ? RtFn::TimeDeltaTrueDivInt
                                             : RtFn::TimeDeltaFloorDivInt;
                VReg v = call_runtime(fn, new_ty(TyKind::TimeDelta), {l, r});
                push_temp(v);
                return v;
            }
        }
        err(b.line, b.col,
            std::format("operator '{}' on '{}' and '{}' is not supported", tok_name(b.op),
                        type_name(lt), type_name(rt)));
        return const_none();
    }
    // str * int / list * int / tuple * int：一次分配、批量拷贝。
    if (b.op == Tok::STAR) {
        auto is_int_ty = [](const TypePtr& t) {
            return t && (t->kind == TyKind::Int || t->kind == TyKind::Bool);
        };
        auto seq_ty = [](const TypePtr& t) {
            return t && (t->kind == TyKind::Str || t->kind == TyKind::Bytes ||
                         t->kind == TyKind::List || t->kind == TyKind::Tuple);
        };
        if ((seq_ty(lt) && is_int_ty(rt)) || (is_int_ty(lt) && seq_ty(rt))) {
            VReg l = expr(*b.left);
            VReg r = expr(*b.right);
            bool left = seq_ty(lt);
            VReg base = left ? l : r;
            VReg n = coerce(left ? r : l, left ? rt : lt, t_int());
            TypePtr cty = left ? lt : rt;
            RtFn fn = cty->kind == TyKind::Str ? RtFn::StrRepeat
                      : cty->kind == TyKind::Bytes ? RtFn::BytesRepeat
                                                   : RtFn::ListRepeat;
            VReg v = call_runtime(fn, ty ? ty : cty, {base, n});
            // **必须同时登记 fresh_vregs。**
            //
            // 原来只 push_temp，漏了 fresh_vregs.insert —— 于是容器构造
            // （lower_expr_container）里 `st_->fresh_vregs.count(v)` 为 0，
            // 走了 retain_elem 分支，多发一次 pa_rc_inc。
            //
            // 实测 bench_rc_gen：`'x' * 16` 和 `str(i)` 都是这种，每轮
            // 多 2 次 inc、2 次 dec，15 万轮 = 60 万次冗余 RC 操作。
            push_temp(v);
            st_->fresh_vregs.insert(v);
            return v;
        }
    }
    // set 的非原地集合运算：`s | t` / `s & t` / `s - t` / `s ^ t` 返回新 set
    // （CPython 语义）。原先落到下面的「后端不支持」拦截。
    if (lt && rt && lt->kind == TyKind::Set && rt->kind == TyKind::Set) {
        RtFn fn = RtFn::SetUnion;
        bool ok = true;
        switch (b.op) {
            case Tok::VBAR: fn = RtFn::SetUnion; break;
            case Tok::AMPER: fn = RtFn::SetIntersect; break;
            case Tok::MINUS: fn = RtFn::SetDifference; break;
            case Tok::CIRCUMFLEX: fn = RtFn::SetSymDiff; break;
            default: ok = false; break;
        }
        if (ok) {
            VReg l = expr(*b.left);
            VReg r = expr(*b.right);
            VReg v = call_runtime(fn, ty ? ty : lt, {l, r});
            push_temp(v);
            return v;
        }
    }
    // 类型检查允许但后端还做不了的组合，在这里一次性拦住。
    // 放过去的话 codegen 会走「按整数处理」的兜底路径，指针相加不报错，
    // 只是算出一个野值——这类静默错误比编译失败难查得多。
    if (ty) {
        bool bad = false;
        switch (ty->kind) {
            case TyKind::Str:
            case TyKind::Bytes: bad = b.op != Tok::PLUS; break;
            case TyKind::List:
            case TyKind::Tuple:
            case TyKind::Set:
            case TyKind::Dict: bad = true; break;
            default: break;
        }
        if (bad) {
            err(b.line, b.col,
                std::format("operator '{}' on '{}' is not supported by the code generator yet",
                            tok_name(b.op), type_name(ty)));
            return const_none();
        }
    }
    VReg l = expr(*b.left);
    VReg r = expr(*b.right);
    // 类实例的算术运算：先试左操作数的 dunder，再试右操作数的反射版。
    if ((lt && lt->kind == TyKind::Class) || (rt && rt->kind == TyKind::Class)) {
        VReg dr = lower_dunder_binop(l, lt, r, rt, b.op, false);
        if (dr != kNoReg) return dr;
    }
    if (ty && ty->kind == TyKind::Complex) {
        auto as_complex = [&](VReg v, TypePtr from) {
            return (from && from->kind == TyKind::Complex)
                       ? v
                       : convert_to(v, from, t_complex());
        };
        l = as_complex(l, lt);
        r = as_complex(r, rt);
        RtFn fn = RtFn::ComplexAdd;
        switch (b.op) {
            case Tok::PLUS: fn = RtFn::ComplexAdd; break;
            case Tok::MINUS: fn = RtFn::ComplexSub; break;
            case Tok::STAR: fn = RtFn::ComplexMul; break;
            case Tok::SLASH: fn = RtFn::ComplexDiv; break;
            default:
                err(b.line, b.col,
                    std::format("operator '{}' on complex is not supported", tok_name(b.op)));
                return const_none();
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {l, r};
        return def_heap(ty, std::move(in));
    }
    // ── P3 big：任意精度算术（与 Complex 同形态，降级期发 CallRt）──
    // 两侧先转换到 big（int/bool 加宽、float 按 int(float) 截断——pa 格
    // 口径，文档化边界），运算全在 rt_big.c（CPython 黄金标准）。
    if (ty && ty->kind == TyKind::Big) {
        auto as_big = [&](VReg v, TypePtr from) {
            return (from && from->kind == TyKind::Big) ? v : convert_to(v, from, t_big());
        };
        l = as_big(l, lt);
        r = as_big(r, rt);
        RtFn fn = RtFn::BigAdd;
        switch (b.op) {
            case Tok::PLUS: fn = RtFn::BigAdd; break;
            case Tok::MINUS: fn = RtFn::BigSub; break;
            case Tok::STAR: fn = RtFn::BigMul; break;
            case Tok::DOUBLESLASH: fn = RtFn::BigFloorDiv; break;
            case Tok::PERCENT: fn = RtFn::BigMod; break;
            case Tok::DOUBLESTAR: fn = RtFn::BigPow; break;
            case Tok::AMPER: fn = RtFn::BigAnd; break;
            case Tok::VBAR: fn = RtFn::BigOr; break;
            case Tok::CIRCUMFLEX: fn = RtFn::BigXor; break;
            case Tok::LSHIFT: fn = RtFn::BigLShift; break;
            case Tok::RSHIFT: fn = RtFn::BigRShift; break;
            default:
                err(b.line, b.col,
                    std::format("operator '{}' on big is not supported", tok_name(b.op)));
                return const_none();
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {l, r};
        return def_heap(ty, std::move(in));
    }
    // big 的真除：结果是 float，但必须按**精确整数商**正确舍入
    // （pa_big_truediv），不能先转 double 再除（大数会丢精度）。
    if (ty && ty->kind == TyKind::Float &&
        ((lt && lt->kind == TyKind::Big) || (rt && rt->kind == TyKind::Big)) &&
        b.op == Tok::SLASH) {
        auto as_big = [&](VReg v, TypePtr from) {
            return (from && from->kind == TyKind::Big) ? v : convert_to(v, from, t_big());
        };
        l = as_big(l, lt);
        r = as_big(r, rt);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::BigTrueDiv);
        in.args = {l, r};
        return def(t_float(), std::move(in));
    }
    if (ty && ty->kind == TyKind::Str) {
        if (lt && lt->kind == TyKind::Exception) l = convert_to(l, lt, t_str());
        if (rt && rt->kind == TyKind::Exception) r = convert_to(r, rt, t_str());
    }
    // unknown 域的动态算术：结果静态类型是 unknown 且有一侧 unknown 时，
    // 两侧可能是 int / boxed float / str，按运行期标签分派（rt_any.c）。
    // 静态类型已知的表达式不会进入这里，热路径零调用。
    if (ty && ty->kind == TyKind::Unknown && (is_unknown(lt) || is_unknown(rt))) {
        int aop = any_arith_op(b.op);
        if (aop >= 0) {
            VReg res = call_runtime(RtFn::AnyArith, t_unknown(),
                                    {const_int(aop), l, r, const_int(any_desc(lt)),
                                     const_int(any_desc(rt))});
            push_temp_unknown(res);
            return res;
        }
    }
    // §1.2 提升：整数结果把两侧都转换到提升后的类型（定宽宽度与
    // signedness 在这里定死，codegen 只看一个类型）。i8+int 的 int 侧
    // 也是 int → 结果 int，转换是恒等；i8+i8 → i8，两侧无需转换。
    if (ty && is_int_kind(ty->kind)) {
        l = coerce(l, lt, ty);
        r = coerce(r, rt, ty);
    }
    // int / int 的结果是 float，两个操作数都得先转；int + float 同理。
    // 放在这里做，codegen 就不用每条浮点指令都猜操作数是什么类型。
    if (ty && ty->kind == TyKind::Float) {
        if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
        if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
    }
    Inst in;
    in.op = IrOp::Bin;
    in.bop = map_bin(b.op);
    in.a = l;
    in.b = r;
    return def_heap(ty, std::move(in));
}

VReg Lowerer::lower_unary(UnaryOp& u) {
    TypePtr ty = tc_.type_of(&u);
    TypePtr opnd_ty = tc_.type_of(u.operand.get());
    VReg v = expr(*u.operand);
    // 类实例的 `not` 先按 __bool__ / __len__ 求值真值（都没有时后端按指针判空）。
    if (u.op == Tok::KW_NOT) v = truth_bool(v, opnd_ty);
    // 用户类的一元运算：__neg__ / __pos__ / __invert__。以前这里直接落
    // 到下面的机器整数指令，把对象指针取负后按结果的静态类型（Class）
    // 做引用计数——野指针，运行期 SIGSEGV。
    if (opnd_ty && opnd_ty->kind == TyKind::Class) {
        const char* dn = u.op == Tok::MINUS    ? "__neg__"
                         : u.op == Tok::PLUS   ? "__pos__"
                         : u.op == Tok::TILDE  ? "__invert__"
                                               : nullptr;
        if (dn) {
            TypePtr want = (ty && ty->kind != TyKind::Unknown) ? ty : nullptr;
            VReg r = dunder_call(v, opnd_ty, dn, {}, want);
            if (r != kNoReg) return r;
            err(u.line, u.col,
                std::format("bad operand type for unary {}: '{}'", tok_name(u.op),
                            type_name(opnd_ty)));
            return const_none();
        }
    }
    if (ty && ty->kind == TyKind::TimeDelta && u.op == Tok::MINUS) {
        VReg out = call_runtime(RtFn::TimeDeltaNeg, ty, {v});
        push_temp(out);
        return out;
    }
    if (ty && ty->kind == TyKind::Complex) {
        if (u.op == Tok::PLUS) return v;
        if (u.op == Tok::MINUS) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexNeg);
            in.args = {v};
            return def_heap(ty, std::move(in));
        }
    }
    // P3 big 的一元运算：-big / ~big（+big 恒等）。
    if (ty && ty->kind == TyKind::Big) {
        if (u.op == Tok::PLUS) return v;
        if (u.op == Tok::MINUS || u.op == Tok::TILDE) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(u.op == Tok::MINUS ? RtFn::BigNeg : RtFn::BigNot);
            in.args = {v};
            return def_heap(ty, std::move(in));
        }
    }
    // unknown 的一元负：可能是 int 也可能是 boxed float，按标签分派。
    if (u.op == Tok::MINUS && is_unknown(opnd_ty)) {
        VReg r = call_runtime(RtFn::AnyNeg, t_unknown(),
                              {v, const_int(any_desc(opnd_ty))});
        push_temp_unknown(r);
        return r;
    }
    Inst in;
    in.op = IrOp::Un;
    switch (u.op) {
        case Tok::MINUS: in.uop = UnKind::Neg; break;
        case Tok::PLUS: in.uop = UnKind::Pos; break;
        case Tok::TILDE: in.uop = UnKind::BitNot; break;
        case Tok::KW_NOT: in.uop = UnKind::Not; break;
        default:
            err(u.line, u.col, "unsupported unary operator");
            in.uop = UnKind::Pos;
            break;
    }
    in.a = v;
    return def(ty, std::move(in));
}

VReg Lowerer::lower_boolop(BoolOp& b) {
    // 单值 BoolOp（语法分析器不会产生，编译期合成 AST 可能有，如 dataclass
    // 单字段 __eq__）：直接求值，不建短路块——下面的循环从 i=1 起，单值时
    // 一条跳转都不发射，当前块会悬空落进下一块（351 实测空列表 eq 恒 False）。
    if (b.values.size() == 1) return expr(*b.values[0]);
    TypePtr ty = tc_.type_of(&b);
    int tmp = new_borrow_slot(ty);

    bool is_and = b.op == Tok::KW_AND;
    int end = new_block(is_and ? "and.end" : "or.end");

    VReg acc = expr(*b.values[0]);
    store_borrow_slot(tmp, acc);
    // 真值必须按操作数自己的类型判断：结果槽的类型是 join 出来的，
    // 拿它去查 __bool__/__len__ 会漏掉类实例。
    TypePtr acc_ty = tc_.type_of(b.values[0].get());

    for (size_t i = 1; i < b.values.size(); ++i) {
        VReg cur_v = load_slot(tmp, ty);
        int rhs = new_block(is_and ? "and.rhs" : "or.rhs");
        int next = (i + 1 < b.values.size()) ? new_block(is_and ? "and.next" : "or.next") : end;

        Inst br;
        br.op = IrOp::Branch;
        br.a = truth_bool(cur_v, acc_ty);
        // and: 真才继续算右边；or: 假才继续算右边
        br.target = is_and ? rhs : next;
        br.target2 = is_and ? next : rhs;
        emit(std::move(br));

        switch_to(rhs);
        VReg v = expr(*b.values[i]);
        store_borrow_slot(tmp, v);
        acc_ty = tc_.type_of(b.values[i].get());
        Inst j;
        j.op = IrOp::Jump;
        j.target = next;
        emit(std::move(j));

        if (next == end) break;
        switch_to(next);
    }

    switch_to(end);
    return load_slot(tmp, ty);
}

// 容器的相等：list/tuple 逐元素比，dict 按键值对（顺序无关），set 按键。
// 元素类型在编译期已知，所以元素怎么比也能定下来（int/str/float/机器字）。
// 元素本身还是容器时只能按机器字比——再往里一层要编译器展开递归。
// 容器元素的比较链：每个字符说明该层元素怎么比。编译器按静态类型生成，
// 运行时按链递归——嵌套容器也能一层一层比下去。
void Lowerer::append_elem_chain(TypePtr container, std::string& chain) {
    if (!container) { chain += 'x'; return; }
    TypePtr et;
    switch (container->kind) {
        case TyKind::List:
        case TyKind::Set:
            et = container->sub.empty() ? t_unknown() : container->sub[0];
            break;
        case TyKind::Tuple: {
            et = container->sub.empty() ? t_unknown() : container->sub[0];
            // 定长异构元组的元素类型各不相同，没法用一种方式比
            if (!container->variadic && container->sub.size() > 1) {
                int k = key_kind(container->sub[0]);
                for (size_t i = 1; i < container->sub.size(); ++i)
                    if (key_kind(container->sub[i]) != k) { et = t_unknown(); break; }
            }
            break;
        }
        case TyKind::Dict:
            // dict 比的是值（键由 dict 自己的 key_kind 比）
            et = container->sub.size() > 1 ? container->sub[1] : t_unknown();
            break;
        default:
            et = t_unknown();
            break;
    }
    if (et && (et->kind == TyKind::List || et->kind == TyKind::Tuple)) {
        chain += 'l';
        append_elem_chain(et, chain);
    } else if (et && et->kind == TyKind::Dict) {
        chain += 'd';
        append_elem_chain(et, chain);
    } else if (et && et->kind == TyKind::Set) {
        chain += 't';
        append_elem_chain(et, chain);
    } else {
        switch (key_kind(et)) {
            case 0: chain += 'i'; break;
            case 1: chain += 's'; break;
            case 2: chain += 'f'; break;
            default: chain += 'x'; break;
        }
    }
}

// 排序描述符（0.312）：与相等链（每层一个字符、同质）不同，排序要支持
// 定长异构元组的逐位置字典序，所以描述符是自定界前缀树：
//   i/f/s/x = int/bool/None、float、str、机器字（unknown/其它）；
//   u = u64（无符号比较，P1 收尾）；
//   l + 子描述符 = list / 变长元组（逐元素字典序 + 短者小）；
//   t + arity 字节 + 逐位置描述符 = 定长元组。
// 运行时 sort_desc_cmp 按同一格式消费（rt_list.c）。深度/长度预算防
// 病态类型；超限返回 false，调用方退 "x"（机器字，修复前行为）。
bool Lowerer::append_sort_desc(TypePtr t, std::string& d, int depth) {
    if (depth > 12 || d.size() > 400) return false;
    if (!t) { d += 'x'; return true; }
    switch (t->kind) {
        case TyKind::Int:
        case TyKind::Bool:
        case TyKind::None:
            d += 'i';
            return true;
        case TyKind::U64:
            // u64 元素按无符号比（≥2^63 的位模式是负数）。其余定宽落
            // default 的 'x'：槽内规范值下机器字有符号比较逐例等价。
            d += 'u';
            return true;
        case TyKind::Float:
            d += 'f';
            return true;
        case TyKind::Str:
            d += 's';
            return true;
        case TyKind::List: {
            d += 'l';
            TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
            return append_sort_desc(et, d, depth + 1);
        }
        case TyKind::Tuple: {
            if (t->variadic || t->sub.empty()) {
                d += 'l';
                TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
                return append_sort_desc(et, d, depth + 1);
            }
            if (t->sub.size() > 255) return false;
            d += 't';
            d += static_cast<char>(static_cast<unsigned char>(t->sub.size()));
            for (const auto& s : t->sub)
                if (!append_sort_desc(s, d, depth + 1)) return false;
            return true;
        }
        default:
            d += 'x';
            return true;
    }
}

VReg Lowerer::lower_container_eq(VReg l, TypePtr lt, VReg r, TypePtr rt) {
    if (!lt || !rt) return kNoReg;
    auto is_seq = [](TypePtr t) {
        return t->kind == TyKind::List || t->kind == TyKind::Tuple;
    };
    if (is_seq(lt) && is_seq(rt)) {
        // list 和 tuple 是不同类型，CPython 里不相等；pyadamas 运行时表示相同，
        // 但静态类型分得清，就按静态类型判。
        bool l_list = lt->kind == TyKind::List;
        bool r_list = rt->kind == TyKind::List;
        if (l_list != r_list) return const_bool(false);
        std::string lc, rc;
        append_elem_chain(lt, lc);
        append_elem_chain(rt, rc);
        // 元素类型不同（list[int] vs list[float] 这种）没法用一条链比，退回指针比较
        if (lc != rc) return kNoReg;
        return call_runtime(RtFn::ListEq, t_bool(), {l, r, const_str(lc)});
    }
    if (lt->kind == TyKind::Dict && rt->kind == TyKind::Dict) {
        std::string lc, rc;
        append_elem_chain(lt, lc);
        append_elem_chain(rt, rc);
        if (lc != rc) return kNoReg;
        return call_runtime(RtFn::DictEq, t_bool(), {l, r, const_str(lc)});
    }
    if (lt->kind == TyKind::Set && rt->kind == TyKind::Set) {
        return call_runtime(RtFn::SetEq, t_bool(), {l, r});
    }
    if (lt->kind == TyKind::Range && rt->kind == TyKind::Range) {
        // range 按边界比，不物化
        return call_runtime(RtFn::RangeEq, t_bool(), {l, r});
    }
    return kNoReg;
}

// 单步比较的公共降级（单 op 与链式比较共用）：big/datetime/complex/
// unknown/定宽提升都在这里分派，链式比较不再退化成机器字比。
VReg Lowerer::lower_cmp_single(VReg l, TypePtr lt, VReg r, TypePtr rt, CmpOp op,
                               int line, int col) {
        // datetime / timedelta 按值比较：调运行时的三路比较，再和 0 比。
        if (lt && rt && lt->kind == rt->kind &&
            (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta) &&
            (op == CmpOp::Eq || op == CmpOp::Ne || op == CmpOp::Lt || op == CmpOp::Le ||
             op == CmpOp::Gt || op == CmpOp::Ge)) {
            RtFn fn = lt->kind == TyKind::TimeDelta ? RtFn::TimeDeltaCmp : RtFn::DateTimeCmp;
            VReg cv = call_runtime(fn, t_int(), {l, r});
            Inst in;
            in.op = IrOp::Cmp;
            in.cop = map_cmp(op);
            in.a = cv;
            in.b = const_int(0);
            return def(t_bool(), std::move(in));
        }
        // `is` / `is not` 是同一性比较：比指针，不比内容。以前直接映射成
        // Eq/Ne，字符串和容器会走按值比较（pa_str_eq 解引用右操作数），
        // 而 None 就是 0，`x is None` 当场段错误。
        if (op == CmpOp::Is || op == CmpOp::IsNot) {
            auto as_bits = [&](VReg v, TypePtr t) -> VReg {
                // 同一性比较必须按机器字比：堆类型要 Mov 成 int，否则
                // codegen 按 vreg 的底层类型（str）走内容比较，None 当
                // NULL 解引用。unknown 同样要 Mov——槽里可能躺着 str，
                // 不 Mov 会让 cmp.eq 按槽的 str 类型走 pa_str_eq。
                TypePtr bt = strip_optional(t);
                if (bt && !ir_is_heap(bt) && bt->kind != TyKind::Unknown) return v;
                Inst mv;
                mv.op = IrOp::Mov;
                mv.a = v;
                return def(t_int(), std::move(mv));
            };
            VReg bl = as_bits(l, lt);
            VReg br = as_bits(r, rt);
            Inst in;
            in.op = IrOp::Cmp;
            in.cop = op == CmpOp::Is ? CmpKind::Eq : CmpKind::Ne;
            in.a = bl;
            in.b = br;
            return def(t_bool(), std::move(in));
        }
        // == / != 在类实例上优先走 __eq__ / __ne__；两边都没有实现时
        // 维持原来的指针比较。
        if (op == CmpOp::Eq || op == CmpOp::Ne) {
            VReg eqres = lower_dunder_eq(l, lt, r, rt, op == CmpOp::Ne);
            if (eqres != kNoReg) return eqres;
            // 容器逐元素比较（CPython 语义），取代原来的指针比较。
            VReg ceq = lower_container_eq(l, lt, r, rt);
            if (ceq != kNoReg) {
                if (op == CmpOp::Ne) {
                    Inst un;
                    un.op = IrOp::Un;
                    un.uop = UnKind::Not;
                    un.a = ceq;
                    return def(t_bool(), std::move(un));
                }
                return ceq;
            }
        }
        // 大小比较：__lt__/__le__/__gt__/__ge__（含右操作数的镜像版）。
        if (op == CmpOp::Lt || op == CmpOp::Le || op == CmpOp::Gt || op == CmpOp::Ge) {
            VReg cmpres = lower_dunder_cmp(l, lt, r, rt, op);
            if (cmpres != kNoReg) return cmpres;
        }
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            VReg flag;
            if (rt && rt->kind == TyKind::Class) {
                std::vector<TypePtr> tys{lt};
                VReg cv = dunder_call(r, rt, "__contains__", {l}, t_bool(), &tys);
                if (cv == kNoReg) {
                    err(line, col,
                        std::format("'{}' does not support 'in' (no usable __contains__)",
                                    type_name(rt)));
                    return const_bool(false);
                }
                flag = cv;
            } else if (rt && (rt->kind == TyKind::Dict || rt->kind == TyKind::Set)) {
                TypePtr kt = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, kt);
                flag = call_runtime(RtFn::DictContains, t_bool(), {r, l});
            } else if (rt && rt->kind == TyKind::Range) {
                l = coerce(l, lt, t_int());
                flag = call_runtime(RtFn::RangeContains, t_bool(), {r, l});
            } else if (rt && (rt->kind == TyKind::List || rt->kind == TyKind::Tuple)) {
                TypePtr et = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, et);
                int ck = key_kind(et);
                if (rt->kind == TyKind::Tuple) {
                    for (auto& sub : rt->sub)
                        if (key_kind(sub) != ck) { ck = 3; break; }
                }
                flag = call_runtime(RtFn::ListContains, t_bool(), {r, l, const_int(ck)});
            } else if (rt && (rt->kind == TyKind::Str || rt->kind == TyKind::Bytes)) {
                if (lt && lt->kind != TyKind::Str && lt->kind != TyKind::Bytes &&
                    lt->kind != TyKind::Unknown) {
                    err(line, col,
                        std::format("'in <str>' requires a string, got '{}'", type_name(lt)));
                    return const_bool(false);
                }
                flag = call_runtime(RtFn::StrContains, t_bool(), {r, l});
            } else {
                err(line, col,
                    std::format("'in' on '{}' is not supported by the code generator yet",
                                type_name(rt)));
                return const_bool(false);
            }
            if (op == CmpOp::NotIn) {
                Inst un;
                un.op = IrOp::Un;
                un.uop = UnKind::Not;
                un.a = flag;
                flag = def(t_bool(), std::move(un));
            }
            return flag;
        }
        if ((lt && lt->kind == TyKind::Complex) || (rt && rt->kind == TyKind::Complex)) {
            if (op != CmpOp::Eq && op != CmpOp::Ne) {
                err(line, col, "complex values are not orderable");
                return const_bool(false);
            }
            auto as_complex = [&](VReg v, TypePtr from) {
                return (from && from->kind == TyKind::Complex)
                           ? v
                           : convert_to(v, from, t_complex());
            };
            l = as_complex(l, lt);
            r = as_complex(r, rt);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexEq);
            in.args = {l, r};
            VReg eq = def(t_bool(), std::move(in));
            if (op == CmpOp::Eq) return eq;
            Inst un;
            un.op = IrOp::Un;
            un.uop = UnKind::Not;
            un.a = eq;
            return def(t_bool(), std::move(un));
        }
        // ── P3 big 比较：big/big、big/int 走 pa_big_cmp（两侧先转 big）；
        // big/float 用**精确**混比（CPython int/float 混比不走 double 近似）。
        {
            bool lb = lt && lt->kind == TyKind::Big;
            bool rb = rt && rt->kind == TyKind::Big;
            if ((lb || rb) && !is_unknown(lt) && !is_unknown(rt) &&
                is_num(lt) && is_num(rt)) {
                auto as_big = [&](VReg v, TypePtr from) {
                    return (from && from->kind == TyKind::Big) ? v
                                                               : convert_to(v, from, t_big());
                };
                if (ir_is_float(lt) || ir_is_float(rt)) {
                    // NaN（cmp=-2）：除 != 外全 false。
                    VReg bv = lb ? l : r;
                    VReg fv = ir_is_float(lt) ? l : r;
                    VReg cv = call_runtime(RtFn::BigCmpF64, t_int(), {bv, fv});
                    if (op == CmpOp::Eq || op == CmpOp::Ne) {
                        Inst in;
                        in.op = IrOp::Cmp;
                        in.cop = op == CmpOp::Eq ? CmpKind::Eq : CmpKind::Ne;
                        in.a = cv;
                        in.b = const_int(0);
                        return def(t_bool(), std::move(in));
                    }
                    Inst in;
                    in.op = IrOp::Cmp;
                    in.cop = map_cmp(op);
                    in.a = cv;
                    in.b = const_int(0);
                    VReg ord = def(t_bool(), std::move(in));
                    Inst nn;
                    nn.op = IrOp::Cmp;
                    nn.cop = CmpKind::Ne;
                    nn.a = cv;
                    nn.b = const_int(-2);
                    VReg notnan = def(t_bool(), std::move(nn));
                    Inst an;
                    an.op = IrOp::Bin;
                    an.bop = BinKind::BitAnd;
                    an.a = ord;
                    an.b = notnan;
                    return def(t_bool(), std::move(an));
                }
                l = as_big(l, lt);
                r = as_big(r, rt);
                VReg cv = call_runtime(RtFn::BigCmp, t_int(), {l, r});
                Inst in;
                in.op = IrOp::Cmp;
                in.cop = map_cmp(op);
                in.a = cv;
                in.b = const_int(0);
                return def(t_bool(), std::move(in));
            }
        }
        // unknown 域的动态比较：任一侧 unknown 时按运行期标签分派
        // （int / boxed float / str 混比；静态类型已知的路径不进这里）。
        if (is_unknown(lt) || is_unknown(rt)) {
            int cop = any_cmp_op(op);
            if (cop >= 0)
                return call_runtime(RtFn::AnyCmp, t_bool(),
                                    {const_int(cop), l, r, const_int(any_desc(lt)),
                                     const_int(any_desc(rt))});
        }
        // 混合数值比较统一到 float：codegen 按左操作数的类型选指令
        if ((ir_is_float(lt) || ir_is_float(rt)) && is_num(lt) && is_num(rt)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
        }
        // §1.2：整数比较按**提升后的类型**比较（同宽 signed+unsigned →
        // unsigned 等）。两侧必须先转换到同一类型：i32 -1 与 u32 1 比较时
        // 若各按自己的机器字比，会拿符号扩展后的 -1 去比，结论正好相反。
        {
            TypePtr ct = int_promote(lt, rt);
            if (ct && is_fixed_int(ct) && is_int_like(lt) && is_int_like(rt)) {
                l = coerce(l, lt, ct);
                r = coerce(r, rt, ct);
            }
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
}

VReg Lowerer::lower_compare(Compare& c) {
    if (c.ops.size() == 1) {
        CmpOp op0 = c.ops[0];
        TypePtr lt0 = tc_.type_of(c.left.get());
        TypePtr rt0 = tc_.type_of(c.comparators[0].get());
        bool lto = lt0 && lt0->kind == TyKind::TypeObj;
        bool rto = rt0 && rt0->kind == TyKind::TypeObj;
        if ((lto || rto) && (op0 == CmpOp::Eq || op0 == CmpOp::Ne ||
                             op0 == CmpOp::Is || op0 == CmpOp::IsNot)) {
            auto operand_name = [&](Expr* e, TypePtr t) -> std::optional<std::string> {
                if (t && t->kind == TyKind::TypeObj && !t->sub.empty() && t->sub[0])
                    return py_type_name(t->sub[0]);
                if (e && e->kind == ExprKind::Name) {
                    auto& id = static_cast<Name*>(e)->id;
                    if (!is_bound_here(id)) {
                        if (auto cit = classes_.find(id);
                            cit != classes_.end() && cit->second)
                            return cit->second->name;
                        if (auto iit = imported_classes_.find(id);
                            iit != imported_classes_.end())
                            return iit->second.cls ? iit->second.cls->name : id;
                        if (TypeChecker::is_builtin_name(id)) return id;
                    }
                }
                return std::nullopt;
            };
            auto ln = operand_name(c.left.get(), lt0);
            auto rn = operand_name(c.comparators[0].get(), rt0);
            if (ln && rn) {
                if (lto) (void)expr(*c.left);  // 保留 type(x) 里 x 的求值副作用
                if (rto) (void)expr(*c.comparators[0]);
                bool eq = *ln == *rn;
                if (op0 == CmpOp::Ne || op0 == CmpOp::IsNot) eq = !eq;
                return const_bool(eq);
            }
        }
        VReg l = expr(*c.left);
        VReg r = expr(*c.comparators[0]);
        CmpOp op = c.ops[0];
        TypePtr lt = tc_.type_of(c.left.get());
        TypePtr rt = tc_.type_of(c.comparators[0].get());
        return lower_cmp_single(l, lt, r, rt, op, c.line, c.col);
    }

    for (auto op : c.ops) {
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            err(c.line, c.col, "chained 'in' comparisons are not supported");
            return const_bool(false);
        }
    }

    // 链式比较：需要短路，用槽保存中间值。
    // P3：每个比较数按**自己的静态类型**落借用槽（big/str 等必须按值
    // 比；统一按 unknown 装载会退化成机器字比），每步走 lower_cmp_single。
    int res = new_temp_slot(t_bool());
    store_slot(res, const_bool(true));
    TypePtr prev_ty = tc_.type_of(c.left.get());
    int prev_slot = new_borrow_slot(prev_ty);
    store_borrow_slot(prev_slot, expr(*c.left));

    int fail = new_block("cmp.fail");
    int done = new_block("cmp.end");

    for (size_t i = 0; i < c.ops.size(); ++i) {
        VReg l = load_slot(prev_slot, prev_ty);
        TypePtr rty = tc_.type_of(c.comparators[i].get());
        VReg r = expr(*c.comparators[i]);
        int cont = new_block("cmp.next");
        VReg cr = lower_cmp_single(l, prev_ty, r, rty, c.ops[i], c.line, c.col);
        Inst br;
        br.op = IrOp::Branch;
        br.a = cr;
        br.target = cont;
        br.target2 = fail;
        emit(std::move(br));

        switch_to(cont);
        prev_slot = new_borrow_slot(rty);
        store_borrow_slot(prev_slot, r);
        prev_ty = rty;
        if (i + 1 == c.ops.size()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = done;
            emit(std::move(j));
        }
    }

    switch_to(fail);
    store_slot(res, const_bool(false));
    Inst j;
    j.op = IrOp::Jump;
    j.target = done;
    emit(std::move(j));

    switch_to(done);
    return load_slot(res, t_bool());
}


}  // namespace pyadamas
