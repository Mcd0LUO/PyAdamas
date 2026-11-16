// lower_gen_async.cpp —— async for 与可挂起函数编译
#include "lower_impl.h"

namespace pyadamas {

void Lowerer::lower_async_for(For& f, VReg seq, TypePtr seq_ty) {
    // 自定义 __aiter__：先调用拿到异步迭代器，再按它的类型驱动。
    if (seq_ty && seq_ty->kind == TyKind::Class && seq_ty->cls) {
        const MethodInfo* ai = seq_ty->cls->method("__aiter__");
        if (!ai) {
            err(f.iter->line, f.iter->col,
                std::format("async for requires an async generator or a class with "
                            "__aiter__, got '{}'",
                            type_name(seq_ty)));
            return;
        }
        TypePtr rt = t_unknown();
        if (ai->call_sig && ai->call_sig->ret) rt = ai->call_sig->ret;
        if (ai->decorated && ai->decorated_ty && ai->decorated_ty->kind == TyKind::Func &&
            ai->decorated_ty->fn && ai->decorated_ty->fn->ret)
            rt = ai->decorated_ty->fn->ret;
        VReg iter = kNoReg;
        if (ai->decorated) {
            iter = dunder_call(seq, seq_ty, "__aiter__", {}, nullptr);
        } else if (ai->slot >= 0) {
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = seq;
            vc.imm = ai->slot;
            iter = def_heap(rt, std::move(vc));
        }
        if (iter == kNoReg) {
            err(f.iter->line, f.iter->col,
                std::format("__aiter__ on '{}' must be an instance method",
                            seq_ty->cls->name));
            return;
        }
        seq = iter;
        seq_ty = rt;
    }
    TypePtr et = t_unknown();
    const MethodInfo* ne = nullptr;
    bool use_anext = false;
    if (seq_ty && seq_ty->kind == TyKind::AsyncGenerator) {
        et = seq_ty->sub.empty() ? t_unknown() : seq_ty->sub[0];
    } else if (seq_ty && seq_ty->kind == TyKind::Class && seq_ty->cls &&
               (ne = seq_ty->cls->method("__anext__")) != nullptr) {
        // __aiter__ 返回自身（或另一个异步迭代器）：每轮 await __anext__()。
        TypePtr nt = ne->call_sig ? ne->call_sig->ret : nullptr;
        if (ne->decorated && ne->decorated_ty && ne->decorated_ty->kind == TyKind::Func &&
            ne->decorated_ty->fn)
            nt = ne->decorated_ty->fn->ret;
        if (nt && nt->kind == TyKind::Coroutine)
            et = nt->sub.empty() ? t_unknown() : nt->sub[0];
        use_anext = true;
    } else if (seq_ty && seq_ty->kind != TyKind::Unknown) {
        err(f.iter->line, f.iter->col,
            std::format("async for requires an async generator or a class with "
                        "__aiter__, got '{}'",
                        type_name(seq_ty)));
        return;
    }
    // 挂起会丢 vreg：序列落帧槽。
    int seq_slot = -1;
    TypePtr sst = seq_ty ? seq_ty : t_unknown();
    if (st_->is_generator) {
        seq_slot = new_temp_slot(sst);
        store_slot(seq_slot, seq);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sst) : seq; };

