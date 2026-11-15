// lower_exc.cpp —— 语句降级：try/except/finally、raise、import
#include "lower_impl.h"

namespace pyadamas {

// except `RichExc as e` 里的 RichExc 是不是富异常类（有字段/方法）。
const ClassInfo* Lowerer::rich_exc_handler(ExceptHandler* h) {
    if (!h || h->name.empty() || !h->type) return nullptr;
    if (h->type->kind != ExprKind::Name) return nullptr;
    auto& n = static_cast<Name&>(*h->type);
    auto it = classes_.find(n.id);
    if (it == classes_.end() || !it->second) return nullptr;
    if (it->second->is_exception_class && it->second->is_rich_exception)
        return it->second.get();
    return nullptr;
}



VReg Lowerer::lower_exc_match(Expr& type) {
    auto one = [&](Expr& e) -> VReg {
        if (e.kind == ExprKind::Name) {
            auto& n = static_cast<Name&>(e);
            bool builtin = TypeChecker::is_exception_type(n.id) && is_builtin(n.id);
            bool custom = false;
            if (auto it = classes_.find(n.id); it != classes_.end())
                custom = it->second->is_exception_class;
            if (builtin || custom)
                return call_runtime(RtFn::ExcMatches, t_bool(), {const_str(n.id)});
        }
        err(e.line, e.col, "except requires an exception name");
        return const_bool(false);
    };
    if (type.kind == ExprKind::Tuple) {
        auto& t = static_cast<TupleExpr&>(type);
        VReg acc = const_bool(false);
        for (auto& el : t.elts) {
            if (!el) continue;
            VReg m = one(*el);
            Inst in;
            in.op = IrOp::Bin;
            in.bop = BinKind::BitOr;
            in.a = acc;
            in.b = m;
            acc = def(t_int(), std::move(in));
        }
        return acc;
    }
    return one(type);
}

bool Lowerer::exc_class_name(Expr& e, std::string& out) {
    if (e.kind != ExprKind::Name) return false;
    auto& n = static_cast<Name&>(e);
    if (TypeChecker::is_exception_type(n.id) && is_builtin(n.id) && !is_local(n.id)) {
        out = n.id;
        return true;
    }
    // 自定义异常类：名字的静态类型是 Class 且标记为异常类。被同名
    // 变量遮住时类型对不上，不会误判。
    TypePtr t = tc_.type_of(&e);
    if (t && t->kind == TyKind::ClassObj && !t->sub.empty()) t = t->sub[0];
    if (t && t->kind == TyKind::Class && t->cls && t->cls->is_exception_class) {
        out = t->cls->name;
        return true;
    }
    return false;
}

void Lowerer::lower_raise(Raise& r) {
    if (!r.exc) {
        if (r.cause) {
            err(r.line, r.col, "'raise from' requires an exception");
            return;
        }
        call_void(RtFn::Reraise, {});
        return;
    }
    VReg cause = kNoReg;
    if (r.cause) {
        TypePtr ct = tc_.type_of(r.cause.get());
        if (ct && ct->kind == TyKind::None) {
            cause = const_int(0);
        } else {
            cause = expr(*r.cause);
            if (!ct || (ct->kind != TyKind::Exception && ct->kind != TyKind::Unknown))
                err(r.cause->line, r.cause->col,
                    "raise from requires an exception or None");
        }
    }
    auto is_exc_name = [&](const std::string& n) {
        if (TypeChecker::is_exception_type(n) && is_builtin(n)) return true;
        auto it = classes_.find(n);
        return it != classes_.end() && it->second->is_exception_class;
    };
    TypePtr xt = tc_.type_of(r.exc.get());

    // 富异常（自定义异常类有字段/方法）：构造真实例后抛实例。

    if (xt && xt->kind == TyKind::Class && xt->cls && xt->cls->is_rich_exception) {

        VReg inst = expr(*r.exc);

        call_void(RtFn::RaiseInstance, {inst, const_str(xt->cls->name)});

        return;

    }

    if (xt && xt->kind == TyKind::Exception) {
        VReg v = expr(*r.exc);
        call_void(RtFn::RaiseValueFrom, {v, cause == kNoReg ? const_int(0) : cause});
        return;
    }
    std::string type;
    VReg msg = kNoReg;
    Expr& e = *r.exc;
    if (e.kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(e);
        if (!is_exc_name(n.id)) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
        type = n.id;
    } else if (e.kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(e);
        if (c.func->kind != ExprKind::Name) {
            err(e.line, e.col, "raise requires a built-in exception name");
            return;
        }
        auto& n = static_cast<Name&>(*c.func);
        if (!is_exc_name(n.id)) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
        type = n.id;
        for (auto& kw : c.keywords) expr(*kw.value);
        // 多位置参：逐个字符串化收集成 list[str]，交给运行期建 args 和消息。
        // 单参数保持旧语义（消息=str，e.args == [msg]），避免行为回归。
        if (c.args.size() > 1) {
            TypePtr lt = mk_list(t_str());
            VReg arglist = call_runtime(
                RtFn::ListNew, lt,
                {const_int(static_cast<int64_t>(c.args.size())),
                 const_int(list_kind_filled(lt))});
            push_temp(arglist);
            for (auto& arg : c.args) {
                TypePtr mt = tc_.type_of(arg.get());
                VReg raw = expr(*arg);
                VReg sm = convert_to(raw, mt, t_str());
                retain_elem(sm, 1);
                call_void(RtFn::ListAppend, {arglist, sm});
            }
            call_void(RtFn::RaiseArgs, {const_str(type), arglist});
            return;
        }
        if (!c.args.empty()) {
            TypePtr mt = tc_.type_of(c.args[0].get());
            if (mt && (mt->kind == TyKind::List || mt->kind == TyKind::Tuple ||
                       mt->kind == TyKind::Dict || mt->kind == TyKind::Set ||
                       mt->kind == TyKind::Class || mt->kind == TyKind::Func)) {
                err(c.args[0]->line, c.args[0]->col,
                    "exception message must be a string or scalar");
                return;
            }
            VReg raw = expr(*c.args[0]);
            msg = convert_to(raw, mt, t_str());
        }
    } else {
        err(e.line, e.col, "raise requires a built-in exception name");
        return;
    }
    if (msg == kNoReg) msg = const_str("");
    if (cause != kNoReg) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ExcValueNew);
        in.args = {const_str(type), const_int(0)};
        VReg v = def_heap(mk_exception(type), std::move(in));
        call_void(RtFn::RaiseValueFrom, {v, cause});
        return;
    }
    call_void(RtFn::Raise, {const_str(type), msg});
}


