// lower_exc.cpp —— 语句降级：try/except/finally、raise、import
#include "lower_impl.h"

namespace pyadamas {

namespace {

// 深度优先遍历一个函数体里的语句树，pre 在进入子语句之前对每个语句调用。
// 不进入嵌套 FunctionDef 的函数体（那是另一个函数）；但进入 ClassDef 的类体
// ——类体语句是在当前函数帧里降级的，它的 try/with/return 都算在本函数头上。
void walk_stmt_tree(std::vector<StmtPtr>& body, const std::function<void(Stmt&)>& pre) {
    for (auto& sp : body) {
        if (!sp) continue;
        Stmt& s = *sp;
        pre(s);
        switch (s.kind) {
            case StmtKind::FunctionDef:
                break;
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(s);
                walk_stmt_tree(t.body, pre);
                for (auto& h : t.handlers) walk_stmt_tree(h.body, pre);
                walk_stmt_tree(t.orelse, pre);
                walk_stmt_tree(t.finalbody, pre);
                break;
            }
            case StmtKind::With:
                walk_stmt_tree(static_cast<With&>(s).body, pre);
                break;
            case StmtKind::If: {
                auto& i = static_cast<If&>(s);
                for (auto& b : i.branches) walk_stmt_tree(b.body, pre);
                walk_stmt_tree(i.orelse, pre);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(s);
                walk_stmt_tree(w.body, pre);
                walk_stmt_tree(w.orelse, pre);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(s);
                walk_stmt_tree(f.body, pre);
                walk_stmt_tree(f.orelse, pre);
                break;
            }
            case StmtKind::Match: {
                auto& m = static_cast<Match&>(s);
                for (auto& c : m.cases) walk_stmt_tree(c.body, pre);
                break;
            }
            case StmtKind::ClassDef:
                walk_stmt_tree(static_cast<ClassDef&>(s).body, pre);
                break;
            default:
                break;
        }
    }
}

}  // namespace

// except `RichExc as e` 里的 RichExc 是不是富异常类（有字段/方法）。
// 本模块类和跨模块导入的类都认：导入类靠接口里的 is_rich_exception 标记。
const ClassInfo* Lowerer::rich_exc_handler(ExceptHandler* h) {
    if (!h || h->name.empty() || !h->type) return nullptr;
    if (h->type->kind != ExprKind::Name) return nullptr;
    auto& n = static_cast<Name&>(*h->type);
    if (auto it = classes_.find(n.id); it != classes_.end() && it->second) {
        if (it->second->is_exception_class && it->second->is_rich_exception)
            return it->second.get();
        return nullptr;
    }
    // 跨模块导入的富异常类：except as e 绑定 mirror 实例。
    if (auto iit = imported_classes_.find(n.id); iit != imported_classes_.end()) {
        const ClassInfo* cls = iit->second.cls.get();
        if (cls && cls->is_exception_class && cls->is_rich_exception) return cls;
    }
    return nullptr;
}

// except / raise 名字位置能接受什么：内置异常名，或本模块/导入的自定义
// 异常类。out 是运行期注册表里的类型名 —— 导入类必须用定义模块里的原名
//（from lib import LibError as LE 的别名不能用于匹配/注册）。
bool Lowerer::exc_type_name(Expr& e, std::string& out) {
    if (e.kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(e);
        if (TypeChecker::is_exception_type(n.id) && is_builtin(n.id)) {
            out = n.id;
            return true;
        }
        if (auto it = classes_.find(n.id); it != classes_.end() && it->second) {
            if (!it->second->is_exception_class) return false;
            out = it->second->name;
            return true;
        }
        if (auto iit = imported_classes_.find(n.id); iit != imported_classes_.end()) {
            const ClassInfo* cls = iit->second.cls.get();
            if (!cls || !cls->is_exception_class) return false;
            out = cls->external_name();
            return true;
        }
        return false;
    }
    if (e.kind == ExprKind::Attribute) {
        // import lib; try: ... except lib.LibError: —— 模块别名下的异常类。
        auto& a = static_cast<Attribute&>(e);
        if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
            if (auto mod = resolve_module_alias(vn->id)) {
                auto it = mod->classes.find(a.attr);
                if (it == mod->classes.end() || !it->second.cls ||
                    !it->second.cls->is_exception_class)
                    return false;
                out = it->second.cls->external_name();
                return true;
            }
        }
        return false;
    }
    return false;
}



