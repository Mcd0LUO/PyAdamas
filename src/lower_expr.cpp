// lower_expr.cpp —— 表达式降级：运算、容器、推导式、赋值
#include "lower_impl.h"

namespace pyadamas {

bool Lowerer::bad_elem(const TypePtr& from, const TypePtr& to) {
    // 元素跨数值域的容器（list[int] → list[float] 等）要逐元素转换并重新分配，
    // 现在没做。宁可报错，也不要让 double 槽里躺整数位模式。
    if (!from || !to || from->kind != to->kind) return false;
    auto diff = [](const TypePtr& a, const TypePtr& b) {
        if (!a || !b || a->kind == TyKind::Unknown || b->kind == TyKind::Unknown)
            return false;
        return (a->kind == TyKind::Float) != (b->kind == TyKind::Float);
    };
    if (from->kind == TyKind::List || from->kind == TyKind::Tuple) {
        if (from->sub.empty() || to->sub.empty()) return false;
        return diff(from->sub[0], to->sub[0]);
    }
    if (from->kind == TyKind::Dict) {
        if (from->sub.size() < 2 || to->sub.size() < 2) return false;
        return diff(from->sub[1], to->sub[1]);
    }
    if (from->kind == TyKind::Set) {
        if (from->sub.empty() || to->sub.empty()) return false;
        return diff(from->sub[0], to->sub[0]);
    }
    return false;
}

VReg Lowerer::local_class_object(const std::string& name) {
    auto it = classes_.find(name);
    if (it == classes_.end()) {
        err(0, 0, std::format("class '{}' is not available here", name));
        return const_none();
    }
    TypePtr obj_ty = mk_class_obj(mk_class(it->second.get()));
    int gi = global_idx_as("__pyadamas$class$" + name, obj_ty);
    return load_global(gi, obj_ty);
}

VReg Lowerer::imported_class_object(const ImportedClass& ic) {
    TypePtr obj_ty = mk_class_obj(mk_class(ic.cls.get()));
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ic.ctor_sig;
    int cgi = extern_global(ic.ctor_symbol, ic.ctor_symbol, fty);
    VReg ctor = load_global(cgi, fty);
    std::string qual = !ic.qualname.empty()
                           ? ic.qualname
                           : (ic.module.empty() ? ic.cls->name : ic.module + "." + ic.cls->name);
    VReg vtable = const_int(0);
    if (ic.cls->imported()) {
        std::string sym = module_class_vtable_symbol(ic.cls->origin_module,
                                                     ic.cls->external_name());
        int gi = extern_global(sym, sym, t_int());
        Inst ga;
        ga.op = IrOp::GlobalAddr;
        ga.idx = gi;
        vtable = def(t_int(), std::move(ga));
    }
    VReg obj = call_runtime(RtFn::ClassNew, obj_ty,
                            {const_int(-1), ctor, vtable, const_str(qual)});
    push_temp(obj);
    return obj;
}

VReg Lowerer::promote_container(VReg v, TypePtr from, TypePtr to) {
    if (!from || !to || from->kind != to->kind) return kNoReg;
    if (from->sub.empty() || to->sub.empty()) return kNoReg;

    if (from->kind == TyKind::Dict) {
        if (from->sub.size() < 2 || to->sub.size() < 2) return kNoReg;
        TypePtr kt = to->sub[0];
        TypePtr sval = from->sub[1];
        TypePtr dval = to->sub[1];
        if (!kt || !sval || !dval) return kNoReg;
        if (dval->kind != TyKind::Float) return kNoReg;
        if (sval->kind != TyKind::Int && sval->kind != TyKind::Bool) return kNoReg;
        if (kt->kind != TyKind::Str) return kNoReg;  // 键不做转换，只处理字符串键
        TypePtr keys_ty = mk_list(kt);
        int kk = key_kind(kt);
        int ko = own_kind(kt);
        int vo = own_kind(dval);
        auto [hslot, eslot] = dunder_key_slots(kt);
        VReg out = call_runtime(RtFn::DictNew, to,
                                {const_int(kk), const_int(ko), const_int(vo),
                                 const_int(hslot), const_int(eslot)});
        push_temp(out);
        int out_slot = new_temp_slot(to);
        store_slot(out_slot, out);
        int src_slot = new_temp_slot(from);
        store_slot(src_slot, v);
        VReg keys = call_runtime(RtFn::DictKeys, keys_ty, {load_slot(src_slot, from)});
        push_temp(keys);
        int keys_slot = new_temp_slot(keys_ty);
        store_slot(keys_slot, keys);
        int n_slot = new_temp_slot(t_int());
        {
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = load_slot(keys_slot, keys_ty);
            store_slot(n_slot, def(t_int(), std::move(ln)));
        }
        int i_slot = new_temp_slot(t_int());
        store_slot(i_slot, const_int(0));

        int cond = new_block("promote.cond");
        int body = new_block("promote.body");
        int end = new_block("promote.end");
        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));
        switch_to(cond);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = load_slot(i_slot, t_int());
        cmp.b = load_slot(n_slot, t_int());
        Inst br;
        br.op = IrOp::Branch;
        br.a = def(t_bool(), std::move(cmp));
        br.target = body;
        br.target2 = end;
        emit(std::move(br));
        switch_to(body);
        Inst getk;
        getk.op = IrOp::IndexLoad;
        getk.a = load_slot(keys_slot, keys_ty);
        getk.b = load_slot(i_slot, t_int());
        VReg key = def(kt, std::move(getk));
        Inst getv;
        getv.op = IrOp::CallRt;
        getv.imm = static_cast<int64_t>(RtFn::DictGet);
        getv.args = {load_slot(src_slot, from), key};
        VReg oldv = def(sval, std::move(getv));
        VReg newv = convert_to(oldv, sval, dval);
        retain_elem(newv, vo);
        call_void(RtFn::DictSet, {load_slot(out_slot, to), key, newv});
        Inst inc;
        inc.op = IrOp::Bin;
        inc.bop = BinKind::Add;
        inc.a = load_slot(i_slot, t_int());
        inc.b = const_int(1);
        store_slot(i_slot, def(t_int(), std::move(inc)));
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));
        switch_to(end);
        return load_slot(out_slot, to);
    }

    if (from->kind == TyKind::Set) {
        TypePtr se = from->sub[0];
        TypePtr de = to->sub[0];
        if (!se || !de) return kNoReg;
        if (de->kind != TyKind::Float) return kNoReg;
        if (se->kind != TyKind::Int && se->kind != TyKind::Bool) return kNoReg;
        // set 复用 dict 的键集合，值固定为 0。
        int kk = key_kind(de);
        int ko = own_kind(de);
        auto [hslot, eslot] = dunder_key_slots(de);
        VReg out = call_runtime(RtFn::DictNew, to,
                                {const_int(kk), const_int(ko), const_int(0),
                                 const_int(hslot), const_int(eslot)});
        push_temp(out);
        int out_slot = new_temp_slot(to);
        store_slot(out_slot, out);
        int src_slot = new_temp_slot(from);
        store_slot(src_slot, v);
        TypePtr keys_ty = mk_list(se);
        VReg keys = call_runtime(RtFn::DictKeys, keys_ty, {load_slot(src_slot, from)});
        push_temp(keys);
        int keys_slot = new_temp_slot(keys_ty);
        store_slot(keys_slot, keys);
        int n_slot = new_temp_slot(t_int());
        {
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = load_slot(keys_slot, keys_ty);
            store_slot(n_slot, def(t_int(), std::move(ln)));
        }
        int i_slot = new_temp_slot(t_int());
        store_slot(i_slot, const_int(0));

        int cond = new_block("promote.cond");
        int body = new_block("promote.body");
        int end = new_block("promote.end");
        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));
        switch_to(cond);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = load_slot(i_slot, t_int());
        cmp.b = load_slot(n_slot, t_int());
        Inst br;
        br.op = IrOp::Branch;
        br.a = def(t_bool(), std::move(cmp));
        br.target = body;
        br.target2 = end;
        emit(std::move(br));
        switch_to(body);
        Inst getk;
        getk.op = IrOp::IndexLoad;
        getk.a = load_slot(keys_slot, keys_ty);
        getk.b = load_slot(i_slot, t_int());
        VReg key = def(se, std::move(getk));
        VReg newv = convert_to(key, se, de);
        call_void(RtFn::DictSet, {load_slot(out_slot, to), newv, const_int(0)});
        Inst inc;
        inc.op = IrOp::Bin;
        inc.bop = BinKind::Add;
        inc.a = load_slot(i_slot, t_int());
        inc.b = const_int(1);
        store_slot(i_slot, def(t_int(), std::move(inc)));
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));
        switch_to(end);
        return load_slot(out_slot, to);
    }

    if (from->kind != TyKind::List && from->kind != TyKind::Tuple) return kNoReg;
    TypePtr se = from->sub[0];
    TypePtr de = to->sub[0];
    if (!se || !de) return kNoReg;
    // 隐式数值提升只有 int/bool → float 一个方向。
    if (de->kind != TyKind::Float) return kNoReg;
    if (se->kind != TyKind::Int && se->kind != TyKind::Bool) return kNoReg;

    TypePtr lt = mk_list(de);
    int ko = list_kind(lt);
    VReg out = call_runtime(RtFn::ListNew, lt, {const_int(0), const_int(ko)});
    push_temp(out);
    int out_slot = new_temp_slot(lt);
    store_slot(out_slot, out);
    int src_slot = new_temp_slot(from);
    store_slot(src_slot, v);
    int n_slot = new_temp_slot(t_int());
    {
        Inst ln;
        ln.op = IrOp::Len;
        ln.a = load_slot(src_slot, from);
        store_slot(n_slot, def(t_int(), std::move(ln)));
    }
    int i_slot = new_temp_slot(t_int());
    store_slot(i_slot, const_int(0));

    int cond = new_block("promote.cond");
    int body = new_block("promote.body");
    int end = new_block("promote.end");
    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = load_slot(i_slot, t_int());
    cmp.b = load_slot(n_slot, t_int());
    VReg less = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = less;
    br.target = body;
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    Inst get;
    get.op = IrOp::IndexLoad;
    get.a = load_slot(src_slot, from);
    get.b = load_slot(i_slot, t_int());
    VReg ev = def(se, std::move(get));
    VReg ce = convert_to(ev, se, de);
    call_void(RtFn::ListAppend, {load_slot(out_slot, lt), ce});
    Inst inc;
    inc.op = IrOp::Bin;
    inc.bop = BinKind::Add;
    inc.a = load_slot(i_slot, t_int());
    inc.b = const_int(1);
    store_slot(i_slot, def(t_int(), std::move(inc)));
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(out_slot, lt);
}

