// lower_call.cpp —— 调用降级：参数绑定、内置/方法/asyncio
#include "lower_impl.h"

namespace py23 {

std::vector<VReg> Lowerer::bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn) {
    std::vector<VReg> args;
    if (!info) {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "keyword arguments require a known function signature");
            return args;
        }
        for (auto& a : c.args) {
            if (a->kind == ExprKind::Starred) {
                err(a->line, a->col, "'*args' is not supported by the code generator yet");
                continue;
            }
            args.push_back(expr(*a));
        }
        return args;
    }

    size_t npos = info->pos.size();
    size_t nkw = info->kwonly.size();
    std::vector<VReg> bound(npos + nkw, kNoReg);

    // ── *args ────────────────────────────────────────────────
    // 变长实参在调用点打包成一个 list，作为最后一个（或倒数第二个）
    // 形参传进去；callee 里它就是一个普通 list 形参。
    TypePtr vty = info->varargs ? (info->vararg_ty ? info->vararg_ty : t_unknown()) : nullptr;
    TypePtr lty = info->varargs ? mk_list(vty) : nullptr;
    int vk = info->varargs ? list_kind(lty) : 0;
    VReg vlist = kNoReg;
    auto vararg_list = [&]() -> VReg {
        if (vlist == kNoReg) {
            vlist = call_runtime(RtFn::ListNew, lty, {const_int(0), const_int(vk)});
            push_temp(vlist);
        }
        return vlist;
    };
    auto append_vararg = [&](VReg v, TypePtr vt) {
        v = coerce(v, vt, vty);
        retain_elem(v, vk);
        call_void(RtFn::ListAppend, {vararg_list(), v});
    };

    // 已知长度的 tuple 可以静态展开，即使 callee 没有 *args。
    auto is_fixed_tuple = [&](TypePtr t) {
        return t && t->kind == TyKind::Tuple && !t->variadic;
    };

    size_t pos_i = 0;
    for (auto& ap : c.args) {
        Expr& a = *ap;
        if (a.kind == ExprKind::Starred) {
            Expr& inner = *static_cast<Starred&>(a).value;
            TypePtr tt = tc_.type_of(&inner);
            if (is_fixed_tuple(tt)) {
                VReg tv = expr(inner);
                for (size_t j = 0; j < tt->sub.size(); ++j) {
                    TypePtr et = tt->sub[j];
                    Inst in;
                    in.op = IrOp::IndexLoad;
                    in.a = tv;
                    in.b = const_int(static_cast<int64_t>(j));
                    VReg ev = def(et, std::move(in));
                    if (pos_i < npos) {
                        bound[pos_i++] = ev;
                    } else if (info->varargs) {
                        append_vararg(ev, et);
                    } else {
                        err(a.line, a.col,
                            std::format("too many positional arguments (expected at most {})", npos));
                        break;
                    }
                }
                continue;
            }
            if (pos_i < npos) {
                expr(inner);
                err(a.line, a.col,
                    "'*' cannot be expanded before the fixed positional parameters are filled; "
                    "pass the iterable after them or make the parameter count static");
                continue;
            }
            if (!info->varargs) {
                expr(inner);
                err(a.line, a.col,
                    std::format("function '{}' does not accept a variable number of arguments",
                                info->name));
                continue;
            }
            VReg s = expr(inner);
            TypePtr st = tc_.type_of(&inner);
            if (st && st->kind == TyKind::Str) {
                VReg l = call_runtime(RtFn::StrToList, mk_list(t_str()), {s});
                push_temp(l);
                s = l;
            } else if (st && st->kind != TyKind::List && st->kind != TyKind::Tuple &&
                       st->kind != TyKind::Unknown) {
                err(a.line, a.col,
                    std::format("cannot expand '{}' into '*args'", type_name(st)));
                continue;
            }
            call_void(RtFn::ListExtend, {vararg_list(), s});
            continue;
        }
        VReg v = expr(a);
        if (pos_i < npos) {
            bound[pos_i++] = v;
        } else if (info->varargs) {
            append_vararg(v, tc_.type_of(&a));
        } else {
            err(a.line, a.col,
                std::format("too many positional arguments (expected at most {})", npos));
        }
    }

    // ── **kwargs ─────────────────────────────────────────────
    TypePtr kwty = info->kwargs ? (info->kwarg_ty ? info->kwarg_ty : t_unknown()) : nullptr;
    TypePtr kdty = info->kwargs ? mk_dict(t_str(), kwty) : nullptr;
    int kown = info->kwargs ? own_kind(kwty) : 0;
    VReg kwd = kNoReg;
    auto kwarg_dict = [&]() -> VReg {
        if (kwd == kNoReg) {
            kwd = call_runtime(RtFn::DictNew, kdty,
                               {const_int(1), const_int(1), const_int(kown)});
            push_temp(kwd);
        }
        return kwd;
    };

    for (auto& k : c.keywords) {
        if (k.arg.empty()) {
            if (!info->kwargs) {
                expr(*k.value);
                err(k.value->line, k.value->col,
                    std::format("'{}' does not accept '**kwargs'", info->name));
                continue;
            }
            VReg m = expr(*k.value);
            TypePtr mt = tc_.type_of(k.value.get());
            if (mt && mt->kind != TyKind::Dict && mt->kind != TyKind::Unknown) {
                err(k.value->line, k.value->col,
                    "cannot unpack a non-dict value into '**kwargs'");
                continue;
            }
            if (mt && mt->kind == TyKind::Dict && !mt->sub.empty() &&
                mt->sub[0]->kind != TyKind::Str && mt->sub[0]->kind != TyKind::Unknown) {
                err(k.value->line, k.value->col, "'**kwargs' keys must be strings");
                continue;
            }
            call_void(RtFn::DictUpdate, {kwarg_dict(), m});
            continue;
        }
        int idx = -1;
        for (size_t i = info->n_posonly; i < npos; ++i)
            if (info->pos[i].name == k.arg) idx = static_cast<int>(i);
        if (idx < 0) {
            for (size_t i = 0; i < nkw; ++i)
                if (info->kwonly[i].name == k.arg) idx = static_cast<int>(npos + i);
        }
        if (idx < 0) {
            if (info->kwargs) {
                VReg v = expr(*k.value);
                v = coerce(v, tc_.type_of(k.value.get()), kwty);
                retain_elem(v, kown);
                call_void(RtFn::DictSet, {kwarg_dict(), const_str(k.arg), v});
                continue;
            }
            err(k.value->line, k.value->col,
                std::format("unexpected keyword argument '{}'", k.arg));
            continue;
        }
        if (bound[idx] != kNoReg) {
            err(k.value->line, k.value->col,
                std::format("got multiple values for argument '{}'", k.arg));
            continue;
        }
        bound[idx] = expr(*k.value);
    }

    // 缺省参数：只支持字面量常量默认值
    auto lit_to_const = [](const LiteralValue& lv) {
        ConstVal cv;
        switch (lv.kind) {
            case LiteralValue::Kind::None: cv.ty = t_none(); break;
            case LiteralValue::Kind::Bool: cv.ty = t_bool(); cv.i = lv.b ? 1 : 0; break;
            case LiteralValue::Kind::Int: cv.ty = t_int(); cv.i = lv.i; break;
            case LiteralValue::Kind::Float: cv.ty = t_float(); cv.f = lv.f; break;
            case LiteralValue::Kind::Str: cv.ty = t_str(); cv.s = lv.s; break;
        }
        return cv;
    };
    auto default_of = [&](size_t pos_index) -> std::optional<ConstVal> {
        // 跨模块接口只有 default_lit，没有 AST。
        if (info && pos_index < info->pos.size() && info->pos[pos_index].default_lit)
            return lit_to_const(*info->pos[pos_index].default_lit);
        // 签名里的默认表达式优先：通过函数值 / lambda 调用时没有
        // FunctionDef 可查，只能靠 FuncInfo 带着的 AST 指针。
        if (info && pos_index < info->pos.size() && info->pos[pos_index].default_expr)
            return as_const(info->pos[pos_index].default_expr);
        if (!defn) return std::nullopt;
        size_t nd = defn->args.defaults.size();
        if (pos_index + nd < npos) return std::nullopt;
        size_t di = pos_index - (npos - nd);
        if (di >= nd) return std::nullopt;
        return as_const(defn->args.defaults[di].get());
    };
    auto emit_const = [&](const ConstVal& cv) -> VReg {
        switch (cv.ty->kind) {
            case TyKind::None: return const_none();
            case TyKind::Bool: return const_bool(cv.i != 0);
            case TyKind::Int: return const_int(cv.i);
            case TyKind::Float: return const_float(cv.f);
            case TyKind::Str: return const_str(cv.s);
            default: return const_none();
        }
    };

    for (size_t i = 0; i < npos; ++i) {
        if (bound[i] != kNoReg) continue;
        if (!info->pos[i].has_default) {
            err(c.line, c.col,
                std::format("missing required argument '{}'", info->pos[i].name));
            bound[i] = const_none();
            continue;
        }
        auto cv = default_of(i);
        if (!cv) {
            err(c.line, c.col,
                std::format("default value of '{}' is not a literal constant; "
                            "the code generator cannot fill it in yet",
                            info->pos[i].name));
            bound[i] = const_none();
            continue;
        }
        bound[i] = emit_const(*cv);
    }
    for (size_t i = 0; i < nkw; ++i) {
        size_t bi = npos + i;
        if (bound[bi] != kNoReg) continue;
        if (!info->kwonly[i].has_default) {
            err(c.line, c.col,
                std::format("missing required keyword argument '{}'", info->kwonly[i].name));
            bound[bi] = const_none();
            continue;
        }
        // kwonly 默认值：签名里带了默认值就直接用，否则回退到 def。
        std::optional<ConstVal> cv;
        if (info && i < info->kwonly.size() && info->kwonly[i].default_lit)
            cv = lit_to_const(*info->kwonly[i].default_lit);
        if (!cv && info && i < info->kwonly.size() && info->kwonly[i].default_expr)
            cv = as_const(info->kwonly[i].default_expr);
        if (!cv && defn && i < defn->args.kw_defaults.size() && defn->args.kw_defaults[i])
            cv = as_const(defn->args.kw_defaults[i].get());
        if (!cv) {
            err(c.line, c.col,
                std::format("default value of '{}' is not a literal constant", info->kwonly[i].name));
            bound[bi] = const_none();
            continue;
        }
        bound[bi] = emit_const(*cv);
    }

    args = std::move(bound);
    // 实参按形参类型做数值转换：`def f(x: float)` 传 int 进来要变成 double，
    // 否则形参槽里躺的是整数位模式。
    for (size_t i = 0; i < args.size() && i < npos + nkw; ++i) {
        TypePtr want = i < npos ? info->pos[i].ty : info->kwonly[i - npos].ty;
        if (args[i] != kNoReg) args[i] = coerce(args[i], vreg_ty(args[i]), want);
    }
    // 变长形参即使没收到实参也要传空容器：callee 的形参槽必须有值。
    if (info->varargs) args.push_back(vararg_list());
    if (info->kwargs) args.push_back(kwarg_dict());
    return args;
}

