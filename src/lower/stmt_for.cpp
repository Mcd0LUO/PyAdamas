// lower/stmt_for.cpp —— for 的各个形态（range/enumerate/zip/序列）
#include "lower/impl.h"
#include "lower/idiom.h"

namespace pyadamas {

static bool can_destructure(const Expr* t, size_t n) {
    if (!t || t->kind != ExprKind::Tuple) return false;
    const auto& tup = static_cast<const TupleExpr&>(*t);
    if (tup.elts.size() != n) return false;
    for (const auto& e : tup.elts)
        if (!e || e->kind != ExprKind::Name) return false;
    return true;
}

// 能按下标逐个取的容器（generator / range 走各自的路径，这里不碰）。
static bool is_countable(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::List:
        case TyKind::Tuple:
        case TyKind::Str:
        case TyKind::Dict:
        case TyKind::Set: return true;
        default: return false;
    }
}

void Lowerer::lower_for(For& f) {
    if (f.is_async) {
        VReg seq = expr(*f.iter);
        lower_async_for(f, seq, tc_.type_of(f.iter.get()));
        return;
    }
    if (f.iter->kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(*f.iter);
        if (c.func->kind == ExprKind::Name &&
            static_cast<Name&>(*c.func).id == "range" && is_builtin("range")) {
            lower_for_range(f, c);
            return;
        }
    }
    if (TypePtr it = tc_.type_of(f.iter.get()); it && it->kind == TyKind::File) {
        lower_for_file(f);
        return;
    }
    // range 对象：边界从对象里读，逐个产出，不物化。
    if (TypePtr it = tc_.type_of(f.iter.get()); it && it->kind == TyKind::Range) {
        VReg r = expr(*f.iter);
        lower_for_range_obj(f, r);
        return;
    }
    lower_for_seq(f);
}

void Lowerer::lower_for_range(For& f, Call& c) {
    if (f.target->kind != ExprKind::Name) {
        err(f.line, f.col, "for-loop variable must be a name for range()");
        return;
    }
    if (c.args.size() < 1 || c.args.size() > 3) {
        err(c.line, c.col, "range() takes 1 to 3 arguments");
        return;
    }
    if (!c.keywords.empty()) {
        err(c.line, c.col, "range() does not take keyword arguments");
        return;
    }

    // 切片拷贝惯用法：for k in range(lo,hi): dst.append(src[k]) → 一次批量
    // 拷贝。必须在参数求值之前尝试：匹配失败不能留下已发射的 IR。
    if (lower_copy_range_idiom(f, c)) return;

    VReg start, stop;
    int64_t step = 1;
    // 动态步长：方向（< 还是 >）在运行期定，循环里每轮多一个分支。
    bool dyn_step = false;
    VReg step_v = kNoReg;
    if (c.args.size() == 1) {
        start = const_int(0);
        stop = expr(*c.args[0]);
    } else {
        start = expr(*c.args[0]);
        stop = expr(*c.args[1]);
    }
    if (c.args.size() == 3) {
        auto cv = as_const(c.args[2].get());
        if (cv && (cv->ty->kind == TyKind::Int || cv->ty->kind == TyKind::Bool)) {
            step = cv->i;
            if (step == 0) {
                err(c.args[2]->line, c.args[2]->col, "range() step cannot be zero");
                return;
            }
        } else {
            step_v = expr(*c.args[2]);
            dyn_step = true;
        }
    }

    lower_for_range_core(f, start, stop, step_v, dyn_step, step, &c);
}

// range 对象当迭代源：边界从对象里读，步长方向运行期定。
void Lowerer::lower_for_range_obj(For& f, VReg r) {
    if (f.target->kind != ExprKind::Name) {
        err(f.line, f.col, "for-loop variable must be a name for range()");
        return;
    }
    VReg start = call_runtime(RtFn::RangeFirst, t_int(), {r});
    VReg stop = call_runtime(RtFn::RangeStop, t_int(), {r});
    VReg step = call_runtime(RtFn::RangeStep, t_int(), {r});
    lower_for_range_core(f, start, stop, step, true, 1, nullptr);
}

