// lower_impl_slots.cpp —— Lowerer 的槽位/全局/返回相关成员
#include "lower_impl.h"

namespace pyadamas {

void Lowerer::store_slot(int slot, VReg v) {
    TypePtr dst = st_->fn->slot_ty[slot];
    TypePtr src = vreg_ty(v);
    const size_t si = static_cast<size_t>(slot);
    // 未知槽位第一次收到已知类型的值时补上类型；后续 load.slot
    // 才能按 float/int 正确选指令。retain_for_store 从这一刻起
    // 按堆槽的规则 retain/dec，槽也进入帧记录的释放清单。
    bool upgraded = false;
    if (dst && dst->kind == TyKind::Unknown && src && src->kind != TyKind::Unknown &&
        !st_->ann_unknown_slots.count(slot)) {
        // 之前可能以 unknown 值写入过这个槽：旧值不一定是堆指针。
        st_->fn->slot_unknown[si] = true;
        st_->fn->slot_ty[slot] = dst = src;
        upgraded = true;
    }
    // P3-1b（0.328）：unknown/堆值流入「已升级的非堆槽」→ 特化失效，回退
    // unknown。object 槽首存 int 被升级成 int 槽；随后 s = s + x（unknown）
    // 若按非堆槽裸存：不 retain、旧值不释放、load.slot 还按 int 读回 ——
    // float 盒被 cleanup 提前释放成野指针、指针按整数打印（函数内累加器
    // 形态，0.327 基线已存在）。回退 unknown 后走下面的 retain/释放口径，
    // 装载按动态分派。int 特化收益保留到首次 unknown/堆写入为止（此前已
    // 发射的 typed load 仍然有效）。旧值是 int/bool 标量，dec_unknown 探针
    // 空操作；常规/readonly 模式探针只查索引不解引用，安全。
    if (dst && !ir_is_heap(dst) && dst->kind != TyKind::Unknown &&
        ((!src || src->kind == TyKind::Unknown) || ir_is_heap(src))) {
        st_->fn->slot_ty[slot] = dst = t_unknown();
        st_->fn->slot_unknown[si] = true;
    }
    const bool borrow = st_->borrow_slots.count(slot) != 0;
    bool moved = !borrow && move_fresh(v, dst);
    if (!moved) retain_for_store(dst, v);
    if (ir_is_heap(dst)) {
        // 堆槽收到 unknown/None 等非堆值后，槽里可能装着整数：之后
        // 所有旧值释放都改用注册表判断，避免把整数当指针 dec。
        if (!ir_is_heap(src))
            st_->fn->slot_unknown[si] = true;
        st_->fn->slot_owned[si] = true;
        VReg old = load_slot(slot, dst);
        store_slot_raw(slot, v);
        if (st_->fn->slot_unknown[si]) rc_dec_unknown(old);
        else rc_dec(old);
    } else if (!borrow && dst && dst->kind == TyKind::Unknown) {
        // **unknown 槽的所有权口径：槽持有 retain 过的那一份引用。**
        //
        // 0.323 之前这里只写不释放：store_slot 的 retain_for_store 给
        // unknown 槽补了引用，出口 release_slots 却因为「unknown 槽可能
        // 是借来的」整类跳过 —— 每写一次就泄漏一份。字符串还是 tracked
        // 叶子时被退出时沿链 free 掩盖，0.321 改成索引叶子后 LSan 立刻
        // 报出来（for v in list[object]、v: object = xs[i] 每元素一份）。
        //
        // 现在按槽记账：写入时 retain 过（moved / 堆源 / unknown 源）就
        // 把 slot_owned 置位，覆盖前按注册表释放旧值，出口 release_slots
        // 按同一个标记释放。标记只置不清（同一槽在不同分支可能写堆也可能
        // 写标量，保守释放对标量是空操作，反向则会漏）。
        const bool owns = moved || ir_is_heap(src) || !src || src->kind == TyKind::Unknown;
        // 旧值无条件按注册表释放：槽在函数序言/生成器帧创建时清零，
        // 第一次 store 读到 0 是空操作。不能按 slot_owned 的静态值
        // 跳过——同一条 store 指令在循环里要执行多次，覆盖的是上一轮
        // 写进去的那份引用（0.323 首版就是这样每轮漏一份）。
        VReg old = load_slot(slot, dst);
        store_slot_raw(slot, v);
        rc_dec_unknown(old);
        if (owns) st_->fn->slot_owned[si] = true;
        st_->fn->slot_unknown[si] = true;
    } else {
        // 未知槽位升级成已知非堆类型：旧值可能还是 retained 过的堆指针。
        if (upgraded && !borrow && st_->fn->slot_owned[si]) {
            VReg old = load_slot(slot, dst);
            rc_dec_unknown(old);
        }
        store_slot_raw(slot, v);
    }}

void Lowerer::store_global(int gi, VReg v) {
    TypePtr dst = mod_.globals[gi].ty;
    TypePtr src = vreg_ty(v);
    bool moved = move_fresh(v, dst);
    if (!moved) retain_for_store(dst, v);
    bool unknown = dst && dst->kind == TyKind::Unknown;
    if (ir_is_heap(dst) || unknown) {
        // unknown 全局和老值释放都走注册表：槽里可能装着整数。
        IRGlobal& g = mod_.globals[gi];
        if (unknown) {
            g.maybe_unknown = true;
            // 注意：所有权已经由上面统一的一次 retain_for_store 处理
            //（unknown 源走 rc_inc_unknown、堆源走 rc_inc、fresh 值
            // 走 move_fresh 转移临时栈那份）。0.323 之前这里又对
            // unknown 源补了一次 rc_inc_unknown，于是全局每写一次就多
            // 留一个引用；release_globals 只释放一次，泄漏那一份在
            // 字符串改成索引叶子后被 LSan 抓出来。
        } else if (!ir_is_heap(src)) {
            g.maybe_unknown = true;
        }
        VReg old = load_global(gi, dst);
        store_global_raw(gi, v);
        if (g.maybe_unknown) rc_dec_unknown(old);
        else rc_dec(old);
    } else {
        store_global_raw(gi, v);
    }}

void Lowerer::store_field(TypePtr fty, VReg obj, int off, VReg v) {
    bool moved = move_fresh(v, fty);
    if (!moved) retain_for_store(fty, v);
    bool unknown = fty && fty->kind == TyKind::Unknown;
    // unknown 字段自己持有一份引用，析构时按注册表释放。这份引用由
    // 上面统一的 retain_for_store / move_fresh 给出，这里不能再补一次：
    // 0.323 之前对 unknown 源又 rc_inc_unknown 了一遍，字段析构只 dec
    // 一次，每写一个 unknown 元素就漏一份引用。
    if (ir_is_heap(fty) || unknown) {
        VReg old = field_load(obj, off, fty);
        field_store(obj, off, v);
        if (unknown) rc_dec_unknown(old);
        else rc_dec(old);
    } else {
        field_store(obj, off, v);
    }}

VReg Lowerer::load_slot(int slot, TypePtr ty) {
    // 槽位类型是唯一真相：调用方给 unknown 时回退到槽位表，
    // 否则 call.rt 结果经临时槽后会在后端退化成整数比较。
    TypePtr actual = ty;
    if ((!actual || actual->kind == TyKind::Unknown) && slot >= 0 &&
        slot < static_cast<int>(st_->fn->slot_ty.size()))
        actual = st_->fn->slot_ty[slot];
    Inst in;
    in.op = IrOp::LoadSlot;
    in.idx = Operand::slot(slot);
    in.ty = actual;
    return def(actual ? actual : t_unknown(), std::move(in));}

VReg Lowerer::load_global(int gi, TypePtr ty) {
    Inst in;
    in.op = IrOp::LoadGlobal;
    in.idx = Operand::global(gi);
    in.ty = ty;
    return def(ty ? ty : t_unknown(), std::move(in));}

void Lowerer::release_slots() {
    VReg zero = kNoReg;
    for (size_t s = 0; s < st_->fn->slot_ty.size(); ++s) {
        if (st_->borrow_slots.count(static_cast<int>(s))) continue;
        TypePtr ty = st_->fn->slot_ty[s];
        const bool unk = !ty || ty->kind == TyKind::Unknown;
        // unknown 槽按 slot_owned 释放：0.323 起写入点（store_slot）和
        // 未知形参入口（inc_params）都会把引用登记成槽所有，覆盖时也按
        // 同一标记释放旧值，所以出口可以对称释放。
        //
        // slot_owned 是槽的静态属性（可能有多个 return 路径），这里不能
        // 在第一条 return 之后清掉它，否则后面的 return/帧记录会漏释放。
        // 仍然跳过的两类：borrow_slots（借来的，见上）；以及没被任何
        // 写入点标记 owned 的 unknown 槽（未经 store_slot 的裸写，例如
        // DictTryGet 的值出口槽），它们始终是借用。
        if (unk) {
            if (!st_->fn->slot_owned[s]) continue;
            VReg v = load_slot(static_cast<int>(s), ty);
            rc_dec_unknown(v);
        } else if (ir_is_heap(ty)) {
            VReg v = load_slot(static_cast<int>(s), ty);
            if (st_->fn->slot_unknown[s]) rc_dec_unknown(v);
            else rc_dec(v);
        } else {
            continue;
        }
        if (zero == kNoReg) zero = const_int(0);
        store_slot_raw(static_cast<int>(s), zero);
    }}

void Lowerer::release_globals() {
    for (size_t gi = 0; gi < mod_.globals.size(); ++gi) {
        IRGlobal& g = mod_.globals[gi];
        if (g.external) continue;
        bool unknown = g.ty && g.ty->kind == TyKind::Unknown;
        if (!ir_is_heap(g.ty) && !unknown) continue;
        // 可能装着 non-heap 值（unknown 流过）时按注册表释放；从没写过
        // unknown 的堆全局还是走更快的 rc_dec。
        if (unknown || g.maybe_unknown)
            rc_dec_unknown(load_global(static_cast<int>(gi), g.ty));
        else
            rc_dec(load_global(static_cast<int>(gi), g.ty));
    }}

VReg Lowerer::own_return_value(VReg v, TypePtr vty) {
    if (v == kNoReg) return v;
    // 值语义：返回 struct 时复制 payload，caller 拿到的是独立对象。
    if (vty && vty->kind == TyKind::Struct && vty->st) v = struct_copy(v, vty->st);
    // 刚产生、还没存过的新值直接把临时栈上的引用转给 caller。
    if (vty && ir_is_heap(vty) && st_->fresh_vregs.count(v)) {
        st_->fresh_vregs.erase(v);
        rc_temp_take(v);
        return v;
    }
    if (ir_is_heap(vty)) rc_inc(v);
    else if (!vty || vty->kind == TyKind::Unknown)
        call_void(RtFn::RcIncUnknown, {v});
    return v;
}

void Lowerer::emit_return(VReg v, TypePtr vty, bool already_owned) {
    if (already_owned) {
        // 调用方已在清理之前接管了返回值，这里只做出口的槽/全局释放。
        if (st_->fn_mark != kNoReg && st_->rc_pushes > 0) rc_cleanup(st_->fn_mark);
        release_slots();
        if (st_->is_entry) {
            call_void(RtFn::VtShutdown, {});
            release_globals();
            call_void(RtFn::GcShutdown, {});
            call_void(RtFn::RcShutdown, {});
        }
        Inst in;
        in.op = IrOp::Return;
        in.a = v;
        emit(std::move(in));
        return;
    }
    // 值语义：返回 struct 时复制 payload，caller 拿到的是独立对象。
    if (v != kNoReg && vty && vty->kind == TyKind::Struct && vty->st)
        v = struct_copy(v, vty->st);
    // 刚产生、还没存过的新值直接把临时栈上的引用转给 caller。
    bool moved = false;
    if (v != kNoReg && vty && ir_is_heap(vty) && st_->fresh_vregs.count(v)) {
        st_->fresh_vregs.erase(v);
        rc_temp_take(v);
        moved = true;
    }
    // **unknown 返回值也必须加引用。**
    //
    // `ir_is_heap(vty)` 对 unknown 返回 false，于是这里什么都不做。但
    // unknown 完全可能是堆对象：`def f(): return a[0]` 里 a 是 list[str]，
    // `a[0]` 的元素类型在 lowering 时是 unknown，返回的就是一个借来的
    // 字符串引用。
    //
    // 后果：caller 拿到这个指针后，被调函数的 rc_cleanup / 列表析构把它
    // 的 rc 降到 0（字符串进池，内存还在但 rc=0），caller 之后对旧值做
    // dec 时命中 `h->rc <= 0` —— panic "refcount decrement on a freed
    // object"，或者更糟：读到已复用的内存。
    //
    // 实测（gc01_min_uaf / gc500）：
    //     def f() -> str:
    //         u = ident(["p" + str(i) for i in range(200)])
    //         return u[0]
    // 500 轮后 panic；PYADAMAS_GC_DISABLE=1 完全正常（因为关掉 GC 后
    // 对象不会在池里被复用，问题被掩盖）。
    //
    // RcIncUnknown 先查堆索引确认是堆指针再 inc，整数安全。
    if (v != kNoReg && !moved) {
        if (ir_is_heap(vty))
            rc_inc(v);
        else if (!vty || vty->kind == TyKind::Unknown)
            call_void(RtFn::RcIncUnknown, {v});
    }
    if (st_->fn_mark != kNoReg && st_->rc_pushes > 0) rc_cleanup(st_->fn_mark);
    release_slots();
    if (st_->is_entry) {
        // vthread 先验：退出前 drain 剩余 vthread、打印未 join 异常、
        // 放弃永久阻塞者（docs/vthread-design.md §退出语义）。
        call_void(RtFn::VtShutdown, {});
        release_globals();
        call_void(RtFn::GcShutdown, {});
        call_void(RtFn::RcShutdown, {});
    }
    Inst in;
    in.op = IrOp::Return;
    in.a = v;
    emit(std::move(in));}

void Lowerer::write_cell(const std::string& name, VReg v, TypePtr vty) {
    TypePtr T = captured_ty(name);
    v = coerce(v, vty, T);
    VReg cell = is_cell(name) ? load_cell(name) : kNoReg;
    if (cell == kNoReg) {
        auto* m = closure();
        int idx = m->env_index.at(name);
        VReg env = load_slot(st_->env_slot, t_unknown());
        cell = field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
    }
    retain_elem(v, list_kind_filled(mk_list(T ? T : t_unknown())));
    Inst in;
    in.op = IrOp::IndexStore;
    in.a = cell;
    in.b = const_int(0);
    in.c = v;
    emit(std::move(in));}

VReg Lowerer::read_env(const std::string& name) {
    auto* m = closure();
    int idx = m->env_index.at(name);
    TypePtr T = captured_ty(name);
    VReg env = load_slot(st_->env_slot, t_unknown());
    VReg c = field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
    Inst in;
    in.op = IrOp::IndexLoad;
    in.a = c;
    in.b = const_int(0);
    return def(T ? T : t_unknown(), std::move(in));}

VReg Lowerer::cell_for_capture(const std::string& name) {
    if (is_cell(name)) return load_cell(name);
    if (is_env(name)) {
        auto* m = closure();
        int idx = m->env_index.at(name);
        TypePtr T = captured_ty(name);
        VReg env = load_slot(st_->env_slot, t_unknown());
        return field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
    }
    return kNoReg;}

void Lowerer::finalize_param_borrows() {
    if (st_->is_generator) return;
    IRFunction* fn = st_->fn;
    const size_t ns = fn->slot_ty.size();
    std::vector<bool> written(ns, false);
    std::unordered_map<VReg, int> uses;
    auto touch = [&](VReg v) {
        if (v != kNoReg) uses[v]++;
    };
    // release_slots 在出口会先 dec 再写 0；这些清零不算「函数体写过
    // 形参」，否则只读形参永远判定不出来。
    std::unordered_set<VReg> zero_regs;
    for (auto& b : fn->blocks)
        for (auto& in : b.insts)
            if (in.op == IrOp::ConstInt && in.imm == 0 && in.dst != kNoReg)
                zero_regs.insert(in.dst);
    for (auto& b : fn->blocks)
        for (auto& in : b.insts) {
            if ((in.op == IrOp::StoreSlot || in.op == IrOp::SlotAddr) && in.idx >= 0 &&
                in.idx < static_cast<int>(ns)) {
                bool exit_zero = in.op == IrOp::StoreSlot &&
                                 ir_is_heap(fn->slot_ty[static_cast<size_t>(in.idx)]) &&
                                 zero_regs.contains(in.a);
                if (!exit_zero) written[static_cast<size_t>(in.idx)] = true;
            }
            touch(in.a);
            touch(in.b);
            touch(in.c);
            for (VReg r : in.args) touch(r);
        }
    std::vector<bool> read_only(ns, false);
    bool any = false;
    for (auto& p : fn->params)
        if (ir_is_heap(p.ty) && p.slot >= 0 && p.slot < static_cast<int>(ns) &&
            !written[static_cast<size_t>(p.slot)]) {
            read_only[static_cast<size_t>(p.slot)] = true;
            // 入口 retain 会被删掉，槽此后只是借用：异常展开不能释放。
            fn->slot_owned[static_cast<size_t>(p.slot)] = false;
            any = true;
        }
    if (!any) return;

    std::vector<std::pair<int, size_t>> dels;
    // 入口 retain：只匹配入口块开头的连续序列，形态不符就停。
    if (!fn->blocks.empty()) {
        auto& ins = fn->blocks.front().insts;
        size_t i = 0;
        if (i < ins.size() && ins[i].op == IrOp::CallRt &&
            static_cast<RtFn>(ins[i].imm) == RtFn::RcMark)
            ++i;
        for (auto& p : fn->params) {
            if (!ir_is_heap(p.ty)) continue;
            if (i + 1 >= ins.size()) break;
            if (ins[i].op != IrOp::LoadSlot || ins[i].idx != p.slot) break;
            const Inst& inc = ins[i + 1];
            if (inc.op != IrOp::CallRt || static_cast<RtFn>(inc.imm) != RtFn::RcInc ||
                inc.args.size() != 1 || inc.args[0] != ins[i].dst)
                break;
            if (read_only[static_cast<size_t>(p.slot)]) {
                dels.push_back({0, i + 1});
                if (ins[i].dst != kNoReg && uses[ins[i].dst] == 1)
                    dels.push_back({0, i});
            }
            i += 2;
        }
    }
    // 出口 release：read-only 槽的 load + rc_dec 只可能是 release_slots。
    for (size_t b = 0; b < fn->blocks.size(); ++b) {
        auto& ins = fn->blocks[b].insts;
        for (size_t i = 0; i + 1 < ins.size(); ++i) {
            if (ins[i].op != IrOp::LoadSlot || ins[i].idx < 0 ||
                ins[i].idx >= static_cast<int>(ns) ||
                !read_only[static_cast<size_t>(ins[i].idx)])
                continue;
            const Inst& dec = ins[i + 1];
            if (dec.op != IrOp::CallRt || static_cast<RtFn>(dec.imm) != RtFn::RcDec ||
                dec.args.size() != 1 || dec.args[0] != ins[i].dst)
                continue;
            dels.push_back({static_cast<int>(b), i + 1});
            if (ins[i].dst != kNoReg && uses[ins[i].dst] == 1)
                dels.push_back({static_cast<int>(b), i});
            // release_slots 紧接着的「槽清零」也一并删掉：槽改成了
            // 借用，内联重映射后这个槽没有落点（slot_map = -1）。
            // 清零用的常量 vreg 是多个槽共用的，只删 store 本身。
            for (size_t k = i + 2; k < ins.size() && k <= i + 3; ++k) {
                if (ins[k].op == IrOp::StoreSlot && ins[k].idx == ins[i].idx) {
                    dels.push_back({static_cast<int>(b), k});
                    break;
                }
                if (ins[k].op != IrOp::ConstInt) break;
            }
            ++i;
        }
    }
    if (dels.empty()) return;
    std::ranges::sort(dels, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    for (auto& [b, idx] : dels) {
        auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
        if (idx < ins.size()) ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(idx));
    }}

}  // namespace pyadamas
