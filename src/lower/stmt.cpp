// lower/stmt.cpp —— 语句降级：控制流与 for 的各种形态
#include "lower/impl.h"
#include "lower/idiom.h"

namespace pyadamas {


// for 的解构目标能否拆成 n 个独立的 Name：`for i, v in ...` 可以，
// `for i, (a, b) in ...` 或带下标的就不行（退回物化路径）。


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


}  // namespace pyadamas
