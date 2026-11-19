// lower_import_misc.cpp —— functools/datetime/pathlib/json 的降级
#include "lower_impl.h"

namespace pyadamas {

bool Lowerer::json_desc(const TypePtr& t, std::string& out, int line, int col,
                        bool for_loads) {
    if (!t || t->kind == TyKind::Unknown) {
        err(line, col, "json needs a statically known type");
        return false;
    }
    switch (t->kind) {
        case TyKind::None: out = "n"; return true;
        case TyKind::Bool: out = "b"; return true;
        case TyKind::Int: out = "i"; return true;
        case TyKind::Float: out = "f"; return true;
        case TyKind::Str: out = "s"; return true;
        case TyKind::List: {
            if (t->sub.empty()) {
                err(line, col, "json: list element type is unknown");
                return false;
            }
            std::string inner;
            if (!json_desc(t->sub[0], inner, line, col, for_loads)) return false;
            out = "(l" + inner + ")";
            return true;
        }
        case TyKind::Dict: {
            if (t->sub.size() < 2) {
                err(line, col, "json: dict type is unknown");
                return false;
            }
            // 键描述符：dumps 按 CPython json 规则把 int/float/bool/None 键
            // 强制转 str（运行期 jb_key_scalar）；loads 的样例键必须是 str
            // ——CPython loads 永远返回 str 键，不做逆向转换。
            char kc = 0;
            if (t->sub[0]) {
                switch (t->sub[0]->kind) {
                    case TyKind::Str: kc = 's'; break;
                    case TyKind::Int: kc = 'i'; break;
                    case TyKind::Float: kc = 'f'; break;
                    case TyKind::Bool: kc = 'b'; break;
                    case TyKind::None: kc = 'n'; break;
                    default: break;
                }
            }
            if (!kc) {
                err(line, col,
                    std::format("json dict keys must be str/int/float/bool/None, got '{}'",
                                type_name(t->sub[0])));
                return false;
            }
            if (for_loads && kc != 's') {
                err(line, col,
                    "json.loads() sample dict keys must be str "
                    "(CPython loads always returns str keys)");
                return false;
            }
            std::string inner;
            if (!json_desc(t->sub[1], inner, line, col, for_loads)) return false;
            out = "(d";
            out += kc;
            out += inner;
            out += ")";
            return true;
        }
        case TyKind::Tuple: {
            if (t->variadic) {
                err(line, col, "json does not support variable-length tuples");
                return false;
            }
            std::string s = "(t";
            for (auto& sub : t->sub) {
                std::string inner;
                if (!json_desc(sub, inner, line, col, for_loads)) return false;
                s += inner;
            }
            s += ")";
            out = std::move(s);
            return true;
        }
        default:
            err(line, col,
                std::format("json does not support type '{}'", type_name(t)));
            return false;
    }
}

// 原始形参下标（pos 在前，kwonly 从 pos.size() 起）→ 类型。
static TypePtr fn_param_ty(const FuncInfoPtr& fn, int idx) {
    int n = static_cast<int>(fn->pos.size());
    if (idx < n) return fn->pos[static_cast<size_t>(idx)].ty;
    return fn->kwonly[static_cast<size_t>(idx - n)].ty;
}

static const std::string& fn_param_name(const FuncInfoPtr& fn, int idx) {
    int n = static_cast<int>(fn->pos.size());
    if (idx < n) return fn->pos[static_cast<size_t>(idx)].name;
    return fn->kwonly[static_cast<size_t>(idx - n)].name;
}

// 闭包捕获的值必须是 cell（单元素 list[T]），析构时由闭包统一 dec。
VReg Lowerer::make_cell_value(VReg v, TypePtr T) {
    TypePtr cell_ty = mk_list(T ? T : t_unknown());
    int kind = list_kind_filled(cell_ty);
    retain_elem(v, kind);
    Inst in;
    in.op = IrOp::MakeList;
    in.imm = kind;
    in.args.push_back(v);
    return def_heap(cell_ty, std::move(in));
}

// functools.partial：合成一个转发函数，把 f 和绑定实参放进闭包 cell，
// 其余形参原样转发。返回的闭包签名就是 PartialPlan::sig。
VReg Lowerer::build_partial_wrapper(Call& c, VReg fnv, TypePtr fty,
                                    const PartialPlan& plan, int line, int col) {
    FuncInfoPtr fn = (fty && fty->kind == TyKind::Func) ? fty->fn : nullptr;
    if (!fn || !plan.sig) {
        err(line, col, "functools.partial() requires a function with a known signature");
        return const_none();
    }
    FuncInfoPtr sig = plan.sig;
    int npos = static_cast<int>(fn->pos.size());
    int ntotal = npos + static_cast<int>(fn->kwonly.size());
    std::vector<int> cap_of_param(static_cast<size_t>(ntotal), -1);
    for (size_t i = 0; i < plan.bound_params.size(); ++i)
        cap_of_param[static_cast<size_t>(plan.bound_params[i])] = static_cast<int>(i) + 1;

    // 实参求值顺序与 CPython 一致：f 之后先位置参数，再关键字参数。
    std::vector<Expr*> arg_exprs;
    for (size_t i = 1; i < c.args.size(); ++i) arg_exprs.push_back(c.args[i].get());
    for (auto& kw : c.keywords) arg_exprs.push_back(kw.value.get());
    std::vector<VReg> bound_vals;
    std::vector<TypePtr> bound_tys;
    for (size_t i = 0; i < arg_exprs.size(); ++i) {
        VReg v = expr(*arg_exprs[i]);
        if (i >= plan.bound_params.size()) continue;
        TypePtr want = fn_param_ty(fn, plan.bound_params[i]);
        v = coerce(v, tc_.type_of(arg_exprs[i]), want);
        bound_vals.push_back(v);
        bound_tys.push_back(want ? want : t_unknown());
    }
    if (plan.bound_params.empty()) return fnv;  // partial(f) 就是 f

    std::string label = std::format("__pa_partial_{}_{}", fn->name, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    TypePtr rty = fn->ret ? fn->ret : t_unknown();
    wf->ret = rty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    // 暴露给调用方的形参：按 sig 的顺序分配槽，再映射回原始形参下标。
    std::vector<int> slot_of_param(static_cast<size_t>(ntotal), -1);
    size_t rem_i = 0;
    auto add_sig_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        if (rem_i < plan.remaining.size())
            slot_of_param[static_cast<size_t>(plan.remaining[rem_i++])] = s;
    };
    for (auto& p : sig->pos) add_sig_param(p);
    for (auto& p : sig->kwonly) add_sig_param(p);

    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    auto read_cap = [&](int cap, TypePtr T) {
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16 + 8 * cap, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    };

    VReg callee = read_cap(0, fty);
    std::vector<VReg> call_args;
    call_args.reserve(static_cast<size_t>(ntotal));
    for (int i = 0; i < ntotal; ++i) {
        TypePtr T = fn_param_ty(fn, i);
        if (cap_of_param[static_cast<size_t>(i)] > 0)
            call_args.push_back(read_cap(cap_of_param[static_cast<size_t>(i)], T));
        else if (slot_of_param[static_cast<size_t>(i)] >= 0)
            call_args.push_back(load_slot(slot_of_param[static_cast<size_t>(i)], T));
        else
            call_args.push_back(const_none());
    }
    Inst call;
    call.op = IrOp::Call;
    call.a = callee;
    call.args = std::move(call_args);
    VReg r = def_heap(rty, std::move(call));
    emit_return(r, rty);
    pop_frame();

    TypePtr out_ty = tc_.type_of(&c);
    if (!out_ty || out_ty->kind != TyKind::Func) {
        out_ty = new_ty(TyKind::Func);
        out_ty->fn = sig;
    }
    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(
        RtFn::ClosureNew, out_ty,
        {code, const_int(static_cast<int64_t>(1 + bound_vals.size()))});
    push_temp(closure);
    VReg fcell = make_cell_value(fnv, fty);
    rc_inc(fcell);
    field_store(closure, 16, fcell);
    for (size_t i = 0; i < bound_vals.size(); ++i) {
        VReg cell = make_cell_value(bound_vals[i], bound_tys[i]);
        rc_inc(cell);
        field_store(closure, 16 + 8 * (static_cast<int>(i) + 1), cell);
    }
    return closure;
}

// @functools.lru_cache：缓存的键按形参逐个套一层 dict，最后一级存返回值。
// maxsize 只求值不参与淘汰（缓存无上限）；键类型必须是 int/bool/float/str，
// unknown 按机器字（和 dict 的既有约定一致）。
VReg Lowerer::build_lru_wrapper(const std::string& base, VReg fnv, TypePtr fty,
                                int line, int col) {
    FuncInfoPtr fn = (fty && fty->kind == TyKind::Func) ? fty->fn : nullptr;
    if (!fn) {
        err(line, col, "@lru_cache requires a function with a known signature");
        return fnv;
    }
    if (fn->varargs || fn->kwargs) {
        err(line, col, "@lru_cache does not support functions with *args/**kwargs");
        return fnv;
    }
    int npos = static_cast<int>(fn->pos.size());
    int ntotal = npos + static_cast<int>(fn->kwonly.size());
    if (ntotal == 0) {
        err(line, col, "@lru_cache requires at least one parameter");
        return fnv;
    }
    for (int i = 0; i < ntotal; ++i) {
        TypePtr t = fn_param_ty(fn, i);
        bool ok = !t || t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                  t->kind == TyKind::Float || t->kind == TyKind::Str ||
                  t->kind == TyKind::Unknown;
        if (!ok) {
            err(line, col,
                std::format("@lru_cache parameter '{}' has unsupported key type '{}'",
                            fn_param_name(fn, i), type_name(t)));
            return fnv;
        }
    }
    TypePtr rty = fn->ret ? fn->ret : t_unknown();
    TypePtr root_ty = mk_dict(fn_param_ty(fn, 0), t_unknown());

    std::string label = std::format("__pa_lru_{}_{}", base, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = rty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<int> param_slots(static_cast<size_t>(ntotal), -1);
    int pi = 0;
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        param_slots[static_cast<size_t>(pi++)] = s;
    };
    for (auto& p : fn->pos) add_param(p);
    for (auto& p : fn->kwonly) add_param(p);

    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    auto read_cap = [&](int cap, TypePtr T) {
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16 + 8 * cap, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    };

    VReg callee = read_cap(0, fty);
    VReg cur = read_cap(1, root_ty);
    VReg ret = const_none();
    for (int i = 0; i < ntotal; ++i) {
        TypePtr kt = fn_param_ty(fn, i);
        int ko = own_kind(kt);
        VReg key = load_slot(param_slots[static_cast<size_t>(i)], kt);
        VReg cond = call_runtime(RtFn::DictContains, t_bool(), {cur, key});
        int hit = new_block("lru.hit");
        int miss = new_block("lru.miss");
        int join = new_block("lru.join");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cond;
        br.target = hit;
        br.target2 = miss;
        emit(std::move(br));

        if (i + 1 < ntotal) {
            TypePtr next_kt = fn_param_ty(fn, i + 1);
            TypePtr next_ty = (i + 2 < ntotal) ? mk_dict(next_kt, t_unknown())
                                               : mk_dict(next_kt, rty);
            int cs = new_temp_slot(next_ty);
            switch_to(miss);
            VReg nd = call_runtime(RtFn::DictNew, next_ty,
                                   {const_int(key_kind(next_kt)),
                                    const_int(own_kind(next_kt)), const_int(2),
                                    const_int(-1), const_int(-1)});
            push_temp(nd);
            retain_elem(key, ko);
            rc_inc(nd);  // 字典持有一份，临时栈那份随后清掉
            call_void(RtFn::DictSet, {cur, key, nd});
            store_slot(cs, nd);
            Inst jm;
            jm.op = IrOp::Jump;
            jm.target = join;
            emit(std::move(jm));

            switch_to(hit);
            VReg got = call_runtime(RtFn::DictGet, next_ty, {cur, key});
            store_slot(cs, got);
            Inst jh;
            jh.op = IrOp::Jump;
            jh.target = join;
            emit(std::move(jh));

            switch_to(join);
            cur = load_slot(cs, next_ty);
            continue;
        }

        // 最后一级：命中直接取；未命中调 f，把返回值存进缓存。
        int rs = new_temp_slot(rty);
        switch_to(miss);
        std::vector<VReg> call_args;
        call_args.reserve(static_cast<size_t>(ntotal));
        for (int k = 0; k < ntotal; ++k)
            call_args.push_back(
                load_slot(param_slots[static_cast<size_t>(k)], fn_param_ty(fn, k)));
        Inst call;
        call.op = IrOp::Call;
        call.a = callee;
        call.args = std::move(call_args);
        VReg rv = def_heap(rty, std::move(call));
        retain_elem(key, ko);
        if (ir_is_heap(rty)) rc_inc(rv);
        call_void(RtFn::DictSet, {cur, key, rv});
        store_slot(rs, rv);
        Inst jm;
        jm.op = IrOp::Jump;
        jm.target = join;
        emit(std::move(jm));

        switch_to(hit);
        VReg got = call_runtime(RtFn::DictGet, rty, {cur, key});
        store_slot(rs, got);
        Inst jh;
        jh.op = IrOp::Jump;
        jh.target = join;
        emit(std::move(jh));

        switch_to(join);
        ret = load_slot(rs, rty);
    }
    emit_return(ret, rty);
    pop_frame();

    TypePtr t0 = fn_param_ty(fn, 0);
    VReg root = call_runtime(RtFn::DictNew, root_ty,
                             {const_int(key_kind(t0)), const_int(own_kind(t0)),
                              const_int(2), const_int(-1), const_int(-1)});
    push_temp(root);
    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, fty, {code, const_int(2)});
    push_temp(closure);
    VReg fcell = make_cell_value(fnv, fty);
    rc_inc(fcell);
    field_store(closure, 16, fcell);
    VReg ccell = make_cell_value(root, root_ty);
    rc_inc(ccell);
    field_store(closure, 24, ccell);
    return closure;
}