VReg Lowerer::lower_construct(Call& c, const ClassInfoPtr& cls) {
    auto cid_it = class_ids_.find(cls.get());
    if (cid_it == class_ids_.end()) {
        err(c.line, c.col,
            std::format("class '{}' is not available here (nested classes are not supported yet)",
                        cls->name));
        return const_none();
    }

    Inst in;
    in.op = IrOp::ObjNew;
    in.imm = cid_it->second;
    VReg obj = def_heap(mk_class(cls.get()), std::move(in));

    // 字段初值：从最根的类往下按布局顺序求值。放在构造点而不是
    // __init__ 里，是为了让「子类覆盖 __init__ 但父类有字段初值」
    // 这种情况也能初始化到。
    std::vector<std::pair<int, const FieldInfo*>> defaults;
    {
        std::vector<const ClassInfo*> chain;
        for (const ClassInfo* k = cls.get(); k; k = k->base) chain.push_back(k);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (auto& f : (*it)->fields) {
                if (!f.has_default || !f.default_expr) continue;
                defaults.push_back({static_cast<int>(8 * (1 + (*it)->field_index(f.name))), &f});
            }
        }
    }
    for (auto& [off, fi] : defaults) {
        VReg v = expr(*fi->default_expr);
        v = coerce(v, vreg_ty(v), fi->ty);
        store_field(fi->ty, obj, off, v);
    }

    if (cls->has_init) {
        if (auto* im = cls->method("__init__"); im && im->call_sig) {
            std::vector<VReg> args = bind_args(c, im->call_sig, im->def);
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = obj;
            vc.imm = im->slot;
            vc.args = std::move(args);
            TypePtr rt = im->call_sig->ret ? im->call_sig->ret : t_none();
            def_heap(rt, std::move(vc));
        }
    }
    return obj;
}

