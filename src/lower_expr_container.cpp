// lower_expr_container.cpp —— 列表/字典/集合/条件表达式构造
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_subscript(Subscript& s) {
    VReg obj = expr(*s.value);
    TypePtr ot = tc_.type_of(s.value.get());
    TypePtr ty = tc_.type_of(&s);

    if (s.slice->kind == ExprKind::Slice) {
        auto& sl = static_cast<SliceExpr&>(*s.slice);
        constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
        auto bound = [&](ExprPtr& e) -> VReg {
            if (!e) return const_int(kNone);
            VReg v = expr(*e);
            return coerce(v, tc_.type_of(e.get()), t_int());
        };
        VReg start = bound(sl.lower);
        VReg stop = bound(sl.upper);
        VReg step = bound(sl.step);
        if (ot && (ot->kind == TyKind::Str || ot->kind == TyKind::Bytes)) {
            VReg v = call_runtime(RtFn::StrSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        if (ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple)) {
            VReg v = call_runtime(RtFn::ListSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        err(s.line, s.col, std::format("'{}' is not sliceable", type_name(ot)));
        return const_none();
    }

    VReg idx = expr(*s.slice);
    // ptr[T] 解引用：Optional[int] 上带元素种类时读 *(p + i)。
    if (ot && ot->kind == TyKind::Optional && ot->celem != CKind::None && !ot->sub.empty() &&
        ot->sub[0]->kind == TyKind::Int) {
        VReg base = guard_not_none(obj, "pointer to dereference");
        idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
        Inst in;
        in.op = IrOp::PtrLoad;
        in.a = base;
        in.b = idx;
        in.imm = ckind_width(ot->celem);
        in.fwidth = static_cast<uint8_t>(in.imm);
        in.funsigned = ot->celem == CKind::UInt32;
        return def(cptr_elem_type(ot->celem), std::move(in));
    }
    if (ot && ot->kind == TyKind::Dict) {
        TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
        idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
        return call_runtime(RtFn::DictGet, ty, {obj, idx});
    }
    if (ot && ot->kind == TyKind::Set) {
        err(s.line, s.col, "set is not subscriptable");
        return const_none();
    }
    if (ot && ot->kind == TyKind::Bytes) {
        // bytes 下标返回 0..255 的整数，不是单字节切片
        idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
        return call_runtime(RtFn::BytesAt, t_int(), {obj, idx});
    }
    if (ot && ot->kind == TyKind::Deque) {
        // 返回 deque 内部的借用引用，和 list 的 IndexLoad 语义一致
        idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
        TypePtr et = tc_.type_of(&s);
        if (!et || et->kind == TyKind::Unknown) et = t_int();
        return call_runtime(RtFn::DequeGet, et, {obj, idx});
    }
    if (ot && ot->kind == TyKind::Range) {
        // range 下标按边界算，不物化
        idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
        return call_runtime(RtFn::RangeGet, t_int(), {obj, idx});
    }
    // 用户类：静态类型是类且 MRO 里有 __getitem__ 时按 dunder 分发。
    // 以前直接落到 IndexLoad，把对象当 PyAdamasList 解引用 -> 段错误。
    if (ot && ot->kind == TyKind::Class) {
        std::vector<TypePtr> gtys{tc_.type_of(s.slice.get())};
        VReg g = dunder_call(obj, ot, "__getitem__", {idx}, nullptr, &gtys);
        if (g != kNoReg) return g;
        err(s.line, s.col,
            std::format("'{}' object is not subscriptable", type_name(ot)));
        return const_none();
    }
    // 外层是 for k in range(len(a)) 且提示仍然有效：记下这条 IndexLoad，
    // 等循环体降级完、确认没有副作用后再改成免检查。
    FuncCtx::BoundsHint* bounds_hit = nullptr;
    if (!st_->bounds_hints.empty() && ot &&
        (ot->kind == TyKind::List || ot->kind == TyKind::Tuple) &&
        s.value->kind == ExprKind::Name && s.slice->kind == ExprKind::Name) {
        const std::string& seq_name = static_cast<Name&>(*s.value).id;
        const std::string& idx_name = static_cast<Name&>(*s.slice).id;
        auto bit = st_->slots.find(seq_name);
        auto sit = st_->slots.find(idx_name);
        if (bit != st_->slots.end() && sit != st_->slots.end() && !is_cell(seq_name) &&
            !is_cell(idx_name)) {
            for (auto* h : st_->bounds_hints)
                if (h->valid && h->seq_slot == bit->second && h->idx_slot == sit->second) {
                    bounds_hit = h;
                    break;
                }
        }
    }
    Inst in;
    in.op = IrOp::IndexLoad;
    in.a = obj;
    in.b = idx;
    // 只有「字符串下标」产生新串；list[str]/tuple[str] 返回的是容器内部的
    // 借用引用，不能入临时栈，否则会比容器多减一次引用。
    VReg r;
    if (ty && ty->kind == TyKind::Str && ot && ot->kind == TyKind::Str)
        r = def_heap(ty, std::move(in));
    else
        r = def(ty, std::move(in));
    if (bounds_hit) bounds_hit->patches.push_back({st_->blk, r});
    return r;
}

VReg Lowerer::lower_list_like(std::vector<ExprPtr>& elts, TypePtr ty, bool is_tuple) {
    std::vector<VReg> args;
    args.reserve(elts.size());
    // 元素所有权类别：0 非堆 / 1 堆 / 2 unknown 或混合。
    // kind 1/2 的元素在写入前 retain；kind 1 的容器销毁时由运行时 release。
    // 空字面量可以停在「未定」，第一次写入时再定型；有元素时容器一开始
    // 就带引用，沿用保守的 2。
    int kind = list_kind(ty);
    if (kind == 3 && !elts.empty()) kind = 2;
    std::vector<VReg> fresh_args;
    for (size_t i = 0; i < elts.size(); ++i) {
        auto& e = elts[i];
        if (e->kind == ExprKind::Starred) {
            err(e->line, e->col, "starred element is not supported by the code generator yet");
            continue;
        }
        VReg v = expr(*e);
        // 字面量的元素类型是按全部元素推出来的（`[1, 2.5]` 是 list[float]），
        // 前面的整数元素得在构造时就转成 double。
        TypePtr want;
        if (ty && !is_tuple && !ty->sub.empty()) want = ty->sub[0];
        else if (ty && is_tuple && !ty->sub.empty() && !ty->variadic && i < ty->sub.size())
            want = ty->sub[i];
        else if (ty && is_tuple && ty->variadic && !ty->sub.empty())
            want = ty->sub[0];
        v = coerce(v, tc_.type_of(e.get()), want);
        // 新值直接交给容器：先不 retain，构造成功后 take 走临时栈的引用。
        // 中途有元素求值抛异常时，临时栈仍持有它，cleanup 会正常释放。
        if (kind != 0 && st_->fresh_vregs.count(v)) {
            st_->fresh_vregs.erase(v);
            fresh_args.push_back(v);
        } else {
            retain_elem(v, kind);
        }
        args.push_back(v);
    }
    Inst in;
    in.op = is_tuple ? IrOp::MakeTuple : IrOp::MakeList;
    in.imm = kind;
    in.args = std::move(args);
    VReg out = def_heap(ty, std::move(in));
    // 容器已经直接持有这些引用，cleanup 不再 dec。
    for (VReg v : fresh_args) rc_temp_take(v);
    return out;
}

VReg Lowerer::lower_dict(DictExpr& d, TypePtr ty) {
    TypePtr kt = t_unknown(), vt = t_unknown();
    if (ty && ty->kind == TyKind::Dict) {
        if (!ty->sub.empty()) kt = ty->sub[0];
        if (ty->sub.size() > 1) vt = ty->sub[1];
    }
    TypePtr dtype = (ty && ty->kind == TyKind::Dict) ? ty : mk_dict(kt, vt);
    int kk = key_kind(kt);
    int ko = own_kind(kt);
    int vo = own_kind(vt);

    auto [hslot, eslot] = dunder_key_slots(kt);
    VReg dict = call_runtime(RtFn::DictNew, dtype,
                             {const_int(kk), const_int(ko), const_int(vo),
                              const_int(hslot), const_int(eslot)});
    push_temp(dict);
    for (size_t i = 0; i < d.values.size(); ++i) {
        if (!d.keys[i]) {
            err(d.line, d.col, "'**' dict unpacking is not supported by the code generator yet");
            continue;
        }
        VReg k = expr(*d.keys[i]);
        k = coerce(k, tc_.type_of(d.keys[i].get()), kt);
        if (!move_fresh(k, kt)) retain_elem(k, ko);
        VReg v = expr(*d.values[i]);
        v = coerce(v, tc_.type_of(d.values[i].get()), vt);
        if (!move_fresh(v, vt)) retain_elem(v, vo);
        call_void(RtFn::DictSet, {dict, k, v});
    }
    return dict;
}

VReg Lowerer::lower_set(SetExpr& s, TypePtr ty) {
    TypePtr et = t_unknown();
    if (ty && ty->kind == TyKind::Set && !ty->sub.empty()) et = ty->sub[0];
    TypePtr stype = (ty && ty->kind == TyKind::Set) ? ty : mk_set(et);
    int ek = key_kind(et);
    int eo = own_kind(et);

    auto [hslot, eslot] = dunder_key_slots(et);
    VReg set = call_runtime(RtFn::DictNew, stype,
                            {const_int(ek), const_int(eo), const_int(0),
                             const_int(hslot), const_int(eslot)});
    push_temp(set);
    for (auto& e : s.elts) {
        VReg v = expr(*e);
        v = coerce(v, tc_.type_of(e.get()), et);
        if (!move_fresh(v, et)) retain_elem(v, eo);
        call_void(RtFn::DictSet, {set, v, const_int(0)});
    }
    return set;
}


VReg Lowerer::lower_ifexp(IfExp& e) {
    TypePtr ty = tc_.type_of(&e);
    int tmp = new_borrow_slot(ty);
    VReg c = expr(*e.test);
    c = truth_bool(c, tc_.type_of(e.test.get()));
    int then_b = new_block("ifexp.then");
    int else_b = new_block("ifexp.else");
    int end = new_block("ifexp.end");

    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = then_b;
    br.target2 = else_b;
    emit(std::move(br));

    switch_to(then_b);
    store_borrow_slot(tmp, expr(*e.body));
    Inst j1;
    j1.op = IrOp::Jump;
    j1.target = end;
    emit(std::move(j1));

    switch_to(else_b);
    store_borrow_slot(tmp, expr(*e.orelse));
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));

    switch_to(end);
    return load_slot(tmp, ty);
}

// ── 协议 dunder ────────────────────────────────────────────────
//
// 静态类型是用户类、且 MRO 里有签名可用的实现时，构造一次普通 VCall：
// vtable 槽按方法名全局分配，子类覆盖天然生效；没有实现就返回 kNoReg，
// 调用点退回默认行为（指针比较 / `<P object>` / 指针非空真值）。
VReg Lowerer::dunder_call(VReg recv, TypePtr recv_ty, const char* name,
                          std::vector<VReg> args, TypePtr want_ret,
                          const std::vector<TypePtr>* arg_tys) {
    if (!recv_ty || recv_ty->kind != TyKind::Class || !recv_ty->cls) return kNoReg;
    const MethodInfo* m = recv_ty->cls->method(name);
    if (!m || m->kind != MethodKind::Normal || !m->call_sig) return kNoReg;
    if (m->call_sig->varargs || m->call_sig->kwargs) return kNoReg;
    if (m->call_sig->pos.size() != args.size()) return kNoReg;
    // 形参是具体类时，实参必须能赋值给它（unknown 放行：由用户自己保证，
    // 方法体里的 isinstance 守卫是运行时行为）。不匹配就不走 dunder，
    // 留给调用点回退。
    if (arg_tys && arg_tys->size() == args.size()) {
        for (size_t i = 0; i < args.size(); ++i) {
            TypePtr pt = m->call_sig->pos[i].ty;
            TypePtr at = (*arg_tys)[i];
            if (!pt || pt->kind != TyKind::Class || !at) continue;
            if (at->kind == TyKind::Unknown) continue;
            if (!assignable(pt, at)) return kNoReg;
        }
    }
    TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
    if (want_ret && !type_eq(rt, want_ret)) return kNoReg;
    if (m->decorated) {
        // 装饰后的 dunder 存在类属性里：取包装函数，把 self 显式传进去。
        const ClassAttrInfo* ca = recv_ty->cls->class_attr(name);
        TypePtr wty = ca ? ca->ty : nullptr;
        FuncInfoPtr winfo =
            (wty && wty->kind == TyKind::Func) ? wty->fn : nullptr;
        if (!winfo || winfo->varargs || winfo->kwargs) return kNoReg;
        if (winfo->pos.size() != args.size() + 1) return kNoReg;
        VReg fv = class_attr_load_dyn(recv, recv_ty->cls, ca);
        Inst in;
        in.op = IrOp::Call;
        in.a = fv;
        in.args.reserve(args.size() + 1);
        in.args.push_back(recv);
        for (VReg a : args) in.args.push_back(a);
        return def_heap(rt, std::move(in));
    }
    if (m->slot < 0) return kNoReg;
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = recv;
    vc.imm = m->slot;
    vc.args = std::move(args);
    return def_heap(rt, std::move(vc));
}


}  // namespace pyadamas
