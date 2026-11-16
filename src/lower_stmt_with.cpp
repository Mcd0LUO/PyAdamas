// lower_stmt_with.cpp —— with/异常清理与文件迭代
#include "lower_impl.h"

namespace pyadamas {

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
    TypePtr rt = c.ret ? c.ret : t_none();
    if (c.exit_cls) {
        // 装饰后的 __exit__/__aexit__ 存在类属性里：按名字取包装函数，
        // 显式补上管理器实例。装饰器可以改返回类型，这里不按原签名收窄。
        const char* nm =
            c.kind == FuncCtx::Cleanup::Kind::AsyncWith ? "__aexit__" : "__exit__";
        VReg r = dunder_call(mgr, mk_class(c.exit_cls), nm, std::move(args), nullptr);
        if (r != kNoReg) return r;
        return const_bool(false);
    }
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = mgr;
    vc.imm = c.exit_slot;
    vc.args = std::move(args);
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
            case FuncCtx::Cleanup::Kind::Finally: {
                const int hslot = c.handler_slot;
                auto* fbody = c.body;
                call_void(RtFn::HandlerDiscard, {load_slot(hslot, t_int())});
                if (fbody) {
                    // 内联的 finally 体里可能有挂起点。handler 已经 discard，
                    // 本条目和更内层已发射的条目都不能进挂起快照，否则恢复时
                    // 会去 suspend 已释放的 handler 记录。
                    auto saved = st_->cleanups;
                    st_->cleanups.resize(i - 1);
                    stmts(*fbody);
                    st_->cleanups = std::move(saved);
                }
                break;
            }
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
            case FuncCtx::Cleanup::Kind::ZeroCostEh: {
                // 零开销 try 的早退（return/break/continue 穿过保护区）：
                // 1) 先退出表驱动展开区（pa_eh_leave，否则 pa_eh_depth 不归零，
                //    之后抛出的异常会误走 _Unwind）；
                // 2) 按水位清理临时值；
                // 3) 有 finally 时内联跑一遍 finalbody —— 早退也必须执行它
                //    （与 CPython 一致：finally 在 return 之前跑）。
                //    注意顺序：先 EhLeave/清理，再跑 finalbody，最后才 return。
                call_void(RtFn::EhLeave, {});
                if (c.mark_slot >= 0)
                    call_void(RtFn::RcCleanup, {load_slot(c.mark_slot, t_int())});
                if (c.body) {
                    // finalbody 里可能有 return/break/continue 跳出：
                    // 与老路径 Finally 分支同样处理 —— 临时清空 cleanups，
                    // 避免重入。
                    auto saved = st_->cleanups;
                    st_->cleanups.resize(i - 1);
                    stmts(*c.body);
                    st_->cleanups = std::move(saved);
                }
                break;
            }
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
    TypePtr ert = en->call_sig->ret ? en->call_sig->ret : t_none();
    VReg ev = kNoReg;
    if (en->decorated) {
        // 装饰后的 __enter__：从类属性取包装函数，显式传管理器实例。
        // 绑定值的类型以包装函数声明的返回值为准。
        ev = dunder_call(load_slot(ms, ct), ct, "__enter__", std::move(eargs), nullptr);
        if (const ClassAttrInfo* ca = ct->cls->class_attr("__enter__");
            ca && ca->ty && ca->ty->kind == TyKind::Func && ca->ty->fn && ca->ty->fn->ret)
            ert = ca->ty->fn->ret;
    } else {
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = load_slot(ms, ct);
        vc.imm = en->slot;
        vc.args = std::move(eargs);
        ev = def_heap(ert, std::move(vc));
    }
    if (ev == kNoReg) {
        err(w.line, w.col,
            std::format("decorated __enter__ of '{}' has an incompatible signature",
                        ct->cls->name));
        ev = const_none();
    }
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
    if (ex->decorated) cf.exit_cls = ct->cls;
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


}  // namespace pyadamas