VReg Lowerer::lower_call(Call& c) {
    // 内建函数
    if (c.func->kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(*c.func);
        if (is_builtin(n.id)) {
            static const std::unordered_set<std::string> kBuiltins = {
                "print", "len", "int", "float", "str", "bool", "range", "next",
            };
            if (kBuiltins.count(n.id)) return lower_builtin_call(n.id, c);
        }
        // 类名直接调用 = 构造。类名不是可求值的表达式，必须在这里拦，
        // 否则会退化成 LoadGlobal 读到一个空槽。
        if (auto it = classes_.find(n.id); it != classes_.end())
            return lower_construct(c, it->second);
        if (auto it = imported_classes_.find(n.id); it != imported_classes_.end())
            return construct_imported(it->second, c, mk_class(it->second.cls.get()));
        if (auto it = native_aliases_.find(n.id); it != native_aliases_.end()) {
            if (it->second.module == "asyncio") return lower_asyncio(it->second.member, c);
            TypePtr ft = tc_.type_of(c.func.get());
            FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;
            return call_native(it->second.module, it->second.member, c, info);
        }
    }

    // 方法调用
    if (c.func->kind == ExprKind::Attribute)
        return lower_method_call(static_cast<Attribute&>(*c.func), c);

    // 普通调用：按签名重排参数
    VReg callee = expr(*c.func);
    TypePtr ft = tc_.type_of(c.func.get());
    FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;

    FunctionDef* fdef = nullptr;
    if (c.func->kind == ExprKind::Name) {
        auto it = func_defs_.find(static_cast<Name&>(*c.func).id);
        if (it != func_defs_.end()) fdef = it->second;
    }

    std::vector<VReg> args = bind_args(c, info, fdef);

    Inst in;
    in.op = IrOp::Call;
    in.a = callee;
    in.args = std::move(args);
    return def_heap(tc_.type_of(&c), std::move(in));
}

