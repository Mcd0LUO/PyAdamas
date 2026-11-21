// lower_stmt.cpp —— 语句降级：控制流与 for 的各种形态
#include "lower_impl.h"
#include "lower_idiom.h"

namespace pyadamas {


// for 的解构目标能否拆成 n 个独立的 Name：`for i, v in ...` 可以，
// `for i, (a, b) in ...` 或带下标的就不行（退回物化路径）。
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


// ────────────────────────── 语句 ──────────────────────────

void Lowerer::stmt(Stmt& s) {
    cur_line_ = s.line;
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
            mark_comp(es.value.get());
            expr(*es.value);
            comp_ctx_node_ = nullptr;
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<Assign&>(s);
            mark_comp(a.value.get());
            VReg v = expr(*a.value);
            comp_ctx_node_ = nullptr;
            TypePtr vt = tc_.type_of(a.value.get());
            for (auto& t : a.targets) assign_to(*t, v, vt);
            break;
        }
        case StmtKind::AugAssign:
            lower_aug_assign(static_cast<AugAssign&>(s)); break;
        case StmtKind::AnnAssign: {
            auto& a = static_cast<AnnAssign&>(s);
            mark_ann_unknown_slot(a);  // P3-1b：object/Any 标注槽禁升级
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
            // `q: asyncio.Queue[float] = asyncio.Queue()`：构造调用本身只
            // 推出 Queue[unknown]，把标注的目标类型交给 lower_asyncio。
            // `s: set[str] = set()` / `d: dict[str, int] = dict()`：空构造
            // 调用自己推不出元素类型，把标注挂给 lower_builtin_container。
            // （容器的字面量形态已由下面的 typed_literal 处理。）
            if (want && a.value->kind == ExprKind::Call &&
                (want->kind == TyKind::Set || want->kind == TyKind::Dict ||
                 want->kind == TyKind::List)) {
                auto& call0 = static_cast<Call&>(*a.value);
                bool bare = call0.func->kind == ExprKind::Name;
                if (bare) {
                    const std::string& cn = static_cast<Name&>(*call0.func).id;
                    bool same = (want->kind == TyKind::Set &&
                                 (cn == "set" || cn == "frozenset")) ||
                                (want->kind == TyKind::Dict && cn == "dict") ||
                                (want->kind == TyKind::List && cn == "list");
                    if (same && call0.args.empty()) pending_ctor_ty_ = want;
                }
            }
            if (want && want->kind == TyKind::AsyncQueue &&
                a.value->kind == ExprKind::Call) {
                auto& call = static_cast<Call&>(*a.value);
                bool is_queue = false;
                if (call.func->kind == ExprKind::Name) {
                    auto it = native_aliases_.find(static_cast<Name&>(*call.func).id);
                    is_queue = it != native_aliases_.end() &&
                               it->second.module == "asyncio" &&
                               it->second.member == "Queue";
                } else if (call.func->kind == ExprKind::Attribute) {
                    auto& at = static_cast<Attribute&>(*call.func);
                    is_queue = at.value->kind == ExprKind::Name &&
                               static_cast<Name&>(*at.value).id == "asyncio" &&
                               at.attr == "Queue";
                }
                if (is_queue) pending_queue_ty_ = want;
            }
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
            pending_queue_ty_ = nullptr;
            pending_ctor_ty_ = nullptr;
            comp_ctx_node_ = nullptr;
            assign_to(*a.target, v, want && want->kind != TyKind::Big ? want : vt);
            break;
        }
        case StmtKind::Return: {
            auto& r = static_cast<Return&>(s);
            if (st_->is_generator) {
                mark_comp(r.value.get());
                VReg v = r.value ? expr(*r.value) : kNoReg;
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
                // 生成器的 return 值由外层 `yield from` 取走（CPython 语义）；
                // 异步生成器没有消费者，返回值丢弃。
                emit_cleanup_exits(0);
                if (!st_->is_async_generator && v != kNoReg)
                    gen_finish(v, vreg_ty(v));
                else
                    gen_finish();
                break;
            }
            VReg v = r.value ? expr(*r.value) : kNoReg;
            // `def f() -> float: return 1` 要在返回处转成 double
            if (v != kNoReg && st_->fn->ret) v = coerce(v, vreg_ty(v), st_->fn->ret);
            // **先接管返回值，再跑清理。**
            //
            // 清理（emit_cleanup_exits / rc_cleanup）会把临时栈上方的值全部
            // 释放 —— 而 return 值本身常常就是其中之一：
            //     def f(s: str) -> str:
            //         try: return s + "!"
            //         except ValueError: return "x"
            // `s + "!"` 是新堆值且在临时栈上，先清理再 take，取到的是已经
            // 释放的对象：caller 一 dec 就 panic "refcount decrement on a
            // freed object"（try 里 return 任何拼接/调用结果都会踩）。
            // 借用值同理：清理可能释放它的属主，先 inc 才是安全的。
            TypePtr rt = v != kNoReg ? vreg_ty(v) : nullptr;
            v = own_return_value(v, rt);
            emit_cleanup_exits(0);
            emit_return(v, rt, v != kNoReg);
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
        case StmtKind::StructDef: break;  // 纯编译期声明，不生成代码
        case StmtKind::Import: lower_import(static_cast<Import&>(s)); break;
        case StmtKind::CImport:
        // cimport 只在编译期生效，不产生运行时对象。
        break;
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

    // 非负提示窗口（lower_idiom.cpp）：入口擦目标槽旧提示，退出擦本层键。
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
void Lowerer::lower_for_enumerate(For& f, Call& c) {
    auto& tup = static_cast<TupleExpr&>(*f.target);
    VReg seq = expr(*c.args[0]);
    TypePtr sty = tc_.type_of(c.args[0].get());
    TypePtr ety = t_unknown();
    if (sty->kind == TyKind::Str) ety = t_str();
    else if (!sty->sub.empty()) ety = sty->sub[0];

    // 生成器里 vreg 不跨 yield：序列落槽。
    int seq_slot = -1;
    if (st_->is_generator) {
        seq_slot = new_temp_slot(sty);
        store_slot(seq_slot, seq);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sty) : seq; };

    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, def(t_int(), std::move(ln)));

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int str_cur = sty->kind == TyKind::Str ? new_str_cursor() : -1;

