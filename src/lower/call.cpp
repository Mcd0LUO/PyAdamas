// lower/call.cpp —— 调用降级：参数绑定、内置/方法/asyncio
#include "lower/impl.h"

#include <limits>

namespace pyadamas {

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
    int vk = info->varargs ? list_kind_filled(lty) : 0;
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
        call_void(RtFn::ListAppend, {vararg_list(), v, heap_arg(vty)});
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
            case LiteralValue::Kind::Bytes: cv.ty = t_bytes(); cv.s = lv.s; break;
            case LiteralValue::Kind::Big: cv.ty = t_big(); cv.s = lv.s; break;
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
            case TyKind::Bytes: return const_bytes(cv.s);
            case TyKind::Big: return const_big(cv.s);
            default: return const_none();
        }
    };

    auto emit_default = [&](const Param& p, const ConstVal& cv) {
        // With **mapping or a callable signature we cannot refine, the
        // checker may not know which defaults are used. Check the final
        // representation before materializing an argument for this slot.
        if (ir_is_float(cv.ty) != ir_is_float(p.ty) &&
            !(ir_is_float(p.ty) && is_num(cv.ty)) && !is_unknown(p.ty)) {
            err(c.line, c.col, std::format(
                "default for '{}' of '{}' has no compatible compiled parameter type",
                p.name, info->name));
            return const_none();
        }
        return emit_const(cv);
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
        bound[i] = emit_default(info->pos[i], *cv);
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
        bound[bi] = emit_default(info->kwonly[i], *cv);
    }

    args = std::move(bound);
    // 实参按形参类型做数值转换：`def f(x: float)` 传 int 进来要变成 double，
    // 否则形参槽里躺的是整数位模式。
    for (size_t i = 0; i < args.size() && i < npos + nkw; ++i) {
        TypePtr want = i < npos ? info->pos[i].ty : info->kwonly[i - npos].ty;
        if (args[i] == kNoReg) continue;
        TypePtr from = vreg_ty(args[i]);
        if (bad_elem(from, want)) {
            VReg conv = promote_container(args[i], from, want);
            if (conv == kNoReg) {
                const std::string& pname =
                    i < npos ? info->pos[i].name : info->kwonly[i - npos].name;
                err(c.line, c.col,
                    std::format("'{}' cannot be passed to parameter '{}' of type '{}': element "
                                "type conversion is not supported by the code generator yet",
                                type_name(from), pname, type_name(want)));
            } else {
                args[i] = conv;
            }
        }
        args[i] = coerce(args[i], from, want);
        // 值语义：结构体按值传参，callee 改自己的副本不影响调用方。
        if (want && want->kind == TyKind::Struct && want->st)
            args[i] = struct_copy(args[i], want->st);
    }
    // 变长形参即使没收到实参也要传空容器：callee 的形参槽必须有值。
    if (info->varargs) args.push_back(vararg_list());
    if (info->kwargs) args.push_back(kwarg_dict());
    return args;
}

