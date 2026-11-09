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
    ++side_effect_seq_;
    // 内建函数
    if (c.func->kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(*c.func);
        // 异常构造器：内置异常名和自定义异常类都产生异常值
        bool exc_ctor = TypeChecker::is_exception_type(n.id) && is_builtin(n.id);
        if (!exc_ctor) {
            auto eit = classes_.find(n.id);
            if (eit != classes_.end() && eit->second->is_exception_class) exc_ctor = true;
        }
        if (exc_ctor) {
            if (!c.keywords.empty())
                err(c.line, c.col, "exception constructor does not take keyword arguments");
            VReg type = const_str(n.id);
            VReg msg = const_int(0);
            if (!c.args.empty()) {
                TypePtr mt = tc_.type_of(c.args[0].get());
                if (mt && (mt->kind == TyKind::List || mt->kind == TyKind::Tuple ||
                           mt->kind == TyKind::Dict || mt->kind == TyKind::Set ||
                           mt->kind == TyKind::Class || mt->kind == TyKind::Func)) {
                    err(c.args[0]->line, c.args[0]->col,
                        "exception message must be a string or scalar");
                }
                VReg raw = expr(*c.args[0]);
                msg = convert_to(raw, mt, t_str());
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ExcValueNew);
            in.args = {type, msg};
            return def_heap(mk_exception(n.id), std::move(in));
        }
        if (is_builtin(n.id)) {
            static const std::unordered_set<std::string> kBuiltins = {
                "print", "len", "int", "float", "str", "bool", "range", "next",
                "abs", "min", "max", "sum", "pow", "any", "all", "complex",
                "ord", "chr", "hex", "bin", "oct", "round", "divmod", "repr", "format",
            };
            if (kBuiltins.count(n.id)) return lower_builtin_call(n.id, c);
            // 名字能解析但语义没实现的内置：以前会退化成调用空指针直接
            // 段错误，这里明确报“未支持”，至少是可用诊断。
            if (TypeChecker::is_builtin_name(n.id)) {
                err(n.line, n.col,
                    std::format("builtin '{}' is not supported by the code generator yet",
                                n.id));
                return const_none();
            }
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

    if (name == "abs") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "abs() takes exactly one argument");
            return const_none();
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::MathFabs, t_float(), {v});
        if (t && t->kind == TyKind::Complex)
            return call_runtime(RtFn::ComplexAbs, t_float(), {v});
        return call_runtime(RtFn::AbsI64, t_int(), {v});
    }

    if (name == "min" || name == "max") return lower_minmax(c, name == "min");
    if (name == "sum") return lower_sum(c);
    if (name == "any" || name == "all") return lower_anyall(c, name == "any");
    if (name == "complex") {
        if (c.args.size() > 2) {
            err(c.line, c.col, "complex() takes at most two arguments");
            return const_none();
        }
        VReg re = const_float(0.0);
        VReg im = const_float(0.0);
        if (!c.args.empty()) {
            TypePtr t0 = tc_.type_of(c.args[0].get());
            VReg v0 = expr(*c.args[0]);
            if (t0 && t0->kind == TyKind::Complex && c.args.size() == 1) return v0;
            re = (t0 && t0->kind == TyKind::Float) ? v0 : convert_to(v0, t0, t_float());
        }
        if (c.args.size() == 2) {
            TypePtr t1 = tc_.type_of(c.args[1].get());
            VReg v1 = expr(*c.args[1]);
            im = (t1 && t1->kind == TyKind::Float) ? v1 : convert_to(v1, t1, t_float());
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexNew);
        in.args = {re, im};
        return def_heap(t_complex(), std::move(in));
    }
    if (name == "pow") {
        if (c.args.size() != 2 && c.args.size() != 3) {
            err(c.line, c.col, "pow() takes 2 or 3 arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind == TyKind::Unknown) ty = t_int();
        if (c.args.size() == 3) {
            VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            VReg m = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_int());
            Inst pw;
            pw.op = IrOp::Bin;
            pw.bop = BinKind::Pow;
            pw.a = a;
            pw.b = b;
            VReg p = def(t_int(), std::move(pw));
            Inst md;
            md.op = IrOp::Bin;
            md.bop = BinKind::Mod;
            md.a = p;
            md.b = m;
            return def(t_int(), std::move(md));
        }
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), ty);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), ty);
        Inst in;
        in.op = IrOp::Bin;
        in.bop = BinKind::Pow;
        in.a = a;
        in.b = b;
        return def(ty, std::move(in));
    }

    if (name == "ord") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "ord() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Unknown)) {
            err(c.line, c.col, std::format("ord() expects a string, got '{}'", type_name(t)));
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        return call_runtime(RtFn::Ord, t_int(), {v});
    }

    if (name == "chr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "chr() takes exactly one argument");
            return const_str("");
        }
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::Chr, t_str(), {v});
        push_temp(s);
        return s;
    }

    if (name == "hex" || name == "bin" || name == "oct") {
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes exactly one argument", name));
            return const_str("");
        }
        int64_t base = name == "hex" ? 16 : name == "bin" ? 2 : 8;
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::IntToBase, t_str(), {v, const_int(base)});
        push_temp(s);
        return s;
    }

    if (name == "round") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col, "round() takes 1 or 2 arguments");
            return const_int(0);
        }
        TypePtr at = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (at && at->kind == TyKind::Float) {
            VReg nd = c.args.size() == 2
                          ? coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int())
                          : const_int(0);
            VReg r = call_runtime(RtFn::RoundF64, t_float(), {v, nd});
            // round(x) 返回 int；round(x, n) 仍返回 float。
            if (c.args.size() == 1) return convert_to(r, t_float(), t_int());
            return r;
        }
        // 整数输入：单参就是它自己；带负 ndigits 要按 10 的幂取整。
        if (c.args.size() == 2 && at &&
            (at->kind == TyKind::Int || at->kind == TyKind::Bool)) {
            VReg nd = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::RoundI64, t_int(), {v, nd});
        }
        return v;
    }

    if (name == "divmod") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "divmod() takes exactly two arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        TypePtr et = t_int();
        if (ty && ty->kind == TyKind::Tuple && !ty->sub.empty()) et = ty->sub[0];
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), et);
        Inst q;
        q.op = IrOp::Bin;
        q.bop = BinKind::FloorDiv;
        q.a = a;
        q.b = b;
        VReg qv = def(et, std::move(q));
        Inst m;
        m.op = IrOp::Bin;
        m.bop = BinKind::Mod;
        m.a = a;
        m.b = b;
        VReg mv = def(et, std::move(m));
        Inst mt;
        mt.op = IrOp::MakeTuple;
        mt.imm = !et ? 2 : et->kind == TyKind::Unknown ? 2 : (ir_is_heap(et) ? 1 : 0);
        mt.args = {qv, mv};
        return def_heap(ty && ty->kind == TyKind::Tuple ? ty : mk_tuple({et, et}),
                        std::move(mt));
    }

    if (name == "repr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "repr() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Str) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ReprStr);
            in.args = {v};
            return def_heap(t_str(), std::move(in));
        }
        if (t && (t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                  t->kind == TyKind::Float || t->kind == TyKind::Complex ||
                  t->kind == TyKind::None))
            return convert_to(v, t, t_str());
        err(c.line, c.col,
            std::format("repr() of '{}' is not supported by the code generator yet",
                        type_name(t)));
        return const_str("");
    }

    if (name == "format") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "format() takes exactly two arguments");
            return const_str("");
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        RtFn fn = RtFn::FmtI64;
        if (vt && vt->kind == TyKind::Float) fn = RtFn::FmtF64;
        else if (vt && vt->kind == TyKind::Bool) fn = RtFn::FmtBool;
        else if (vt && vt->kind == TyKind::Str) fn = RtFn::FmtStr;
        else if (vt && vt->kind == TyKind::Complex) fn = RtFn::FmtComplex;
        else if (!vt || (vt->kind != TyKind::Int && vt->kind != TyKind::Unknown)) {
            err(c.line, c.col,
                std::format("format() of '{}' is not supported yet", type_name(vt)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg spec = expr(*c.args[1]);
        if (spec == kNoReg) return const_str("");
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {v, spec};
        return def_heap(t_str(), std::move(in));
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
        // int(str, base)
        if (name == "int" && c.args.size() == 2) {
            TypePtr st = tc_.type_of(c.args[0].get());
            VReg sv = expr(*c.args[0]);
            if (!st || st->kind != TyKind::Str) {
                err(c.line, c.col, "int(x, base) requires a string first argument");
                return const_int(0);
            }
            VReg bv = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::StrToI64, t_int(), {sv, bv});
        }
        if (c.args.size() > 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr from = tc_.type_of(c.args[0].get());
        // 字符串解析：以前走 Convert 会把字符串指针直接当数值，静默算错。
        if (name == "int" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToI64, t_int(), {v, const_int(10)});
        if (name == "float" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToF64, t_float(), {v});
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Exception) {
            VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
            push_temp(s);
            return s;
        }
        return convert_to(v, from, to);
    }

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

// min()/max()：多实参逐个比较；单实参按容器迭代。空序列抛 ValueError。
VReg Lowerer::lower_minmax(Call& c, bool is_min) {
    if (c.args.empty()) {
        err(c.line, c.col,
            std::format("{}() expects at least one argument", is_min ? "min" : "max"));
        return const_none();
    }
    TypePtr acc_ty = tc_.type_of(&c);
    if (!acc_ty || acc_ty->kind == TyKind::Unknown) acc_ty = t_unknown();
    else if (!is_num(acc_ty) && acc_ty->kind != TyKind::Str) {
        err(c.line, c.col,
            std::format("{}() arguments must be numbers or strings", is_min ? "min" : "max"));
        return const_none();
    }
    CmpKind cop = is_min ? CmpKind::Lt : CmpKind::Gt;
    int best = new_temp_slot(acc_ty);

    if (c.args.size() == 1) {
        TypePtr seq_ty = tc_.type_of(c.args[0].get());
        bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                             seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Dict ||
                             seq_ty->kind == TyKind::Set);
        if (!ok) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("cannot iterate over '{}' in {}()", type_name(seq_ty),
                            is_min ? "min" : "max"));
            return const_none();
        }
        TypePtr elem_ty = t_unknown();
        if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
        else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];

        // SIMD min/max：int/bool 列表用 AVX2 归约；float 仍走标量以保持
        // NaN 下的比较顺序语义。
        if (opts_.opt_level >= 2 &&
            (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
            (elem_ty->kind == TyKind::Int || elem_ty->kind == TyKind::Bool) &&
            (acc_ty->kind == TyKind::Int || acc_ty->kind == TyKind::Bool)) {
            VReg seq = expr(*c.args[0]);
            return call_runtime(is_min ? RtFn::ListMinI64 : RtFn::ListMaxI64, acc_ty,
                                {seq});
        }

        VReg seq = expr(*c.args[0]);
        int seq_slot = new_temp_slot(seq_ty);
        store_slot(seq_slot, seq);
        Inst ln;
        ln.op = IrOp::Len;
        ln.a = seq;
        VReg n = def(t_int(), std::move(ln));
        int n_slot = new_temp_slot(t_int());
        store_slot(n_slot, n);

        int empty_b = new_block("minmax.empty");
        int nonempty_b = new_block("minmax.nonempty");
        Inst ce;
        ce.op = IrOp::Cmp;
        ce.cop = CmpKind::Eq;
        ce.a = n;
        ce.b = const_int(0);
        VReg is_empty = def(t_bool(), std::move(ce));
        Inst be;
        be.op = IrOp::Branch;
        be.a = is_empty;
        be.target = empty_b;
        be.target2 = nonempty_b;
        emit(std::move(be));
        switch_to(empty_b);
        call_void(RtFn::Raise, {const_str("ValueError"),
                                const_str(is_min ? "min() arg is an empty sequence"
                                                 : "max() arg is an empty sequence")});
        switch_to(nonempty_b);

        auto elem_at = [&](VReg idx) -> VReg {
            VReg sv = load_slot(seq_slot, seq_ty);
            if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
                return call_runtime(RtFn::DictKeyAt, elem_ty, {sv, idx});
            Inst get;
            get.op = (opts_.opt_level >= 1 &&
                      (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                         ? IrOp::IndexLoadUnchecked
                         : IrOp::IndexLoad;
            get.a = sv;
            get.b = idx;
            return seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                               : def(elem_ty, std::move(get));
        };

        VReg first = coerce(elem_at(const_int(0)), elem_ty, acc_ty);
        store_slot(best, first);
        int idx = new_temp_slot(t_int());
        store_slot(idx, const_int(1));

        int cond = new_block("minmax.cond");
        int body = new_block("minmax.body");
        int choose = new_block("minmax.choose");
        int next = new_block("minmax.next");
        int end = new_block("minmax.end");
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
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        VReg ev = coerce(elem_at(load_slot(idx, t_int())), elem_ty, acc_ty);
        Inst cmpb;
        cmpb.op = IrOp::Cmp;
        cmpb.cop = cop;
        cmpb.a = ev;
        cmpb.b = load_slot(best, acc_ty);
        VReg take = def(t_bool(), std::move(cmpb));
        Inst bt;
        bt.op = IrOp::Branch;
        bt.a = take;
        bt.target = choose;
        bt.target2 = next;
        emit(std::move(bt));
        switch_to(choose);
        store_slot(best, ev);
        Inst jc;
        jc.op = IrOp::Jump;
        jc.target = next;
        emit(std::move(jc));

        switch_to(next);
        Inst add;
        add.op = IrOp::Bin;
        add.bop = BinKind::Add;
        add.a = load_slot(idx, t_int());
        add.b = const_int(1);
        store_slot(idx, def(t_int(), std::move(add)));
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));

        switch_to(end);
        return load_slot(best, acc_ty);
    }

    VReg v0 = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), acc_ty);
    store_slot(best, v0);
    for (size_t i = 1; i < c.args.size(); ++i) {
        VReg v = coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), acc_ty);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = cop;
        cmp.a = v;
        cmp.b = load_slot(best, acc_ty);
        VReg take = def(t_bool(), std::move(cmp));
        int choose = new_block("minmax.choose");
        int next = new_block("minmax.next");
        Inst br;
        br.op = IrOp::Branch;
        br.a = take;
        br.target = choose;
        br.target2 = next;
        emit(std::move(br));
        switch_to(choose);
        store_slot(best, v);
        Inst j;
        j.op = IrOp::Jump;
        j.target = next;
        emit(std::move(j));
        switch_to(next);
    }
    return load_slot(best, acc_ty);
}