void Lowerer::assign_to(Expr& target, VReg v, TypePtr vty) {
    // 以 vreg 上记录的类型为准：调用方传进来的 vty 有时是目标侧的类型
    TypePtr src_ty = vreg_ty(v);
    if (!src_ty || src_ty->kind == TyKind::Unknown) src_ty = vty;
    // 值语义：`q = p` 必须复制一份 payload，不能共享同一个对象。
    if (src_ty && src_ty->kind == TyKind::Struct && src_ty->st)
        v = struct_copy(v, src_ty->st);
    switch (target.kind) {
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(target);
            if (int cs = comp_slot(n.id); cs >= 0) {
                TypePtr want = st_->fn->slot_ty[cs];
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
                v = coerce(v, src_ty, want);
                store_slot(cs, v);
                break;
            }
            if (is_cell(n.id) || is_env(n.id)) {
                write_cell(n.id, v, src_ty);
                break;
            }
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                int gi = global_idx(n.id);
                TypePtr want = mod_.globals[gi].ty;
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
                v = coerce(v, src_ty, want);
                store_global(gi, v);
            } else {
                int slot = st_->slots[n.id];
                TypePtr want = st_->fn->slot_ty[slot];
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
                v = coerce(v, src_ty, want);
                store_slot(slot, v);
            }
            break;
        }
        case ExprKind::Tuple:
        case ExprKind::List: {
            auto& elts = (target.kind == ExprKind::Tuple ? static_cast<TupleExpr&>(target).elts
                                                         : static_cast<ListExpr&>(target).elts);
            for (size_t i = 0; i < elts.size(); ++i) {
                if (!elts[i]) continue;
                if (elts[i]->kind == ExprKind::Starred) {
                    err(elts[i]->line, elts[i]->col,
                        "starred unpacking is not supported by the code generator yet");
                    continue;
                }
                TypePtr et = tc_.type_of(elts[i].get());
                Inst in;
                in.op = IrOp::IndexLoad;
                in.a = v;
                in.b = const_int(static_cast<int64_t>(i));
                // 从字符串解包出的每个元素都是新串，必须入临时栈。
                VReg ev = (src_ty && src_ty->kind == TyKind::Str)
                              ? def_heap(et, std::move(in))
                              : def(et, std::move(in));
                assign_to(*elts[i], ev, et);
            }
            break;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<Subscript&>(target);
            if (s.slice->kind == ExprKind::Slice) {
                ++side_effect_seq_;  // 切片赋值可能改变列表长度
                auto& sl = static_cast<SliceExpr&>(*s.slice);
                TypePtr ot = tc_.type_of(s.value.get());
                if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple &&
                            ot->kind != TyKind::Unknown)) {
                    err(target.line, target.col,
                        std::format("'{}' object does not support item assignment",
                                    type_name(ot)));
                    break;
                }
                if (src_ty && src_ty->kind != TyKind::List && src_ty->kind != TyKind::Tuple &&
                    src_ty->kind != TyKind::Str && src_ty->kind != TyKind::Unknown) {
                    err(target.line, target.col,
                        std::format("cannot assign '{}' to a slice", type_name(src_ty)));
                    break;
                }
                VReg obj = expr(*s.value);
                constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
                auto bound = [&](ExprPtr& e) -> VReg {
                    if (!e) return const_int(kNone);
                    VReg b = expr(*e);
                    return coerce(b, tc_.type_of(e.get()), t_int());
                };
                VReg start = bound(sl.lower);
                VReg stop = bound(sl.upper);
                VReg step = bound(sl.step);
                VReg src = v;
                if (src_ty && src_ty->kind == TyKind::Str) {
                    src = call_runtime(RtFn::StrToList, mk_list(t_str()), {v});
                    push_temp(src);
                }
                call_void(RtFn::ListSliceAssign, {obj, start, stop, step, src});
                break;
            }
            VReg obj = expr(*s.value);
            VReg idx = expr(*s.slice);
            TypePtr ot = tc_.type_of(s.value.get());
            if (ot && ot->kind == TyKind::Optional && ot->celem != CKind::None &&
                !ot->sub.empty() && ot->sub[0]->kind == TyKind::Int) {
                VReg base = guard_not_none(obj, "pointer to dereference");
                idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
                TypePtr et = cptr_elem_type(ot->celem);
                VReg val = coerce(v, src_ty, et);
                Inst in;
                in.op = IrOp::PtrStore;
                in.a = base;
                in.b = idx;
                in.c = val;
                in.imm = ckind_width(ot->celem);
                in.fwidth = static_cast<uint8_t>(in.imm);
                in.ty = et;
                emit(std::move(in));
                break;
            }
            if (ot && ot->kind == TyKind::Dict) {
                TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
                TypePtr vt = ot->sub.size() > 1 ? ot->sub[1] : t_unknown();
                idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
                v = coerce(v, src_ty, vt);
                retain_elem(idx, own_kind(kt));
                retain_elem(v, own_kind(vt));
                call_void(RtFn::DictSet, {obj, idx, v});
                break;
            }
            if (ot && ot->kind == TyKind::Set) {
                err(target.line, target.col, "'set' object does not support item assignment");
                break;
            }
            // 列表有元素类型，存进去的值要跟着转
            if (ot && ot->kind == TyKind::List && !ot->sub.empty())
                v = coerce(v, src_ty, ot->sub[0]);
            // 运行时 list_set 只负责 dec 旧值；新值的 retain 在这里做。
            retain_elem(v, list_kind(strip_optional(ot)));
            Inst in;
            in.op = IrOp::IndexStore;
            in.a = obj;
            in.b = idx;
            in.c = v;
            emit(std::move(in));
            break;
        }
        case ExprKind::Attribute: {
            auto& at = static_cast<Attribute&>(target);
            if (auto* n = dynamic_cast<Name*>(at.value.get())) {
                if (const CImportDecl* cd = tc_.cmodule(n->id)) {
                    bool handled = false;
                    for (auto& cv : cd->vars) {
                        if (cv.py_name != at.attr) continue;
                        handled = true;
                        if (cv.kind == CKind::Str || cv.kind == CKind::OptStr) {
                            err(target.line, target.col,
                                std::format("cannot assign to C string variable '{}.{}'",
                                            n->id, at.attr));
                            break;
                        }
                        v = coerce(v, src_ty, cvar_type(cv));
                        Inst si;
                        si.op = IrOp::StoreExtern;
                        si.idx = extern_var_idx(cv);
                        si.a = v;
                        emit(std::move(si));
                        break;
                    }
                    if (handled) break;
                    err(target.line, target.col,
                        std::format("cannot assign to cimport attribute '{}.{}'", n->id, at.attr));
                    break;
                }
            }
            if (auto* n = dynamic_cast<Name*>(at.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    err(target.line, target.col,
                        std::format("cannot assign to builtin module attribute '{}.{}'", n->id,
                                    at.attr));
                    break;
                }
            }
            if (auto gi = module_member(at)) {
                TypePtr mt = mod_.globals[*gi].ty;
                v = coerce(v, src_ty, mt);
                store_global(*gi, v);
                break;
            }
            TypePtr raw = tc_.type_of(at.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr)) {
                const StructInfo* s = ot->st;
                const StructFieldInfo* f = s ? s->field(at.attr) : nullptr;
                if (!f) {
                    err(target.line, target.col,
                        std::format("struct '{}' has no field '{}'",
                                    s ? s->name : std::string("?"), at.attr));
                    break;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                v = coerce(v, src_ty, f->ty);
                field_store(obj, static_cast<int>(f->offset), v,
                            static_cast<uint8_t>(f->size), f->c_unsigned);
                break;
            }
            if (ot && ot->kind == TyKind::ClassObj && !ot->sub.empty()) ot = ot->sub[0];
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                // `C.x = v`：写类属性的共享存储
                if (is_class_receiver(at)) {
                    const ClassInfo* rcls = class_receiver_cls(at, ot);
                    const ClassAttrInfo* ca = rcls ? rcls->class_attr(at.attr) : nullptr;
                    if (!ca) {
                        err(target.line, target.col,
                            std::format("class '{}' has no attribute '{}'",
                                        rcls ? rcls->name : ot->cls->name, at.attr));
                        break;
                    }
                    class_attr_store(rcls, ca, v, src_ty);
                    break;
                }
                if (const PropertyInfo* p = ot->cls->property(at.attr)) {
                    ++side_effect_seq_;  // setter 可能改动容器
                    if (!p->owner || p->setter < 0 ||
                        p->setter >= static_cast<int>(p->owner->methods.size())) {
                        err(target.line, target.col,
                            std::format("property '{}' has no setter", at.attr));
                        break;
                    }
                    const MethodInfo& sm = p->owner->methods[p->setter];
                    VReg obj = expr(*at.value);
                    if (opt) obj = guard_not_none(obj, std::format("property '{}'", at.attr));
                    TypePtr vt = sm.call_sig && !sm.call_sig->pos.empty()
                                     ? sm.call_sig->pos[0].ty
                                     : t_unknown();
                    v = coerce(v, src_ty, vt);
                    Inst vc;
                    vc.op = IrOp::VCall;
                    vc.a = obj;
                    vc.imm = sm.slot;
                    vc.args = {v};
                    emit(std::move(vc));
                    break;
                }
                auto* f = ot->cls->field(at.attr);
                if (!f) {
                    err(target.line, target.col,
                        std::format("'{}' object has no field '{}'", ot->cls->name, at.attr));
                    break;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                v = coerce(v, src_ty, f->ty);
                int off = static_cast<int>(8 * (1 + ot->cls->field_index(at.attr)));
                store_field(f->ty, obj, off, v);
                break;
            }
            err(target.line, target.col, "unsupported assignment target");
            break;
        }
        default:
            err(target.line, target.col, "unsupported assignment target");
            break;
    }
}