void Lowerer::lower_try(Try& t) {
    if (!t.finalbody.empty()) {
        lower_finally(t);
        return;
    }
    lower_try_core(t);
}

void Lowerer::lower_try_core(Try& t) {
    if (t.handlers.empty()) {
        stmts(t.body);
        stmts(t.orelse);
        return;
    }

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("try.body");
    int land = new_block("try.land");
    int end = new_block("try.end");
    int else_b = t.orelse.empty() ? end : new_block("try.else");
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));

    switch_to(body);
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Handler;
    cf.handler_slot = hs;
    cf.land = land;
    st_->cleanups.push_back(cf);
    stmts(t.body);
    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        Inst j;
        j.op = IrOp::Jump;
        j.target = else_b;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    struct HBody {
        ExceptHandler* h;
        int block;
    };
    std::vector<HBody> bodies;
    bool bare = false;
    for (auto& h : t.handlers) {
        int hb = new_block("except.body");
        bodies.push_back({&h, hb});
        if (bare) continue;
        if (h.type) {
            VReg m = lower_exc_match(*h.type);
            int next = new_block("except.next");
            Inst cb;
            cb.op = IrOp::Branch;
            cb.a = m;
            cb.target = hb;
            cb.target2 = next;
            emit(std::move(cb));
            switch_to(next);
        } else {
            bare = true;
            Inst j;
            j.op = IrOp::Jump;
            j.target = hb;
            emit(std::move(j));
        }
    }
    if (!bare) {
        call_void(RtFn::ExcReraise, {});
        end_unreachable();
    }

    for (auto& hb : bodies) {
        switch_to(hb.block);
        // 富异常（有字段/方法）的 except as e：取回真实例，类型是那个类；
        // 否则照旧取扁平异常值。
        const ClassInfo* rich = rich_exc_handler(hb.h);
        TypePtr slot_ty = rich
                              ? static_cast<TypePtr>(mk_class(const_cast<ClassInfo*>(rich)))
                              : static_cast<TypePtr>(mk_exception("Exception"));
        VReg ev = call_runtime(rich ? RtFn::ExcAcceptInstance : RtFn::ExcAcceptValue,
                               slot_ty, {});
        int shadow = -1;
        if (!hb.h->name.empty()) {
            shadow = st_->fn->new_slot(slot_ty);
            comp_slots_.emplace_back();
            comp_slots_.back()[hb.h->name] = shadow;
            store_slot(shadow, ev);
        }
        // 槽已经 retain 过；释放创建时的那一个引用。不能留在临时栈上：
        // 协程里 except 体的 handler 水位在这之后，语句清理会越过它。
        rc_dec(ev);

        // except 体再抛异常时，也要先 ExcDone 再继续展开，避免
        // current 异常上下文和 save 节点累积。
        int hh = push_handler();
        VReg sj2 = setjmp_handler(hh);
        int run = new_block("except.run");
        int hland = new_block("except.land");
        int hend = new_block("except.end");
        Inst b2;
        b2.op = IrOp::Branch;
        b2.a = sj2;
        b2.target = hland;
        b2.target2 = run;
        emit(std::move(b2));
        switch_to(run);

        FuncCtx::Cleanup hc;
        hc.kind = FuncCtx::Cleanup::Kind::Handler;
        hc.handler_slot = hh;
        // 挂起后重挂 handler 时，异常要跳回 except 体自己的展开块
        //（先 ExcDone 再继续向外抛），不能留 -1。
        hc.land = hland;
        st_->cleanups.push_back(hc);
        FuncCtx::Cleanup done;
        done.kind = FuncCtx::Cleanup::Kind::ExcDone;
        st_->cleanups.push_back(done);
        stmts(hb.h->body);
        st_->cleanups.pop_back();
        st_->cleanups.pop_back();
        if (shadow >= 0) comp_slots_.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hh, t_int())});
            call_void(RtFn::ExcDone, {});
            Inst j;
            j.op = IrOp::Jump;
            j.target = hend;
            emit(std::move(j));
        }

        switch_to(hland);
        call_void(RtFn::HandlerLeave, {load_slot(hh, t_int())});
        call_void(RtFn::ExcDone, {});
        call_void(RtFn::ExcReraise, {});
        end_unreachable();
        switch_to(hend);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }

    if (else_b != end) {
        switch_to(else_b);
        stmts(t.orelse);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }
    switch_to(end);
}