// sum(iterable, start=0)：计数器循环 + 加法，累加器落槽。
VReg Lowerer::lower_sum(Call& c) {
    if (c.args.empty() || c.args.size() > 2) {
        err(c.line, c.col, "sum() takes one or two arguments");
        return const_none();
    }
    TypePtr acc_ty = tc_.type_of(&c);
    if (!acc_ty || acc_ty->kind == TyKind::Unknown) acc_ty = t_unknown();
    else if (!is_num(acc_ty)) {
        err(c.line, c.col, std::format("sum() over non-numeric '{}'", type_name(acc_ty)));
        return const_none();
    }
    TypePtr seq_ty = tc_.type_of(c.args[0].get());
    bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                         seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Dict ||
                         seq_ty->kind == TyKind::Set);
    if (!ok) {
        err(c.args[0]->line, c.args[0]->col,
            std::format("cannot iterate over '{}' in sum()", type_name(seq_ty)));
        return const_none();
    }
    TypePtr elem_ty = t_unknown();
    if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
    else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];

    // SIMD 归约：只覆盖同域 list/tuple[int|bool] 与 float；混合数值
    // （如 int 列表 + float start）继续走标量循环保持转换语义。
    if (opts_.opt_level >= 2 &&
        (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
        !seq_ty->sub.empty() && acc_ty) {
        bool ints = (elem_ty->kind == TyKind::Int || elem_ty->kind == TyKind::Bool) &&
                    (acc_ty->kind == TyKind::Int || acc_ty->kind == TyKind::Bool);
        bool floats = opts_.fast_math && elem_ty->kind == TyKind::Float &&
                      acc_ty->kind == TyKind::Float;
        if (ints || floats) {
            VReg seq = expr(*c.args[0]);
            VReg start = floats ? const_float(0.0) : const_int(0);
            if (c.args.size() == 2)
                start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
            return call_runtime(ints ? RtFn::ListSumI64 : RtFn::ListSumF64, acc_ty,
                                {seq, start});
        }
    }

    VReg seq = expr(*c.args[0]);
    int seq_slot = new_temp_slot(seq_ty);
    store_slot(seq_slot, seq);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, n);
    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));

    VReg start = acc_ty && acc_ty->kind == TyKind::Float ? const_float(0.0) : const_int(0);
    if (c.args.size() == 2)
        start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
    int acc = new_temp_slot(acc_ty);
    store_slot(acc, start);

    int cond = new_block("sum.cond");
    int body = new_block("sum.body");
    int incr = new_block("sum.incr");
    int end = new_block("sum.end");
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
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    VReg sv = load_slot(seq_slot, seq_ty);
    VReg iv = load_slot(idx, t_int());
    VReg ev;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
        ev = call_runtime(RtFn::DictKeyAt, elem_ty, {sv, iv});
    else {
        Inst get;
        get.op = (opts_.opt_level >= 1 &&
                  (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                     ? IrOp::IndexLoadUnchecked
                     : IrOp::IndexLoad;
        get.a = sv;
        get.b = iv;
        ev = seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                         : def(elem_ty, std::move(get));
    }
    ev = coerce(ev, elem_ty, acc_ty);
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(acc, acc_ty);
    add.b = ev;
    store_slot(acc, def(acc_ty, std::move(add)));
    Inst jin;
    jin.op = IrOp::Jump;
    jin.target = incr;
    emit(std::move(jin));

    switch_to(incr);
    Inst add2;
    add2.op = IrOp::Bin;
    add2.bop = BinKind::Add;
    add2.a = load_slot(idx, t_int());
    add2.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add2)));
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(acc, acc_ty);
}