VReg Lowerer::lower_functools(const std::string& member, Call& c) {
    if (member == "reduce") {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "functools.reduce() does not take keyword arguments");
        }
        if (c.args.size() < 2 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("functools.reduce() expects 2 or 3 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr ft = tc_.type_of(c.args[0].get());
        if (ft && ft->kind != TyKind::Func && ft->kind != TyKind::BoundMethod &&
            ft->kind != TyKind::Unknown) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("functools.reduce() expects a function, got '{}'", type_name(ft)));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt) rt = t_unknown();
        VReg f = expr(*c.args[0]);
        TypePtr elem = t_unknown();
        VReg xs = seq_as_list(c.args[1].get(), elem);
        // 回调的形参域决定折叠走哪套寄存器传参。只看结果域不够：
        // reduce(fadd, xs) 的 xs 是裸 list 时结果域是 unknown，但回调
        // 形参是 float，float 位模式会被当整数塞进整型实参寄存器。
        // 两个形参都是 float 时，回调入口恰好是 xmm0/xmm1，double 版跳板
        // 才对得上；元素域是 float/unknown/int 时都能按 double 读或提升。
        bool cb_fp = false;
        if (ft && ft->kind == TyKind::Func && ft->fn && ft->fn->pos.size() >= 2) {
            const TypePtr& p0 = ft->fn->pos[0].ty;
            const TypePtr& p1 = ft->fn->pos[1].ty;
            cb_fp = p0 && p0->kind == TyKind::Float && p1 && p1->kind == TyKind::Float;
        }
        bool elem_fp_like = !elem || elem->kind == TyKind::Unknown ||
                            elem->kind == TyKind::Float ||
                            elem->kind == TyKind::Int || elem->kind == TyKind::Bool;
        // 回调要 double 但元素是 str/类等非数值域：两套跳板都不对，
        // 拒绝比静默算错好（CPython 这里也是运行期 TypeError）。
        if (cb_fp && !elem_fp_like) {
            err(c.args[1]->line, c.args[1]->col,
                "reduce() over non-numeric elements cannot use a float callback");
            return const_none();
        }
        bool use_fp = rt->kind == TyKind::Float || cb_fp;
        // 运行时按结果域读元素：float 版把列表当 double 数组，元素域必须
        // 一致，否则会读出错误的位模式。int 列表先逐元素提升成 float；
        // 但回调必须能按 double 调用（形参是 float/unknown），否则算错。
        if (use_fp && elem && elem->kind != TyKind::Float && elem->kind != TyKind::Unknown) {
            bool cb_ok = false;
            if (ft && ft->kind == TyKind::Func && ft->fn && ft->fn->pos.size() >= 2) {
                auto compatible = [](const TypePtr& t) {
                    return !t || t->kind == TyKind::Unknown || t->kind == TyKind::Float;
                };
                cb_ok = compatible(ft->fn->pos[0].ty) && compatible(ft->fn->pos[1].ty);
            }
            if (!cb_ok) {
                err(c.args[1]->line, c.args[1]->col,
                    "reduce() over mixed int/float requires a float callback");
                return const_none();
            }
            VReg conv = promote_container(xs, mk_list(elem), mk_list(t_float()));
            if (conv == kNoReg) {
                err(c.args[1]->line, c.args[1]->col,
                    "reduce() over this element type is not supported yet");
                return const_none();
            }
            xs = conv;
        }
        bool has_init = c.args.size() == 3;
        VReg init = use_fp ? const_float(0.0) : const_int(0);
        if (has_init)
            init = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), rt);
        VReg v = call_runtime(use_fp ? RtFn::FuncReduceF : RtFn::FuncReduce, rt,
                              {f, xs, init, const_int(has_init ? 1 : 0)});
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }
    if (member == "wraps") {
        // pyadamas 不建模 __name__/__doc__，@functools.wraps(f) 这一层在装饰器
        // 处理里直接跳过；当普通函数调用说明用户想拿返回值，做不了。
        err(c.line, c.col, "functools.wraps() is only supported as a decorator");
        for (auto& a : c.args) expr(*a);
        return const_none();
    }
    if (member == "partial") {
        if (c.args.empty()) {
            err(c.line, c.col, "functools.partial() expects at least 1 argument");
            return const_none();
        }
        TypePtr ft = tc_.type_of(c.args[0].get());
        auto plan = tc_.partial_plan(&c);
        if (!plan || !ft || ft->kind != TyKind::Func || !ft->fn) {
            err(c.line, c.col,
                "functools.partial() requires a function with a known signature");
            for (auto& a : c.args) expr(*a);
            for (auto& kw : c.keywords) expr(*kw.value);
            return const_none();
        }
        VReg fnv = expr(*c.args[0]);
        return build_partial_wrapper(c, fnv, ft, *plan, c.line, c.col);
    }
    if (member == "lru_cache") {
        // @lru_cache 在装饰器处理里合成包装闭包；当普通调用说明想拿返回值。
        err(c.line, c.col, "functools.lru_cache() is only supported as a decorator");
        for (auto& a : c.args) expr(*a);
        for (auto& kw : c.keywords) expr(*kw.value);
        return const_none();
    }
    err(c.line, c.col, std::format("functools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_datetime(const std::string& member, Call& c) {
    // timedelta 收同名关键字参数，其它构造函数不收。
    if (!c.keywords.empty() && member != "timedelta") {
        err(c.line, c.col, std::format("datetime.{}() does not take keyword arguments", member));
    }
    if (member == "timedelta") {
        TypePtr tdty = tc_.type_of(&c);
        if (!tdty || tdty->kind != TyKind::TimeDelta) tdty = new_ty(TyKind::TimeDelta);
        static const char* kParams[7] = {"days",         "seconds", "microseconds",
                                         "milliseconds", "minutes", "hours",
                                         "weeks"};
        VReg vals[7];
        bool set[7] = {false, false, false, false, false, false, false};
        for (int i = 0; i < 7; ++i) vals[i] = const_int(0);
        if (c.args.size() > 7) {
            err(c.line, c.col,
                std::format("datetime.timedelta() takes at most 7 arguments, got {}",
                            c.args.size()));
        }
        for (size_t i = 0; i < c.args.size() && i < 7; ++i) {
            vals[i] = coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
            set[i] = true;
        }
        for (auto& kw : c.keywords) {
            int idx = -1;
            for (int i = 0; i < 7; ++i)
                if (kw.arg == kParams[i]) idx = i;
            if (idx < 0) {
                err(kw.value->line, kw.value->col,
                    std::format("datetime.timedelta() got an unexpected keyword argument '{}'",
                                kw.arg));
                expr(*kw.value);
                continue;
            }
            if (set[idx]) {
                err(kw.value->line, kw.value->col,
                    std::format("datetime.timedelta() got multiple values for argument '{}'",
                                kw.arg));
                expr(*kw.value);
                continue;
            }
            vals[idx] = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
            set[idx] = true;
        }
        // 单位换算放进整数运算，运行时只接收 days / seconds / microseconds。
        auto mul_add = [&](VReg a, int64_t k, VReg b) {
            Inst m;
            m.op = IrOp::Bin;
            m.bop = BinKind::Mul;
            m.a = a;
            m.b = const_int(k);
            VReg mv = def(t_int(), std::move(m));
            Inst ad;
            ad.op = IrOp::Bin;
            ad.bop = BinKind::Add;
            ad.a = mv;
            ad.b = b;
            return def(t_int(), std::move(ad));
        };
        VReg days = mul_add(vals[6], 7, vals[0]);                 // weeks * 7 + days
        VReg micros = mul_add(vals[3], 1000, vals[2]);            // millis * 1000 + micros
        VReg secs = mul_add(vals[5], 3600, mul_add(vals[4], 60, vals[1]));
        VReg v = call_runtime(RtFn::TimeDeltaNew, tdty, {days, secs, micros});
        push_temp(v);
        return v;
    }
    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind != TyKind::DateTime) rt = new_ty(TyKind::DateTime);
    if (member == "now" || member == "today") {
        if (!c.args.empty())
            err(c.line, c.col, std::format("datetime.{}() takes no arguments", member));
        VReg v = call_runtime(RtFn::DateTimeNow, rt, {const_int(member == "today" ? 1 : 0)});
        push_temp(v);
        return v;
    }
    // 实参按从左到右求值（C++ 不保证花括号里的求值顺序，必须分开写）。
    auto iarg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    if (member == "date") {
        if (c.args.size() != 3) {
            err(c.line, c.col, "datetime.date() takes 3 arguments (year, month, day)");
            return const_none();
        }
        VReg y = iarg(0);
        VReg mo = iarg(1);
        VReg d = iarg(2);
        VReg v = call_runtime(RtFn::DateNew, rt, {y, mo, d});
        push_temp(v);
        return v;
    }
    if (member == "datetime") {
        if (c.args.size() != 3 && c.args.size() != 6) {
            err(c.line, c.col, "datetime.datetime() takes 3 or 6 arguments");
            return const_none();
        }
        VReg y = iarg(0);
        VReg mo = iarg(1);
        VReg d = iarg(2);
        VReg h = const_int(0);
        VReg mi = const_int(0);
        VReg s = const_int(0);
        if (c.args.size() == 6) {
            h = iarg(3);
            mi = iarg(4);
            s = iarg(5);
        }
        VReg v = call_runtime(RtFn::DateTimeNew, rt, {y, mo, d, h, mi, s});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("datetime.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_pathlib(const std::string& member, Call& c) {
    if (member == "Path") {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "pathlib.Path() does not take keyword arguments");
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, "pathlib.Path() takes exactly one argument");
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Path) rt = new_ty(TyKind::Path);
        VReg s = expr(*c.args[0]);
        s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
        VReg v = call_runtime(RtFn::PathNew, rt, {s});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("pathlib.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_json(const std::string& member, Call& c) {
    // dumps 收 indent= / sort_keys= / ensure_ascii= / separators= / allow_nan=，
    // 其它几个不收关键字。
    if (!c.keywords.empty() && member != "dumps") {
        err(c.line, c.col, std::format("json.{}() does not take keyword arguments", member));
    }
    if (member == "dumps") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("json.dumps() expects 1 positional argument, got {}", c.args.size()));
            return const_none();
        }
        // indent 用 INT64_MIN 哨兵表示 None（紧凑）；0/负数是「换行不缩进」，
        // 与 CPython 一致。flags 打包：bit0 sort_keys / bit1 ensure_ascii /
        // bit2 allow_nan（默认 0b110）。seps 传 tuple[str,str] 指针或 0。
        VReg indent = const_int(INT64_MIN);
        VReg sort_keys = const_int(0);
        VReg ensure_ascii = const_int(1);
        VReg allow_nan = const_int(1);
        VReg seps = const_int(0);
        for (auto& kw : c.keywords) {
            if (kw.arg == "indent") {
                TypePtr kt = tc_.type_of(kw.value.get());
                if (kt && kt->kind == TyKind::None) {
                    indent = const_int(INT64_MIN);
                } else {
                    indent = coerce(expr(*kw.value), kt, t_int());
                }
            } else if (kw.arg == "sort_keys") {
                sort_keys = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
            } else if (kw.arg == "ensure_ascii") {
                ensure_ascii = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
            } else if (kw.arg == "allow_nan") {
                allow_nan = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
            } else if (kw.arg == "separators") {
                TypePtr st = tc_.type_of(kw.value.get());
                if (!st || st->kind != TyKind::Tuple || st->variadic ||
                    st->sub.size() != 2 || !st->sub[0] ||
                    st->sub[0]->kind != TyKind::Str || !st->sub[1] ||
                    st->sub[1]->kind != TyKind::Str) {
                    err(kw.value->line, kw.value->col,
                        "json.dumps() separators must be a tuple[str, str]");
                }
                seps = expr(*kw.value);
            } else {
                err(kw.value->line, kw.value->col,
                    std::format("json.dumps() got an unexpected keyword argument '{}'", kw.arg));
            }
        }
        auto bin = [&](BinKind k, VReg a, VReg b) {
            Inst in;
            in.op = IrOp::Bin;
            in.bop = k;
            in.a = a;
            in.b = b;
            return def(t_int(), std::move(in));
        };
        VReg flags = bin(BinKind::BitOr, sort_keys,
                         bin(BinKind::Shl, ensure_ascii, const_int(1)));
        flags = bin(BinKind::BitOr, flags, bin(BinKind::Shl, allow_nan, const_int(2)));
        TypePtr vt = tc_.type_of(c.args[0].get());
        std::string desc;
        if (!json_desc(vt, desc, c.args[0]->line, c.args[0]->col)) return const_none();
        if (vt && vt->kind == TyKind::None) return const_str("null");
        VReg v = expr(*c.args[0]);
        VReg s = call_runtime(RtFn::JsonDumps, t_str(),
                              {v, const_str(desc), indent, flags, seps});
        push_temp(s);
        return s;
    }
    if (member == "loads") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("json.loads() expects 2 arguments (text, sample), got {}",
                            c.args.size()));
            return const_none();
        }
        // 样例值只提供类型，运行时不求值（类型检查阶段已经检查过）。
        TypePtr sample = tc_.type_of(c.args[1].get());
        std::string desc;
        if (!json_desc(sample, desc, c.args[1]->line, c.args[1]->col, /*for_loads=*/true))
            return const_none();
        VReg text = expr(*c.args[0]);
        VReg v = call_runtime(RtFn::JsonLoads, sample, {text, const_str(desc)});
        if (ir_is_heap(sample)) push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("json.{} is not supported", member));
    return const_none();
}


}  // namespace pyadamas
