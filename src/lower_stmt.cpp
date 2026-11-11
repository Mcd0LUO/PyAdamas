// lower_stmt.cpp —— 语句降级：控制流、with/try、import
#include "lower_impl.h"

namespace py23 {


// ────────────────────────── 语句 ──────────────────────────

void Lowerer::stmt(Stmt& s) {
    // 每条语句一个临时值栈水位：表达式产生的堆值在语句结束时统一释放。
    // 赋值/传参/返回已经各 retain 了一份，所以清理不会影响仍然存活的值。
    // 语句完全没产生临时堆值时，把 mark/cleanup 一起删掉（纯整数代码
    // 不该为 RC 付出任何运行时开销）。
    int mark_block = -1;
    size_t mark_pos = 0;
    VReg stmt_mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    size_t pushes_before = st_->rc_pushes;
    auto mark_comp = [&](Expr* v) {
        if (v && (v->kind == ExprKind::ListComp || v->kind == ExprKind::SetComp ||
                  v->kind == ExprKind::DictComp || v->kind == ExprKind::GeneratorExp))
            comp_ctx_node_ = v;
    };
    switch (s.kind) {
        case StmtKind::Expr: {
            auto& es = static_cast<ExprStmt&>(s);
            if (es.value->kind == ExprKind::Yield || es.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = es.value.get();
            if (es.value->kind == ExprKind::Await) await_ctx_node_ = es.value.get();
            mark_comp(es.value.get());
            expr(*es.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<Assign&>(s);
            if (a.value->kind == ExprKind::Yield || a.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = a.value.get();
            if (a.value->kind == ExprKind::Await) await_ctx_node_ = a.value.get();
            mark_comp(a.value.get());
            VReg v = expr(*a.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            TypePtr vt = tc_.type_of(a.value.get());
            for (auto& t : a.targets) assign_to(*t, v, vt);
            break;
        }
        case StmtKind::AugAssign: {
            auto& a = static_cast<AugAssign&>(s);
            VReg cur_v = expr(*a.target);
            TypePtr cur_t = tc_.type_of(a.target.get());
            VReg rhs = expr(*a.value);
            TypePtr rhs_t = tc_.type_of(a.value.get());
            TypePtr ty = tc_.type_of(a.target.get());
            if (ty && ty->kind == TyKind::Float) {
                if (!ir_is_float(cur_t)) cur_v = convert_to(cur_v, cur_t, t_float());
                if (!ir_is_float(rhs_t)) rhs = convert_to(rhs, rhs_t, t_float());
            }
            // 类实例的增量赋值：__iadd__ 优先，其次普通 __add__，
            // 右侧是类时还有反射版 __radd__。
            bool cls_involved = (cur_t && cur_t->kind == TyKind::Class) ||
                                (rhs_t && rhs_t->kind == TyKind::Class);
            if (cls_involved) {
                VReg dr = lower_dunder_binop(cur_v, cur_t, rhs, rhs_t, a.op, true);
                if (dr != kNoReg) {
                    assign_to(*a.target, dr, vreg_ty(dr));
                    break;
                }
            }
            Inst in;
            in.op = IrOp::Bin;
            in.bop = map_bin(aug_to_bin(a.op));
            in.a = cur_v;
            in.b = rhs;
            VReg r = def_heap(ty, std::move(in));
            // 下标目标会重新求值一次下标；Python 只求一次。
            // 纯下标表达式没有副作用，先这样，等有优化遍再消。
            assign_to(*a.target, r, ty);
            break;
        }
        case StmtKind::AnnAssign: {
            auto& a = static_cast<AnnAssign&>(s);
            if (!a.value) break;  // 纯声明：槽本来就是 0（None）
            TypePtr vt = tc_.type_of(a.value.get());
            // `x: list[float] = [1, 2]`：字面量必须按标注的元素类型构造。
            // 照字面量自己的 list[int] 构造，槽会被当成 list[float] 读，
            // 每个元素都是整数位模式冒充 double。
            TypePtr want;
            if (a.target->kind == ExprKind::Name)
                want = storage_ty(static_cast<Name&>(*a.target).id);
            VReg v;
            bool typed_literal = want && vt && want->kind == vt->kind &&
                                 (want->kind == TyKind::List || want->kind == TyKind::Tuple ||
                                  want->kind == TyKind::Dict || want->kind == TyKind::Set);
            if (a.value->kind == ExprKind::Yield || a.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = a.value.get();
            if (a.value->kind == ExprKind::Await) await_ctx_node_ = a.value.get();
            mark_comp(a.value.get());
            if (typed_literal && a.value->kind == ExprKind::List)
                v = lower_list_like(static_cast<ListExpr&>(*a.value).elts, want, false);
            else if (typed_literal && a.value->kind == ExprKind::Tuple)
                v = lower_list_like(static_cast<TupleExpr&>(*a.value).elts, want, true);
            // `d: dict[str, int] = {}`：空字面量自己推不出 key 类型，
            // 必须按标注构造，否则运行时 key_kind 是「按机器字比较」，
            // 动态 str key 查不回来。set 同理（元素可能要补转换）。
            else if (typed_literal && a.value->kind == ExprKind::Dict)
                v = lower_dict(static_cast<DictExpr&>(*a.value), want);
            else if (typed_literal && a.value->kind == ExprKind::Set)
                v = lower_set(static_cast<SetExpr&>(*a.value), want);
            else
                v = expr(*a.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            assign_to(*a.target, v, want ? want : vt);
            break;
        }
        case StmtKind::Return: {
            auto& r = static_cast<Return&>(s);
            if (st_->is_generator) {
                if (r.value && r.value->kind == ExprKind::Await)
                    await_ctx_node_ = r.value.get();
                mark_comp(r.value.get());
                VReg v = r.value ? expr(*r.value) : kNoReg;
                await_ctx_node_ = nullptr;
                comp_ctx_node_ = nullptr;
                if (st_->is_coroutine && !st_->is_async_generator) {
                    // 协程的返回值要送到 await 处（或 asyncio.run）。
                    if (v != kNoReg && st_->async_result)
                        v = coerce(v, vreg_ty(v), st_->async_result);
                    // 清理里可能有 await（async with 的 __aexit__）：
                    // vreg 不跨挂起，返回值先落帧槽。
                    int ret_slot = -1;
                    TypePtr rty = v != kNoReg ? vreg_ty(v) : nullptr;
                    if (v != kNoReg) {
                        ret_slot = new_temp_slot(rty);
                        store_slot(ret_slot, v);
                    }
                    emit_cleanup_exits(0);
                    if (ret_slot >= 0) v = load_slot(ret_slot, rty);
                    gen_finish(v, rty);
                    break;
                }
                // 生成器/异步生成器的 return 只表示结束，返回值不保留。
                emit_cleanup_exits(0);
                gen_finish();
                break;
            }
            VReg v = r.value ? expr(*r.value) : kNoReg;
            // `def f() -> float: return 1` 要在返回处转成 double
            if (v != kNoReg && st_->fn->ret) v = coerce(v, vreg_ty(v), st_->fn->ret);
            // return 也是退出路径：先跑完所有活跃清理动作。
            emit_cleanup_exits(0);
            emit_return(v, v != kNoReg ? vreg_ty(v) : nullptr);
            break;
        }
        case StmtKind::Break: {
            if (st_->loops.empty()) {
                unsupported(s, "'break'");
                break;
            }
            emit_cleanup_exits(st_->loop_cleanup_depth.back());
            Inst in;
            in.op = IrOp::Jump;
            in.target = st_->loops.back().first;
            emit(std::move(in));
            break;
        }
        case StmtKind::Continue: {
            if (st_->loops.empty()) {
                unsupported(s, "'continue'");
                break;
            }
            emit_cleanup_exits(st_->loop_cleanup_depth.back());
            Inst in;
            in.op = IrOp::Jump;
            in.target = st_->loops.back().second;
            emit(std::move(in));
            break;
        }
        case StmtKind::Pass:
            break;
        case StmtKind::If: lower_if(static_cast<If&>(s)); break;
        case StmtKind::While: lower_while(static_cast<While&>(s)); break;
        case StmtKind::For: lower_for(static_cast<For&>(s)); break;
        case StmtKind::FunctionDef: lower_funcdef(static_cast<FunctionDef&>(s)); break;
        case StmtKind::Assert: lower_assert(static_cast<Assert&>(s)); break;
        case StmtKind::Try: lower_try(static_cast<Try&>(s)); break;
        case StmtKind::Delete: lower_delete(static_cast<Delete&>(s)); break;
        case StmtKind::Match: lower_match(static_cast<Match&>(s)); break;
        case StmtKind::Raise: lower_raise(static_cast<Raise&>(s)); break;
        case StmtKind::With: lower_with(static_cast<With&>(s)); break;
        case StmtKind::ClassDef: lower_classdef(static_cast<ClassDef&>(s)); break;
        case StmtKind::Import: lower_import(static_cast<Import&>(s)); break;
        case StmtKind::ImportFrom: lower_import_from(static_cast<ImportFrom&>(s)); break;
        case StmtKind::Global: {
            for (auto& n : static_cast<Global&>(s).names) st_->globals_decl.insert(n);
            break;
        }
        case StmtKind::Nonlocal: break;  // 名字解析已经由 cell/env 接管
    }
    if (st_->rc_pushes != pushes_before) {
        if (!terminated()) {
            if (st_->is_generator) cleanup_gen_mark();
            else rc_cleanup(stmt_mark);
        }
    } else if (!st_->is_generator) {
        if (!terminated() && !cur().insts.empty()) {
            const Inst& last = cur().insts.back();
            if (last.op == IrOp::CallRt && static_cast<RtFn>(last.imm) == RtFn::RcCleanup &&
                last.args.size() == 1 && last.args[0] == stmt_mark)
                cur().insts.pop_back();
        }
        erase_mark(mark_block, mark_pos, stmt_mark);
    }
}

void Lowerer::lower_if(If& f) {
    int end = new_block("if.end");

    for (size_t i = 0; i < f.branches.size(); ++i) {
        auto& br = f.branches[i];
        VReg c = expr(*br.cond);
        c = truth_bool(c, tc_.type_of(br.cond.get()));
        int then_b = new_block("if.then");
        int next_b;
        if (i + 1 < f.branches.size())
            next_b = new_block("if.next");
        else
            next_b = f.orelse.empty() ? end : new_block("if.else");

        Inst j;
        j.op = IrOp::Branch;
        j.a = c;
        j.target = then_b;
        j.target2 = next_b;
        emit(std::move(j));

        switch_to(then_b);
        stmts(br.body);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
        switch_to(next_b);
    }

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
}

void Lowerer::lower_while(While& f) {
    int cond = new_block("while.cond");
    int body = new_block("while.body");
    int end = new_block("while.end");
    int orelse = f.orelse.empty() ? end : new_block("while.else");

    // 条件表达式每轮都会产生临时值；在 body 入口和退出路径清理到
    // 循环水位，长循环的 RSS 才不会随迭代数增长。条件不产生堆值时
    // 整条脚手架会被删掉。
    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    size_t pushes_before_cond = st_->rc_pushes;
    VReg c = expr(*f.cond);
    c = truth_bool(c, tc_.type_of(f.cond.get()));
    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    st_->loops.push_back({end, cond});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = cond;
        emit(std::move(jj));
    }

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
    // 循环内（条件或循环体）完全没有临时堆值时，把循环水位整条删掉。
    // 注意不能只看条件：`continue`/`break` 会跳过语句级清理，循环体里
    // 的临时值要靠循环水位兜底。
    if (!st_->is_generator && st_->rc_pushes == pushes_before_cond) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
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
        call_void(RtFn::Raise,
                  {const_str("ValueError"), const_str("range() arg 3 must not be zero")});
        Inst jz;
        jz.op = IrOp::Jump;
        jz.target = ok_blk;
        emit(std::move(jz));
        switch_to(ok_blk);
    }

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

void Lowerer::lower_for_seq(For& f) {
    VReg seq = expr(*f.iter);
    TypePtr seq_ty = tc_.type_of(f.iter.get());
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
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = sv;
        get.b = iv2;
        // 遍历字符串时每个下标是 py23_str_char_at 新造的单字节串；
        // 遍历列表/元组时是容器内部的借用引用。
        ev = seq_ty->kind == TyKind::Str ? def_heap(et, std::move(get))
                                         : def(et, std::move(get));
    }
    assign_to(*f.target, ev, et);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    int se_before_body = side_effect_seq_;
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    // 列表/元组按 0..len-1 遍历；循环体没有调用/del/切片赋值时
    // 长度不会变，元素下标可以免检查。生成器帧可能跨 yield 挂起，
    // 直接跳过。
    if (opts_.opt_level >= 1 && load_blk >= 0 && !st_->is_generator &&
        (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
        side_effect_seq_ == se_before_body) {
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

int Lowerer::push_handler() {
    st_->fn->has_handlers = true;
    VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
    int slot = new_temp_slot(t_int());
    store_slot(slot, h);
    return slot;
}

VReg Lowerer::setjmp_handler(int slot) {
    Inst in;
    in.op = IrOp::Setjmp;
    in.a = load_slot(slot, t_int());
    return def(t_int(), std::move(in));
}

VReg Lowerer::emit_exit_call(const FuncCtx::Cleanup& c, bool exceptional) {
    if (c.kind == FuncCtx::Cleanup::Kind::FileClose) {
        call_void(RtFn::FileClose, {load_slot(c.mgr_slot, t_file())});
        return const_bool(false);
    }
    if (c.kind == FuncCtx::Cleanup::Kind::LockRelease) {
        call_void(RtFn::LockRelease, {load_slot(c.mgr_slot, t_unknown())});
        return const_bool(false);
    }
    if (c.kind == FuncCtx::Cleanup::Kind::TimeoutExit) {
        call_void(RtFn::TimeoutExit, {load_slot(c.mgr_slot, t_unknown())});
        return const_bool(false);
    }
    if (c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel) {
        call_void(RtFn::TaskGroupCancel, {load_slot(c.mgr_slot, t_unknown())});
        return const_bool(false);
    }
    if (c.kind == FuncCtx::Cleanup::Kind::SemRelease) {
        call_void(RtFn::SemRelease, {load_slot(c.mgr_slot, t_unknown())});
        return const_bool(false);
    }
    std::vector<VReg> args;
    for (int i = 0; i < c.nargs; ++i) {
        if (exceptional && i == 0) {
            VReg t = call_runtime(RtFn::ExcTypeName, t_str(), {});
            push_temp(t);
            args.push_back(t);
        } else if (exceptional && i == 1) {
            VReg m = call_runtime(RtFn::ExcMessage, t_str(), {});
            push_temp(m);
            args.push_back(m);
        } else {
            args.push_back(const_none());
        }
    }
    VReg mgr = load_slot(c.mgr_slot, t_unknown());
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = mgr;
    vc.imm = c.exit_slot;
    vc.args = std::move(args);
    TypePtr rt = c.ret ? c.ret : t_none();
    return def_heap(rt, std::move(vc));
}

void Lowerer::emit_cleanup_exits(size_t down_to) {
    // 只发射、不改编译期栈：break/continue/return 可能只在某个分支上，
    // 其它路径的清理仍然有效。顺序必须从内到外。
    for (size_t i = st_->cleanups.size(); i > down_to; --i) {
        const auto& c = st_->cleanups[i - 1];
        switch (c.kind) {
            case FuncCtx::Cleanup::Kind::Handler:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                break;
            case FuncCtx::Cleanup::Kind::With:
                // 先弹 handler 再调 __exit__：__exit__ 自己抛出的异常
                // 应该由外层处理，不能被同一个 with 再抓一次。
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                emit_exit_call(c, false);
                break;
            case FuncCtx::Cleanup::Kind::AsyncWith: {
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                VReg coro = emit_exit_call(c, false);
                // return/break/continue 路径也要把 __aexit__ 跑完。
                suspend_on_awaitable(coro, c.inner_ret, 1, 1, i - 1);
                break;
            }
            case FuncCtx::Cleanup::Kind::Finally:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                if (c.body) stmts(*c.body);
                break;
            case FuncCtx::Cleanup::Kind::ExcDone:
                call_void(RtFn::ExcDone, {});
                break;
            case FuncCtx::Cleanup::Kind::FileClose:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                call_void(RtFn::FileClose, {load_slot(c.mgr_slot, t_file())});
                break;
            case FuncCtx::Cleanup::Kind::LockRelease:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                call_void(RtFn::LockRelease, {load_slot(c.mgr_slot, t_unknown())});
                break;
            case FuncCtx::Cleanup::Kind::TimeoutExit:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                call_void(RtFn::TimeoutExit, {load_slot(c.mgr_slot, t_unknown())});
                break;
            case FuncCtx::Cleanup::Kind::TaskGroupCancel:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                call_void(RtFn::TaskGroupCancel, {load_slot(c.mgr_slot, t_unknown())});
                break;
            case FuncCtx::Cleanup::Kind::SemRelease:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                call_void(RtFn::SemRelease, {load_slot(c.mgr_slot, t_unknown())});
                break;
        }
    }
}

void Lowerer::lower_with(With& w) {
    // __enter__/__exit__ 可能改动容器；保守关闭外层循环的免检查。
    ++side_effect_seq_;
    if (w.is_async) {
        lower_async_with_item(w, 0);
        return;
    }
    lower_with_item(w, 0);
}

// for line in f：按行读，EOF 结束。continue 跳到下一行，break 到 end；
// 每轮的行临时值在下一轮开头统一清理。
void Lowerer::lower_for_file(For& f) {
    TypePtr ft = t_file();
    VReg file = expr(*f.iter);
    int ms = new_temp_slot(ft);
    store_slot(ms, file);

    int check = new_block("for.file.check");
    int body = new_block("for.file.body");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() { rc_cleanup(loop_mark); };
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = check;
    emit(std::move(j));

    switch_to(check);
    clean();
    Inst rd;
    rd.op = IrOp::CallRt;
    rd.imm = static_cast<int64_t>(RtFn::FileReadLine);
    rd.args = {load_slot(ms, ft), const_int(-1)};
    rd.ty = t_str();
    VReg line = def(t_str(), std::move(rd));
    push_temp(line);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = line;
    VReg n = def(t_int(), std::move(ln));
    Inst c0;
    c0.op = IrOp::Cmp;
    c0.cop = CmpKind::Eq;
    c0.a = n;
    c0.b = const_int(0);
    VReg empty = def(t_bool(), std::move(c0));
    Inst br;
    br.op = IrOp::Branch;
    br.a = empty;
    br.target = orelse;
    br.target2 = body;
    emit(std::move(br));

    switch_to(body);
    st_->loops.push_back({end, check});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    assign_to(*f.target, line, t_str());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = check;
        emit(std::move(jb));
    }

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst je;
            je.op = IrOp::Jump;
            je.target = end;
            emit(std::move(je));
        }
    }
    switch_to(end);
    clean();
    if (st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(check, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

void Lowerer::lower_with_item(With& w, size_t idx) {
    if (idx >= w.items.size()) {
        stmts(w.body);
        return;
    }
    auto& item = w.items[idx];
    TypePtr raw = tc_.type_of(item.context.get());
    TypePtr ct = strip_optional(raw);
    VReg mgr = expr(*item.context);
    if (ct != raw && raw && raw->kind == TyKind::Optional)
        mgr = guard_not_none(mgr, std::format("with item"));
    if (ct && ct->kind == TyKind::File) {
        // with open(...) as f：f 就是文件对象；退出时（正常/异常/跳出）
        // 都走 FileClose。
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        if (item.optional_vars) assign_to(*item.optional_vars, mgr, ct);

        int hs = push_handler();
        VReg sj = setjmp_handler(hs);
        int body = new_block("with.body");
        int land = new_block("with.land");
        int end = new_block("with.end");
        Inst br;
        br.op = IrOp::Branch;
        br.a = sj;
        br.target = land;
        br.target2 = body;
        emit(std::move(br));
        switch_to(body);

        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::FileClose;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);

        lower_with_item(w, idx + 1);

        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            emit_exit_call(cf, false);
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        st_->cleanups.pop_back();

        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        emit_exit_call(cf, true);
        call_void(RtFn::ExcReraise, {});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (!ct || ct->kind != TyKind::Class || !ct->cls) {
        err(item.context->line, item.context->col,
            "with requires a class instance with __enter__/__exit__");
        lower_with_item(w, idx + 1);
        return;
    }
    auto* en = ct->cls->method("__enter__");
    auto* ex = ct->cls->method("__exit__");
    if (!en || !ex || !en->call_sig || !ex->call_sig) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define __enter__/__exit__", ct->cls->name));
        lower_with_item(w, idx + 1);
        return;
    }
    int ms = new_temp_slot(ct);
    store_slot(ms, mgr);

    Call enter_call;
    enter_call.line = w.line;
    enter_call.col = w.col;
    std::vector<VReg> eargs = bind_args(enter_call, en->call_sig, en->def);
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = load_slot(ms, ct);
    vc.imm = en->slot;
    vc.args = std::move(eargs);
    TypePtr ert = en->call_sig->ret ? en->call_sig->ret : t_none();
    VReg ev = def_heap(ert, std::move(vc));
    if (item.optional_vars) assign_to(*item.optional_vars, ev, ert);

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("with.body");
    int land = new_block("with.land");
    int end = new_block("with.end");
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));
    switch_to(body);

    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::With;
    cf.handler_slot = hs;
    cf.land = land;
    cf.mgr_slot = ms;
    cf.exit_slot = ex->slot;
    cf.nargs = static_cast<int>(ex->call_sig->pos.size());
    cf.ret = ex->call_sig->ret ? ex->call_sig->ret : t_none();
    st_->cleanups.push_back(cf);

    lower_with_item(w, idx + 1);

    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        emit_exit_call(cf, false);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }
    st_->cleanups.pop_back();

    switch_to(land);
    // 异常展开：先弹 handler（顺带释放受保护区临时值），再调 __exit__。
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    VReg keep = emit_exit_call(cf, true);
    int suppress = new_block("with.suppress");
    int rethrow = new_block("with.rethrow");
    Inst cb;
    cb.op = IrOp::Branch;
    cb.a = keep;
    cb.target = suppress;
    cb.target2 = rethrow;
    emit(std::move(cb));
    switch_to(suppress);
    call_void(RtFn::ExcClear, {});
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));
    switch_to(rethrow);
    call_void(RtFn::ExcReraise, {});
    end_unreachable();
    switch_to(end);
}

void Lowerer::lower_async_with_item(With& w, size_t idx) {
    if (idx >= w.items.size()) {
        stmts(w.body);
        return;
    }
    if (!st_->is_coroutine) {
        err(w.line, w.col, "'async with' outside async function");
        stmts(w.body);
        return;
    }
    auto& item = w.items[idx];
    TypePtr raw = tc_.type_of(item.context.get());
    TypePtr ct = strip_optional(raw);
    VReg mgr = expr(*item.context);
    if (ct != raw && raw && raw->kind == TyKind::Optional)
        mgr = guard_not_none(mgr, "async with item");
    if (ct && ct->kind == TyKind::AsyncLock) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::LockAcquire, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());

        int hs = push_handler();
        VReg sj = setjmp_handler(hs);
        int body = new_block("awith.body");
        int land = new_block("awith.land");
        int end = new_block("awith.end");
        {
            Inst br;
            br.op = IrOp::Branch;
            br.a = sj;
            br.target = land;
            br.target2 = body;
            emit(std::move(br));
        }
        switch_to(body);
        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::LockRelease;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::LockRelease, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::LockRelease, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncEvent) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::EventWait, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());
        lower_async_with_item(w, idx + 1);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncSemaphore) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::SemAcquire, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());
        int hs = push_handler();
        VReg sj = setjmp_handler(hs);
        int body = new_block("awith.body");
        int land = new_block("awith.land");
        int end = new_block("awith.end");
        {
            Inst br;
            br.op = IrOp::Branch;
            br.a = sj;
            br.target = land;
            br.target2 = body;
            emit(std::move(br));
        }
        switch_to(body);
        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::SemRelease;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::SemRelease, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::SemRelease, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncTimeout) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        if (st_->is_generator) cleanup_gen_mark();
        call_void(RtFn::TimeoutEnter, {load_slot(ms, ct)});
        int hs = push_handler();
        VReg sj = setjmp_handler(hs);
        int body = new_block("awith.body");
        int land = new_block("awith.land");
        int end = new_block("awith.end");
        {
            Inst br;
            br.op = IrOp::Branch;
            br.a = sj;
            br.target = land;
            br.target2 = body;
            emit(std::move(br));
        }
        switch_to(body);
        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::TimeoutExit;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::TimeoutExit, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::TimeoutExit, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncTaskGroup) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        if (st_->is_generator) cleanup_gen_mark();
        if (item.optional_vars) assign_to(*item.optional_vars, load_slot(ms, ct), ct);
        int hs = push_handler();
        VReg sj = setjmp_handler(hs);
        int body = new_block("awith.body");
        int land = new_block("awith.land");
        int end = new_block("awith.end");
        {
            Inst br;
            br.op = IrOp::Branch;
            br.a = sj;
            br.target = land;
            br.target2 = body;
            emit(std::move(br));
        }
        switch_to(body);
        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::TaskGroupCancel;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            VReg g = call_runtime(RtFn::TaskGroupExit, mk_coroutine(mk_list(t_unknown())),
                                  {load_slot(ms, ct)});
            push_temp(g);
            suspend_on_awaitable(g, mk_list(t_unknown()), w.line, w.col);
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::TaskGroupCancel, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (!ct || ct->kind != TyKind::Class || !ct->cls) {
        err(item.context->line, item.context->col,
            "async with requires a class instance with async __aenter__/__aexit__");
        lower_async_with_item(w, idx + 1);
        return;
    }
    auto* en = ct->cls->method("__aenter__");
    auto* ex = ct->cls->method("__aexit__");
    if (!en || !en->call_sig || !en->call_sig->ret ||
        en->call_sig->ret->kind != TyKind::Coroutine) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define async __aenter__", ct->cls->name));
        lower_async_with_item(w, idx + 1);
        return;
    }
    if (!ex || !ex->call_sig || !ex->call_sig->ret ||
        ex->call_sig->ret->kind != TyKind::Coroutine) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define async __aexit__", ct->cls->name));
        lower_async_with_item(w, idx + 1);
        return;
    }
    int ms = new_temp_slot(ct);
    store_slot(ms, mgr);

    // __aenter__：调用得到协程，await 出绑定值。
    Call enter_call;
    enter_call.line = w.line;
    enter_call.col = w.col;
    std::vector<VReg> eargs = bind_args(enter_call, en->call_sig, en->def);
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = load_slot(ms, ct);
    vc.imm = en->slot;
    vc.args = std::move(eargs);
    VReg enter_coro = def_heap(en->call_sig->ret, std::move(vc));
    TypePtr ert = en->call_sig->ret->sub.empty() ? t_unknown() : en->call_sig->ret->sub[0];
    VReg ev = suspend_on_awaitable(enter_coro, ert, item.context->line, item.context->col);
    if (item.optional_vars) assign_to(*item.optional_vars, ev, ert);

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("awith.body");
    int land = new_block("awith.land");
    int end = new_block("awith.end");
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = sj;
        br.target = land;
        br.target2 = body;
        emit(std::move(br));
    }
    switch_to(body);

    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::AsyncWith;
    cf.handler_slot = hs;
    cf.land = land;
    cf.mgr_slot = ms;
    cf.exit_slot = ex->slot;
    cf.nargs = static_cast<int>(ex->call_sig->pos.size());
    cf.ret = ex->call_sig->ret;
    cf.inner_ret = ex->call_sig->ret->sub.empty() ? t_unknown() : ex->call_sig->ret->sub[0];
    st_->cleanups.push_back(cf);

    lower_async_with_item(w, idx + 1);

    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg coro = emit_exit_call(cf, false);
        suspend_on_awaitable(coro, cf.inner_ret, w.line, w.col);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    // 异常上下文不能带着 pending 穿过 await，先抄到槽里再清空。
    VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
    push_temp(tname);
    VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
    push_temp(tmsg);
    int ts = new_temp_slot(t_str());
    store_slot(ts, tname);
    int msgs = new_temp_slot(t_str());
    store_slot(msgs, tmsg);
    call_void(RtFn::ExcClear, {});
    {
        std::vector<VReg> args;
        for (int i = 0; i < cf.nargs; ++i) {
            if (i == 0) args.push_back(load_slot(ts, t_str()));
            else if (i == 1) args.push_back(load_slot(msgs, t_str()));
            else args.push_back(const_none());
        }
        Inst vc2;
        vc2.op = IrOp::VCall;
        vc2.a = load_slot(ms, ct);
        vc2.imm = ex->slot;
        vc2.args = std::move(args);
        VReg coro = def_heap(cf.ret, std::move(vc2));
        VReg keep = suspend_on_awaitable(coro, cf.inner_ret, w.line, w.col);
        int suppress = new_block("awith.suppress");
        int rethrow = new_block("awith.rethrow");
        Inst cb;
        cb.op = IrOp::Branch;
        cb.a = keep;
        cb.target = suppress;
        cb.target2 = rethrow;
        emit(std::move(cb));
        switch_to(suppress);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
        switch_to(rethrow);
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
    }
    switch_to(end);
}


}  // namespace py23