// any()/all()：短路迭代。any 遇到真值返回 True，all 遇到假值返回 False。
VReg Lowerer::lower_anyall(Call& c, bool is_any) {
    if (c.args.size() != 1) {
        err(c.line, c.col,
            std::format("{}() takes exactly one argument", is_any ? "any" : "all"));
        return const_bool(false);
    }
    TypePtr seq_ty = tc_.type_of(c.args[0].get());
    bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                         seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Dict ||
                         seq_ty->kind == TyKind::Set);
    if (!ok) {
        err(c.args[0]->line, c.args[0]->col,
            std::format("cannot iterate over '{}' in {}()", type_name(seq_ty),
                        is_any ? "any" : "all"));
        return const_bool(false);
    }
    TypePtr elem_ty = t_unknown();
    if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
    else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];

    VReg seq = expr(*c.args[0]);
    int seq_slot = new_temp_slot(seq_ty);
    store_slot(seq_slot, seq);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, n);
    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int result = new_temp_slot(t_bool());
    store_slot(result, const_bool(!is_any));

    int cond = new_block("anyall.cond");
    int body = new_block("anyall.body");
    int incr = new_block("anyall.incr");
    int stop = new_block("anyall.stop");
    int end = new_block("anyall.end");
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
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    VReg sv = load_slot(seq_slot, seq_ty);
    VReg iv = load_slot(idx, t_int());
    VReg ev;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
        ev = call_runtime(RtFn::DictKeyAt, elem_ty, {sv, iv});
    else {
        Inst get;
        get.op = (opts_.opt_level >= 1 &&
                  (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                     ? IrOp::IndexLoadUnchecked
                     : IrOp::IndexLoad;
        get.a = sv;
        get.b = iv;
        ev = seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                         : def(elem_ty, std::move(get));
    }
    Inst ib;
    ib.op = IrOp::Branch;
    ib.a = ev;
    ib.target = is_any ? stop : incr;
    ib.target2 = is_any ? incr : stop;
    emit(std::move(ib));

    switch_to(stop);
    store_slot(result, const_bool(is_any));
    Inst js;
    js.op = IrOp::Jump;
    js.target = end;
    emit(std::move(js));

    switch_to(incr);
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(result, t_bool());
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

    // sep 在参数前求值一次并落槽；end 在参数都输出完才求值。
    int sep_slot = -1;
    if (kw_sep) {
        VReg s = expr(*kw_sep->value);
        sep_slot = new_temp_slot(t_str());
        store_slot(sep_slot, s);
    }
    int first = new_temp_slot(t_int());
    store_slot(first, const_int(1));

    auto before_item = [&]() {
        int need = new_block("print.sep");
        int done = new_block("print.item");
        Inst br;
        br.op = IrOp::Branch;
        br.a = load_slot(first, t_int());
        br.target = done;
        br.target2 = need;
        emit(std::move(br));
        switch_to(need);
        if (kw_sep) {
            call_void(RtFn::PrintStr, {load_slot(sep_slot, t_str())});
        } else {
            call_void(RtFn::PrintSep, {});
        }
        Inst j;
        j.op = IrOp::Jump;
        j.target = done;
        emit(std::move(j));
        switch_to(done);
    };
    auto after_item = [&]() { store_slot(first, const_int(0)); };

    for (auto& arg : c.args) {
        if (arg->kind == ExprKind::Starred) {
            auto& st = static_cast<Starred&>(*arg);
            TypePtr sty = tc_.type_of(st.value.get());
            bool ok = sty && (sty->kind == TyKind::List || sty->kind == TyKind::Tuple ||
                              sty->kind == TyKind::Str || sty->kind == TyKind::Dict ||
                              sty->kind == TyKind::Set);
            if (!ok) {
                err(arg->line, arg->col,
                    std::format("cannot expand '{}' in print()", type_name(sty)));
                continue;
            }
            TypePtr ety = t_unknown();
            if (sty->kind == TyKind::Str) ety = t_str();
            else if (!sty->sub.empty()) ety = sty->sub[0];

            VReg seq = expr(*st.value);
            int seq_slot = new_temp_slot(sty);
            store_slot(seq_slot, seq);
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = seq;
            VReg n = def(t_int(), std::move(ln));
            int n_slot = new_temp_slot(t_int());
            store_slot(n_slot, n);
            int idx = new_temp_slot(t_int());
            store_slot(idx, const_int(0));

            int cond = new_block("print.star.cond");
            int body = new_block("print.star.body");
            int end = new_block("print.star.end");
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
            br.target2 = end;
            emit(std::move(br));

            switch_to(body);
            before_item();
            VReg sv = load_slot(seq_slot, sty);
            VReg iv = load_slot(idx, t_int());
            VReg ev;
            if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set)
                ev = call_runtime(RtFn::DictKeyAt, ety, {sv, iv});
            else {
                Inst get;
                get.op = (opts_.opt_level >= 1 &&
                          (sty->kind == TyKind::List || sty->kind == TyKind::Tuple))
                             ? IrOp::IndexLoadUnchecked
                             : IrOp::IndexLoad;
                get.a = sv;
                get.b = iv;
                ev = sty->kind == TyKind::Str ? def_heap(ety, std::move(get))
                                              : def(ety, std::move(get));
            }
            print_value(ev, ety, arg->line, arg->col);
            after_item();
            Inst add;
            add.op = IrOp::Bin;
            add.bop = BinKind::Add;
            add.a = load_slot(idx, t_int());
            add.b = const_int(1);
            store_slot(idx, def(t_int(), std::move(add)));
            Inst jb;
            jb.op = IrOp::Jump;
            jb.target = cond;
            emit(std::move(jb));
            switch_to(end);
            continue;
        }
        before_item();
        TypePtr at = tc_.type_of(arg.get());
        VReg v = expr(*arg);
        print_value(v, at, arg->line, arg->col);
        after_item();
    }
    if (kw_end) call_void(RtFn::PrintStr, {expr(*kw_end->value)});
    else call_void(RtFn::PrintEnd, {});
    return const_none();
}