    int hs_slot = new_temp_slot(t_int());
    int next_try = new_block("afor.next");
    int fetch = new_block("afor.fetch");
    int land = new_block("afor.land");
    int reraise_b = new_block("afor.reraise");
    int body = new_block("afor.body");
    int end = new_block("afor.end");
    int orelse = f.orelse.empty() ? end : new_block("afor.else");

    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = next_try;
        emit(std::move(j));
    }

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

    // 这一层 handler 保护「__anext__ + await」，异常（含 StopAsyncIteration）
    // 由 landing 分派；挂起时它和普通 try 一样会被摘下/重挂。
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Handler;
    cf.handler_slot = hs_slot;
    cf.land = land;
    st_->cleanups.push_back(cf);

    switch_to(fetch);
    VReg w = kNoReg;
    if (use_anext) {
        if (ne->decorated) {
            w = dunder_call(seq_val(), seq_ty, "__anext__", {}, nullptr);
        } else if (ne->slot >= 0) {
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = seq_val();
            vc.imm = ne->slot;
            w = def_heap(mk_coroutine(et), std::move(vc));
        }
        if (w == kNoReg) {
            err(f.iter->line, f.iter->col,
                std::format("__anext__ on '{}' must be an async instance method",
                            seq_ty->cls->name));
            return;
        }
    } else {
        Inst call;
        call.op = IrOp::CallRt;
        call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
        call.args = {seq_val()};
        w = def_heap(mk_coroutine(et), std::move(call));
    }
    VReg val = suspend_on_awaitable(w, et, f.line, f.col,
                                    static_cast<size_t>(-1), false);
    st_->cleanups.pop_back();
    call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
    // handler 已离开，现在才能把到手的堆值挂到临时栈（语句末释放）。
    if (ir_is_heap(et)) push_temp(val);
    assign_to(*f.target, val, et);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = body;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
    VReg m = call_runtime(RtFn::ExcMatches, t_bool(), {const_str("StopAsyncIteration")});
    int stop_ok = new_block("afor.stop");
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
        Inst j;
        j.op = IrOp::Jump;
        j.target = orelse;
        emit(std::move(j));
    }

    switch_to(body);
    st_->loops.push_back({end, next_try});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst j;
        j.op = IrOp::Jump;
        j.target = next_try;
        emit(std::move(j));
    }

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }
    switch_to(end);
}