// ────────────────────────── 表达式 ──────────────────────────

VReg Lowerer::expr(Expr& e) {
    switch (e.kind) {
        case ExprKind::Constant: return lower_constant(static_cast<Constant&>(e));
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(e);
            // @classmethod 的 cls：编译期就绑定成所属类。运行时不需要真值
            // （调用点传的类 id 只是占位），直接给常量再改标类型——这样既
            // 不会按堆类型做 rc，也避开了被内层闭包捕获时的 cell 问题。
            if (!st_->class_self_name.empty() && n.id == st_->class_self_name &&
                st_->class_self_ty) {
                if (st_->class_self_dynamic) {
                    int slot = st_->slots.count(n.id) ? st_->slots[n.id] : -1;
                    VReg raw = slot >= 0 ? load_slot(slot, t_int()) : const_int(0);
                    return retype(raw, mk_class_obj(
                                          mk_class(const_cast<ClassInfo*>(st_->class_self_ty))));
                }
                auto cid = class_ids_.find(st_->class_self_ty);
                VReg raw = const_int(cid == class_ids_.end() ? 0 : cid->second);
                return retype(raw, mk_class(const_cast<ClassInfo*>(st_->class_self_ty)));
            }
            if (int cs = comp_slot(n.id); cs >= 0)
                return load_slot(cs, st_->fn->slot_ty[cs]);
            // 类体命名空间：前面几行定义的类属性
            if (auto it = st_->class_body_attrs.find(n.id); it != st_->class_body_attrs.end())
                return load_global(it->second, mod_.globals[static_cast<size_t>(it->second)].ty);
            // 嵌套函数直接引用自己：取当前闭包，而不是外层绑定。
            if (st_->self_def && n.id == st_->self_def->name && !is_cell(n.id) &&
                !is_env(n.id) && !st_->slots.count(n.id)) {
                if (st_->env_slot >= 0) return load_slot(st_->env_slot, st_->self_type);
                Inst fa;
                fa.op = IrOp::FuncAddr;
                fa.imm = st_->self_idx;
                return def(st_->self_type, std::move(fa));
            }
            if (is_cell(n.id)) return read_cell(n.id);
            if (is_env(n.id)) return read_env(n.id);
            if (auto it = imported_classes_.find(n.id); it != imported_classes_.end())
                return imported_class_object(it->second);
            if (native_aliases_.count(n.id)) {
                err(e.line, e.col,
                    std::format("native function '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (resolve_module_alias(n.id)) {
                err(e.line, e.col, std::format("module '{}' cannot be used as a value", n.id));
                return const_none();
            }
            // 类名在值位置：取类定义点写好的运行期类对象单例。
            if (!is_local(n.id) && classes_.count(n.id)) return local_class_object(n.id);
            TypePtr ty = name_ty(n.id);
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                Inst in;
                in.op = IrOp::LoadGlobal;
                in.idx = global_idx(n.id);
                in.ty = ty;
                return def(ty, std::move(in));
            }
            return load_slot(st_->slots[n.id], ty);
        }
        case ExprKind::BinOp: return lower_binop(static_cast<BinOp&>(e));
        case ExprKind::UnaryOp: return lower_unary(static_cast<UnaryOp&>(e));
        case ExprKind::BoolOp: return lower_boolop(static_cast<BoolOp&>(e));
        case ExprKind::Compare: return lower_compare(static_cast<Compare&>(e));
        case ExprKind::Call: return lower_call(static_cast<Call&>(e));
        case ExprKind::Subscript: return lower_subscript(static_cast<Subscript&>(e));
        case ExprKind::List: {
            auto& l = static_cast<ListExpr&>(e);
            return lower_list_like(l.elts, tc_.type_of(&e), false);
        }
        case ExprKind::Tuple: {
            auto& t = static_cast<TupleExpr&>(e);
            return lower_list_like(t.elts, tc_.type_of(&e), true);
        }
        case ExprKind::Set:
            return lower_set(static_cast<SetExpr&>(e), tc_.type_of(&e));
        case ExprKind::Dict:
            return lower_dict(static_cast<DictExpr&>(e), tc_.type_of(&e));
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::DictComp:
        case ExprKind::GeneratorExp:
            return lower_comp(e);
        case ExprKind::IfExp: return lower_ifexp(static_cast<IfExp&>(e));
        case ExprKind::JoinedStr: return lower_joined(static_cast<JoinedStr&>(e));
        case ExprKind::NamedExpr: {
            auto& n = static_cast<NamedExpr&>(e);
            VReg v = expr(*n.value);
            assign_to(*n.target, v, tc_.type_of(n.value.get()));
            return v;
        }
        case ExprKind::Yield: return lower_yield(e);
        case ExprKind::Await: return lower_await(e);
        case ExprKind::YieldFrom:
            return lower_yield_from(static_cast<YieldFrom&>(e));
        case ExprKind::FormattedValue: {
            // 顶层 FormattedValue 只会出现在 f-string 内部，单独出现说明
            // 上游构造有问题；按表达式处理即可。
            auto& fv = static_cast<FormattedValue&>(e);
            return convert_to(expr(*fv.value), tc_.type_of(fv.value.get()), t_str());
        }
        case ExprKind::Attribute: {
            auto& a = static_cast<Attribute&>(e);
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (const CImportDecl* cd = tc_.cmodule(n->id)) {
                    for (auto& k : cd->consts) {
                        if (k.name != a.attr) continue;
                        switch (k.kind) {
                            case CKind::Float: return const_float(k.f);
                            case CKind::Bool: return const_bool(k.b);
                            case CKind::Str: return const_str(k.s);
                            default: return const_int(k.i);
                        }
                    }
                    for (auto& v : cd->vars) {
                        if (v.py_name != a.attr) continue;
                        int vidx = extern_var_idx(v);
                        TypePtr vt = cvar_type(v);
                        Inst in;
                        in.op = IrOp::LoadExtern;
                        in.idx = vidx;
                        in.ty = vt;
                        return def_heap(vt, std::move(in));
                    }
                    for (auto& x : cd->externs)
                        if (x.py_name == a.attr) {
                            err(e.line, e.col,
                                std::format("extern function '{}.{}' must be called", n->id,
                                            a.attr));
                            return const_none();
                        }
                    err(e.line, e.col,
                        std::format("cimport '{}' has no member '{}'", n->id, a.attr));
                    return const_none();
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    VReg bv;
                    TypePtr bty;
                    if (builtin_value(mod->name, a.attr, bv, bty)) return bv;
                    err(e.line, e.col,
                        std::format("native function '{}.{}' must be called", n->id, a.attr));
                    return const_none();
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id)) {
                    if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end())
                        return imported_class_object(ci->second);
                }
            }
            if (auto gi = module_member(a))
                return load_global(*gi, mod_.globals[*gi].ty);
            TypePtr raw = tc_.type_of(a.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::ClassObj) {
                if (a.attr == "__name__") {
                    VReg cv = expr(*a.value);
                    VReg s = call_runtime(RtFn::ClassName, t_str(), {cv});
                    push_temp(s);
                    return s;
                }
                if (!ot->sub.empty()) ot = ot->sub[0];
            }
            if (ot && (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr)) {
                const StructInfo* s = ot->st;
                if (!s) {
                    err(e.line, e.col, std::format("struct has no field '{}'", a.attr));
                    return const_none();
                }
                const StructFieldInfo* f = s->field(a.attr);
                if (!f) {
                    err(e.line, e.col,
                        std::format("struct '{}' has no field '{}'", s->name, a.attr));
                    return const_none();
                }
                VReg obj = expr(*a.value);
                if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                return field_load(obj, static_cast<int>(f->offset), f->ty,
                                  static_cast<uint8_t>(f->size), f->c_unsigned);
            }
            if (ot && ot->kind == TyKind::Exception) {
                VReg ev = expr(*a.value);
                if (a.attr == "args")
                    return call_runtime(RtFn::ExcValueArgs, mk_tuple({t_str()}), {ev});
                if (a.attr == "__cause__")
                    return call_runtime(RtFn::ExcValueCause,
                                        mk_optional(mk_exception("Exception")), {ev});
                err(e.line, e.col,
                    std::format("'exception' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::TypeObj) {
                VReg tv = expr(*a.value);  // interned 类型名
                if (a.attr == "__name__") return retype(tv, t_str());
                err(e.line, e.col,
                    std::format("'type' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::File) {
                VReg fv = expr(*a.value);
                if (opt) fv = guard_not_none(fv, std::format("attribute '{}'", a.attr));
                if (a.attr == "closed")
                    return call_runtime(RtFn::FileClosed, t_bool(), {fv});
                if (a.attr == "name" || a.attr == "mode" || a.attr == "encoding" ||
                    a.attr == "errors") {
                    RtFn fn = a.attr == "name"     ? RtFn::FileName
                              : a.attr == "mode"   ? RtFn::FileMode
                              : a.attr == "encoding" ? RtFn::FileEncoding
                                                     : RtFn::FileErrors;
                    VReg s = call_runtime(fn, t_str(), {fv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col,
                    std::format("'file' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::DateTime) {
                VReg dv = expr(*a.value);
                if (opt) dv = guard_not_none(dv, std::format("attribute '{}'", a.attr));
                static const std::unordered_map<std::string, int64_t> kDtFields = {
                    {"year", 0}, {"month", 1}, {"day", 2},
                    {"hour", 3}, {"minute", 4}, {"second", 5},
                };
                if (auto it = kDtFields.find(a.attr); it != kDtFields.end())
                    return call_runtime(RtFn::DateTimeField, t_int(), {dv, const_int(it->second)});
                err(e.line, e.col,
                    std::format("'datetime' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Path) {
                VReg pv = expr(*a.value);
                if (opt) pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "name" || a.attr == "stem" || a.attr == "suffix") {
                    RtFn fn = a.attr == "name"    ? RtFn::PathName
                              : a.attr == "stem"  ? RtFn::PathStem
                                                  : RtFn::PathSuffix;
                    VReg s = call_runtime(fn, t_str(), {pv});
                    push_temp(s);
                    return s;
                }
                if (a.attr == "parent") {
                    VReg s = call_runtime(RtFn::PathParent, new_ty(TyKind::Path), {pv});
                    push_temp(s);
                    return s;
                }
                if (a.attr == "parts") {
                    TypePtr lt = mk_list(t_str());
                    VReg v = call_runtime(RtFn::PathParts, lt, {pv});
                    push_temp(v);
                    return v;
                }
                err(e.line, e.col,
                    std::format("'pathlib.Path' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::CompletedProcess) {
                VReg pv = expr(*a.value);
                if (opt)
                    pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "returncode")
                    return call_runtime(RtFn::SubprocessReturnCode, t_int(), {pv});
                if (a.attr == "stdout" || a.attr == "stderr") {
                    RtFn fn = a.attr == "stdout" ? RtFn::SubprocessStdout
                                                 : RtFn::SubprocessStderr;
                    VReg s = call_runtime(fn, t_str(), {pv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col,
                    std::format("'CompletedProcess' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Popen) {
                VReg pv = expr(*a.value);
                if (opt) pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "pid") return call_runtime(RtFn::PopenPid, t_int(), {pv});
                if (a.attr == "returncode")
                    return call_runtime(RtFn::PopenReturnCode, t_int(), {pv});
                if (a.attr == "stdout" || a.attr == "stderr" || a.attr == "stdin") {
                    RtFn fn = a.attr == "stdout"   ? RtFn::PopenStdout
                              : a.attr == "stderr" ? RtFn::PopenStderr
                                                   : RtFn::PopenStdin;
                    VReg f = call_runtime(fn, t_file(), {pv});
                    push_temp(f);
                    return f;
                }
                err(e.line, e.col,
                    std::format("'Popen' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Regex) {
                VReg rv = expr(*a.value);
                if (opt) rv = guard_not_none(rv, std::format("attribute '{}'", a.attr));
                if (a.attr == "groups")
                    return call_runtime(RtFn::RegexPatternGroups, t_int(), {rv});
                if (a.attr == "flags")
                    return call_runtime(RtFn::RegexPatternFlags, t_int(), {rv});
                if (a.attr == "pattern") {
                    VReg s = call_runtime(RtFn::RegexPatternStr, t_str(), {rv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col, std::format("re.Pattern has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Match) {
                VReg mv = expr(*a.value);
                if (opt) mv = guard_not_none(mv, std::format("attribute '{}'", a.attr));
                if (a.attr == "lastindex")
                    return call_runtime(RtFn::RegexLastIndex, t_int(), {mv});
                if (a.attr == "lastgroup") {
                    VReg s = call_runtime(RtFn::RegexLastGroup, t_str(), {mv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col, std::format("re.Match has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                // 类接收者：`C.x` / `cls.x` 读的是类属性那份共享存储，
                // 接收者本身不是可求值的值，不能走下面的实例路径。
                if (is_class_receiver(a)) {
                    const ClassInfo* rcls = class_receiver_cls(a, ot);
                    if (rcls) {
                        if (const ClassAttrInfo* ca = rcls->class_attr(a.attr))
                            return class_attr_load(rcls, ca);
                        if (rcls->method(a.attr)) return lower_method_value(a);
                        if (rcls->field(a.attr))
                            err(e.line, e.col,
                                std::format("'{}.{}' is an instance field, not a class attribute",
                                            rcls->name, a.attr));
                        else
                            err(e.line, e.col,
                                std::format("class '{}' has no attribute '{}'", rcls->name,
                                            a.attr));
                        return const_none();
                    }
                }
                if (const PropertyInfo* p = ot->cls->property(a.attr)) {
                    ++side_effect_seq_;  // getter 可能改动容器
                    if (!p->owner || p->getter < 0 ||
                        p->getter >= static_cast<int>(p->owner->methods.size())) {
                        err(e.line, e.col,
                            std::format("property '{}' has no getter", a.attr));
                        return const_none();
                    }
                    const MethodInfo& gm = p->owner->methods[p->getter];
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("property '{}'", a.attr));
                    Inst vc;
                    vc.op = IrOp::VCall;
                    vc.a = obj;
                    vc.imm = gm.slot;
                    TypePtr rt = gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret
                                                                 : t_unknown();
                    return def_heap(rt, std::move(vc));
                }
                if (auto* f = ot->cls->field(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    int off = static_cast<int>(8 * (1 + ot->cls->field_index(a.attr)));
                    return field_load(obj, off, f->ty);
                }
                // 实例字段没有，回退到类属性（Python 的实例查找顺序）。
                // 装饰方法在类属性里也有包装函数，但方法值要绑定接收者，
                // 必须按方法处理。
                if (ot->cls->method(a.attr)) return lower_method_value(a);
                if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    return class_attr_load_dyn(obj, ot->cls, ca);
                }
                err(e.line, e.col,
                    std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
                return const_none();
            }
            err(e.line, e.col, "attribute access is only supported as a method call");
            return const_none();
        }
        case ExprKind::Starred: {
            err(e.line, e.col, "starred expression is not allowed here");
            return const_none();
        }
        case ExprKind::Slice:
            err(e.line, e.col, "slice is not supported by the code generator yet");
            return const_none();
        case ExprKind::Lambda:
            err(e.line, e.col,
                "lambda in a class-body value is not supported by the code generator yet");
            return const_none();
        default:
            err(e.line, e.col, std::format("expression kind {} is not supported by the code generator",
                                          static_cast<int>(e.kind)));
            return const_none();
    }
}

VReg Lowerer::lower_constant(Constant& c) {
    switch (c.ck) {
        case Constant::CK::None: return const_none();
        case Constant::CK::Bool: return const_bool(c.b);
        case Constant::CK::Int: return const_int(c.i);
        case Constant::CK::Float: return const_float(c.f);
        case Constant::CK::Complex: {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexNew);
            in.args = {const_float(0.0), const_float(c.im)};
            return def_heap(t_complex(), std::move(in));
        }
        case Constant::CK::Str: return const_str(c.s);
        case Constant::CK::Bytes: return const_str(c.s);
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
        if (t && t->kind == TyKind::Int) desc.push_back('i');
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
        int kind = list_kind(list_ty);
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
    if (ty && ty->kind == TyKind::Str) {
        if (lt && lt->kind == TyKind::Exception) l = convert_to(l, lt, t_str());
        if (rt && rt->kind == TyKind::Exception) r = convert_to(r, rt, t_str());
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
    VReg v = expr(*u.operand);
    // 类实例的 `not` 先按 __len__ 求值真值（没有 __len__ 时后端按指针判空）。
    if (u.op == Tok::KW_NOT) v = truth_bool(v, tc_.type_of(u.operand.get()));
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
                if (t && t->kind == TyKind::TypeObj)
                    return py_type_name(t->sub.empty() ? t_unknown() : t->sub[0]);
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
        // `is` / `is not` 是同一性比较：比指针，不比内容。以前直接映射成
        // Eq/Ne，字符串和容器会走按值比较（pa_str_eq 解引用右操作数），
        // 而 None 就是 0，`x is None` 当场段错误。
        if (op == CmpOp::Is || op == CmpOp::IsNot) {
            auto as_bits = [&](VReg v, TypePtr t) -> VReg {
                if (!t || !ir_is_heap(t)) return v;  // 标量本身就是位模式
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
                    err(c.line, c.col,
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
                    err(c.line, c.col,
                        std::format("'in <str>' requires a string, got '{}'", type_name(lt)));
                    return const_bool(false);
                }
                flag = call_runtime(RtFn::StrContains, t_bool(), {r, l});
            } else {
                err(c.line, c.col,
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
                err(c.line, c.col, "complex values are not orderable");
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
        // 混合数值比较统一到 float：codegen 按左操作数的类型选指令
        if ((ir_is_float(lt) || ir_is_float(rt)) && is_num(lt) && is_num(rt)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
    }

    for (auto op : c.ops) {
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            err(c.line, c.col, "chained 'in' comparisons are not supported");
            return const_bool(false);
        }
    }

    // 链式比较：需要短路，用槽保存中间值。
    int res = new_temp_slot(t_bool());
    // 槽类型取左操作数：链式比较里存的是同一个值，类型决定真值判断。
    // 它只是借用在表达式内传递，生命周期由临时栈负责。
    int cur_slot = new_borrow_slot(tc_.type_of(c.left.get()));
    store_slot(res, const_bool(true));
    store_borrow_slot(cur_slot, expr(*c.left));

    int fail = new_block("cmp.fail");
    int done = new_block("cmp.end");

    for (size_t i = 0; i < c.ops.size(); ++i) {
        VReg l = load_slot(cur_slot, t_unknown());
        VReg r = expr(*c.comparators[i]);
        int cont = new_block("cmp.next");
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = map_cmp(c.ops[i]);
        cmp.a = l;
        cmp.b = r;
        VReg cr = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cr;
        br.target = cont;
        br.target2 = fail;
        emit(std::move(br));

        switch_to(cont);
        store_borrow_slot(cur_slot, r);
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
    int kind = list_kind(ty);
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
        retain_elem(v, kind);
        args.push_back(v);
    }
    Inst in;
    in.op = is_tuple ? IrOp::MakeTuple : IrOp::MakeList;
    in.imm = kind;
    in.args = std::move(args);
    return def_heap(ty, std::move(in));
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
        retain_elem(k, ko);
        VReg v = expr(*d.values[i]);
        v = coerce(v, tc_.type_of(d.values[i].get()), vt);
        retain_elem(v, vo);
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
        retain_elem(v, eo);
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

VReg Lowerer::dunder_str(VReg v, TypePtr ty, bool prefer_repr) {
    // repr 语义（容器元素、repr()、!r）只用 __repr__；str 语义先 __str__，
    // 没有再回退 __repr__。和 CPython 的默认关系一致。
    if (prefer_repr) return dunder_call(v, ty, "__repr__", {}, t_str());
    VReg s = dunder_call(v, ty, "__str__", {}, t_str());
    if (s != kNoReg) return s;
    return dunder_call(v, ty, "__repr__", {}, t_str());
}

VReg Lowerer::dunder_len(VReg v, TypePtr ty) {
    return dunder_call(v, ty, "__len__", {}, t_int());
}

VReg Lowerer::dunder_bool(VReg v, TypePtr ty) {
    return dunder_call(v, ty, "__bool__", {}, t_bool());
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
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Popen)
        return const_str("<subprocess.Popen object>");
    Inst in;
    in.op = IrOp::Convert;
    in.a = v;
    VReg r = def(to, std::move(in));
    // 数值转换产出标量；转成字符串才是一个新的堆对象。
    if (to->kind == TyKind::Str && (!from || from->kind != TyKind::Str))
        push_temp(r);
    return r;
}

// 存储位置（槽 / 全局 / 形参 / 列表元素）有静态类型，值可能落在另一个
// 数值域上——典型是 `x: float = 1`。不补这条 convert，整数 1 的位模式
// 会被当成 double 读出来（5e-324），而且错得毫无声响。
// 只在数值域之间转：指针类之间的差异属于类型检查该报的错，这里不猜。
VReg Lowerer::coerce(VReg v, TypePtr from, TypePtr to) {
    if (!from || !to) return v;
    if (type_eq(from, to)) return v;
    auto num = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Float; };
    if (!num(from->kind) || !num(to->kind)) return v;
    bool from_int = from->kind != TyKind::Float;
    bool to_int = to->kind != TyKind::Float;
    if (from_int == to_int) return v;  // int/bool 互换不用动，机器表示一样
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
            // 没有 !s/!r 的普通格式化：有 __format__ 就用它（和 format() 一致），
            // 否则按 __str__ 转成字符串再走字符串的格式说明。
            // （无转换时 conversion 是 -1。）
            if (fv.conversion < 0 && vt && vt->kind == TyKind::Class) {
                VReg r = kNoReg;
                if (vt->cls && vt->cls->method("__format__"))
                    r = dunder_call(raw, vt, "__format__", {spec}, t_str());
                if (r != kNoReg) {
                    raw = r;
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
            if (!ct || ct->kind == TyKind::Unknown) {
                VReg s = convert_to(raw, ct, t_str());
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::FmtStr);
                in.args = {s, spec};
                piece = def_heap(t_str(), std::move(in));
            } else {
                switch (ct->kind) {
                    case TyKind::Bool: piece = fmt_call(RtFn::FmtBool); break;
                    case TyKind::Int: piece = fmt_call(RtFn::FmtI64); break;
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