    int cond = new_block("for.enum.cond");
    int body = new_block("for.enum.body");
    int incr = new_block("for.enum.incr");
    int end = new_block("for.enum.end");
    int orelse = f.orelse.empty() ? end : new_block("for.enum.else");

    int mark_block = -1;
    size_t mark_pos = 0;
    VReg mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(mark);
    };
    size_t pushes_before = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = load_slot(idx, t_int());
    cmp.b = load_slot(n_slot, t_int());
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv = load_slot(idx, t_int());
    assign_to(*tup.elts[0], iv, t_int());
    VReg ev;
    if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
        ev = call_runtime(RtFn::DictKeyAt, ety, {seq_val(), iv});
    } else if (sty->kind == TyKind::Str) {
        ev = str_char_at(seq_val(), iv, str_cur);
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = seq_val();
        get.b = iv;
        ev = def(ety, std::move(get));
    }
    assign_to(*tup.elts[1], ev, ety);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jo;
            jo.op = IrOp::Jump;
            jo.target = end;
            emit(std::move(jo));
        }
    }

    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before) {
        erase_cleanup(body, mark);
        erase_cleanup(incr, mark);
        erase_cleanup(end, mark);
        erase_mark(mark_block, mark_pos, mark);
    }
}

// for a, b in zip(xs, ys)：双游标，不建元组表。长度取两者较小值。
void Lowerer::lower_for_zip(For& f, Call& c) {
    auto& tup = static_cast<TupleExpr&>(*f.target);
    VReg a = expr(*c.args[0]);
    VReg b = expr(*c.args[1]);
    TypePtr at = tc_.type_of(c.args[0].get());
    TypePtr bt = tc_.type_of(c.args[1].get());
    TypePtr ea = t_unknown();
    TypePtr eb = t_unknown();
    if (at->kind == TyKind::Str) ea = t_str();
    else if (!at->sub.empty()) ea = at->sub[0];
    if (bt->kind == TyKind::Str) eb = t_str();
    else if (!bt->sub.empty()) eb = bt->sub[0];

    int as = -1;
    int bs = -1;
    if (st_->is_generator) {
        as = new_temp_slot(at);
        store_slot(as, a);
        bs = new_temp_slot(bt);
        store_slot(bs, b);
    }
    auto aval = [&]() { return as >= 0 ? load_slot(as, at) : a; };
    auto bval = [&]() { return bs >= 0 ? load_slot(bs, bt) : b; };

    // n = min(len(a), len(b))
    Inst la;
    la.op = IrOp::Len;
    la.a = a;
    VReg na = def(t_int(), std::move(la));
    Inst lb;
    lb.op = IrOp::Len;
    lb.a = b;
    VReg nb = def(t_int(), std::move(lb));
    int n_slot = new_temp_slot(t_int());
    int pick_a = new_block("for.zip.a");
    int pick_b = new_block("for.zip.b");
    int after = new_block("for.zip.min");
    Inst cm;
    cm.op = IrOp::Cmp;
    cm.cop = CmpKind::Lt;
    cm.a = na;
    cm.b = nb;
    VReg lt = def(t_bool(), std::move(cm));
    Inst br0;
    br0.op = IrOp::Branch;
    br0.a = lt;
    br0.target = pick_a;
    br0.target2 = pick_b;
    emit(std::move(br0));
    switch_to(pick_a);
    store_slot(n_slot, na);
    Inst ja;
    ja.op = IrOp::Jump;
    ja.target = after;
    emit(std::move(ja));
    switch_to(pick_b);
    store_slot(n_slot, nb);
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = after;
    emit(std::move(jb));
    switch_to(after);

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int a_cur = at->kind == TyKind::Str ? new_str_cursor() : -1;
    int b_cur = bt->kind == TyKind::Str ? new_str_cursor() : -1;

    int cond = new_block("for.zip.cond");
    int body = new_block("for.zip.body");
    int incr = new_block("for.zip.incr");
    int end = new_block("for.zip.end");
    int orelse = f.orelse.empty() ? end : new_block("for.zip.else");

    int mark_block = -1;
    size_t mark_pos = 0;
    VReg mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(mark);
    };
    size_t pushes_before = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = load_slot(idx, t_int());
    cmp.b = load_slot(n_slot, t_int());
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv = load_slot(idx, t_int());
    VReg va;
    if (at->kind == TyKind::Dict || at->kind == TyKind::Set) {
        va = call_runtime(RtFn::DictKeyAt, ea, {aval(), iv});
    } else if (at->kind == TyKind::Str) {
        va = str_char_at(aval(), iv, a_cur);
    } else {
        Inst g1;
        g1.op = IrOp::IndexLoad;
        g1.a = aval();
        g1.b = iv;
        va = def(ea, std::move(g1));
    }
    assign_to(*tup.elts[0], va, ea);
    VReg vb;
    if (bt->kind == TyKind::Dict || bt->kind == TyKind::Set) {
        vb = call_runtime(RtFn::DictKeyAt, eb, {bval(), iv});
    } else if (bt->kind == TyKind::Str) {
        vb = str_char_at(bval(), iv, b_cur);
    } else {
        Inst g2;
        g2.op = IrOp::IndexLoad;
        g2.a = bval();
        g2.b = iv;
        vb = def(eb, std::move(g2));
    }
    assign_to(*tup.elts[1], vb, eb);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jo;
            jo.op = IrOp::Jump;
            jo.target = end;
            emit(std::move(jo));
        }
    }

    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before) {
        erase_cleanup(body, mark);
        erase_cleanup(incr, mark);
        erase_cleanup(end, mark);
        erase_mark(mark_block, mark_pos, mark);
    }
}

void Lowerer::lower_for_seq(For& f) {
    // for i, v in enumerate(xs) / for a, b in zip(xs, ys)：直接计数循环，
    // 不物化 [下标, 值] 的元组表——每个元素省一次堆分配。
    if (f.iter->kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(*f.iter);
        if (c.func->kind == ExprKind::Name && c.keywords.empty()) {
            const std::string& fn = static_cast<Name&>(*c.func).id;
            if (fn == "enumerate" && is_builtin("enumerate") && c.args.size() == 1 &&
                can_destructure(f.target.get(), 2) &&
                is_countable(tc_.type_of(c.args[0].get()))) {
                lower_for_enumerate(f, c);
                return;
            }
            if (fn == "zip" && is_builtin("zip") && c.args.size() == 2 &&
                can_destructure(f.target.get(), 2) &&
                is_countable(tc_.type_of(c.args[0].get())) &&
                is_countable(tc_.type_of(c.args[1].get()))) {
                lower_for_zip(f, c);
                return;
            }
        }
    }
    VReg seq = expr(*f.iter);
    TypePtr seq_ty = tc_.type_of(f.iter.get());
    // deque 没有按下标直取的迭代路径：先转成列表再走普通 list 迭代。
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


}  // namespace pyadamas
