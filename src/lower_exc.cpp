// lower_exc.cpp —— 语句降级：try/except/finally、raise、import
#include "lower_impl.h"

namespace py23 {

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
        if (!c.keywords.empty()) {
            err(e.line, e.col, "exception constructor does not take keyword arguments");
            return;
        }
        if (c.args.size() > 1) {
            err(e.line, e.col, "exception constructor takes at most one message argument");
            return;
        }
        type = n.id;
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
        VReg ev = call_runtime(RtFn::ExcAcceptValue, mk_exception("Exception"), {});
        int shadow = -1;
        if (!hb.h->name.empty()) {
            shadow = st_->fn->new_slot(mk_exception("Exception"));
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

/* finally 体里如果含跳出这个 finally 的 return/break/continue，目前
   没有复制控制流的机制，直接拒绝；循环内部的 break/continue 是合法的。 */
static bool outer_control(const std::vector<StmtPtr>& body, int loop_depth) {
    for (auto& s : body) {
        switch (s->kind) {
            case StmtKind::Return: return true;
            case StmtKind::Break:
            case StmtKind::Continue: return loop_depth == 0;
            case StmtKind::If: {
                auto& f = static_cast<If&>(*s);
                for (auto& b : f.branches)
                    if (outer_control(b.body, loop_depth)) return true;
                if (outer_control(f.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(*s);
                if (outer_control(w.body, loop_depth + 1)) return true;
                if (outer_control(w.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(*s);
                if (outer_control(f.body, loop_depth + 1)) return true;
                if (outer_control(f.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::With: {
                auto& w = static_cast<With&>(*s);
                if (outer_control(w.body, loop_depth)) return true;
                break;
            }
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(*s);
                if (outer_control(t.body, loop_depth)) return true;
                for (auto& h : t.handlers)
                    if (outer_control(h.body, loop_depth)) return true;
                if (outer_control(t.orelse, loop_depth)) return true;
                if (outer_control(t.finalbody, loop_depth)) return true;
                break;
            }
            default: break;
        }
    }
    return false;
}

void Lowerer::lower_finally(Try& t) {
    if (outer_control(t.finalbody, 0)) {
        err(t.line, t.col,
            "return/break/continue inside finally is not supported by the code generator yet");
        return;
    }
    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
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
    st_->cleanups.push_back(cf);
    lower_try_core(t);
    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        ++finally_body_depth_;
        stmts(t.finalbody);
        --finally_body_depth_;
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    ++finally_body_depth_;
    stmts(t.finalbody);
    --finally_body_depth_;
    call_void(RtFn::ExcReraise, {});
    end_unreachable();
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



}  // namespace py23