void Lowerer::lower_finally(Try& t) {
    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    if (!st_->is_generator && !st_->is_coroutine) {
        // 非可挂起函数：体内不可能有挂起点，finalbody 内联两份。
        int body = new_block("finally.body");
        int land = new_block("finally.land");
        int end = new_block("finally.end");
        Inst br;
        br.op = IrOp::Branch;
        br.a = sj;
        br.target = land;
        br.target2 = body;
        emit(std::move(br));

        switch_to(body);
        FuncCtx::Cleanup cf;
        cf.kind = FuncCtx::Cleanup::Kind::Finally;
        cf.handler_slot = hs;
        cf.land = land;
        cf.body = &t.finalbody;
        if (!st_->loops.empty()) {
            cf.loop_brk = st_->loops.back().first;
            cf.loop_cont = st_->loops.back().second;
            cf.loop_depth = st_->loop_cleanup_depth.back();
        }
        st_->cleanups.push_back(cf);
        lower_try_core(t);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            stmts(t.finalbody);
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }

        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        // 待重抛异常先转成异常值存槽：finally 里的 return/break/continue
        // 终止控制流时异常被吞掉，全局 pending_exc 也不会残留。
        TypePtr exc_ty = mk_exception("Exception");
        int pend_slot = new_temp_slot(exc_ty);
        VReg pv = call_runtime(RtFn::ExcTakeValue, exc_ty, {});
        push_temp(pv);
        store_slot(pend_slot, pv);
        stmts(t.finalbody);
        if (!terminated()) {
            call_void(RtFn::ExcReraiseValueIf, {load_slot(pend_slot, exc_ty)});
            end_unreachable();
        }
        switch_to(end);
        return;
    }
    // 可挂起函数里 finally 体只编译一份：正常路径和异常 landing 都跳到 fin，
    // 体内挂起点因此只有一套状态机。异常路径把待重抛异常转成堆上的异常值存进
    // 帧槽，避免挂起期间全局 pending_exc 被调用方的 raise/clear 冲掉。
    TypePtr exc_ty = mk_exception("Exception");
    int pend_slot = new_temp_slot(exc_ty);
    int body = new_block("finally.body");
    int land = new_block("finally.land");
    int fin = new_block("finally.fin");
    int end = new_block("finally.end");
    auto jump = [&](int target) {
        Inst j;
        j.op = IrOp::Jump;
        j.target = target;
        emit(std::move(j));
    };
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));

    switch_to(body);
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Finally;
    cf.handler_slot = hs;
    cf.land = land;
    cf.body = &t.finalbody;
    if (!st_->loops.empty()) {
        cf.loop_brk = st_->loops.back().first;
        cf.loop_cont = st_->loops.back().second;
        cf.loop_depth = st_->loop_cleanup_depth.back();
    }
    st_->cleanups.push_back(cf);
    lower_try_core(t);
    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        store_slot(pend_slot, const_int(0));
        jump(fin);
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    VReg pv = call_runtime(RtFn::ExcTakeValue, exc_ty, {});
    push_temp(pv);
    store_slot(pend_slot, pv);
    jump(fin);

    switch_to(fin);
    stmts(t.finalbody);
    if (!terminated()) {
        call_void(RtFn::ExcReraiseValueIf, {load_slot(pend_slot, exc_ty)});
        jump(end);
    }
    switch_to(end);
}

