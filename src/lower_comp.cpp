// lower_comp.cpp —— 推导式降级（list/set/dict/生成器/异步）
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_comp(Expr& e) {
    std::vector<ExprPtr>* gens = nullptr;
    Expr* elt_expr = nullptr;
    Expr* key_expr = nullptr;
    Expr* val_expr = nullptr;
    bool is_set = false;
    bool is_dict = false;
    switch (e.kind) {
        case ExprKind::ListComp: {
            auto& c = static_cast<ListComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::GeneratorExp: {
            auto& c = static_cast<GeneratorExp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::SetComp: {
            auto& c = static_cast<SetComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            is_set = true;
            break;
        }
        case ExprKind::DictComp: {
            auto& c = static_cast<DictComp&>(e);
            gens = &c.generators;
            key_expr = c.key.get();
            val_expr = c.value.get();
            is_dict = true;
            break;
        }
        default: return const_none();
    }

    TypePtr rty = tc_.type_of(&e);
    TypePtr elem_ty = t_unknown();
    TypePtr val_ty = t_unknown();

    bool any_async = false;
    for (auto& g : *gens)
        if (static_cast<Comprehension&>(*g).is_async) any_async = true;
    bool async_mode = false;
    if (any_async) {
        if (!st_->is_coroutine) {
            err(e.line, e.col, "'async for' in a comprehension requires an async function");
        } else if (comp_ctx_node_ != &e) {
            err(e.line, e.col,
                "'async for' in a comprehension is only supported as a statement, "
                "assignment or return value");
        } else if (e.kind == ExprKind::GeneratorExp) {
            err(e.line, e.col, "async generator expressions are not supported yet");
        } else {
            async_mode = true;
        }
        if (!async_mode) return const_none();
    }

    VReg acc;
    if (is_dict) {
        if (!rty || rty->kind != TyKind::Dict) rty = mk_dict(t_unknown(), t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        if (rty->sub.size() > 1) val_ty = rty->sub[1];
        auto [hslot, eslot] = dunder_key_slots(elem_ty);
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(own_kind(val_ty)), const_int(hslot),
                            const_int(eslot)});
        push_temp(acc);
    } else if (is_set) {
        if (!rty || rty->kind != TyKind::Set) rty = mk_set(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        auto [hslot, eslot] = dunder_key_slots(elem_ty);
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(0), const_int(hslot), const_int(eslot)});
        push_temp(acc);
    } else {
        if (!rty || rty->kind != TyKind::List) rty = mk_list(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        Inst mk;
        mk.op = IrOp::MakeList;
        mk.imm = list_kind(rty);
        acc = def_heap(rty, std::move(mk));
    }

    // 挂起后 vreg 不存活：累加器落帧槽，循环内外都从槽里读。
    int acc_slot = -1;
    if (async_mode) {
        acc_slot = new_temp_slot(rty);
        store_slot(acc_slot, acc);
    }
    auto acc_val = [&]() { return acc_slot >= 0 ? load_slot(acc_slot, rty) : acc; };

    auto emit_result = [&]() {
        if (is_dict) {
            VReg k = expr(*key_expr);
            k = coerce(k, tc_.type_of(key_expr), elem_ty);
            if (!move_fresh(k, elem_ty)) retain_elem(k, own_kind(elem_ty));
            VReg v = expr(*val_expr);
            v = coerce(v, tc_.type_of(val_expr), val_ty);
            if (!move_fresh(v, val_ty)) retain_elem(v, own_kind(val_ty));
            call_void(RtFn::DictSet, {acc_val(), k, v});
            return;
        }
        VReg v = expr(*elt_expr);
        v = coerce(v, tc_.type_of(elt_expr), elem_ty);
        if (is_set) {
            if (!move_fresh(v, elem_ty)) retain_elem(v, own_kind(elem_ty));
            call_void(RtFn::DictSet, {acc_val(), v, const_int(0)});
        } else {
            if (!move_fresh(v, elem_ty)) retain_elem(v, list_kind(rty));
            call_void(RtFn::ListAppend, {acc_val(), v});
        }
    };

    comp_slots_.emplace_back();

    std::function<void(size_t)> gen = [&](size_t gi) {
        if (gi >= gens->size()) {
            emit_result();
            return;
        }
        auto& g = static_cast<Comprehension&>(*(*gens)[gi]);
        VReg seq = expr(*g.iter);
        TypePtr sty = tc_.type_of(g.iter.get());
        // bytes 迭代产出 int（0..255）：先转 list[int] 再走普通列表路径。
        if (sty && sty->kind == TyKind::Bytes) {
            VReg xs = call_runtime(RtFn::BytesToList, mk_list(t_int()), {seq});
            push_temp(xs);
            seq = xs;
            sty = mk_list(t_int());
        }

        if (g.is_async) {
            TypePtr sst = sty ? sty : t_unknown();
            TypePtr ety = t_unknown();
            if (sty && sty->kind == TyKind::AsyncGenerator && !sty->sub.empty())
                ety = sty->sub[0];
            int seq_slot = new_temp_slot(sst);
            store_slot(seq_slot, seq);
            int hs_slot = new_temp_slot(t_int());

            int next_try = new_block("comp.anext");
            int fetch = new_block("comp.afetch");
            int land = new_block("comp.aland");
            int reraise_b = new_block("comp.areaise");
            int end = new_block("comp.aend");

            Inst j;
            j.op = IrOp::Jump;
            j.target = next_try;
            emit(std::move(j));

            switch_to(next_try);
            VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
            store_slot(hs_slot, h);
            VReg sj = setjmp_handler(hs_slot);
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = sj;
                br.target = land;
                br.target2 = fetch;
                emit(std::move(br));
            }
            FuncCtx::Cleanup cf;
            cf.kind = FuncCtx::Cleanup::Kind::Handler;
            cf.handler_slot = hs_slot;
            cf.land = land;
            st_->cleanups.push_back(cf);

            switch_to(fetch);
            Inst call;
            call.op = IrOp::CallRt;
            call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
            call.args = {load_slot(seq_slot, sst)};
            VReg w = def_heap(mk_coroutine(ety), std::move(call));
            VReg val = suspend_on_awaitable(w, ety, g.line, g.col,
                                            static_cast<size_t>(-1), false);
            st_->cleanups.pop_back();
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
            // handler 离开后入临时栈，避免 leave 的水位清理释放掉刚送达的值。
            if (ir_is_heap(ety)) push_temp(val);

            if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
                int slot = comp_slot(tn->id);
                if (slot < 0) {
                    slot = st_->fn->new_slot(ety);
                    comp_slots_.back()[tn->id] = slot;
                }
                store_slot(slot, val);
            } else {
                assign_to(*g.target, val, ety);
            }

            for (auto& cond_expr : g.ifs) {
                VReg c = expr(*cond_expr);
                c = truth_bool(c, tc_.type_of(cond_expr.get()));
                int ok_b = new_block("comp.aif");
                Inst cb;
                cb.op = IrOp::Branch;
                cb.a = c;
                cb.target = ok_b;
                cb.target2 = next_try;
                emit(std::move(cb));
                switch_to(ok_b);
            }

            gen(gi + 1);
            if (!terminated()) {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = next_try;
                emit(std::move(jj));
            }

            switch_to(land);
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
            VReg m = call_runtime(RtFn::ExcMatches, t_bool(),
                                  {const_str("StopAsyncIteration")});
            int stop_ok = new_block("comp.astop");
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = m;
                br.target = stop_ok;
                br.target2 = reraise_b;
                emit(std::move(br));
            }
            switch_to(reraise_b);
            call_void(RtFn::ExcReraise, {});
            end_unreachable();
            switch_to(stop_ok);
            call_void(RtFn::ExcClear, {});
            {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = end;
                emit(std::move(jj));
            }
            switch_to(end);
            return;
        }

        // 生成器当迭代源：没有长度也没有下标，流式消费。
        if (sty && sty->kind == TyKind::Generator) {
            TypePtr ety = sty->sub.empty() ? t_unknown() : sty->sub[0];
            emit_gen_for_each(seq, sty, [&](VReg ev, TypePtr, int cont, int) {
                if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
                    int slot = comp_slot(tn->id);
                    if (slot < 0) {
                        slot = st_->fn->new_slot(ety);
                        comp_slots_.back()[tn->id] = slot;
                    }
                    store_slot(slot, ev);
                } else {
                    assign_to(*g.target, ev, ety);
                }
                // 过滤器不成立就直接跳下一轮（cont 是 emit_gen_for_each
                // 的取元素块）。
                for (auto& cond_expr : g.ifs) {
                    VReg c = expr(*cond_expr);
                    c = truth_bool(c, tc_.type_of(cond_expr.get()));
                    int ok_b = new_block("comp.gif");
                    Inst cb;
                    cb.op = IrOp::Branch;
                    cb.a = c;
                    cb.target = ok_b;
                    cb.target2 = cont;
                    emit(std::move(cb));
                    switch_to(ok_b);
                }
                gen(gi + 1);
            });
            return;
        }

        bool ok = sty && (sty->kind == TyKind::List || sty->kind == TyKind::Tuple ||
                          sty->kind == TyKind::Str || sty->kind == TyKind::Dict ||
                          sty->kind == TyKind::Set || sty->kind == TyKind::Range);
        if (!ok) {
            err(g.iter->line, g.iter->col,
                std::format("cannot iterate over '{}' in a comprehension", type_name(sty)));
            return;
        }
        TypePtr ety = t_unknown();
        if (sty->kind == TyKind::Str) ety = t_str();
        else if (!sty->sub.empty()) ety = sty->sub[0];

        // 本层元素装载的位置；循环体降级完、且期间没有用户副作用时，
        // 把它改成免检查。必须在 erase_cleanup 之前打补丁。
        int gen_load_blk = -1;
        size_t gen_load_pos = 0;

        Inst ln;
        ln.op = IrOp::Len;
        ln.a = seq;
        VReg n = def(t_int(), std::move(ln));
        if (sty->kind == TyKind::Range) {
            // range 的长度按边界算，不物化
            n = call_runtime(RtFn::RangeLen, t_int(), {seq});
        }

        int seq_slot = -1;
        int n_slot = -1;
        if (async_mode) {
            seq_slot = new_temp_slot(sty);
            store_slot(seq_slot, seq);
            n_slot = new_temp_slot(t_int());
            store_slot(n_slot, n);
        }
        auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sty) : seq; };
        auto n_val = [&]() { return n_slot >= 0 ? load_slot(n_slot, t_int()) : n; };

        int idx = new_temp_slot(t_int());
        store_slot(idx, const_int(0));

        int cond = new_block("comp.cond");
        int body = new_block("comp.body");
        int incr = new_block("comp.incr");
        int end = new_block("comp.end");

        // 水位取在序列求值之后：seq 本身可能是临时容器，不能被每轮清掉。
        // async 模式下临时值随挂起统一清理，不需要局部水位。
        int mark_block = -1;
        size_t mark_pos = 0;
        VReg mark = async_mode ? kNoReg : rc_mark_at(mark_block, mark_pos);
        auto clean = [&]() {
            if (async_mode) cleanup_gen_mark();
            else rc_cleanup(mark);
        };
        size_t pushes_before = st_->rc_pushes;

        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        VReg iv = load_slot(idx, t_int());
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = iv;
        cmp.b = n_val();
        VReg cc = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = body;
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        clean();
        VReg iv2 = load_slot(idx, t_int());
        VReg ev;
        if (sty->kind == TyKind::Range) {
            // range 的元素按边界算，不物化
            ev = call_runtime(RtFn::RangeGet, ety, {seq_val(), iv2});
        } else if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
            ev = call_runtime(RtFn::DictKeyAt, ety, {seq_val(), iv2});
        } else {
            if (!async_mode && opts_.opt_level >= 1 &&
                (sty->kind == TyKind::List || sty->kind == TyKind::Tuple)) {
                gen_load_blk = st_->blk;
                gen_load_pos = st_->fn->blocks[gen_load_blk].insts.size();
            }
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = seq_val();
            get.b = iv2;
            ev = sty->kind == TyKind::Str ? def_heap(ety, std::move(get))
                                          : def(ety, std::move(get));
        }
        if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
            int slot = comp_slot(tn->id);
            if (slot < 0) {
                slot = st_->fn->new_slot(ety);
                comp_slots_.back()[tn->id] = slot;
            }
            store_slot(slot, ev);
        } else {
            assign_to(*g.target, ev, ety);
        }

        int se_gen_start = side_effect_seq_;

        // 过滤器：任何一条不成立就跳到自增块。
        for (auto& cond_expr : g.ifs) {
            VReg c = expr(*cond_expr);
            c = truth_bool(c, tc_.type_of(cond_expr.get()));
            int ok_b = new_block("comp.if");
            Inst cb;
            cb.op = IrOp::Branch;
            cb.a = c;
            cb.target = ok_b;
            cb.target2 = incr;
            emit(std::move(cb));
            switch_to(ok_b);
        }

        gen(gi + 1);
        if (opts_.opt_level >= 1 && gen_load_blk >= 0 &&
            side_effect_seq_ == se_gen_start) {
            auto& insts = st_->fn->blocks[gen_load_blk].insts;
            if (gen_load_pos < insts.size())
                insts[gen_load_pos].op = IrOp::IndexLoadUnchecked;
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

        switch_to(end);
        clean();
        if (!async_mode && !st_->is_generator && st_->rc_pushes == pushes_before) {
            erase_cleanup(body, mark);
            erase_cleanup(incr, mark);
            erase_cleanup(end, mark);
            erase_mark(mark_block, mark_pos, mark);
        }
    };

    gen(0);
    comp_slots_.pop_back();
    return async_mode ? acc_val() : acc;
}

}  // namespace pyadamas