VReg Lowerer::lower_builtin_call(const std::string& name, Call& c) {
    // print 的关键字是 sep / end，得单独处理，不能走下面这条通用拦截
    if (name == "print") return lower_print(c);

    if (!c.keywords.empty()) {
        err(c.line, c.col, std::format("{}() does not take keyword arguments here", name));
    }

    if (name == "len") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "len() takes exactly one argument");
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::Len;
        in.a = v;
        return def(t_int(), std::move(in));
    }

    if (name == "next") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "next() takes exactly one argument");
            return const_none();
        }
        TypePtr gt = tc_.type_of(c.args[0].get());
        if (!gt || gt->kind != TyKind::Generator) {
            err(c.line, c.col, std::format("next() requires a generator, got '{}'", type_name(gt)));
            return const_none();
        }
        TypePtr et = gt->sub.empty() ? t_unknown() : gt->sub[0];
        VReg g = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenNext);
        in.args = {g};
        // 运行时按位返回（浮点也是整数寄存器），vreg 类型记为 unknown；
        // 堆元素才入临时栈。
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        return v;
    }

    if (name == "range") {
        if (c.args.empty() || c.args.size() > 3) {
            err(c.line, c.col, "range() takes 1 to 3 arguments");
            return const_none();
        }
        auto arg = [&](size_t i) {
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        VReg start = const_int(0);
        VReg stop = arg(0);
        VReg step = const_int(1);
        if (c.args.size() >= 2) {
            start = stop;
            stop = arg(1);
        }
        if (c.args.size() == 3) step = arg(2);
        VReg v = call_runtime(RtFn::RangeNew, mk_list(t_int()), {start, stop, step});
        push_temp(v);
        return v;
    }

    if (name == "int" || name == "float" || name == "str" || name == "bool") {
        TypePtr to = name == "int"     ? t_int()
                     : name == "float" ? t_float()
                     : name == "str"   ? t_str()
                                       : t_bool();
        if (c.args.empty()) return to->kind == TyKind::Int ? const_int(0)
                                       : to->kind == TyKind::Float ? const_float(0)
                                       : to->kind == TyKind::Str   ? const_str("")
                                                                   : const_bool(false);
        if (c.args.size() > 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        return convert_to(v, tc_.type_of(c.args[0].get()), to);
    }

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

VReg Lowerer::lower_asyncio(const std::string& member, Call& c) {
    TypePtr rt = tc_.type_of(&c);
    if (!c.keywords.empty()) {
        err(c.line, c.col, std::format("asyncio.{}() does not take keyword arguments", member));
    }

    if (member == "run") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.run() takes exactly one coroutine");
            return const_none();
        }
        VReg coro = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncioRun);
        in.args = {coro};
        // 结果是协程的返回值，运行时按机器字返回（浮点也是位模式）。
        VReg v = def(t_unknown(), std::move(in));
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }

    if (member == "Future") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Future() takes no arguments");
        VReg f = call_runtime(RtFn::AsyncioFutureNew,
                              rt ? rt : mk_future(t_unknown()), {});
        push_temp(f);
        return f;
    }

    if (member == "wait_readable" || member == "wait_writable") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("asyncio.{}() takes exactly one fd", member));
            return const_none();
        }
        VReg fd = expr(*c.args[0]);
        fd = coerce(fd, tc_.type_of(c.args[0].get()), t_int());
        VReg w = call_runtime(RtFn::AsyncioWaitFd,
                              rt ? rt : mk_coroutine(t_none()),
                              {fd, const_int(member == "wait_writable" ? 1 : 0)});
        push_temp(w);
        return w;
    }

    if (member == "create_task") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.create_task() takes exactly one coroutine");
            return const_none();
        }
        VReg coro = expr(*c.args[0]);
        VReg t = call_runtime(RtFn::AsyncioCreateTask,
                              rt ? rt : mk_task(t_unknown()), {coro});
        push_temp(t);
        return t;
    }

    if (member == "sleep") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.sleep() takes exactly one argument");
            return const_none();
        }
        VReg secs = expr(*c.args[0]);
        secs = coerce(secs, tc_.type_of(c.args[0].get()), t_float());
        VReg w = call_runtime(RtFn::AsyncioSleep,
                              rt ? rt : mk_coroutine(t_none()), {secs});
        push_temp(w);
        return w;
    }

    if (member == "gather") {
        std::vector<VReg> vals;
        for (auto& a : c.args) {
            VReg v = expr(*a);
            TypePtr vt = vreg_ty(v);
            if (ir_is_heap(vt)) rc_inc(v);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
            vals.push_back(v);
        }
        Inst lst;
        lst.op = IrOp::MakeList;
        lst.imm = 1;
        lst.args = std::move(vals);
        VReg L = def_heap(mk_list(t_unknown()), std::move(lst));
        VReg w = call_runtime(RtFn::AsyncioGather,
                              rt ? rt : mk_coroutine(mk_list(t_unknown())), {L});
        push_temp(w);
        return w;
    }

    err(c.line, c.col,
        std::format("asyncio.{} is not supported by the code generator yet", member));
    return const_none();
}