void Lowerer::lower_assert(Assert& a) {
    VReg c = expr(*a.test);
    c = truth_bool(c, tc_.type_of(a.test.get()));
    int ok = new_block("assert.ok");
    int bad = new_block("assert.fail");
    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = ok;
    br.target2 = bad;
    emit(std::move(br));

    switch_to(bad);
    VReg msg;
    if (a.msg) {
        TypePtr mt = tc_.type_of(a.msg.get());
        VReg raw = expr(*a.msg);
        msg = convert_to(raw, mt, t_str());
    } else {
        msg = const_str("assertion failed");
    }
    call_void(RtFn::Raise, {const_str("AssertionError"), msg});
    Inst jj;
    jj.op = IrOp::Jump;
    jj.target = ok;
    emit(std::move(jj));

    switch_to(ok);
}

void Lowerer::delete_target(Expr& target) {
    switch (target.kind) {
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(target);
            if (int cs = comp_slot(n.id); cs >= 0) {
                TypePtr T = st_->fn->slot_ty[cs];
                VReg old = load_slot(cs, T);
                store_slot_raw(cs, const_int(0));
                if (ir_is_heap(T)) rc_dec(old);
                return;
            }
            if (is_cell(n.id) || is_env(n.id)) {
                TypePtr T = captured_ty(n.id);
                write_cell(n.id, zero_value(T), T);
                return;
            }
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                int gi = global_idx(n.id);
                TypePtr T = mod_.globals[gi].ty;
                VReg old = load_global(gi, T);
                store_global_raw(gi, const_int(0));
                if (ir_is_heap(T)) rc_dec(old);
                return;
            }
            int slot = st_->slots[n.id];
            TypePtr T = st_->fn->slot_ty[slot];
            VReg old = load_slot(slot, T);
            store_slot_raw(slot, const_int(0));
            if (ir_is_heap(T)) rc_dec(old);
            return;
        }
        case ExprKind::Tuple:
        case ExprKind::List: {
            auto& elts = target.kind == ExprKind::Tuple
                             ? static_cast<TupleExpr&>(target).elts
                             : static_cast<ListExpr&>(target).elts;
            for (auto& e : elts)
                if (e) delete_target(*e);
            return;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<Subscript&>(target);
            VReg obj = expr(*s.value);
            TypePtr ot = tc_.type_of(s.value.get());
            if (s.slice->kind == ExprKind::Slice) {
                if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Unknown)) {
                    err(target.line, target.col,
                        std::format("'{}' does not support slice deletion",
                                    type_name(ot)));
                    return;
                }
                auto& sl = static_cast<SliceExpr&>(*s.slice);
                constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
                auto bound = [&](ExprPtr& e) -> VReg {
                    if (!e) return const_int(kNone);
                    VReg b = expr(*e);
                    return coerce(b, tc_.type_of(e.get()), t_int());
                };
                VReg start = bound(sl.lower);
                VReg stop = bound(sl.upper);
                VReg step = bound(sl.step);
                Inst mk;
                mk.op = IrOp::MakeList;
                mk.imm = 2;
                VReg empty = def_heap(mk_list(t_unknown()), std::move(mk));
                call_void(RtFn::ListSliceAssign, {obj, start, stop, step, empty});
                return;
            }
            VReg idx = expr(*s.slice);
            if (ot && ot->kind == TyKind::Dict) {
                TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
                idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
                call_void(RtFn::DictRemove, {obj, idx, const_int(0)});
                return;
            }
            if (ot && ot->kind == TyKind::List) {
                idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
                call_void(RtFn::ListDelIndex, {obj, idx});
                return;
            }
            err(target.line, target.col,
                std::format("'{}' does not support item deletion", type_name(ot)));
            return;
        }
        case ExprKind::Attribute: {
            auto& at = static_cast<Attribute&>(target);
            TypePtr raw = tc_.type_of(at.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                if (const PropertyInfo* p = ot->cls->property(at.attr)) {
                    if (!p->owner || p->deleter < 0 ||
                        p->deleter >= static_cast<int>(p->owner->methods.size())) {
                        err(target.line, target.col,
                            std::format("property '{}' has no deleter", at.attr));
                        return;
                    }
                    const MethodInfo& dm = p->owner->methods[p->deleter];
                    VReg obj = expr(*at.value);
                    if (opt) obj = guard_not_none(obj, std::format("property '{}'", at.attr));
                    Inst vc;
                    vc.op = IrOp::VCall;
                    vc.a = obj;
                    vc.imm = dm.slot;
                    emit(std::move(vc));
                    return;
                }
                auto* f = ot->cls->field(at.attr);
                if (!f) {
                    err(target.line, target.col,
                        std::format("'{}' object has no field '{}'", ot->cls->name, at.attr));
                    return;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                int off = static_cast<int>(8 * (1 + ot->cls->field_index(at.attr)));
                store_field(f->ty, obj, off, const_none());
                return;
            }
            err(target.line, target.col, "unsupported del target");
            return;
        }
        default:
            err(target.line, target.col, "unsupported del target");
            return;
    }
}

void Lowerer::lower_delete(Delete& d) {
    // del 可能改变列表长度（del xs[i] / del xs[a:b]），保守地算一次
    // 副作用，让外层 for/推导式关闭下标免检查。
    ++side_effect_seq_;
    for (auto& t : d.targets)
        if (t) delete_target(*t);
}



}  // namespace pyadamas