VReg Lowerer::lower_construct(Call& c, const ClassInfoPtr& cls) {
    auto cid_it = class_ids_.find(cls.get());
    if (cls->is_enum) {
        // Color(1)：按值线性反查成员（CPython EnumMeta.__call__）。
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, std::format("enum '{}' takes exactly one value argument",
                                           cls->name));
            return const_none();
        }
        const FieldInfo* vfl = cls->field("value");
        if (!vfl || !vfl->ty || vfl->ty->kind != TyKind::Int) {
            err(c.line, c.col,
                "enum construction of str-valued enums is not supported yet");
            return const_none();
        }
        VReg arg = expr(*c.args[0]);
        arg = coerce(arg, tc_.type_of(c.args[0].get()), t_int());
        TypePtr inst_ty = mk_class(cls.get());
        int tmp = new_borrow_slot(inst_ty);
        int end = new_block("enum.end");
        int off = static_cast<int>(8 * (1 + cls->field_index("value")));
        for (auto& em : cls->enum_members) {
            const ClassAttrInfo* ca = cls->class_attr(em.name);
            if (!ca) continue;
            VReg mobj = class_attr_load(cls.get(), ca);
            VReg mv = field_load(mobj, off, t_int());
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = arg;
            cmp.b = mv;
            VReg eq = def(t_bool(), std::move(cmp));
            int hit = new_block("enum.hit");
            int next = new_block("enum.next");
            Inst br;
            br.op = IrOp::Branch;
            br.a = eq;
            br.target = hit;
            br.target2 = next;
            emit(std::move(br));
            switch_to(hit);
            store_borrow_slot(tmp, mobj);
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
            switch_to(next);
        }
        call_void(RtFn::Raise,
                  {const_str("ValueError"), const_str("enum value is not a valid member")});
        Inst jd;
        jd.op = IrOp::Jump;
        jd.target = end;
        emit(std::move(jd));
        switch_to(end);
        return load_slot(tmp, inst_ty);
    }

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
                if (!f.has_default) continue;
                defaults.push_back({static_cast<int>(8 * (1 + (*it)->field_index(f.name))), &f});
            }
        }
    }
    for (auto& [off, fi] : defaults) {
        VReg v;
        if (fi->default_lit)
            v = const_literal(*fi->default_lit);
        else if (fi->default_expr)
            v = expr(*fi->default_expr);
        else {
            err(c.line, c.col,
                std::format("non-literal default of imported field '{}' is not supported yet",
                            fi->name));
            continue;
        }
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
            if (im->decorated) {
                // 装饰后的 __init__ 在类属性里：取包装函数并把 obj 显式传进去。
                TypePtr rt = im->call_sig->ret ? im->call_sig->ret : t_none();
                VReg r = dunder_call(obj, mk_class(cls.get()), "__init__",
                                     std::move(args), rt);
                if (r == kNoReg)
                    err(c.line, c.col,
                        std::format("decorated __init__ of '{}' has an incompatible signature",
                                    cls->name));
            } else {
                // 刚构造出来的对象类型是确定的：本模块的类可以直接调用
                // 它 vtable 槽里的实现（子类构造函数同理，槽已按 MRO 解析）。
                bool direct = false;
                if (auto cid = class_ids_.find(cls.get());
                    cid != class_ids_.end() && im->slot >= 0) {
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
                            cd.idx = Operand::func(fidx);
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
        // 跨模块导入的自定义异常类：pass-only 产生扁平异常值，富异常走
        // 导入类构造。类型名用定义模块里的原名（别名不参与匹配）。
        std::string exc_name = n.id;
        // 富异常（有字段/方法）不产生扁平值，走普通类构造（vtable 实例）。
        const ClassInfo* rich_exc = nullptr;
        if (!exc_ctor) {
            auto eit = classes_.find(n.id);
            if (eit != classes_.end() && eit->second->is_exception_class) {
                if (eit->second->is_rich_exception) rich_exc = eit->second.get();
                else {
                    exc_ctor = true;
                    exc_name = eit->second->name;
                }
            } else if (auto iit = imported_classes_.find(n.id);
                       iit != imported_classes_.end() && iit->second.cls &&
                       iit->second.cls->is_exception_class) {
                if (iit->second.cls->is_rich_exception)
                    return construct_imported(iit->second, c, mk_class(iit->second.cls.get()));
                exc_ctor = true;
                exc_name = iit->second.cls->external_name();
            }
        }
        if (rich_exc) return lower_construct(c, classes_[n.id]);
        if (exc_ctor) {
            VReg type = const_str(exc_name);
            // 关键字实参：求值但丢弃（简化）。多位置参 → 建 args 列表。
            for (auto& kw : c.keywords) expr(*kw.value);
            if (c.args.size() > 1 || (c.args.empty() && c.keywords.empty())) {
                if (c.args.empty()) {
                    Inst in;
                    in.op = IrOp::CallRt;
                    in.imm = static_cast<int64_t>(RtFn::ExcValueNew);
                    in.args = {type, const_int(0)};
                    return def_heap(mk_exception(exc_name), std::move(in));
                }
                TypePtr lt = mk_list(t_str());
                VReg al = call_runtime(RtFn::ListNew, lt,
                                       {const_int(static_cast<int64_t>(c.args.size())),
                                        const_int(list_kind_filled(lt))});
                push_temp(al);
                for (auto& arg : c.args) {
                    TypePtr mt = tc_.type_of(arg.get());
                    VReg raw = expr(*arg);
                    VReg sm = convert_to(raw, mt, t_str());
                    retain_elem(sm, 1);
                    call_void(RtFn::ListAppend, {al, sm, const_int(1)});
                }
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::ExcValueNewArgs);
                in.args = {type, al};
                return def_heap(mk_exception(exc_name), std::move(in));
            }
            // 单位置参：走单消息 ExcValueNew（保持 e.args == [msg]）
            VReg msg = const_int(0);
            if (!c.args.empty()) {
                TypePtr mt = tc_.type_of(c.args[0].get());
                VReg raw = expr(*c.args[0]);
                msg = convert_to(raw, mt, t_str());
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ExcValueNew);
            in.args = {type, msg};
            return def_heap(mk_exception(exc_name), std::move(in));
        }
        // sizeof / addr 是值类型 struct 的特殊形式；用户自己定义了同名
        // 函数或类时按普通调用处理。
        if ((n.id == "sizeof" || n.id == "addr") && !func_defs_.count(n.id) &&
            !classes_.count(n.id))
            return lower_struct_builtin(n.id, c);
        if (is_builtin(n.id)) {
            static const std::unordered_set<std::string> kBuiltins = {
                "print", "len", "int", "float", "str", "bool", "range", "next",
                "abs", "min", "max", "sum", "pow", "any", "all", "complex",
                "ord", "chr", "hex", "bin", "oct", "round", "divmod", "repr", "format",
                "list", "tuple", "dict", "set", "sorted", "reversed", "enumerate", "zip",
                "open", "input", "iter", "isinstance", "getattr", "setattr", "hasattr",
                "callable", "issubclass", "hash", "type", "id", "frozenset",
                "slice", "vars", "dir", "__pa_capwords", "__pa_sha256", "__pa_sha256_hex",
                "__pa_b64encode", "__pa_b64decode", "__pa_any_items", "bytes", "len_bytes",
                "__pa_exc_is_a", "__pa_close", "__pa_tb_frames",
                "join", "sleep", "make_chan",
                // 定宽整数显式转换（P1）：cast_i8 … cast_u64
                "cast_i8", "cast_i16", "cast_i32", "cast_i64", "cast_int",
                "cast_u8", "cast_u16", "cast_u32", "cast_u64", "cast_big",
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
        // @classmethod 里的 cls(...)：entry 模块静态绑定到所属类；
        // 非入口模块走运行期类对象，用实际子类的 ctor 包装构造。
        if (!st_->class_self_name.empty() && n.id == st_->class_self_name &&
            st_->class_self_ty) {
            if (st_->class_self_dynamic) {
                VReg cls_obj = expr(*c.func);
                const MethodInfo* init = st_->class_self_ty->method("__init__");
                FuncInfoPtr ctor_sig =
                    init && init->call_sig ? init->call_sig : std::make_shared<FuncInfo>();
                VReg callee = call_runtime(RtFn::ClassCtor, t_int(), {cls_obj});
                std::vector<VReg> args = bind_args(c, ctor_sig, init ? init->def : nullptr);
                Inst in;
                in.op = IrOp::Call;
                in.a = callee;
                in.args = std::move(args);
                return def_heap(mk_class(const_cast<ClassInfo*>(st_->class_self_ty)),
                                std::move(in));
            }
            auto it = classes_.find(st_->class_self_ty->name);
            if (it != classes_.end()) return lower_construct(c, it->second);
        }
        // 值类型构造：Point(1.0, 2.0)。按字段声明顺序逐位置求值。
        if (StructInfo* si = tc_.struct_info(n.id))
            return lower_struct_construct(c, si);
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
            if (it->second.module == "operator")
                return lower_operator(it->second.member, c);
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
        // 免写直调：from c import X —— 符号经 dlsym(RTLD_DEFAULT) 解析。
        if (TypePtr ft = tc_.type_of(c.func.get());
            ft && ft->kind == TyKind::Func && ft->fn && ft->fn->c_lazy)
            return lower_c_lazy_call(c, ft->fn->c_symbol.empty() ? n.id : ft->fn->c_symbol,
                                     kNoReg);
    }

    // 实例调用：静态类型是类实例（类名构造已在上面拦掉），调用走 __call__。
    {
        TypePtr nt = tc_.type_of(c.func.get());
        bool class_ref = false;
        if (c.func->kind == ExprKind::Name) {
            auto& n = static_cast<Name&>(*c.func);
            class_ref = (classes_.count(n.id) && !st_->slots.count(n.id)) ||
                        imported_classes_.count(n.id) ||
                        (!st_->class_self_name.empty() && n.id == st_->class_self_name);
        }
        if (!class_ref && nt && nt->kind == TyKind::Class && nt->cls) {
            const MethodInfo* m = nt->cls->method("__call__");
            if (m && m->call_sig) {
                VReg recv = expr(*c.func);
                std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
                VReg r = dunder_call(recv, nt, "__call__", std::move(args), nullptr);
                if (r != kNoReg) return r;
                err(c.line, c.col,
                    std::format("decorated __call__ of '{}' has an incompatible signature",
                                nt->cls->name));
                return const_none();
            }
            err(c.line, c.col,
                std::format("'{}' object is not callable", nt->cls->name));
            for (auto& a : c.args) expr(*a);
            for (auto& kw : c.keywords) expr(*kw.value);
            return const_none();
        }
    }

    // 类对象值当构造器：静态类型仍是 ClassObj[Class]，按原类展开构造。
    if (TypePtr ft0 = tc_.type_of(c.func.get()); ft0 && ft0->kind == TyKind::ClassObj) {
        // P4（lang_gaps）：异常类对象值当构造器（E = ValueError; E("x")）。
        // 运行期从类对象读名字构造异常值——不用静态内层名：Exception 的
        // type_eq 不区分名字，形参回填可能留着首个调用点的类型。
        if (!ft0->sub.empty() && ft0->sub[0] && ft0->sub[0]->kind == TyKind::Exception) {
            TypePtr exc_ty = mk_exception(ft0->sub[0]->name);
            VReg cls = expr(*c.func);
            for (auto& kw : c.keywords) expr(*kw.value);
            if (c.args.size() > 1) {
                TypePtr lt = mk_list(t_str());
                VReg al = call_runtime(RtFn::ListNew, lt,
                                       {const_int(static_cast<int64_t>(c.args.size())),
                                        const_int(list_kind_filled(lt))});
                push_temp(al);
                for (auto& arg : c.args) {
                    TypePtr mt = tc_.type_of(arg.get());
                    VReg raw = expr(*arg);
                    VReg sm = convert_to(raw, mt, t_str());
                    retain_elem(sm, 1);
                    call_void(RtFn::ListAppend, {al, sm, const_int(1)});
                }
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::ExcClassValueNewArgs);
                in.args = {cls, al};
                return def_heap(exc_ty, std::move(in));
            }
            VReg msg = const_int(0);
            if (!c.args.empty()) {
                TypePtr mt = tc_.type_of(c.args[0].get());
                VReg raw = expr(*c.args[0]);
                msg = convert_to(raw, mt, t_str());
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ExcClassValueNew);
            in.args = {cls, msg};
            return def_heap(exc_ty, std::move(in));
        }
        ClassInfo* cls = class_obj_cls(ft0);
        // P4（lang_gaps）：自定义 pass-only 异常类对象值 → 扁平异常值
        // （与内建异常类同口径；运行期读类名，不用静态名）。富异常类
        // 仍走实例构造。
        if (cls && cls->is_exception_class && !cls->is_rich_exception) {
            TypePtr exc_ty = mk_exception(cls->name);
            VReg cv = expr(*c.func);
            for (auto& kw : c.keywords) expr(*kw.value);
            if (c.args.size() > 1) {
                TypePtr lt = mk_list(t_str());
                VReg al = call_runtime(RtFn::ListNew, lt,
                                       {const_int(static_cast<int64_t>(c.args.size())),
                                        const_int(list_kind_filled(lt))});
                push_temp(al);
                for (auto& arg : c.args) {
                    TypePtr mt = tc_.type_of(arg.get());
                    VReg raw = expr(*arg);
                    VReg sm = convert_to(raw, mt, t_str());
                    retain_elem(sm, 1);
                    call_void(RtFn::ListAppend, {al, sm, const_int(1)});
                }
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::ExcClassValueNewArgs);
                in.args = {cv, al};
                return def_heap(exc_ty, std::move(in));
            }
            VReg msg = const_int(0);
            if (!c.args.empty()) {
                TypePtr mt = tc_.type_of(c.args[0].get());
                VReg raw = expr(*c.args[0]);
                msg = convert_to(raw, mt, t_str());
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ExcClassValueNew);
            in.args = {cv, msg};
            return def_heap(exc_ty, std::move(in));
        }
        if (cls) {
            if (auto it = classes_.find(cls->name);
                it != classes_.end() && it->second.get() == cls)
                return lower_construct(c, it->second);
            if (const ImportedClass* ic = find_imported_class(cls))
                return construct_imported(*ic, c, mk_class(cls));
        }
        err(c.line, c.col, "class value is not available for construction here");
        return const_none();
    }

    // 方法调用
    if (c.func->kind == ExprKind::Attribute)
        return lower_method_call(static_cast<Attribute&>(*c.func), c);

    // 类型对象值当被调者：机器表示是类型名字符串，不是闭包；间接调用
    // 会把字符串头当代码指针解引用。按名字的直调（int(x)）已在上面拦掉，
    // 这里明确拒绝动态调用，别静默崩溃。
    if (TypePtr ft0 = tc_.type_of(c.func.get()); ft0 && ft0->kind == TyKind::TypeObj) {
        err(c.line, c.col,
            "calling a type object value is not supported yet; call the type by name "
            "(e.g. int(x))");
        for (auto& a : c.args) expr(*a);
        for (auto& kw : c.keywords) expr(*kw.value);
        return const_none();
    }

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
    return value_as_list(x, tc_.type_of(e), elem_out);
}