VReg Lowerer::lower_print(Call& c) {
    // print 没有对应的运行时函数，在降级期就地展开成一串输出调用。
    // sep / end 是字符串值，直接交给 PrintStr 原样写出，不再经过格式化。
    Keyword* kw_sep = nullptr;
    Keyword* kw_end = nullptr;
    for (auto& kw : c.keywords) {
        if (kw.arg.empty()) {
            err(kw.value->line, kw.value->col, "print() does not take '**kwargs'");
            continue;
        }
        if (kw.arg != "sep" && kw.arg != "end") {
            if (kw.arg == "file" || kw.arg == "flush") {
                // 语法和类型都合法，只是这里没有重定向输出流的机制。
                // 前端放行，到这里才拒，免得 check 把真实代码误杀。
                err(kw.value->line, kw.value->col,
                    std::format("print() '{}' is not supported by the code generator yet",
                                kw.arg));
                continue;
            }
            err(kw.value->line, kw.value->col,
                std::format("print() got an unexpected keyword argument '{}'", kw.arg));
            continue;
        }
        TypePtr kt = tc_.type_of(kw.value.get());
        if (kt && kt->kind != TyKind::Str && kt->kind != TyKind::Unknown) {
            err(kw.value->line, kw.value->col,
                std::format("print() '{}' must be a string, got '{}'", kw.arg, type_name(kt)));
            continue;
        }
        (kw.arg == "sep" ? kw_sep : kw_end) = &kw;
    }

    // 求值顺序按源码从左到右：sep 第一次用到时才求值，end 在参数都输出完才求值。
    VReg sep = kNoReg;
    bool sep_ready = false;
    for (size_t i = 0; i < c.args.size(); ++i) {
        if (i) {
            if (kw_sep) {
                if (!sep_ready) {
                    sep = expr(*kw_sep->value);
                    sep_ready = true;
                }
                call_void(RtFn::PrintStr, {sep});
            } else {
                call_void(RtFn::PrintSep, {});
            }
        }
        if (c.args[i]->kind == ExprKind::Starred) {
            err(c.args[i]->line, c.args[i]->col,
                "'*args' is not supported by the code generator yet");
            continue;
        }
        TypePtr at = tc_.type_of(c.args[i].get());
        VReg v = expr(*c.args[i]);
        print_value(v, at, c.args[i]->line, c.args[i]->col);
    }
    if (kw_end) call_void(RtFn::PrintStr, {expr(*kw_end->value)});
    else call_void(RtFn::PrintEnd, {});
    return const_none();
}

