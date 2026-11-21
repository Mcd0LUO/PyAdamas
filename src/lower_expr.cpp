// lower_expr.cpp —— 表达式降级：容器提升与公共入口
#include "lower_impl.h"

namespace pyadamas {


bool Lowerer::bad_elem(const TypePtr& from, const TypePtr& to) {
    // 元素跨数值域的容器（list[int] → list[float] 等）要逐元素转换并重新分配，
    // 现在没做。宁可报错，也不要让 double 槽里躺整数位模式。
    if (!from || !to || from->kind != to->kind) return false;
    auto diff = [](const TypePtr& a, const TypePtr& b) {
        if (!a || !b || a->kind == TyKind::Unknown || b->kind == TyKind::Unknown)
            return false;
        // float 与 big 都是「槽里表示的域」不同：int 位模式搬过去会被重新解释。
        return ((a->kind == TyKind::Float) != (b->kind == TyKind::Float)) ||
               ((a->kind == TyKind::Big) != (b->kind == TyKind::Big));
    };
    // 元组：元素类型不一致时由 convert_to 的重建分支逐元素转换（0.359）。
    // 走下面的 promote_container 拷贝循环会把 int 位模式原样搬进 float/big
    // 槽里（`def f() -> tuple[float, float]: return 1, 2` 曾打印
    // (5e-324, 1e-323)）。
    if (from->kind == TyKind::Tuple && !to->variadic) return false;
    if (from->kind == TyKind::List || from->kind == TyKind::Tuple) {
        if (from->sub.empty() || to->sub.empty()) return false;
        return diff(from->sub[0], to->sub[0]);
    }
    if (from->kind == TyKind::Dict) {
        if (from->sub.size() < 2 || to->sub.size() < 2) return false;
        // 键也要看：dict[int,str] 赋给 dict[big,str] 时键不做转换，运行期按
        // big 哈希查 raw 键永远查不到（静默 False）。这里让它明确报错。
        return diff(from->sub[0], to->sub[0]) || diff(from->sub[1], to->sub[1]);
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
        ga.idx = Operand::global(gi);
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

    // ── list 元素级加宽（int/float/str → big）──────────────────────
    // 类型检查允许 list[int] 赋给 list[big]，但列表长度是动态的，不能像元组
    // 那样按常量下标重建 —— 这里发一个拷贝循环：新建 list[big]，逐元素
    // convert_to 后 append。不做的话元素仍是原始 int，读取时被当 big 指针
    // 解引用（实测段错误）。
    if (from->kind == TyKind::List && !to->sub.empty() && to->sub[0] &&
        to->sub[0]->kind == TyKind::Big) {
        TypePtr se = from->sub[0];
        TypePtr de = to->sub[0];
        if (!se) return kNoReg;
        if (!(se->kind == TyKind::Int || se->kind == TyKind::Bool ||
              se->kind == TyKind::Float || se->kind == TyKind::Str))
            return kNoReg;
        // 运行时签名是 pa_list_new(cap, elem_kind)：漏了 kind 会 panic
        // 「invalid list element kind」。elem_kind 用堆种类（big 是堆元素）。
        VReg out = call_runtime(RtFn::ListNew, to,
                                {const_int(0), const_int(list_kind(to))});
        push_temp(out);
        int out_slot = new_temp_slot(to);
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
        int cond = new_block("promote.list.cond");
        int body = new_block("promote.list.body");
        int end = new_block("promote.list.end");
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
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = load_slot(src_slot, from);
        get.b = load_slot(i_slot, t_int());
        VReg elem = def(se, std::move(get));
        VReg conv = convert_to(elem, se, de);
        retain_elem(conv, own_kind(de));
        call_void(RtFn::ListAppend,
                  {load_slot(out_slot, to), conv, const_int(own_kind(de))});
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
    // ── set 元素级加宽（int/float/str → big）──────────────────────
    if (from->kind == TyKind::Set && !to->sub.empty() && to->sub[0] &&
        to->sub[0]->kind == TyKind::Big) {
        TypePtr se = from->sub[0];
        TypePtr de = to->sub[0];
        if (se && (se->kind == TyKind::Int || se->kind == TyKind::Bool ||
                   se->kind == TyKind::Float || se->kind == TyKind::Str)) {
            int kk = hash_key_kind(de);
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
            int cond = new_block("promote.set.cond");
            int body = new_block("promote.set.body");
            int end = new_block("promote.set.end");
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
            VReg nk = convert_to(key, se, de);
            retain_elem(nk, ko);
            call_void(RtFn::DictSet, {load_slot(out_slot, to), nk, const_int(0)});
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
    call_void(RtFn::ListAppend, {load_slot(out_slot, lt), ce, heap_arg(de)});
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


}  // namespace pyadamas