// 生成器/协程 = resume 状态机 + 创建包装。
//   resume: 槽位在堆帧里，入口按 state 分发到续跑块；
//   包装:   普通函数，按原签名收参，分配帧、把实参 retain 进帧槽、
//           绑定闭包环境，返回生成器/协程对象。函数值指向包装。
int Lowerer::compile_suspendable_function(FunctionDef& f, FuncInfoPtr info,
                                          const std::string& label, int frame_kind,
                                          const ClassInfo* owner) {
    bool is_coro = frame_kind != 0;   // 协程和异步生成器都能 await
    bool is_agen = frame_kind == 2;
    IRFunction* irf = mod_.add_func(label + "$resume");
    int ridx = static_cast<int>(mod_.funcs.size()) - 1;
    IRFunction* cw = mod_.add_func(label);
    int cidx = static_cast<int>(mod_.funcs.size()) - 1;
    if (owner) fn_owner_class_[irf] = owner;

    irf->ret = t_none();
    irf->is_generator = true;
    TypePtr gen_ty = (info && info->ret)
                         ? info->ret
                         : (is_agen ? mk_async_generator(t_unknown())
                                    : (is_coro ? mk_coroutine(t_unknown())
                                               : mk_generator(t_unknown())));
    cw->ret = gen_ty;

    const Scope* scope = tc_.scope_of(&f);
    push_frame(irf, scope);
    if (auto it = closure_meta_.find(&f); it != closure_meta_.end()) st_->closure = &it->second;
    st_->is_generator = true;
    st_->is_coroutine = is_coro;
    st_->is_async_generator = is_agen;
    st_->async_result = (is_coro && !is_agen && info) ? info->async_result : nullptr;
    const ClosureMeta* meta = st_->closure;

    int entry = new_block("g.entry");
    int start = new_block("g.start");
    switch_to(start);

    // 形参：原值放普通槽；被内层捕获的参数另外有一个 cell 槽，
    // create_cells 在第一次启动时建 cell。
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = irf->new_slot(T);
        irf->params.push_back(IRParam{p.name, T, s});
        if (meta && meta->cell_index.count(p.name)) {
            int cs = irf->new_slot(mk_list(T));
            st_->cell_slots[p.name] = cs;
            st_->param_slots[p.name] = s;
            st_->slots[p.name] = cs;
        } else {
            st_->slots[p.name] = s;
        }
    };
    if (info) {
        for (auto& p : info->pos) add_param(p);
        for (auto& p : info->kwonly) add_param(p);
        if (info->varargs) {
            TypePtr et = info->vararg_ty ? info->vararg_ty : t_unknown();
            Param p;
            p.name = info->vararg_name;
            p.ty = mk_list(et);
            add_param(p);
        }
        if (info->kwargs) {
            TypePtr vt = info->kwarg_ty ? info->kwarg_ty : t_unknown();
            Param p;
            p.name = info->kwarg_name;
            p.ty = mk_dict(t_str(), vt);
            add_param(p);
        }
    }
    if (scope) {
        for (auto& [name, sym] : scope->vars) {
            if (st_->slots.count(name)) continue;
            if (st_->globals_decl.count(name)) continue;
            TypePtr T = sym.ty ? sym.ty : t_unknown();
            if (meta && meta->cell_index.count(name)) {
                int cs = irf->new_slot(mk_list(T));
                st_->cell_slots[name] = cs;
                st_->slots[name] = cs;
            } else {
                st_->slots[name] = irf->new_slot(T);
            }
        }
    }

    // 闭包环境是帧里的一个普通堆槽：创建时 retain 闭包，帧销毁时 dec。
    TypePtr env_ty = new_ty(TyKind::Func);
    int env_slot = -1;
    if (meta && !meta->env.empty()) {
        env_slot = irf->new_slot(env_ty);
        st_->env_slot = env_slot;
    }
    irf->env_slot = -1;  // %r10 只在创建包装里用

    if (auto pit = func_parent_.find(&f); pit != func_parent_.end() && pit->second) {
        st_->self_def = &f;
        st_->self_idx = cidx;
        st_->self_type = new_ty(TyKind::Func);
        st_->self_type->fn = info;
    }

    // 临时值水位每次 resume 重新记录（vreg 不跨 yield）。
    int mark_slot = irf->new_slot(t_int());
    st_->gen_mark_slot = mark_slot;
    // 异常上下文三件套：值是 malloc 指针 / 整数，不参与 RC。
    st_->exc_cur_slot = irf->new_slot(t_int());
    st_->exc_top_slot = irf->new_slot(t_int());
    st_->exc_tok_slot = irf->new_slot(t_int());
    st_->fn_mark = kNoReg;

    // 创建包装已经 retain 过实参，这里不再 inc_params。
    create_cells();
    stmts(f.body);
    // 挂起点位置已经全部确定：把跨挂起仍活跃的 vreg 落进帧槽。
    spill_across_suspends();
    if (!terminated()) gen_finish();

    // ── close 路径：按挂起点快照跑 finally / __exit__，再释放帧槽 ──
    auto has_runtime_handler = [](const FuncCtx::Cleanup& c) {
        return c.kind == FuncCtx::Cleanup::Kind::Handler ||
               c.kind == FuncCtx::Cleanup::Kind::With ||
               c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
               c.kind == FuncCtx::Cleanup::Kind::Finally ||
               c.kind == FuncCtx::Cleanup::Kind::LockRelease ||
               c.kind == FuncCtx::Cleanup::Kind::TimeoutExit ||
               c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel ||
               c.kind == FuncCtx::Cleanup::Kind::SemRelease;
    };
    std::vector<std::pair<int, int>> close_targets;
    for (auto& gs : st_->gen_states) {
        // 只要挂起点有 handler 记录，就要一条 close 块：即使没有 with/finally
        // 动作，也必须把摘下来的记录 free 掉。
        bool has_action = false;
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c)) has_action = true;
        if (!has_action) continue;
        int cb = new_block("g.close");
        switch_to(cb);
        for (size_t i = gs.cleanups.size(); i > 0; --i) {
            const auto& c = gs.cleanups[i - 1];
            if (c.kind == FuncCtx::Cleanup::Kind::With) {
                emit_exit_call(c, false);
            } else if (c.kind == FuncCtx::Cleanup::Kind::LockRelease) {
                call_void(RtFn::LockRelease, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::TimeoutExit) {
                call_void(RtFn::TimeoutExit, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel) {
                call_void(RtFn::TaskGroupCancel, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::SemRelease) {
                call_void(RtFn::SemRelease, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::Finally && c.body) {
                // 重放发生在函数体降级完之后，loops 已空；恢复 finally
                // 词法上所属的循环，break/continue 才能找到目标块。
                bool pushed_loop = false;
                if (st_->loops.empty() && c.loop_brk >= 0) {
                    st_->loops.push_back({c.loop_brk, c.loop_cont});
                    st_->loop_cleanup_depth.push_back(c.loop_depth);
                    pushed_loop = true;
                }
                ++close_replay_depth_;
                stmts(*c.body);
                --close_replay_depth_;
                if (pushed_loop) {
                    st_->loops.pop_back();
                    st_->loop_cleanup_depth.pop_back();
                }
            }
        }
        // 挂起时摘下来的 handler 记录不会有人再弹栈，close 路径负责释放。
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c))
                call_void(RtFn::HandlerFree, {load_slot(c.handler_slot, t_int())});
        cleanup_gen_mark();
        release_slots();
        exc_leave();
        Inst d;
        d.op = IrOp::GenDone;
        emit(std::move(d));
        close_targets.push_back({gs.id, cb});
    }
    int close_default = new_block("g.close.default");
    switch_to(close_default);
    cleanup_gen_mark();
    release_slots();
    exc_leave();
    {
        Inst d;
        d.op = IrOp::GenDone;
        emit(std::move(d));
    }

    // ── 入口分发 ────────────────────────────────────────────────
    // 每次 resume 先取临时栈水位；close 走清理分发，否则按 state 恢复。
    // try/with/finally 的 handler 在 yield 时被摘掉，恢复时必须按原顺序
    // 重新压栈并重新 setjmp（jmp_buf 属于已经返回的旧 resume 帧）。
    switch_to(entry);
    VReg m0 = call_runtime(RtFn::RcMark, t_int(), {});
    store_slot(mark_slot, m0);
    // 每次 resume 都换成本帧保存的异常上下文；挂起/退出时换回去。
    exc_enter();
    Inst gsit;
    gsit.op = IrOp::GenGetState;
    VReg state = def(t_int(), std::move(gsit));
    Inst gcl;
    gcl.op = IrOp::GenClosing;
    VReg closing = def(t_int(), std::move(gcl));
    int close_disp = new_block("g.close.dispatch");
    int normal_disp = new_block("g.dispatch");
    // 协程/异步生成器的失败注入和生成器的 throw() 共用这条分发。
    int throw_disp = new_block("g.throw.dispatch");
    int after_close = new_block("g.not.closing");
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = closing;
        br.target = close_disp;
        br.target2 = after_close;
        emit(std::move(br));
    }
    switch_to(after_close);
    {
        Inst ge;
        ge.op = IrOp::GenGetExc;
        VReg exc = def(t_int(), std::move(ge));
        Inst br;
        br.op = IrOp::Branch;
        br.a = exc;
        br.target = throw_disp;
        br.target2 = normal_disp;
        emit(std::move(br));
    }

    int cur = normal_disp;
    for (auto& gs : st_->gen_states) {
        switch_to(cur);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        cmp.a = state;
        cmp.b = const_int(gs.id);
        VReg cc = def(t_bool(), std::move(cmp));
        std::vector<const FuncCtx::Cleanup*> hs;
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c)) hs.push_back(&c);
        int target = gs.cont;
        if (!hs.empty()) target = new_block("g.arm");
        int nextb = new_block("g.disp");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = target;
        br.target2 = nextb;
        emit(std::move(br));
        if (!hs.empty()) {
            switch_to(target);
            for (const auto* c : hs) {
                call_void(RtFn::HandlerRearm, {load_slot(c->handler_slot, t_int())});
                VReg sj = setjmp_handler(c->handler_slot);
                int nb = new_block("g.arm.next");
                Inst ab;
                ab.op = IrOp::Branch;
                ab.a = sj;
                ab.target = c->land;
                ab.target2 = nb;
                emit(std::move(ab));
                switch_to(nb);
            }
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = gs.cont;
            emit(std::move(jj));
        }
        cur = nextb;
    }
    switch_to(cur);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = start;
        emit(std::move(j));
    }

    // ── 异常注入分发 ────────────────────────────────────────────
    // 被 await 的对象失败（协程）或调用方 throw()（生成器）时，把异常放回
    // 挂起点。恢复路径先重挂 handler 再抛：try/except 才能抓到跨挂起的异常。
    // 处于 yield from 委托中的挂起点不在这里抛，而是把异常交给子生成器。
    {
        int tcur = throw_disp;
        for (auto& gs : st_->gen_states) {
            switch_to(tcur);
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = state;
            cmp.b = const_int(gs.id);
            VReg cc = def(t_bool(), std::move(cmp));
            std::vector<const FuncCtx::Cleanup*> hs;
            for (auto& c : gs.cleanups)
                if (has_runtime_handler(c)) hs.push_back(&c);
            int arm = new_block("g.throw.arm");
            int nextb = new_block("g.throw.disp");
            Inst br;
            br.op = IrOp::Branch;
            br.a = cc;
            br.target = arm;
            br.target2 = nextb;
            emit(std::move(br));
            switch_to(arm);
            // handler 的 setjmp 环境必须在本帧重新捕获：注入的异常可能是
            // 由另一个调用点（next/send/throw）触发的，直接 longjmp 到上次
            // resume 留下的环境会跨越不同的栈帧深度。
            for (const auto* c : hs) {
                call_void(RtFn::HandlerRearm, {load_slot(c->handler_slot, t_int())});
                VReg sj = setjmp_handler(c->handler_slot);
                int nb = new_block("g.throw.arm.next");
                Inst ab;
                ab.op = IrOp::Branch;
                ab.a = sj;
                ab.target = c->land;
                ab.target2 = nb;
                emit(std::move(ab));
                switch_to(nb);
            }
            if (gs.delegate_cond >= 0) {
                // 委托给子生成器：取走帧里的异常（所有权转移），交回委托
                // 循环的 cond，由它改走 GenThrowOpt。
                Inst te;
                te.op = IrOp::GenTakeExc;
                VReg excp = def(t_int(), std::move(te));
                store_slot(gs.delegate_exc_slot, excp);
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = gs.delegate_cond;
                emit(std::move(jj));
            } else {
                Inst rt;
                rt.op = IrOp::GenRethrow;
                emit(std::move(rt));
            }
            tcur = nextb;
        }
        switch_to(tcur);
        // 未启动的生成器没有挂起点：入口即抛出。
        Inst rt;
        rt.op = IrOp::GenRethrow;
        emit(std::move(rt));
    }

    switch_to(close_disp);
    int ccur = st_->blk;
    for (auto& [id, blk] : close_targets) {
        switch_to(ccur);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        cmp.a = state;
        cmp.b = const_int(id);
        VReg cc = def(t_bool(), std::move(cmp));
        int nextb = new_block("g.close.disp");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = blk;
        br.target2 = nextb;
        emit(std::move(br));
        ccur = nextb;
    }
    switch_to(ccur);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = close_default;
        emit(std::move(j));
    }

    IRGenerator gin;
    gin.resume = ridx;
    gin.n_slots = static_cast<int>(irf->slot_ty.size());
    gin.frame_kind = frame_kind;
    gin.slot_heap.assign(static_cast<size_t>(gin.n_slots), 0);
    for (int s = 0; s < gin.n_slots; ++s) {
        if (st_->borrow_slots.count(s)) continue;
        if (!ir_is_heap(irf->slot_ty[static_cast<size_t>(s)])) continue;
        // 2 = 可能装着非堆值：帧销毁/异常逃出释放时走注册表。
        gin.slot_heap[static_cast<size_t>(s)] =
            (static_cast<size_t>(s) < irf->slot_unknown.size() &&
             irf->slot_unknown[static_cast<size_t>(s)])
                ? 2
                : 1;
    }
    int gidx = static_cast<int>(mod_.generators.size());
    mod_.generators.push_back(std::move(gin));

    pop_frame();

    // 创建包装：参数槽与 resume 完全一致，帧里的槽位按同一编号填。
    cw->params = irf->params;
    cw->slot_ty = irf->slot_ty;
    cw->env_slot = env_slot;
    push_frame(cw, nullptr);
    int b0 = new_block("entry");
    switch_to(b0);
    Inst gn;
    gn.op = IrOp::GenNew;
    gn.imm = gidx;
    VReg gen = def(gen_ty, std::move(gn));
    for (auto& p : cw->params) {
        VReg v = load_slot(p.slot, p.ty);
        if (ir_is_heap(p.ty)) rc_inc(v);
        Inst si;
        si.op = IrOp::GenSetSlot;
        si.idx = Operand::slot(p.slot);
        si.args = {gen, v};
        emit(std::move(si));
    }
    if (env_slot >= 0) {
        VReg ev = load_slot(env_slot, env_ty);
        rc_inc(ev);
        Inst si;
        si.op = IrOp::GenSetSlot;
        si.idx = Operand::slot(env_slot);
        si.args = {gen, ev};
        emit(std::move(si));
    }
    Inst r;
    r.op = IrOp::Return;
    r.a = gen;
    emit(std::move(r));
    pop_frame();
    return cidx;
}


// ── 跨挂起的 vreg 溢出 ─────────────────────────────────────────
//
// resume 每次被调用都重建 C 栈帧，vreg 落在栈上的那份值在 yield/await
// 返回后就没了。续跑块的 live-in 恰好就是「挂起期间必须继续活着」的
// 值：挂起前 retain 一份写进堆帧槽，恢复后再装回 vreg 并交给临时栈。
// 这样 yield/await 能出现在任意子表达式里，复合表达式不用逐个改。

}  // namespace pyadamas