VReg Lowerer::lower_method_call(Attribute& at, Call& c) {
    // 内置模块的原生函数/常量。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
            if (mod->name == "asyncio") return lower_asyncio(at.attr, c);
            auto it = mod->globals.find(at.attr);
            if (it != mod->globals.end() && it->second && it->second->kind == TyKind::Func)
                return call_native(mod->name, at.attr, c, it->second->fn);
            err(c.line, c.col,
                std::format("module attribute '{}.{}' is not callable", n->id, at.attr));
            return const_none();
        }
    }
    // m.C()：导入类的构造包装。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id)) {
            if (auto ci = mod->classes.find(at.attr); ci != mod->classes.end())
                return construct_imported(ci->second, c, mk_class(ci->second.cls.get()));
        }
    }
    // m.f()：外部全局里存的是函数闭包，加载后按普通 call 调用。
    if (auto gi = module_member(at)) {
        TypePtr ft = mod_.globals[*gi].ty;
        FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;
        if (!info) {
            err(c.line, c.col, "module attribute is not callable");
            return const_none();
        }
        VReg callee = load_global(*gi, ft);
        std::vector<VReg> args = bind_args(c, info, nullptr);
        Inst in;
        in.op = IrOp::Call;
        in.a = callee;
        in.args = std::move(args);
        TypePtr rt = info->ret ? info->ret : t_unknown();
        return def_heap(rt, std::move(in));
    }
    TypePtr raw = tc_.type_of(at.value.get());
    TypePtr recv_ty = strip_optional(raw);
    bool opt = recv_ty != raw;
    VReg recv = expr(*at.value);

    // 用户类的方法调用：从对象的 vtable 里取函数地址。槽号在编译期
    // 就定了，运行时只有一次间接跳转，不需要按名字查表。
    if (recv_ty && recv_ty->kind == TyKind::Class && recv_ty->cls) {
        if (opt) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
        auto* m = recv_ty->cls->method(at.attr);
        if (!m) {
            err(c.line, c.col,
                std::format("'{}' object has no method '{}'", recv_ty->cls->name, at.attr));
            return const_none();
        }
        if (!m->call_sig) return const_none();
        std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = recv;
        vc.imm = m->slot;
        vc.args = std::move(args);
        TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
        return def_heap(rt, std::move(vc));
    }

    // 内置类型的方法表是静态的，Optional 上没有；走到这里说明没做
    // None 检查。报错比让 py23_list_append 拿到 0 更可查。
    if (opt) {
        err(c.line, c.col,
            std::format("method '{}' called on a maybe-None value; add a None check first",
                        at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Generator) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "__next__") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.__next__() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenNext);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "send") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "generator.send() takes exactly one argument");
                return const_none();
            }
            VReg sv = expr(*c.args[0]);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenSend);
            in.args = {recv, sv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "close") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.close() takes no arguments");
            call_void(RtFn::GenClose, {recv});
            return const_none();
        }
        err(c.line, c.col, std::format("'generator' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Task) {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("task.{}() takes no arguments", at.attr));
        }
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            RtFn fn = at.attr == "done"       ? RtFn::AsyncioTaskDone
                      : at.attr == "cancel"   ? RtFn::AsyncioTaskCancel
                                              : RtFn::AsyncioTaskCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "result") {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncioTaskResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            VReg v = call_runtime(RtFn::AsyncioTaskException, mk_optional(t_str()), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'task' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Future) {
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            if (!c.args.empty())
                err(c.line, c.col, std::format("future.{}() takes no arguments", at.attr));
            RtFn fn = at.attr == "done"       ? RtFn::FutureDone
                      : at.attr == "cancel"   ? RtFn::FutureCancel
                                              : RtFn::FutureCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "set_result") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.set_result() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            TypePtr vt = vreg_ty(v);
            if (ir_is_heap(vt)) rc_inc(v);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
            call_void(RtFn::FutureSetResult, {recv, v});
            return const_none();
        }
        if (at.attr == "set_exception") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.set_exception() takes exactly one argument");
                return const_none();
            }
            VReg msg = expr(*c.args[0]);
            call_void(RtFn::FutureSetException, {recv, msg});
            return const_none();
        }
        if (at.attr == "add_done_callback") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.add_done_callback() takes exactly one argument");
                return const_none();
            }
            VReg cb = expr(*c.args[0]);
            TypePtr ct = vreg_ty(cb);
            if (ir_is_heap(ct)) rc_inc(cb);
            else if (!ct || ct->kind == TyKind::Unknown) rc_inc_unknown(cb);
            call_void(RtFn::FutureAddCallback, {recv, cb});
            return const_none();
        }
        if (at.attr == "result") {
            if (!c.args.empty()) err(c.line, c.col, "future.result() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::FutureResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            if (!c.args.empty()) err(c.line, c.col, "future.exception() takes no arguments");
            VReg v = call_runtime(RtFn::FutureException, mk_optional(t_str()), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'future' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Dict) {
        TypePtr kt = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        TypePtr vt = recv_ty->sub.size() > 1 ? recv_ty->sub[1] : t_unknown();
        if (at.attr == "get") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "dict.get() does not take keyword arguments");
                return const_none();
            }
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.get() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            if (c.args.size() == 1)
                return call_runtime(RtFn::DictGet, vt, {recv, key});
            VReg def = expr(*c.args[1]);
            def = coerce(def, tc_.type_of(c.args[1].get()), vt);
            return call_runtime(RtFn::DictGetOr, vt, {recv, key, def});
        }
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("dict.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "keys" || at.attr == "values" || at.attr == "items") {
            if (!c.args.empty()) {
                err(c.line, c.col, std::format("dict.{}() takes no arguments", at.attr));
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::List) {
                rt = at.attr == "keys" ? mk_list(kt)
                     : at.attr == "values" ? mk_list(vt)
                                           : mk_list(mk_tuple({kt, vt}));
            }
            RtFn fn = at.attr == "keys" ? RtFn::DictKeys
                      : at.attr == "values" ? RtFn::DictValues
                                            : RtFn::DictItems;
            VReg v = call_runtime(fn, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "dict.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Dict && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col,
                    "dict.update() expects a dict argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "pop") {
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.pop() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            VReg has = const_int(0);
            VReg def = const_int(0);
            if (c.args.size() == 2) {
                has = const_int(1);
                def = expr(*c.args[1]);
                def = coerce(def, tc_.type_of(c.args[1].get()), vt);
                // 运行时把默认值原样返回；补一份引用，让返回值统一按
                // 「新引用」进临时栈。
                retain_elem(def, own_kind(vt));
            }
            VReg v = call_runtime(RtFn::DictPop, vt, {recv, key, has, def});
            if (ir_is_heap(vt)) push_temp(v);
            return v;
        }
        if (at.attr == "popitem") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.popitem() takes no arguments");
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({kt, vt});
            VReg v = call_runtime(RtFn::DictPopItem, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        err(c.line, c.col,
            std::format("dict method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Set) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("set.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "add") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.add() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            retain_elem(v, own_kind(et));
            call_void(RtFn::DictSet, {recv, v, const_int(0)});
            return const_none();
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Set && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col, "set.update() expects a set argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "remove" || at.attr == "discard") {
            if (c.args.size() != 1) {
                err(c.line, c.col,
                    std::format("set.{}() takes exactly one argument", at.attr));
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            call_void(RtFn::DictRemove,
                      {recv, v, const_int(at.attr == "discard" ? 1 : 0)});
            return const_none();
        }
        if (at.attr == "pop") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.pop() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::DictPopLast, et, {recv});
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        err(c.line, c.col,
            std::format("set method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::List && at.attr == "append") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "list.append() takes exactly one argument");
            return const_none();
        }
        if (!c.keywords.empty()) {
            err(c.line, c.col, "list.append() does not take keyword arguments");
        }
        VReg v = expr(*c.args[0]);
        // 运行时不再做隐式 retain：容器元素的所有权由降级器按静态类型决定。
        retain_elem(v, list_kind(recv_ty));
        call_void(RtFn::ListAppend, {recv, v});
        return const_none();
    }
    if (recv_ty && recv_ty->kind == TyKind::List && at.attr == "pop") {
        if (!c.args.empty()) {
            err(c.line, c.col, "list.pop(index) is not supported yet");
            return const_none();
        }
        Inst len;
        len.op = IrOp::Len;
        len.a = recv;
        VReg n = def(t_int(), std::move(len));
        Inst sub;
        sub.op = IrOp::Bin;
        sub.bop = BinKind::Sub;
        sub.a = n;
        sub.b = const_int(1);
        VReg last = def(t_int(), std::move(sub));
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = recv;
        get.b = last;
        VReg v = def(et, std::move(get));
        // kind 1 的列表持有元素引用，截断时会 dec；把返回值领出来，
        // 作为新的临时值入栈，否则返回值会随截断变成悬空指针。
        if (list_kind(recv_ty) == 1 && ir_is_heap(et)) {
            rc_inc(v);
            push_temp(v);
        }
        Inst setlen;
        setlen.op = IrOp::CallRt;
        setlen.imm = static_cast<int64_t>(RtFn::ListSetLen);
        setlen.args = {recv, last};
        emit(std::move(setlen));
        return v;
    }

    err(c.line, c.col, std::format("method '{}' is not supported by the code generator yet",
                                   at.attr));
    return const_none();
}


}  // namespace py23