VReg Lowerer::lower_super_call(Attribute& at, Call& c) {
    auto fit = fn_owner_class_.find(st_->fn);
    const ClassInfo* owner = fit == fn_owner_class_.end() ? nullptr : fit->second;
    if (!owner) {
        err(c.line, c.col, "super() is only supported directly inside a method body");
        return const_none();
    }
    if (st_->fn->params.empty()) {
        err(c.line, c.col, "super() requires a method receiver");
        return const_none();
    }
    const MethodInfo* m = nullptr;
    bool after = false;
    for (const ClassInfo* k : owner->mro) {
        if (!after) {
            if (k == owner) after = true;
            continue;
        }
        if (auto* km = k->own_method(at.attr)) {
            m = km;
            break;
        }
    }
    if (!m) {
        err(c.line, c.col,
            std::format("super() has no method '{}' after class '{}'", at.attr, owner->name));
        return const_none();
    }
    if (m->kind != MethodKind::Normal) {
        err(c.line, c.col,
            std::format("super().{} is not a plain method (decorators on super targets are "
                        "not supported yet)", at.attr));
        return const_none();
    }
    if (!m->call_sig) {
        err(c.line, c.col, std::format("super().{} has no known signature", at.attr));
        return const_none();
    }
    auto cid = class_ids_.find(owner);
    if (cid == class_ids_.end()) {
        err(c.line, c.col, "super() is not available for this class");
        return const_none();
    }
    std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
    const IRParam& selfp = st_->fn->params.front();
    VReg self = load_slot(selfp.slot, selfp.ty);
    Inst sv;
    sv.op = IrOp::SuperVCall;
    sv.a = self;
    sv.idx = cid->second;
    sv.imm = m->slot;
    sv.b = const_str(at.attr);
    sv.args = std::move(args);
    TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
    return def_heap(rt, std::move(sv));
}