// 计数循环的本体：range(...) 调用和 range 对象共用。
void Lowerer::lower_for_range_core(For& f, VReg start, VReg stop, VReg step_v, bool dyn_step,
                                    int64_t step, const Call* c) {
    // 循环计数器独立于用户变量：Python 里在循环体里给 i 重新赋值
    // 不影响迭代，计数器必须是私有的。
    int ctr = new_temp_slot(t_int());
    store_slot(ctr, start);
    // 生成器里 vreg 不跨 yield 存活：上界必须落槽，每次条件判断重新读。
    int stop_slot = -1;
    if (st_->is_generator) {
        stop_slot = new_temp_slot(t_int());
        store_slot(stop_slot, stop);
    }
    // 动态步长同样要落槽（生成器里跨 yield 不存活）。
    int step_slot = -1;
    if (dyn_step) {
        if (st_->is_generator) {
            step_slot = new_temp_slot(t_int());
            store_slot(step_slot, step_v);
        }
        // 步长 0 是 ValueError（CPython 在 range() 求值时抛）。
        VReg sv0 = step_slot >= 0 ? load_slot(step_slot, t_int()) : step_v;
        Inst zc;
        zc.op = IrOp::Cmp;
        zc.cop = CmpKind::Eq;
        zc.a = sv0;
        zc.b = const_int(0);
        VReg isz = def(t_bool(), std::move(zc));
        int zero_blk = new_block("range.step.zero");
        int ok_blk = new_block("range.step.ok");
        Inst brz;
        brz.op = IrOp::Branch;
        brz.a = isz;
        brz.target = zero_blk;
        brz.target2 = ok_blk;
        emit(std::move(brz));
        switch_to(zero_blk);
        call_void(raise_rt(RtFn::Raise),
                  {const_str("ValueError"), const_str("range() arg 3 must not be zero")});
        Inst jz;
        jz.op = IrOp::Jump;
        jz.target = ok_blk;
        emit(std::move(jz));
        switch_to(ok_blk);
    }

    // 非负提示窗口（lower/idiom.cpp）：入口擦目标槽旧提示，退出擦本层键。
    int target_slot = -1;
    bool nn_hint = enter_nonneg_window(f, start, ctr, dyn_step, step, target_slot);

    int cond = new_block("for.cond");
    int body = new_block("for.body");
    int incr = new_block("for.incr");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    auto cmp_branch = [&](VReg a, VReg b, CmpKind k) {
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = k;
        cmp.a = a;
        cmp.b = b;
        VReg cc = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = body;
        br.target2 = orelse;
        emit(std::move(br));
    };

    switch_to(cond);
    VReg cv = load_slot(ctr, t_int());
    VReg stop_v = stop_slot >= 0 ? load_slot(stop_slot, t_int()) : stop;
    if (dyn_step) {
        // 方向是循环不变量，分支预测得住；比较本身多一次而已。
        VReg sv = step_slot >= 0 ? load_slot(step_slot, t_int()) : step_v;
        Inst sp;
        sp.op = IrOp::Cmp;
        sp.cop = CmpKind::Gt;
        sp.a = sv;
        sp.b = const_int(0);
        VReg pos = def(t_bool(), std::move(sp));
        int cpos = new_block("for.cond.lt");
        int cneg = new_block("for.cond.gt");
        Inst brd;
        brd.op = IrOp::Branch;
        brd.a = pos;
        brd.target = cpos;
        brd.target2 = cneg;
        emit(std::move(brd));
        switch_to(cpos);
        cmp_branch(cv, stop_v, CmpKind::Lt);
        switch_to(cneg);
        cmp_branch(cv, stop_v, CmpKind::Gt);
    } else {
        cmp_branch(cv, stop_v, step > 0 ? CmpKind::Lt : CmpKind::Gt);
    }

    // range(len(a)) 且 start=0/step=1：计数器贴出的 k 恰好是 0..len(a)-1。
    // 循环体给 a/k 赋值（写槽）会作废提示；调用、del、切片赋值等副作用由
    // side_effect_seq_ 统一挡（它们都可能改变 a 的长度）。
    FuncCtx::BoundsHint bounds_hint;
    bool have_bounds_hint = false;
    if (opts_.opt_level >= 1 && !st_->is_generator && c) {
        auto int_is = [](const Expr* e, int64_t v) {
            if (!e || e->kind != ExprKind::Constant) return false;
            const auto& ce = static_cast<const Constant&>(*e);
            return ce.ck == Constant::CK::Int && ce.i == v;
        };
        bool zero_start =
            c->args.size() == 1 || (c->args.size() >= 2 && int_is(c->args[0].get(), 0));
        bool unit_step = !dyn_step && (c->args.size() < 3 || step == 1);
        const Expr* stop_arg = c->args.size() == 1 ? c->args[0].get() : c->args[1].get();
        if (zero_start && unit_step && stop_arg->kind == ExprKind::Call) {
            const auto& lc = static_cast<const Call&>(*stop_arg);
            if (lc.func->kind == ExprKind::Name &&
                static_cast<Name&>(*lc.func).id == "len" && lc.args.size() == 1 &&
                lc.args[0]->kind == ExprKind::Name) {
                const std::string& seq_name = static_cast<Name&>(*lc.args[0]).id;
                const std::string& idx_name = static_cast<Name&>(*f.target).id;
                auto bit = st_->slots.find(seq_name);
                auto sit = st_->slots.find(idx_name);
                if (bit != st_->slots.end() && sit != st_->slots.end() &&
                    !is_cell(seq_name) && !is_cell(idx_name)) {
                    bounds_hint.seq_slot = bit->second;
                    bounds_hint.idx_slot = sit->second;
                    have_bounds_hint = true;
                }
            }
        }
    }


    switch_to(body);
    clean();
    VReg iv = load_slot(ctr, t_int());
    assign_to(*f.target, iv, t_int());
    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    int se_before_body = side_effect_seq_;
    if (have_bounds_hint) st_->bounds_hints.push_back(&bounds_hint);
    stmts(f.body);
    exit_nonneg_window(nn_hint, ctr, target_slot);
    if (have_bounds_hint) st_->bounds_hints.pop_back();
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (have_bounds_hint && bounds_hint.valid && side_effect_seq_ == se_before_body) {
        for (auto [b, v] : bounds_hint.patches) {
            auto& insts = st_->fn->blocks[static_cast<size_t>(b)].insts;
            for (auto& in : insts)
                if (in.op == IrOp::IndexLoad && in.dst == v) {
                    in.op = IrOp::IndexLoadUnchecked;
                    break;
                }
        }
    }
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    VReg iv2 = load_slot(ctr, t_int());
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = iv2;
    add.b = dyn_step ? (step_slot >= 0 ? load_slot(step_slot, t_int()) : step_v)
                      : const_int(step);
    VReg iv3 = def(t_int(), std::move(add));
    store_slot(ctr, iv3);
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(incr, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

// for i, v in enumerate(xs)：不建 [下标, 值] 的元组表，直接计数 + 取元素。
// 语义等价于 CPython 的惰性 enumerate：循环体里改 xs 会看到新元素（以前
// 是先把整张表建好，属于快照语义）。
void Lowerer::lower_for_seq(For& f) {
    // for i, v in enumerate(xs) / for a, b in zip(xs, ys)：直接计数循环，
    // 不物化 [下标, 值] 的元组表——每个元素省一次堆分配。
    if (f.iter->kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(*f.iter);
        if (c.func->kind == ExprKind::Name && c.keywords.empty()) {
            const std::string& fn = static_cast<Name&>(*c.func).id;
            // 目标是单个名字时也可以走快速路径（由 bind_iter_pair 把两个值
            // 构成元组再赋值），这样 `for kv in enumerate(d)` 才有迭代守卫。
            auto pair_target = [&]() {
                return can_destructure(f.target.get(), 2) ||
                       f.target->kind == ExprKind::Name;
            };
            if (fn == "enumerate" && is_builtin("enumerate") && c.args.size() == 1 &&
                pair_target() &&
                is_countable(tc_.type_of(c.args[0].get()))) {
                lower_for_enumerate(f, c);
                return;
            }
            if (fn == "zip" && is_builtin("zip") && c.args.size() == 2 &&
                pair_target() &&
                is_countable(tc_.type_of(c.args[0].get())) &&
                is_countable(tc_.type_of(c.args[1].get()))) {
                lower_for_zip(f, c);
                return;
            }
        }
    }
    VReg seq = expr(*f.iter);
    TypePtr seq_ty = tc_.type_of(f.iter.get());
    // 循环期间必须自己持有一份序列引用。序列表达式若是从变量/属性/下标
    // 读来的借用，循环体内重绑该名字（`for x in a: a = ...`）会把最后一
    // 份引用 dec 掉，后续迭代读到已释放对象（UAF：a=[1,2,3] 只迭代出 2
    // 个元素）。新分配的堆值已由临时栈持有一份引用；借用在这里补一份，
    // 随语句边界的 rc_cleanup 释放，生命周期与 CPython 的迭代器一致。
    // 已在临时栈上的值再补一份也不会多算：inc 与这次 push_temp 成对，
    // 只有净值时才会影响 rc_pushes（保守方向）。
    if (ir_is_heap(seq_ty) && !st_->fresh_vregs.count(seq)) {
        rc_inc(seq);
        push_temp(seq);
    }
    // deque 没有按下标直取的迭代路径：先转成列表再走普通 list 迭代。
    // enum 类对象（for c in Color）：物化成员表后按 list 迭代。
    if (seq_ty && seq_ty->kind == TyKind::ClassObj && !seq_ty->sub.empty() &&
        seq_ty->sub[0] && seq_ty->sub[0]->cls && seq_ty->sub[0]->cls->is_enum) {
        const ClassInfo* ec = seq_ty->sub[0]->cls;
        VReg xs = enum_member_list(ec);
        seq = xs;
        seq_ty = mk_list(mk_class(const_cast<ClassInfo*>(ec)));
    }
    if (seq_ty && seq_ty->kind == TyKind::Deque) {
        VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {seq});
        push_temp(xs);
        seq = xs;
        seq_ty = mk_list(t_int());
    }
    // bytes 迭代产出 int（0..255）：先转 list[int] 再走普通列表路径。
    if (seq_ty && seq_ty->kind == TyKind::Bytes) {
        VReg xs = call_runtime(RtFn::BytesToList, mk_list(t_int()), {seq});
        push_temp(xs);
        seq = xs;
        seq_ty = mk_list(t_int());
    }
    // 用户类实现 __iter__ 时先调用它，再按返回类型（generator/list/...）
    // 走现有的迭代路径。
    if (seq_ty && seq_ty->kind == TyKind::Class) {
        VReg it = dunder_call(seq, seq_ty, "__iter__", {}, nullptr);
        if (it == kNoReg) {
            err(f.iter->line, f.iter->col,
                std::format("cannot iterate over '{}' (no usable __iter__)",
                            type_name(seq_ty)));
            return;
        }
        seq = it;
        seq_ty = vreg_ty(it);
    }
    if (seq_ty && seq_ty->kind == TyKind::Generator) {
        lower_for_gen(f, seq, seq_ty);
        return;
    }
    // unknown（动态属性读出的列表等）：运行期按种类物化成 list[unknown]。
    if (!seq_ty || seq_ty->kind == TyKind::Unknown) {
        seq = call_runtime(RtFn::AnyIterList, mk_list(t_unknown()), {seq});
        push_temp(seq);
        seq_ty = mk_list(t_unknown());
    }
    if (!seq_ty || (seq_ty->kind != TyKind::List && seq_ty->kind != TyKind::Tuple &&
                    seq_ty->kind != TyKind::Str && seq_ty->kind != TyKind::Dict &&
                    seq_ty->kind != TyKind::Set)) {
        err(f.iter->line, f.iter->col,
            std::format("cannot iterate over '{}' in the code generator", type_name(seq_ty)));
        return;
    }

    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    // 生成器里 vreg 不跨 yield 存活：序列和长度都必须落槽。
    int seq_slot = -1;
    int n_slot = -1;
    if (st_->is_generator) {
        seq_slot = new_temp_slot(seq_ty);
        store_slot(seq_slot, seq);
        n_slot = new_temp_slot(t_int());
        store_slot(n_slot, n);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, seq_ty) : seq; };
    auto n_val = [&]() { return n_slot >= 0 ? load_slot(n_slot, t_int()) : n; };

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    /* 遍历 str 时挂顺序游标：每轮 O(1) 取码点，整体 O(n)；
       没有游标就是每轮从头扫（O(n^2)）。 */
    int str_cur = seq_ty->kind == TyKind::Str ? new_str_cursor() : -1;

    int cond = new_block("for.cond");
    int body = new_block("for.body");
    int incr = new_block("for.incr");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    // 水位取在序列求值之后：seq 本身可能是个临时列表，不能被每轮清掉。
    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    int ver_slot = -1;
    if (seq_ty->kind == TyKind::Dict) {
        ver_slot = new_temp_slot(t_int());
        store_slot(ver_slot, call_runtime(RtFn::DictVersion, t_int(), {seq_val()}));
    }
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    // 迭代期间容器大小变化 → CPython 抛**可捕获**的 RuntimeError，且在
    // 「还有没有下一个」之前检查：删到空之后仍要在下一次取元素时报错，
    // 而不是被下标上界悄悄结束。放在 cond 也顺带覆盖「删一个再加一个」
    // （大小不变，越界检查发现不了）。
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set) {
        Inst lc;
        lc.op = IrOp::Len;
        lc.a = seq_val();
        VReg cur_len = def(t_int(), std::move(lc));
        Inst ne;
        ne.op = IrOp::Cmp;
        ne.cop = CmpKind::Ne;
        ne.a = cur_len;
        ne.b = n_val();
        VReg ne_cc = def(t_bool(), std::move(ne));
        int changed = new_block("for.iter.changed");
        int ok_b = new_block("for.iter.ok");
        Inst brc;
        brc.op = IrOp::Branch;
        brc.a = ne_cc;
        brc.target = changed;
        brc.target2 = ok_b;
        emit(std::move(brc));
        switch_to(changed);
        call_void(raise_rt(RtFn::Raise),
                  {const_str("RuntimeError"),
                   const_str(seq_ty->kind == TyKind::Dict
                                 ? "dictionary changed size during iteration"
                                 : "Set changed size during iteration")});
        switch_to(ok_b);
        if (seq_ty->kind == TyKind::Dict && ver_slot >= 0) {
            VReg cur_v = call_runtime(RtFn::DictVersion, t_int(), {seq_val()});
            Inst ve;
            ve.op = IrOp::Cmp;
            ve.cop = CmpKind::Ne;
            ve.a = cur_v;
            ve.b = load_slot(ver_slot, t_int());
            int kchanged = new_block("for.iter.keys");
            int kok = new_block("for.iter.keys_ok");
            Inst vbr;
            vbr.op = IrOp::Branch;
            vbr.a = def(t_bool(), std::move(ve));
            vbr.target = kchanged;
            vbr.target2 = kok;
            emit(std::move(vbr));
            switch_to(kchanged);
            call_void(raise_rt(RtFn::Raise),
                      {const_str("RuntimeError"),
                       const_str("dictionary keys changed during iteration")});
            switch_to(kok);
        }
    }
    VReg iv = load_slot(idx, t_int());
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = iv;
    cmp.b = n_val();
    VReg cc = def(t_bool(), std::move(cmp));
    // 缓存长度的比较指令位置：循环体若有副作用（调用 / del / 切片赋值…）
    // 可能改变列表长度，事后要把这里改成「每轮重读 len」（见下方补丁）。
    const size_t cond_cmp_pos = st_->fn->blocks[static_cast<size_t>(cond)].insts.size() - 1;
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv2 = load_slot(idx, t_int());
    TypePtr et = tc_.type_of(f.target.get());
    if (!et || et->kind == TyKind::Unknown) {
        if (seq_ty->kind == TyKind::Str)
            et = t_str();
        else if (!seq_ty->sub.empty())
            et = seq_ty->sub[0];
    }
    VReg ev;
    VReg sv = seq_val();
    int load_blk = -1;
    size_t load_pos = 0;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set) {
        // 字典/集合按插入顺序按下标取 key；返回的是容器内部的借用引用。
        ev = call_runtime(RtFn::DictKeyAt, et, {sv, iv2});
    } else {
        load_blk = st_->blk;
        load_pos = st_->fn->blocks[load_blk].insts.size();
        if (seq_ty->kind == TyKind::Str) {
            // 遍历字符串时每个下标是一个码点串（新分配，入临时栈）；
            // 列表/元组是容器内部的借用引用。
            ev = str_char_at(sv, iv2, str_cur);
        } else {
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = sv;
            get.b = iv2;
            ev = def(et, std::move(get));
        }
    }
    assign_to(*f.target, ev, et);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    int se_before_body = side_effect_seq_;
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    // **循环体可能改变列表长度时，退回每轮重读 len。**
    //
    // CPython 的 list 迭代器每取一个元素都读当前 PyList_GET_SIZE：体内
    // pop() 只会让它提前结束、append() 会让新元素也被遍历。缓存入口长度
    // 会把这两种都算错（前者多报 IndexError，后者漏元素）。
    //
    // 判据复用 side_effect_seq_（调用/del/切片赋值/setter 都会 ++），所以
    // 「体内没有任何副作用」的热循环仍然只读一次 len（连 IndexLoadUnchecked
    // 优化也依赖同一个判定）。只有脏循环付每轮一次 len 的代价。
    //
    // 元组长度不可变，不需要补丁；str/dict/set 维持原口径（dict 的
    // 「迭代中改大小」CPython 是 RuntimeError，属另一条语义，不在此列）。
    if (seq_ty->kind == TyKind::List && side_effect_seq_ != se_before_body) {
        auto& cinsts = st_->fn->blocks[static_cast<size_t>(cond)].insts;
        if (cond_cmp_pos < cinsts.size() && cinsts[cond_cmp_pos].op == IrOp::Cmp) {
            std::vector<Inst> pre;
            VReg seq_cond = seq;
            if (seq_slot >= 0) {
                // 生成器：seq vreg 不跨挂起存活，从帧槽取回。
                seq_cond = static_cast<VReg>(st_->fn->vreg_ty.size());
                st_->fn->vreg_ty.push_back(seq_ty);
                Inst ld;
                ld.op = IrOp::LoadSlot;
                ld.idx = seq_slot;
                ld.dst = seq_cond;
                ld.ty = seq_ty;
                pre.push_back(std::move(ld));
            }
            VReg len_now = static_cast<VReg>(st_->fn->vreg_ty.size());
            st_->fn->vreg_ty.push_back(t_int());
            Inst ln2;
            ln2.op = IrOp::Len;
            ln2.a = seq_cond;
            ln2.dst = len_now;
            ln2.ty = t_int();
            pre.push_back(std::move(ln2));
            cinsts[cond_cmp_pos].b = len_now;
            cinsts.insert(cinsts.begin() + static_cast<std::ptrdiff_t>(cond_cmp_pos),
                          pre.begin(), pre.end());
        }
    }
    // 列表/元组按 0..len-1 遍历，元素下标的边界检查可以免掉：
    //   - 循环体无副作用：长度不会变，整个循环的守卫都成立；
    //   - 有副作用：取元素紧跟在条件分支之后，中间只有循环自己的索引槽
    //     读和一句空操作的 rc_cleanup（loop_mark 之后没有临时值）。此刻
    //     i < len(seq) 刚被条件证明，用户代码还没有机会改容器；下一轮
    //     条件会重读 len，与 CPython 迭代器的语义一致。
    // 生成器帧可能跨 yield 挂起，直接跳过。
    bool body_load_first = load_blk >= 0;
    if (body_load_first) {
        const auto& pre = st_->fn->blocks[static_cast<size_t>(load_blk)].insts;
        for (size_t k = 0; k < load_pos && k < pre.size(); ++k) {
            const Inst& p = pre[k];
            const bool idx_load = p.op == IrOp::LoadSlot && p.idx == idx;
            const bool noop_clean = p.op == IrOp::CallRt &&
                                    static_cast<RtFn>(p.imm) == RtFn::RcCleanup;
            if (!idx_load && !noop_clean) {
                body_load_first = false;
                break;
            }
        }
    }
    if (opts_.opt_level >= 1 && load_blk >= 0 && !st_->is_generator &&
        (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
        (side_effect_seq_ == se_before_body || body_load_first)) {
        auto& insts = st_->fn->blocks[load_blk].insts;
        if (load_pos < insts.size()) insts[load_pos].op = IrOp::IndexLoadUnchecked;
    }
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    VReg iv3 = load_slot(idx, t_int());
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = iv3;
    add.b = const_int(1);
    VReg iv4 = def(t_int(), std::move(add));
    store_slot(idx, iv4);
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(incr, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}



}  // namespace pyadamas
