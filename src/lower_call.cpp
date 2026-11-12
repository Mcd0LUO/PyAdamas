// lower_call.cpp —— 调用降级：参数绑定、内置/方法/asyncio
#include "lower_impl.h"

#include <limits>

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
                               {const_int(1), const_int(1), const_int(kown),
                                const_int(-1), const_int(-1)});
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
        if (args[i] == kNoReg) continue;
        TypePtr from = vreg_ty(args[i]);
        if (bad_elem(from, want)) {
            const std::string& pname =
                i < npos ? info->pos[i].name : info->kwonly[i - npos].name;
            err(c.line, c.col,
                std::format("'{}' cannot be passed to parameter '{}' of type '{}': element "
                            "type conversion is not supported by the code generator yet",
                            type_name(from), pname, type_name(want)));
        }
        args[i] = coerce(args[i], from, want);
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

    // 隐式字段遮蔽了同名类属性（方法里写了 self.x = ...，而 x 是类属性）：
    // 构造时先把类属性的当前值拷进字段，这样「还没赋值就读 self.x」看到的
    // 是类属性默认值，而不是 0 / None。
    for (const ClassInfo* k = cls.get(); k; k = k->base) {
        for (const FieldInfo& f : k->fields) {
            if (!f.implicit) continue;
            const ClassAttrInfo* ca = cls->class_attr(f.name);
            if (!ca) continue;
            int off = static_cast<int>(8 * (1 + cls->field_index(f.name)));
            VReg v = class_attr_load(cls.get(), ca);
            v = coerce(v, vreg_ty(v), f.ty);
            store_field(f.ty, obj, off, v);
        }
    }

    if (cls->has_init) {
        if (auto* im = cls->method("__init__"); im && im->call_sig) {
            std::vector<VReg> args = bind_args(c, im->call_sig, im->def);
            // 刚构造出来的对象类型是确定的：本模块的类可以直接调用
            // 它 vtable 槽里的实现（子类构造函数同理，槽已按 MRO 解析）。
            bool direct = false;
            if (auto cid = class_ids_.find(cls.get()); cid != class_ids_.end() &&
                im->slot >= 0) {
                const auto& vt = mod_.classes[static_cast<size_t>(cid->second)].vtable;
                if (im->slot < static_cast<int>(vt.size())) {
                    int fidx = vt[static_cast<size_t>(im->slot)];
                    const IRFunction* callee =
                        (fidx >= 0 && fidx < static_cast<int>(mod_.funcs.size()))
                            ? mod_.funcs[static_cast<size_t>(fidx)].get()
                            : nullptr;
                    if (callee && callee->env_slot < 0) {
                        Inst cd;
                        cd.op = IrOp::CallDirect;
                        cd.idx = fidx;
                        cd.args.push_back(obj);
                        for (VReg a : args) cd.args.push_back(a);
                        TypePtr rt = im->call_sig->ret ? im->call_sig->ret : t_none();
                        def_heap(rt, std::move(cd));
                        direct = true;
                    }
                }
            }
            if (!direct) {
                Inst vc;
                vc.op = IrOp::VCall;
                vc.a = obj;
                vc.imm = im->slot;
                vc.args = std::move(args);
                TypePtr rt = im->call_sig->ret ? im->call_sig->ret : t_none();
                def_heap(rt, std::move(vc));
            }
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
                "list", "tuple", "dict", "set", "sorted", "reversed", "enumerate", "zip",
                "open", "input", "iter", "isinstance", "getattr", "setattr", "hasattr",
                "callable", "issubclass", "hash", "type", "id", "frozenset",
                "slice", "vars", "dir",
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
        // @classmethod 里的 cls(...)：静态绑定到所属类，等价于类名构造。
        if (!st_->class_self_name.empty() && n.id == st_->class_self_name &&
            st_->class_self_ty) {
            auto it = classes_.find(st_->class_self_ty->name);
            if (it != classes_.end()) return lower_construct(c, it->second);
        }
        // 类名直接调用 = 构造。类名不是可求值的表达式，必须在这里拦，
        // 否则会退化成 LoadGlobal 读到一个空槽。
        if (auto it = classes_.find(n.id); it != classes_.end())
            return lower_construct(c, it->second);
        if (auto it = imported_classes_.find(n.id); it != imported_classes_.end())
            return construct_imported(it->second, c, mk_class(it->second.cls.get()));
        if (auto it = native_aliases_.find(n.id); it != native_aliases_.end()) {
            if (it->second.module == "asyncio") return lower_asyncio(it->second.member, c);
            if (it->second.module == "itertools")
                return lower_itertools(it->second.member, c);
            if (it->second.module == "collections")
                return lower_collections(it->second.member, c);
            if (it->second.module == "functools")
                return lower_functools(it->second.member, c);
            if (it->second.module == "datetime")
                return lower_datetime(it->second.member, c);
            if (it->second.module == "pathlib")
                return lower_pathlib(it->second.member, c);
            if (it->second.module == "json")
                return lower_json(it->second.member, c);
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

VReg Lowerer::seq_as_list(Expr* e, TypePtr& elem_out) {
    VReg x = expr(*e);
    TypePtr xt = tc_.type_of(e);
    elem_out = t_unknown();
    if (!xt) return x;
    switch (xt->kind) {
        case TyKind::Str: {
            elem_out = t_str();
            VReg l = call_runtime(RtFn::StrToList, mk_list(t_str()), {x});
            push_temp(l);
            return l;
        }
        case TyKind::Dict:
        case TyKind::Set: {
            elem_out = xt->sub.empty() ? t_unknown() : xt->sub[0];
            VReg l = call_runtime(RtFn::DictKeys, mk_list(elem_out), {x});
            push_temp(l);
            return l;
        }
        case TyKind::Range: {
            // 需要真列表时（list(range(n)) / sorted(range(n))）才物化
            elem_out = xt->sub.empty() ? t_unknown() : xt->sub[0];
            VReg l = call_runtime(RtFn::RangeToList, mk_list(elem_out), {x});
            push_temp(l);
            return l;
        }
        case TyKind::List:
        case TyKind::Tuple:
            elem_out = xt->sub.empty() ? t_unknown() : xt->sub[0];
            return x;
        case TyKind::File: {
            elem_out = t_str();
            VReg l = call_runtime(RtFn::FileReadLines, mk_list(t_str()), {x, const_int(-1)});
            push_temp(l);
            return l;
        }
        case TyKind::Generator: {
            elem_out = xt->sub.empty() ? t_unknown() : xt->sub[0];
            return gen_to_list(x, xt);
        }
        default:
            return x;
    }
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

    if (member == "Event") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Event() takes no arguments");
        VReg ev = call_runtime(RtFn::EventNew, rt ? rt : mk_async_event(), {});
        push_temp(ev);
        return ev;
    }

    if (member == "Lock") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Lock() takes no arguments");
        VReg lk = call_runtime(RtFn::LockNew, rt ? rt : mk_async_lock(), {});
        push_temp(lk);
        return lk;
    }

    if (member == "Queue") {
        if (c.args.size() > 1)
            err(c.line, c.col, "asyncio.Queue() takes at most one argument");
        VReg maxsize = const_int(0);
        if (!c.args.empty())
            maxsize = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg q = call_runtime(RtFn::QueueNew, rt ? rt : mk_async_queue(t_unknown()),
                              {maxsize});
        push_temp(q);
        return q;
    }

    if (member == "Semaphore") {
        if (c.args.size() > 1)
            err(c.line, c.col, "asyncio.Semaphore() takes at most one argument");
        VReg value = const_int(1);
        if (!c.args.empty())
            value = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::SemNew, rt ? rt : mk_async_semaphore(), {value});
        push_temp(s);
        return s;
    }

    if (member == "TaskGroup") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.TaskGroup() takes no arguments");
        VReg g = call_runtime(RtFn::TaskGroupNew, rt ? rt : mk_async_taskgroup(), {});
        push_temp(g);
        return g;
    }

    if (member == "timeout") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.timeout() takes exactly one argument");
            return const_none();
        }
        VReg secs = expr(*c.args[0]);
        secs = coerce(secs, tc_.type_of(c.args[0].get()), t_float());
        VReg to = call_runtime(RtFn::AsyncTimeoutNew, rt ? rt : mk_async_timeout(), {secs});
        push_temp(to);
        return to;
    }

    if (member == "wait_for") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "asyncio.wait_for() takes exactly two arguments");
            return const_none();
        }
        VReg aw = expr(*c.args[0]);
        VReg secs = expr(*c.args[1]);
        secs = coerce(secs, tc_.type_of(c.args[1].get()), t_float());
        VReg w = call_runtime(RtFn::AsyncioWaitFor,
                              rt ? rt : mk_coroutine(t_unknown()), {aw, secs});
        push_temp(w);
        return w;
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




}  // namespace py23