VReg Lowerer::lower_exc_match(Expr& type) {
    auto one = [&](Expr& e) -> VReg {
        std::string name;
        if (exc_type_name(e, name))
            return call_runtime(RtFn::ExcMatches, t_bool(), {const_str(name)});
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
        call_void(raise_rt(RtFn::Reraise), {});
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
    TypePtr xt = tc_.type_of(r.exc.get());

    // 富异常（自定义异常类有字段/方法）：构造真实例后抛实例。

    if (xt && xt->kind == TyKind::Class && xt->cls && xt->cls->is_rich_exception) {

        VReg inst = expr(*r.exc);

        call_void(RtFn::RaiseInstance, {inst, const_str(xt->cls->name)});

        return;

    }

    if (xt && xt->kind == TyKind::Exception) {
        VReg v = expr(*r.exc);
        call_void(raise_rt(RtFn::RaiseValueFrom), {v, cause == kNoReg ? const_int(0) : cause});
        return;
    }
    std::string type;
    VReg msg = kNoReg;
    Expr& e = *r.exc;
    if (e.kind == ExprKind::Name) {
        if (!exc_type_name(e, type)) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
    } else if (e.kind == ExprKind::Attribute) {
        // 裸 raise lib.LibError：模块属性形式的异常类。
        if (!exc_type_name(e, type)) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
    } else if (e.kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(e);
        if (c.func->kind != ExprKind::Name && c.func->kind != ExprKind::Attribute) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
        if (!exc_type_name(*c.func, type)) {
            err(e.line, e.col, "raise requires an exception name or instance");
            return;
        }
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
        call_void(raise_rt(RtFn::RaiseValueFrom), {v, cause});
        return;
    }
    call_void(raise_rt(RtFn::Raise), {const_str(type), msg});
}


void Lowerer::lower_try(Try& t) {
    // 零开销异常（Z2-b2 / Z4）：本函数被判成可接管的形状时，唯一那条 try 走
    // 新路径 —— 包括带 finally 的情形（lower_try_zero_cost 自己处理 finalbody）。
    // 这一判断必须排在 finally 分派之前，否则带 finally 的 try 永远进不了新路径。
    if (st_->zero_cost_tries.count(&t) != 0) {
        lower_try_zero_cost(t);
        return;
    }
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
    // 零开销异常（Z2-b2）：本函数被判成最小形状时，唯一那条 try 走新路径。
    if (st_->zero_cost_tries.count(&t) != 0) {
        lower_try_zero_cost(t);
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
    call_void(raise_rt(RtFn::Raise), {const_str("AssertionError"), msg});
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



// ── 零开销异常（Z2-b2，--zero-cost-exc）───────────────────────────────
//
// 第一刀只接一种形状：非可挂起函数里的「单层 catch-all try」——
//   - 函数体里恰好一条 Try、没有任何 With；
//   - 那条 Try 恰好一个 handler，且是 catch-all（裸 except / except Exception），
//     没有 else / finally；
//   - try 体与 handler 体里没有 return / break / continue（第一刀要求「try
//     之后才 return」：跨出保护区的早退暂时不接）；
//   - try 体里的 raise 只用「无参 / 单位置参」形态，且不是富异常实例
//     （多参 raise / 富异常实例走 RaiseArgs / RaiseInstance，第一刀不接）。
// 其余函数（生成器/协程、with、finally、嵌套 try、多 handler）继续走 setjmp
// 路径，一个字节都不变。
void Lowerer::collect_zero_cost_tries(FunctionDef& f) {
    // 逐个 try 独立判定：满足形状的收进集合、走 LSDA 路径，其余留在 setjmp 路径。
    // 嵌套时内外层可以分别选择路径（codegen 侧同一函数的多条保护区合成一份 LSDA）。
    //
    // 排除条件：
    //   - 带 else（要求 try 体正常结束后才执行，需要在保护区终点插跳转）；
    //   - 体内有 with（with 可能抑制异常，语义上是 cleanup 不是 catch）；
    //   - 富异常实例 / 多参 raise（没有 _Unwind 版）。
    std::vector<Try*> all;
    walk_stmt_tree(f.body, [&](Stmt& s) {
        if (s.kind == StmtKind::Try) all.push_back(&static_cast<Try&>(s));
    });

    for (Try* tp : all) {
        Try& t = *tp;
        if (!t.orelse.empty()) continue;

        bool bad = false;
        auto check = [&](Stmt& s) {
            if (bad) return;
            switch (s.kind) {
                case StmtKind::Raise: {
                    auto& r = static_cast<Raise&>(s);
                    if (!r.exc) break;  // 裸 raise 由 pa_eh_reraise 支持
                    TypePtr xt = tc_.type_of(r.exc.get());
                    if (xt && xt->kind == TyKind::Class && xt->cls &&
                        xt->cls->is_rich_exception) {
                        bad = true;
                        break;
                    }
                    if (r.exc->kind == ExprKind::Call) {
                        auto& c = static_cast<Call&>(*r.exc);
                        if (c.args.size() > 1) bad = true;
                        for (auto& a : c.args)
                            if (a && a->kind == ExprKind::Starred) bad = true;
                    }
                    break;
                }
                case StmtKind::With:
                    // with 可能抑制异常（__exit__ 返回 True），不是纯 cleanup。
                    bad = true;
                    break;
                default:
                    break;
            }
        };
        walk_stmt_tree(t.body, check);
        for (auto& hb : t.handlers) walk_stmt_tree(hb.body, check);
        if (!bad) st_->zero_cost_tries.insert(&t);
    }
}

// 零开销 try 的降级。landing pad 只做两件事：pa_eh_land 把展开包装里的
// 异常装回全局（与 longjmp 落地后逐项一致），然后 RcCleanup 复刻老路径
// pa_handler_leave 的临时值水位清理；后面的 except 分发块与 old path 逐句
// 相同（ExcMatches / ExcAcceptValue / ExcDone 一行不改）。
void Lowerer::lower_try_zero_cost(Try& t) {
    // finally 存在时：正常路径（含 handler 体正常结束）在 end 块跑一遍
    // finalbody；异常路径（LP 里、handler 全不匹配）跑 finalbody 后重抛。
    // 两条路径各内联一份 —— 与老路径非可挂起函数的做法一致。
    const bool has_fin = !t.finalbody.empty();
    // finally 体单独一块：try 体正常结束、handler 体正常结束都跳到它，
    // 跑完再进 end。这样 finalbody 只内联一份，不会因为 handler 正常结束
    // 而重复执行（异常路径那份在 LP 里，见下）。
    const int fin_block = has_fin ? new_block("try.finally") : -1;
    int body = new_block("try.body");

    // 临时值水位：复刻 pa_handler_push 里 h->mark = pa_rc_mark() 的那一半。
    VReg mark = call_runtime(RtFn::RcMark, t_int(), {});
    int mark_slot = new_temp_slot(t_int());
    store_slot(mark_slot, mark);
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = body;
    emit(std::move(jb));

    switch_to(body);
    IRFunction::EhRegion region;
    region.begin_block = body;
    {
        Inst eb;
        eb.op = IrOp::EhBegin;
        emit(std::move(eb));
    }
    // 注册一条清理项：try 体里若 return/break/continue 提前离开保护区，
    // emit_cleanup_exits 会先 pa_eh_leave（退出表驱动展开区）再按水位清临时值。
    // 没有这一条，早退会让 pa_eh_depth 一直 > 0，之后抛出的异常会误走 _Unwind。
    {
        FuncCtx::Cleanup zc;
        zc.kind = FuncCtx::Cleanup::Kind::ZeroCostEh;
        zc.mark_slot = mark_slot;
        st_->cleanups.push_back(zc);
    }
    ++st_->zc_raise_depth;
    stmts(t.body);
    --st_->zc_raise_depth;
    st_->cleanups.pop_back();

    if (terminated()) {
        // 早退由 ZeroCostEh 清理项处理；走到这里说明还有别的东西提前终止了控制流。
        // 形状判定漏了东西。宁可报错，也不要生成一条没有终点标签的保护区。
        err(t.line, t.col, "internal: zero-cost try body ended with a terminator");
        return;
    }

    // 保护区终点单开一块：保证 .LEHE 标签物理上落在 try 体所有指令之后、
    // landing pad 之前，且不把 landing pad 自己圈进保护区。
    // 必须显式 jump 过去：opt_forward 的 prune_unreachable 只认 Jump/Branch
    // 边、不吃 fallthrough，落空块会被当死块删掉。
    int body_end = new_block("try.body.end");
    int land = new_block("try.land");
    int end = new_block("try.end");
    Inst jbe;
    jbe.op = IrOp::Jump;
    jbe.target = body_end;
    emit(std::move(jbe));
    switch_to(body_end);
    {
        Inst ee;
        ee.op = IrOp::EhEnd;
        emit(std::move(ee));
    }
    call_void(RtFn::RcCleanup, {load_slot(mark_slot, t_int())});
    Inst je;
    je.op = IrOp::Jump;
    je.target = has_fin ? fin_block : end;
    emit(std::move(je));

    // landing pad 入口：personality 把 GR0（%rax）设成展开包装指针。
    switch_to(land);
    region.lp_block = land;
    region.end_block = body_end;
    st_->fn->eh_regions.push_back(region);
    {
        Inst lp;
        lp.op = IrOp::EhLandingPad;
        emit(std::move(lp));
    }
    call_void(RtFn::EhLand, {});
    call_void(RtFn::RcCleanup, {load_slot(mark_slot, t_int())});

    // except 分发链：与 lower_try_core 的 land 块逐句对应（HandlerLeave → 上面的
    // RcCleanup；不匹配的 ExcReraise → _Unwind 版 EhRethrow，它会重启 phase 1，
    // 所以内层不匹配时外层 handler 仍有机会接住）。
    struct ZcHBody {
        ExceptHandler* h;
        int block;
    };
    std::vector<ZcHBody> bodies;
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
        // 没有 handler 接住。有 finally 时先跑 finalbody 再重抛；
        // 重抛用 pa_eh_rethrow（重启 phase 1），所以外层 try 仍有机会接住。
        // 这里内联一份 finalbody 而不是跳 fin_block —— fin_block 跑完是进 end
        // （正常结束），而这条路径必须重抛，控制流终点不同。
        if (has_fin) {
            stmts(t.finalbody);
            // finalbody 里若 return/break/continue 终止了控制流，异常就被吞掉，
            // 这与 CPython 的语义一致（finally 里的 return 会丢弃异常）。
        }
        if (!terminated()) {
            call_void(RtFn::EhRethrow, {});
            end_unreachable();
        }
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
        // 槽已经 retain 过；释放创建时的那一个引用（与 lower_try_core 同）。
        rc_dec(ev);
        stmts(hb.h->body);
        if (shadow >= 0) comp_slots_.pop_back();
        if (terminated()) {
            // except 体里 return/break/continue 是合法的：异常已经被这个 handler
            // 接住并处理完，控制流正常离开。跳过 ExcDone 与跳到 end 的代码，
            // 直接处理下一个 handler 体（它的块虽然不可达，但仍要生成，
            // 因为分派链可能从别处跳进来）。
            continue;
        }
        call_void(RtFn::ExcDone, {});
        Inst jj;
        jj.op = IrOp::Jump;
        // 有 finally 时先跳 fin 块跑 finalbody（只跑一份，不在这里内联）；
        // 没有 finally 直接进 end。
        jj.target = has_fin ? fin_block : end;
        emit(std::move(jj));
    }

    // finally 结构（有 finally 时）：
    //   try 体正常结束 / handler 体正常结束  ->  fin 块（跑 finalbody）-> end
    //   LP 里没有 handler 接住              ->  fin 块（跑 finalbody）-> 重抛
    // 用 fin 块而不是把 finalbody 内联两处，避免 handler 正常结束时重复执行。
    if (has_fin) {
        // 正常路径（try 体结束 / handler 体结束）落到这里：跑 finalbody 后进 end。
        switch_to(fin_block);
        stmts(t.finalbody);
        Inst jf;
        jf.op = IrOp::Jump;
        jf.target = end;
        emit(std::move(jf));
    }

    switch_to(end);
}

}  // namespace pyadamas