VReg Lowerer::lower_method_call(Attribute& at, Call& c) {
    ++side_effect_seq_;
    // super().m(...)：沿运行期 MRO 找下一个实现。
    if (auto* sup = dynamic_cast<Call*>(at.value.get())) {
        if (auto* sn = dynamic_cast<Name*>(sup->func.get());
            sn && sn->id == "super" && !is_bound_here("super")) {
            if (!sup->args.empty() || !sup->keywords.empty()) {
                err(c.line, c.col, "only zero-argument super() is supported");
                return const_none();
            }
            return lower_super_call(at, c);
        }
    }
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
    // C.f(...)：类名接收者的静态方法直接调用；普通方法需要实例。
    if (auto* n = dynamic_cast<Name*>(at.value.get());
        n && !is_bound_here(n->id) && !imported_classes_.count(n->id)) {
        auto cit = classes_.find(n->id);
        if (cit != classes_.end() && cit->second && !cit->second->is_exception_class) {
            const ClassInfo* cls = cit->second.get();
            const MethodInfo* m = cls->method(at.attr);
            if (!m) {
                err(c.line, c.col,
                    std::format("'{}' has no method '{}'", cls->name, at.attr));
                return const_none();
            }
            if (m->kind != MethodKind::Static) {
                err(c.line, c.col,
                    std::format("'{}.{}' is an instance method; call it on an instance",
                                cls->name, at.attr));
                return const_none();
            }
            if (!m->call_sig) return const_none();
            const ClassInfo* owner = cls->method_owner(at.attr);
            auto cid = owner ? class_ids_.find(owner) : class_ids_.end();
            if (cid == class_ids_.end()) {
                err(c.line, c.col,
                    std::format("static method '{}.{}' is not available here", cls->name,
                                at.attr));
                return const_none();
            }
            const auto& vt = mod_.classes[cid->second].vtable;
            if (m->slot < 0 || m->slot >= static_cast<int>(vt.size()) || vt[m->slot] < 0) {
                err(c.line, c.col,
                    std::format("static method '{}.{}' was not compiled", cls->name, at.attr));
                return const_none();
            }
            std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
            Inst cd;
            cd.op = IrOp::CallDirect;
            cd.idx = vt[m->slot];
            cd.args = std::move(args);
            TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
            return def_heap(rt, std::move(cd));
        }
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
        vc.op = m->kind == MethodKind::Static ? IrOp::VCallStatic : IrOp::VCall;
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

    if (recv_ty && recv_ty->kind == TyKind::AsyncGenerator) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "__anext__") {
            if (!c.args.empty())
                err(c.line, c.col, "async generator.__anext__() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
            in.args = {recv};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "asend") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "async generator.asend() takes exactly one argument");
                return const_none();
            }
            VReg sv = expr(*c.args[0]);
            TypePtr vt = vreg_ty(sv);
            if (ir_is_heap(vt)) rc_inc(sv);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(sv);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAsend);
            in.args = {recv, sv};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "athrow") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "async generator.athrow() takes exactly one argument");
                return const_none();
            }
            VReg ev = expr(*c.args[0]);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAthrow);
            in.args = {recv, ev};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "aclose") {
            if (!c.args.empty())
                err(c.line, c.col, "async generator.aclose() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAClose);
            in.args = {recv};
            return def_heap(mk_coroutine(t_none()), std::move(in));
        }
        err(c.line, c.col,
            std::format("'async generator' object has no method '{}'", at.attr));
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
            VReg v = call_runtime(RtFn::AsyncioTaskException,
                                  mk_optional(mk_exception("Exception")), {recv});
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
            TypePtr mt = vreg_ty(msg);
            if (mt && mt->kind == TyKind::Exception)
                call_void(RtFn::FutureSetExceptionValue, {recv, msg});
            else
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
            VReg v = call_runtime(RtFn::FutureException,
                                  mk_optional(mk_exception("Exception")), {recv});
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