VReg Lowerer::value_as_list(VReg x, TypePtr xt, TypePtr& elem_out) {
    elem_out = t_unknown();
    if (!xt) return x;
    switch (xt->kind) {
        case TyKind::Str: {
            elem_out = t_str();
            VReg l = call_runtime(RtFn::StrToList, mk_list(t_str()), {x});
            push_temp(l);
            return l;
        }
        case TyKind::Bytes: {
            elem_out = t_int();
            VReg l = call_runtime(RtFn::BytesToList, mk_list(t_int()), {x});
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
        case TyKind::Unknown: {
            // list(unknown) / sum(unknown) 等：运行期按种类物化。
            VReg l = call_runtime(RtFn::AnyIterList, mk_list(t_unknown()), {x});
            push_temp(l);
            return l;
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
        // 用 typecheck 推出的返回类型落 vreg：以前一律 t_unknown()，`None` 返回值
        // 会被 print 当成 int 0（实测 `print(asyncio.run(noop()))` 输出 0 而非 None）。
        // float 保持 unknown：运行期给的是 double 位模式，直接标 float 会按数值解释错。
        TypePtr rty = rt;
        if (!rty || rty->kind == TyKind::Unknown || rty->kind == TyKind::Float)
            rty = t_unknown();
        VReg v = def(rty, std::move(in));
        if (ir_is_heap(rty)) push_temp(v);
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
        // 构造表达式本身只推出 Queue[unknown]；带标注的赋值会把
        // Queue[float] 之类的目标类型暂存下来。
        TypePtr qt = rt;
        if (pending_queue_ty_ && pending_queue_ty_->kind == TyKind::AsyncQueue &&
            (!qt || qt->kind != TyKind::AsyncQueue || qt->sub.empty() ||
             !qt->sub[0] || qt->sub[0]->kind == TyKind::Unknown))
            qt = pending_queue_ty_;
        pending_queue_ty_ = nullptr;
        VReg q = call_runtime(RtFn::QueueNew, qt ? qt : mk_async_queue(t_unknown()),
                              {maxsize, const_int(queue_kind(qt))});
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
        TypePtr task_elem = (rt && rt->kind == TyKind::Task && !rt->sub.empty())
                                ? rt->sub[0]
                                : nullptr;
        VReg t = call_runtime(RtFn::AsyncioCreateTask,
                              rt ? rt : mk_task(t_unknown()),
                              {coro, const_int(own_kind(task_elem))});
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
        // gather 结果列表的元素所有权按静态结果类型选择，避免把 float
        // 位模式当堆指针去 dec。
        TypePtr res_list = (rt && rt->kind == TyKind::Coroutine && !rt->sub.empty())
                               ? rt->sub[0]
                               : nullptr;
        VReg w = call_runtime(RtFn::AsyncioGather,
                              rt ? rt : mk_coroutine(mk_list(t_unknown())),
                              {L, const_int(list_kind_filled(res_list))});
        push_temp(w);
        return w;
    }

    err(c.line, c.col,
        std::format("asyncio.{} is not supported by the code generator yet", member));
    return const_none();
}




VReg Lowerer::lower_struct_builtin(const std::string& name, Call& c) {
    if (c.args.empty()) {
        err(c.line, c.col, std::format("{}() takes exactly one argument", name));
        return const_int(0);
    }
    if (name == "sizeof") {
        // 编译期常量：类型检查阶段已经保证实参是 struct 类型或值。
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind == TyKind::Struct && t->st) return const_int(t->st->size);
        err(c.args[0]->line, c.args[0]->col, "sizeof() expects a struct type or value");
        return const_int(0);
    }
    // addr(p)：payload 地址就是结构体值的机器表示，直接改类型即可。
    VReg v = expr(*c.args[0]);
    TypePtr t = vreg_ty(v);
    if (t && t->kind == TyKind::Struct) return retype(v, mk_struct_ptr(t->st));
    if (t && t->kind == TyKind::StructPtr) return v;
    err(c.args[0]->line, c.args[0]->col, "addr() expects a struct value");
    return const_none();
}

VReg Lowerer::lower_struct_construct(Call& c, StructInfo* s) {
    std::vector<VReg> vals;
    vals.reserve(s->fields.size());
    for (size_t i = 0; i < c.args.size() && i < s->fields.size(); ++i) {
        TypePtr at = tc_.type_of(c.args[i].get());
        VReg v = expr(*c.args[i]);
        vals.push_back(coerce(v, at, s->fields[i].ty));
    }
    for (size_t i = s->fields.size(); i < c.args.size(); ++i) expr(*c.args[i]);
    VReg obj = struct_new(s);
    for (size_t i = 0; i < vals.size(); ++i) {
        const StructFieldInfo& f = s->fields[i];
        field_store(obj, static_cast<int>(f.offset), vals[i],
                    static_cast<uint8_t>(f.size));
    }
    return obj;
}

}  // namespace pyadamas
