// lower_expr.cpp —— 表达式降级：运算、容器、推导式、赋值
#include "lower_impl.h"

namespace py23 {

void Lowerer::assign_to(Expr& target, VReg v, TypePtr vty) {
    // 以 vreg 上记录的类型为准：调用方传进来的 vty 有时是目标侧的类型
    TypePtr src_ty = vreg_ty(v);
    if (!src_ty || src_ty->kind == TyKind::Unknown) src_ty = vty;
    // 元素跨数值域的容器（list[int] → list[float]）要逐元素转换并重新分配，
    // 现在没做。宁可报错，也不要让 double 槽里躺整数位模式。
    auto bad_elem = [](const TypePtr& from, const TypePtr& to) {
        if (!from || !to || from->kind != to->kind) return false;
        if (from->kind != TyKind::List && from->kind != TyKind::Tuple) return false;
        if (from->sub.empty() || to->sub.empty() || !from->sub[0] || !to->sub[0]) return false;
        if (from->sub[0]->kind == TyKind::Unknown || to->sub[0]->kind == TyKind::Unknown)
            return false;
        return (from->sub[0]->kind == TyKind::Float) != (to->sub[0]->kind == TyKind::Float);
    };
    switch (target.kind) {
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(target);
            if (int cs = comp_slot(n.id); cs >= 0) {
                TypePtr want = st_->fn->slot_ty[cs];
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_slot(cs, v);
                break;
            }
            if (is_cell(n.id) || is_env(n.id)) {
                write_cell(n.id, v, src_ty);
                break;
            }
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                int gi = global_idx(n.id);
                TypePtr want = mod_.globals[gi].ty;
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_global(gi, v);
            } else {
                int slot = st_->slots[n.id];
                TypePtr want = st_->fn->slot_ty[slot];
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_slot(slot, v);
            }
            break;
        }
        case ExprKind::Tuple:
        case ExprKind::List: {
            auto& elts = (target.kind == ExprKind::Tuple ? static_cast<TupleExpr&>(target).elts
                                                         : static_cast<ListExpr&>(target).elts);
            for (size_t i = 0; i < elts.size(); ++i) {
                if (!elts[i]) continue;
                if (elts[i]->kind == ExprKind::Starred) {
                    err(elts[i]->line, elts[i]->col,
                        "starred unpacking is not supported by the code generator yet");
                    continue;
                }
                TypePtr et = tc_.type_of(elts[i].get());
                Inst in;
                in.op = IrOp::IndexLoad;
                in.a = v;
                in.b = const_int(static_cast<int64_t>(i));
                // 从字符串解包出的每个元素都是新串，必须入临时栈。
                VReg ev = (src_ty && src_ty->kind == TyKind::Str)
                              ? def_heap(et, std::move(in))
                              : def(et, std::move(in));
                assign_to(*elts[i], ev, et);
            }
            break;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<Subscript&>(target);
            if (s.slice->kind == ExprKind::Slice) {
                auto& sl = static_cast<SliceExpr&>(*s.slice);
                TypePtr ot = tc_.type_of(s.value.get());
                if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple &&
                            ot->kind != TyKind::Unknown)) {
                    err(target.line, target.col,
                        std::format("'{}' does not support slice assignment", type_name(ot)));
                    break;
                }
                if (src_ty && src_ty->kind != TyKind::List && src_ty->kind != TyKind::Tuple &&
                    src_ty->kind != TyKind::Str && src_ty->kind != TyKind::Unknown) {
                    err(target.line, target.col,
                        std::format("cannot assign '{}' to a slice", type_name(src_ty)));
                    break;
                }
                VReg obj = expr(*s.value);
                constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
                auto bound = [&](ExprPtr& e) -> VReg {
                    if (!e) return const_int(kNone);
                    VReg b = expr(*e);
                    return coerce(b, tc_.type_of(e.get()), t_int());
                };
                VReg start = bound(sl.lower);
                VReg stop = bound(sl.upper);
                VReg step = bound(sl.step);
                VReg src = v;
                if (src_ty && src_ty->kind == TyKind::Str) {
                    src = call_runtime(RtFn::StrToList, mk_list(t_str()), {v});
                    push_temp(src);
                }
                call_void(RtFn::ListSliceAssign, {obj, start, stop, step, src});
                break;
            }
            VReg obj = expr(*s.value);
            VReg idx = expr(*s.slice);
            TypePtr ot = tc_.type_of(s.value.get());
            if (ot && ot->kind == TyKind::Dict) {
                TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
                TypePtr vt = ot->sub.size() > 1 ? ot->sub[1] : t_unknown();
                idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
                v = coerce(v, src_ty, vt);
                retain_elem(idx, own_kind(kt));
                retain_elem(v, own_kind(vt));
                call_void(RtFn::DictSet, {obj, idx, v});
                break;
            }
            if (ot && ot->kind == TyKind::Set) {
                err(target.line, target.col, "set does not support item assignment");
                break;
            }
            // 列表有元素类型，存进去的值要跟着转
            if (ot && ot->kind == TyKind::List && !ot->sub.empty())
                v = coerce(v, src_ty, ot->sub[0]);
            // 运行时 list_set 只负责 dec 旧值；新值的 retain 在这里做。
            retain_elem(v, list_kind(strip_optional(ot)));
            Inst in;
            in.op = IrOp::IndexStore;
            in.a = obj;
            in.b = idx;
            in.c = v;
            emit(std::move(in));
            break;
        }
        case ExprKind::Attribute: {
            auto& at = static_cast<Attribute&>(target);
            if (auto* n = dynamic_cast<Name*>(at.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    err(target.line, target.col,
                        std::format("cannot assign to builtin module attribute '{}.{}'", n->id,
                                    at.attr));
                    break;
                }
            }
            if (auto gi = module_member(at)) {
                TypePtr mt = mod_.globals[*gi].ty;
                v = coerce(v, src_ty, mt);
                store_global(*gi, v);
                break;
            }
            TypePtr raw = tc_.type_of(at.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                auto* f = ot->cls->field(at.attr);
                if (!f) {
                    err(target.line, target.col,
                        std::format("'{}' object has no field '{}'", ot->cls->name, at.attr));
                    break;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                v = coerce(v, src_ty, f->ty);
                int off = static_cast<int>(8 * (1 + ot->cls->field_index(at.attr)));
                store_field(f->ty, obj, off, v);
                break;
            }
            err(target.line, target.col, "unsupported assignment target");
            break;
        }
        default:
            err(target.line, target.col, "unsupported assignment target");
            break;
    }
}

// ────────────────────────── 表达式 ──────────────────────────

VReg Lowerer::expr(Expr& e) {
    switch (e.kind) {
        case ExprKind::Constant: return lower_constant(static_cast<Constant&>(e));
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(e);
            if (int cs = comp_slot(n.id); cs >= 0)
                return load_slot(cs, st_->fn->slot_ty[cs]);
            // 嵌套函数直接引用自己：取当前闭包，而不是外层绑定。
            if (st_->self_def && n.id == st_->self_def->name && !is_cell(n.id) &&
                !is_env(n.id) && !st_->slots.count(n.id)) {
                if (st_->env_slot >= 0) return load_slot(st_->env_slot, st_->self_type);
                Inst fa;
                fa.op = IrOp::FuncAddr;
                fa.imm = st_->self_idx;
                return def(st_->self_type, std::move(fa));
            }
            if (is_cell(n.id)) return read_cell(n.id);
            if (is_env(n.id)) return read_env(n.id);
            if (imported_classes_.count(n.id)) {
                err(e.line, e.col,
                    std::format("class '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (native_aliases_.count(n.id)) {
                err(e.line, e.col,
                    std::format("native function '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (resolve_module_alias(n.id)) {
                err(e.line, e.col, std::format("module '{}' cannot be used as a value", n.id));
                return const_none();
            }
            // 类名不是一个运行时可求值的值：没有类对象，构造必须在调用点
            // 直接展开。悄悄读一个空全局槽只会得到 0，比报错难查得多。
            if (!is_local(n.id) && classes_.count(n.id)) {
                err(e.line, e.col,
                    std::format("class '{}' cannot be used as a value yet", n.id));
                return const_none();
            }
            TypePtr ty = name_ty(n.id);
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                Inst in;
                in.op = IrOp::LoadGlobal;
                in.idx = global_idx(n.id);
                in.ty = ty;
                return def(ty, std::move(in));
            }
            return load_slot(st_->slots[n.id], ty);
        }
        case ExprKind::BinOp: return lower_binop(static_cast<BinOp&>(e));
        case ExprKind::UnaryOp: return lower_unary(static_cast<UnaryOp&>(e));
        case ExprKind::BoolOp: return lower_boolop(static_cast<BoolOp&>(e));
        case ExprKind::Compare: return lower_compare(static_cast<Compare&>(e));
        case ExprKind::Call: return lower_call(static_cast<Call&>(e));
        case ExprKind::Subscript: return lower_subscript(static_cast<Subscript&>(e));
        case ExprKind::List: {
            auto& l = static_cast<ListExpr&>(e);
            return lower_list_like(l.elts, tc_.type_of(&e), false);
        }
        case ExprKind::Tuple: {
            auto& t = static_cast<TupleExpr&>(e);
            return lower_list_like(t.elts, tc_.type_of(&e), true);
        }
        case ExprKind::Set:
            return lower_set(static_cast<SetExpr&>(e), tc_.type_of(&e));
        case ExprKind::Dict:
            return lower_dict(static_cast<DictExpr&>(e), tc_.type_of(&e));
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::DictComp:
        case ExprKind::GeneratorExp:
            return lower_comp(e);
        case ExprKind::IfExp: return lower_ifexp(static_cast<IfExp&>(e));
        case ExprKind::JoinedStr: return lower_joined(static_cast<JoinedStr&>(e));
        case ExprKind::NamedExpr: {
            auto& n = static_cast<NamedExpr&>(e);
            VReg v = expr(*n.value);
            assign_to(*n.target, v, tc_.type_of(n.value.get()));
            return v;
        }
        case ExprKind::Yield: return lower_yield(e);
        case ExprKind::Await: return lower_await(e);
        case ExprKind::YieldFrom:
            return lower_yield_from(static_cast<YieldFrom&>(e));
        case ExprKind::FormattedValue: {
            // 顶层 FormattedValue 只会出现在 f-string 内部，单独出现说明
            // 上游构造有问题；按表达式处理即可。
            auto& fv = static_cast<FormattedValue&>(e);
            return convert_to(expr(*fv.value), tc_.type_of(fv.value.get()), t_str());
        }
        case ExprKind::Attribute: {
            auto& a = static_cast<Attribute&>(e);
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    VReg bv;
                    TypePtr bty;
                    if (builtin_value(mod->name, a.attr, bv, bty)) return bv;
                    err(e.line, e.col,
                        std::format("native function '{}.{}' must be called", n->id, a.attr));
                    return const_none();
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id)) {
                    if (mod->classes.count(a.attr)) {
                        err(e.line, e.col,
                            std::format("class '{}.{}' cannot be used as a value", n->id, a.attr));
                        return const_none();
                    }
                }
            }
            if (auto gi = module_member(a))
                return load_global(*gi, mod_.globals[*gi].ty);
            TypePtr raw = tc_.type_of(a.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::Exception) {
                VReg ev = expr(*a.value);
                if (a.attr == "args")
                    return call_runtime(RtFn::ExcValueArgs, mk_tuple({t_str()}), {ev});
                if (a.attr == "__cause__")
                    return call_runtime(RtFn::ExcValueCause,
                                        mk_optional(mk_exception("Exception")), {ev});
                err(e.line, e.col,
                    std::format("'exception' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                if (auto* f = ot->cls->field(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    int off = static_cast<int>(8 * (1 + ot->cls->field_index(a.attr)));
                    return field_load(obj, off, f->ty);
                }
                if (ot->cls->method(a.attr))
                    err(e.line, e.col,
                        std::format("method '{}.{}' must be called with ()", ot->cls->name, a.attr));
                else
                    err(e.line, e.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
                return const_none();
            }
            err(e.line, e.col, "attribute access is only supported as a method call");
            return const_none();
        }
        case ExprKind::Starred: {
            err(e.line, e.col, "starred expression is not allowed here");
            return const_none();
        }
        case ExprKind::Slice:
            err(e.line, e.col, "slice is not supported by the code generator yet");
            return const_none();
        case ExprKind::Lambda:
            err(e.line, e.col,
                "lambda in a class-body value is not supported by the code generator yet");
            return const_none();
        default:
            err(e.line, e.col, std::format("expression kind {} is not supported by the code generator",
                                          static_cast<int>(e.kind)));
            return const_none();
    }
}

VReg Lowerer::lower_constant(Constant& c) {
    switch (c.ck) {
        case Constant::CK::None: return const_none();
        case Constant::CK::Bool: return const_bool(c.b);
        case Constant::CK::Int: return const_int(c.i);
        case Constant::CK::Float: return const_float(c.f);
        case Constant::CK::Complex: {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexNew);
            in.args = {const_float(0.0), const_float(c.im)};
            return def_heap(t_complex(), std::move(in));
        }
        case Constant::CK::Str: return const_str(c.s);
        case Constant::CK::Ellipsis:
            err(c.line, c.col, "Ellipsis is not supported by the code generator");
            return const_none();
    }
    return const_none();
}

VReg Lowerer::lower_binop(BinOp& b) {
    TypePtr ty = tc_.type_of(&b);
    TypePtr lt = tc_.type_of(b.left.get());
    TypePtr rt = tc_.type_of(b.right.get());
    // 类型检查允许但后端还做不了的组合，在这里一次性拦住。
    // 放过去的话 codegen 会走「按整数处理」的兜底路径，指针相加不报错，
    // 只是算出一个野值——这类静默错误比编译失败难查得多。
    if (ty) {
        bool bad = false;
        switch (ty->kind) {
            case TyKind::Str: bad = b.op != Tok::PLUS; break;
            case TyKind::List:
            case TyKind::Tuple:
            case TyKind::Set:
            case TyKind::Dict: bad = true; break;
            default: break;
        }
        if (bad) {
            err(b.line, b.col,
                std::format("operator '{}' on '{}' is not supported by the code generator yet",
                            tok_name(b.op), type_name(ty)));
            return const_none();
        }
    }
    VReg l = expr(*b.left);
    VReg r = expr(*b.right);
    if (ty && ty->kind == TyKind::Complex) {
        auto as_complex = [&](VReg v, TypePtr from) {
            return (from && from->kind == TyKind::Complex)
                       ? v
                       : convert_to(v, from, t_complex());
        };
        l = as_complex(l, lt);
        r = as_complex(r, rt);
        RtFn fn = RtFn::ComplexAdd;
        switch (b.op) {
            case Tok::PLUS: fn = RtFn::ComplexAdd; break;
            case Tok::MINUS: fn = RtFn::ComplexSub; break;
            case Tok::STAR: fn = RtFn::ComplexMul; break;
            case Tok::SLASH: fn = RtFn::ComplexDiv; break;
            default:
                err(b.line, b.col,
                    std::format("operator '{}' on complex is not supported", tok_name(b.op)));
                return const_none();
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {l, r};
        return def_heap(ty, std::move(in));
    }
    if (ty && ty->kind == TyKind::Str) {
        if (lt && lt->kind == TyKind::Exception) l = convert_to(l, lt, t_str());
        if (rt && rt->kind == TyKind::Exception) r = convert_to(r, rt, t_str());
    }
    // int / int 的结果是 float，两个操作数都得先转；int + float 同理。
    // 放在这里做，codegen 就不用每条浮点指令都猜操作数是什么类型。
    if (ty && ty->kind == TyKind::Float) {
        if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
        if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
    }
    Inst in;
    in.op = IrOp::Bin;
    in.bop = map_bin(b.op);
    in.a = l;
    in.b = r;
    return def_heap(ty, std::move(in));
}

VReg Lowerer::lower_unary(UnaryOp& u) {
    TypePtr ty = tc_.type_of(&u);
    VReg v = expr(*u.operand);
    if (ty && ty->kind == TyKind::Complex) {
        if (u.op == Tok::PLUS) return v;
        if (u.op == Tok::MINUS) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexNeg);
            in.args = {v};
            return def_heap(ty, std::move(in));
        }
    }
    Inst in;
    in.op = IrOp::Un;
    switch (u.op) {
        case Tok::MINUS: in.uop = UnKind::Neg; break;
        case Tok::PLUS: in.uop = UnKind::Pos; break;
        case Tok::TILDE: in.uop = UnKind::BitNot; break;
        case Tok::KW_NOT: in.uop = UnKind::Not; break;
        default:
            err(u.line, u.col, "unsupported unary operator");
            in.uop = UnKind::Pos;
            break;
    }
    in.a = v;
    return def(ty, std::move(in));
}

VReg Lowerer::lower_boolop(BoolOp& b) {
    TypePtr ty = tc_.type_of(&b);
    int tmp = new_borrow_slot(ty);

    bool is_and = b.op == Tok::KW_AND;
    int end = new_block(is_and ? "and.end" : "or.end");

    VReg acc = expr(*b.values[0]);
    store_borrow_slot(tmp, acc);

    for (size_t i = 1; i < b.values.size(); ++i) {
        VReg cur_v = load_slot(tmp, ty);
        int rhs = new_block(is_and ? "and.rhs" : "or.rhs");
        int next = (i + 1 < b.values.size()) ? new_block(is_and ? "and.next" : "or.next") : end;

        Inst br;
        br.op = IrOp::Branch;
        br.a = cur_v;
        // and: 真才继续算右边；or: 假才继续算右边
        br.target = is_and ? rhs : next;
        br.target2 = is_and ? next : rhs;
        emit(std::move(br));

        switch_to(rhs);
        VReg v = expr(*b.values[i]);
        store_borrow_slot(tmp, v);
        Inst j;
        j.op = IrOp::Jump;
        j.target = next;
        emit(std::move(j));

        if (next == end) break;
        switch_to(next);
    }

    switch_to(end);
    return load_slot(tmp, ty);
}

VReg Lowerer::lower_compare(Compare& c) {
    if (c.ops.size() == 1) {
        VReg l = expr(*c.left);
        VReg r = expr(*c.comparators[0]);
        CmpOp op = c.ops[0];
        TypePtr lt = tc_.type_of(c.left.get());
        TypePtr rt = tc_.type_of(c.comparators[0].get());
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            VReg flag;
            if (rt && (rt->kind == TyKind::Dict || rt->kind == TyKind::Set)) {
                TypePtr kt = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, kt);
                flag = call_runtime(RtFn::DictContains, t_bool(), {r, l});
            } else if (rt && (rt->kind == TyKind::List || rt->kind == TyKind::Tuple)) {
                TypePtr et = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, et);
                int ck = key_kind(et);
                if (rt->kind == TyKind::Tuple) {
                    for (auto& sub : rt->sub)
                        if (key_kind(sub) != ck) { ck = 3; break; }
                }
                flag = call_runtime(RtFn::ListContains, t_bool(), {r, l, const_int(ck)});
            } else if (rt && rt->kind == TyKind::Str) {
                if (lt && lt->kind != TyKind::Str && lt->kind != TyKind::Unknown) {
                    err(c.line, c.col,
                        std::format("'in <str>' requires a string, got '{}'", type_name(lt)));
                    return const_bool(false);
                }
                flag = call_runtime(RtFn::StrContains, t_bool(), {r, l});
            } else {
                err(c.line, c.col,
                    std::format("'in' on '{}' is not supported by the code generator yet",
                                type_name(rt)));
                return const_bool(false);
            }
            if (op == CmpOp::NotIn) {
                Inst un;
                un.op = IrOp::Un;
                un.uop = UnKind::Not;
                un.a = flag;
                flag = def(t_bool(), std::move(un));
            }
            return flag;
        }
        if ((lt && lt->kind == TyKind::Complex) || (rt && rt->kind == TyKind::Complex)) {
            if (op != CmpOp::Eq && op != CmpOp::Ne) {
                err(c.line, c.col, "complex values are not orderable");
                return const_bool(false);
            }
            auto as_complex = [&](VReg v, TypePtr from) {
                return (from && from->kind == TyKind::Complex)
                           ? v
                           : convert_to(v, from, t_complex());
            };
            l = as_complex(l, lt);
            r = as_complex(r, rt);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ComplexEq);
            in.args = {l, r};
            VReg eq = def(t_bool(), std::move(in));
            if (op == CmpOp::Eq) return eq;
            Inst un;
            un.op = IrOp::Un;
            un.uop = UnKind::Not;
            un.a = eq;
            return def(t_bool(), std::move(un));
        }
        // 混合数值比较统一到 float：codegen 按左操作数的类型选指令
        if ((ir_is_float(lt) || ir_is_float(rt)) && is_num(lt) && is_num(rt)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
    }

    for (auto op : c.ops) {
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            err(c.line, c.col, "chained 'in' comparisons are not supported");
            return const_bool(false);
        }
    }

    // 链式比较：需要短路，用槽保存中间值。
    int res = new_temp_slot(t_bool());
    // 槽类型取左操作数：链式比较里存的是同一个值，类型决定真值判断。
    // 它只是借用在表达式内传递，生命周期由临时栈负责。
    int cur_slot = new_borrow_slot(tc_.type_of(c.left.get()));
    store_slot(res, const_bool(true));
    store_borrow_slot(cur_slot, expr(*c.left));

    int fail = new_block("cmp.fail");
    int done = new_block("cmp.end");

    for (size_t i = 0; i < c.ops.size(); ++i) {
        VReg l = load_slot(cur_slot, t_unknown());
        VReg r = expr(*c.comparators[i]);
        int cont = new_block("cmp.next");
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = map_cmp(c.ops[i]);
        cmp.a = l;
        cmp.b = r;
        VReg cr = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cr;
        br.target = cont;
        br.target2 = fail;
        emit(std::move(br));

        switch_to(cont);
        store_borrow_slot(cur_slot, r);
        if (i + 1 == c.ops.size()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = done;
            emit(std::move(j));
        }
    }

    switch_to(fail);
    store_slot(res, const_bool(false));
    Inst j;
    j.op = IrOp::Jump;
    j.target = done;
    emit(std::move(j));

    switch_to(done);
    return load_slot(res, t_bool());
}

VReg Lowerer::lower_subscript(Subscript& s) {
    VReg obj = expr(*s.value);
    TypePtr ot = tc_.type_of(s.value.get());
    TypePtr ty = tc_.type_of(&s);

    if (s.slice->kind == ExprKind::Slice) {
        auto& sl = static_cast<SliceExpr&>(*s.slice);
        constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
        auto bound = [&](ExprPtr& e) -> VReg {
            if (!e) return const_int(kNone);
            VReg v = expr(*e);
            return coerce(v, tc_.type_of(e.get()), t_int());
        };
        VReg start = bound(sl.lower);
        VReg stop = bound(sl.upper);
        VReg step = bound(sl.step);
        if (ot && ot->kind == TyKind::Str) {
            VReg v = call_runtime(RtFn::StrSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        if (ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple)) {
            VReg v = call_runtime(RtFn::ListSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        err(s.line, s.col, std::format("'{}' is not sliceable", type_name(ot)));
        return const_none();
    }

    VReg idx = expr(*s.slice);
    if (ot && ot->kind == TyKind::Dict) {
        TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
        idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
        return call_runtime(RtFn::DictGet, ty, {obj, idx});
    }
    if (ot && ot->kind == TyKind::Set) {
        err(s.line, s.col, "set is not subscriptable");
        return const_none();
    }
    Inst in;
    in.op = IrOp::IndexLoad;
    in.a = obj;
    in.b = idx;
    // 只有「字符串下标」产生新串；list[str]/tuple[str] 返回的是容器内部的
    // 借用引用，不能入临时栈，否则会比容器多减一次引用。
    if (ty && ty->kind == TyKind::Str && ot && ot->kind == TyKind::Str)
        return def_heap(ty, std::move(in));
    return def(ty, std::move(in));
}

VReg Lowerer::lower_list_like(std::vector<ExprPtr>& elts, TypePtr ty, bool is_tuple) {
    std::vector<VReg> args;
    args.reserve(elts.size());
    // 元素所有权类别：0 非堆 / 1 堆 / 2 unknown 或混合。
    // kind 1/2 的元素在写入前 retain；kind 1 的容器销毁时由运行时 release。
    int kind = list_kind(ty);
    for (size_t i = 0; i < elts.size(); ++i) {
        auto& e = elts[i];
        if (e->kind == ExprKind::Starred) {
            err(e->line, e->col, "starred element is not supported by the code generator yet");
            continue;
        }
        VReg v = expr(*e);
        // 字面量的元素类型是按全部元素推出来的（`[1, 2.5]` 是 list[float]），
        // 前面的整数元素得在构造时就转成 double。
        TypePtr want;
        if (ty && !is_tuple && !ty->sub.empty()) want = ty->sub[0];
        else if (ty && is_tuple && !ty->sub.empty() && !ty->variadic && i < ty->sub.size())
            want = ty->sub[i];
        else if (ty && is_tuple && ty->variadic && !ty->sub.empty())
            want = ty->sub[0];
        v = coerce(v, tc_.type_of(e.get()), want);
        retain_elem(v, kind);
        args.push_back(v);
    }
    Inst in;
    in.op = is_tuple ? IrOp::MakeTuple : IrOp::MakeList;
    in.imm = kind;
    in.args = std::move(args);
    return def_heap(ty, std::move(in));
}

VReg Lowerer::lower_dict(DictExpr& d, TypePtr ty) {
    TypePtr kt = t_unknown(), vt = t_unknown();
    if (ty && ty->kind == TyKind::Dict) {
        if (!ty->sub.empty()) kt = ty->sub[0];
        if (ty->sub.size() > 1) vt = ty->sub[1];
    }
    TypePtr dtype = (ty && ty->kind == TyKind::Dict) ? ty : mk_dict(kt, vt);
    int kk = key_kind(kt);
    int ko = own_kind(kt);
    int vo = own_kind(vt);

    VReg dict = call_runtime(RtFn::DictNew, dtype,
                             {const_int(kk), const_int(ko), const_int(vo)});
    push_temp(dict);
    for (size_t i = 0; i < d.values.size(); ++i) {
        if (!d.keys[i]) {
            err(d.line, d.col, "'**' dict unpacking is not supported by the code generator yet");
            continue;
        }
        VReg k = expr(*d.keys[i]);
        k = coerce(k, tc_.type_of(d.keys[i].get()), kt);
        retain_elem(k, ko);
        VReg v = expr(*d.values[i]);
        v = coerce(v, tc_.type_of(d.values[i].get()), vt);
        retain_elem(v, vo);
        call_void(RtFn::DictSet, {dict, k, v});
    }
    return dict;
}

VReg Lowerer::lower_set(SetExpr& s, TypePtr ty) {
    TypePtr et = t_unknown();
    if (ty && ty->kind == TyKind::Set && !ty->sub.empty()) et = ty->sub[0];
    TypePtr stype = (ty && ty->kind == TyKind::Set) ? ty : mk_set(et);
    int ek = key_kind(et);
    int eo = own_kind(et);

    VReg set = call_runtime(RtFn::DictNew, stype,
                            {const_int(ek), const_int(eo), const_int(0)});
    push_temp(set);
    for (auto& e : s.elts) {
        VReg v = expr(*e);
        v = coerce(v, tc_.type_of(e.get()), et);
        retain_elem(v, eo);
        call_void(RtFn::DictSet, {set, v, const_int(0)});
    }
    return set;
}

VReg Lowerer::lower_comp(Expr& e) {
    std::vector<ExprPtr>* gens = nullptr;
    Expr* elt_expr = nullptr;
    Expr* key_expr = nullptr;
    Expr* val_expr = nullptr;
    bool is_set = false;
    bool is_dict = false;
    switch (e.kind) {
        case ExprKind::ListComp: {
            auto& c = static_cast<ListComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::GeneratorExp: {
            auto& c = static_cast<GeneratorExp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::SetComp: {
            auto& c = static_cast<SetComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            is_set = true;
            break;
        }
        case ExprKind::DictComp: {
            auto& c = static_cast<DictComp&>(e);
            gens = &c.generators;
            key_expr = c.key.get();
            val_expr = c.value.get();
            is_dict = true;
            break;
        }
        default: return const_none();
    }

    TypePtr rty = tc_.type_of(&e);
    TypePtr elem_ty = t_unknown();
    TypePtr val_ty = t_unknown();

    bool any_async = false;
    for (auto& g : *gens)
        if (static_cast<Comprehension&>(*g).is_async) any_async = true;
    bool async_mode = false;
    if (any_async) {
        if (!st_->is_coroutine) {
            err(e.line, e.col, "'async for' in a comprehension requires an async function");
        } else if (comp_ctx_node_ != &e) {
            err(e.line, e.col,
                "'async for' in a comprehension is only supported as a statement, "
                "assignment or return value");
        } else if (e.kind == ExprKind::GeneratorExp) {
            err(e.line, e.col, "async generator expressions are not supported yet");
        } else {
            async_mode = true;
        }
        if (!async_mode) return const_none();
    }

    VReg acc;
    if (is_dict) {
        if (!rty || rty->kind != TyKind::Dict) rty = mk_dict(t_unknown(), t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        if (rty->sub.size() > 1) val_ty = rty->sub[1];
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(own_kind(val_ty))});
        push_temp(acc);
    } else if (is_set) {
        if (!rty || rty->kind != TyKind::Set) rty = mk_set(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(0)});
        push_temp(acc);
    } else {
        if (!rty || rty->kind != TyKind::List) rty = mk_list(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        Inst mk;
        mk.op = IrOp::MakeList;
        mk.imm = list_kind(rty);
        acc = def_heap(rty, std::move(mk));
    }

    // 挂起后 vreg 不存活：累加器落帧槽，循环内外都从槽里读。
    int acc_slot = -1;
    if (async_mode) {
        acc_slot = new_temp_slot(rty);
        store_slot(acc_slot, acc);
    }
    auto acc_val = [&]() { return acc_slot >= 0 ? load_slot(acc_slot, rty) : acc; };

    auto emit_result = [&]() {
        if (is_dict) {
            VReg k = expr(*key_expr);
            k = coerce(k, tc_.type_of(key_expr), elem_ty);
            retain_elem(k, own_kind(elem_ty));
            VReg v = expr(*val_expr);
            v = coerce(v, tc_.type_of(val_expr), val_ty);
            retain_elem(v, own_kind(val_ty));
            call_void(RtFn::DictSet, {acc_val(), k, v});
            return;
        }
        VReg v = expr(*elt_expr);
        v = coerce(v, tc_.type_of(elt_expr), elem_ty);
        if (is_set) {
            retain_elem(v, own_kind(elem_ty));
            call_void(RtFn::DictSet, {acc_val(), v, const_int(0)});
        } else {
            retain_elem(v, list_kind(rty));
            call_void(RtFn::ListAppend, {acc_val(), v});
        }
    };

    comp_slots_.emplace_back();

    std::function<void(size_t)> gen = [&](size_t gi) {
        if (gi >= gens->size()) {
            emit_result();
            return;
        }
        auto& g = static_cast<Comprehension&>(*(*gens)[gi]);
        VReg seq = expr(*g.iter);
        TypePtr sty = tc_.type_of(g.iter.get());

        if (g.is_async) {
            TypePtr sst = sty ? sty : t_unknown();
            TypePtr ety = t_unknown();
            if (sty && sty->kind == TyKind::AsyncGenerator && !sty->sub.empty())
                ety = sty->sub[0];
            int seq_slot = new_temp_slot(sst);
            store_slot(seq_slot, seq);
            int hs_slot = new_temp_slot(t_int());

            int next_try = new_block("comp.anext");
            int fetch = new_block("comp.afetch");
            int land = new_block("comp.aland");
            int reraise_b = new_block("comp.areaise");
            int end = new_block("comp.aend");

            Inst j;
            j.op = IrOp::Jump;
            j.target = next_try;
            emit(std::move(j));

            switch_to(next_try);
            VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
            store_slot(hs_slot, h);
            VReg sj = setjmp_handler(hs_slot);
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = sj;
                br.target = land;
                br.target2 = fetch;
                emit(std::move(br));
            }
            FuncCtx::Cleanup cf;
            cf.kind = FuncCtx::Cleanup::Kind::Handler;
            cf.handler_slot = hs_slot;
            cf.land = land;
            st_->cleanups.push_back(cf);

            switch_to(fetch);
            Inst call;
            call.op = IrOp::CallRt;
            call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
            call.args = {load_slot(seq_slot, sst)};
            VReg w = def_heap(mk_coroutine(ety), std::move(call));
            VReg val = suspend_on_awaitable(w, ety, g.line, g.col);
            st_->cleanups.pop_back();
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});

            if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
                int slot = comp_slot(tn->id);
                if (slot < 0) {
                    slot = st_->fn->new_slot(ety);
                    comp_slots_.back()[tn->id] = slot;
                }
                store_slot(slot, val);
            } else {
                assign_to(*g.target, val, ety);
            }

            for (auto& cond_expr : g.ifs) {
                VReg c = expr(*cond_expr);
                int ok_b = new_block("comp.aif");
                Inst cb;
                cb.op = IrOp::Branch;
                cb.a = c;
                cb.target = ok_b;
                cb.target2 = next_try;
                emit(std::move(cb));
                switch_to(ok_b);
            }

            gen(gi + 1);
            if (!terminated()) {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = next_try;
                emit(std::move(jj));
            }

            switch_to(land);
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
            VReg m = call_runtime(RtFn::ExcMatches, t_bool(),
                                  {const_str("StopAsyncIteration")});
            int stop_ok = new_block("comp.astop");
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = m;
                br.target = stop_ok;
                br.target2 = reraise_b;
                emit(std::move(br));
            }
            switch_to(reraise_b);
            call_void(RtFn::ExcReraise, {});
            end_unreachable();
            switch_to(stop_ok);
            call_void(RtFn::ExcClear, {});
            {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = end;
                emit(std::move(jj));
            }
            switch_to(end);
            return;
        }

        bool ok = sty && (sty->kind == TyKind::List || sty->kind == TyKind::Tuple ||
                          sty->kind == TyKind::Str || sty->kind == TyKind::Dict ||
                          sty->kind == TyKind::Set);
        if (!ok) {
            err(g.iter->line, g.iter->col,
                std::format("cannot iterate over '{}' in a comprehension", type_name(sty)));
            return;
        }
        TypePtr ety = t_unknown();
        if (sty->kind == TyKind::Str) ety = t_str();
        else if (!sty->sub.empty()) ety = sty->sub[0];

        Inst ln;
        ln.op = IrOp::Len;
        ln.a = seq;
        VReg n = def(t_int(), std::move(ln));

        int seq_slot = -1;
        int n_slot = -1;
        if (async_mode) {
            seq_slot = new_temp_slot(sty);
            store_slot(seq_slot, seq);
            n_slot = new_temp_slot(t_int());
            store_slot(n_slot, n);
        }
        auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sty) : seq; };
        auto n_val = [&]() { return n_slot >= 0 ? load_slot(n_slot, t_int()) : n; };

        int idx = new_temp_slot(t_int());
        store_slot(idx, const_int(0));

        int cond = new_block("comp.cond");
        int body = new_block("comp.body");
        int incr = new_block("comp.incr");
        int end = new_block("comp.end");

        // 水位取在序列求值之后：seq 本身可能是临时容器，不能被每轮清掉。
        // async 模式下临时值随挂起统一清理，不需要局部水位。
        int mark_block = -1;
        size_t mark_pos = 0;
        VReg mark = async_mode ? kNoReg : rc_mark_at(mark_block, mark_pos);
        auto clean = [&]() {
            if (async_mode) cleanup_gen_mark();
            else rc_cleanup(mark);
        };
        size_t pushes_before = st_->rc_pushes;

        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        VReg iv = load_slot(idx, t_int());
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = iv;
        cmp.b = n_val();
        VReg cc = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = body;
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        clean();
        VReg iv2 = load_slot(idx, t_int());
        VReg ev;
        if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
            ev = call_runtime(RtFn::DictKeyAt, ety, {seq_val(), iv2});
        } else {
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = seq_val();
            get.b = iv2;
            ev = sty->kind == TyKind::Str ? def_heap(ety, std::move(get))
                                          : def(ety, std::move(get));
        }
        if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
            int slot = comp_slot(tn->id);
            if (slot < 0) {
                slot = st_->fn->new_slot(ety);
                comp_slots_.back()[tn->id] = slot;
            }
            store_slot(slot, ev);
        } else {
            assign_to(*g.target, ev, ety);
        }

        // 过滤器：任何一条不成立就跳到自增块。
        for (auto& cond_expr : g.ifs) {
            VReg c = expr(*cond_expr);
            int ok_b = new_block("comp.if");
            Inst cb;
            cb.op = IrOp::Branch;
            cb.a = c;
            cb.target = ok_b;
            cb.target2 = incr;
            emit(std::move(cb));
            switch_to(ok_b);
        }

        gen(gi + 1);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = incr;
            emit(std::move(jj));
        }

        switch_to(incr);
        clean();
        VReg iv3 = load_slot(idx, t_int());
        Inst add;
        add.op = IrOp::Bin;
        add.bop = BinKind::Add;
        add.a = iv3;
        add.b = const_int(1);
        VReg iv4 = def(t_int(), std::move(add));
        store_slot(idx, iv4);
        Inst ji;
        ji.op = IrOp::Jump;
        ji.target = cond;
        emit(std::move(ji));

        switch_to(end);
        clean();
        if (!async_mode && !st_->is_generator && st_->rc_pushes == pushes_before) {
            erase_cleanup(body, mark);
            erase_cleanup(incr, mark);
            erase_cleanup(end, mark);
            erase_mark(mark_block, mark_pos, mark);
        }
    };

    gen(0);
    comp_slots_.pop_back();
    return async_mode ? acc_val() : acc;
}

VReg Lowerer::lower_ifexp(IfExp& e) {
    TypePtr ty = tc_.type_of(&e);
    int tmp = new_borrow_slot(ty);
    VReg c = expr(*e.test);
    int then_b = new_block("ifexp.then");
    int else_b = new_block("ifexp.else");
    int end = new_block("ifexp.end");

    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = then_b;
    br.target2 = else_b;
    emit(std::move(br));

    switch_to(then_b);
    store_borrow_slot(tmp, expr(*e.body));
    Inst j1;
    j1.op = IrOp::Jump;
    j1.target = end;
    emit(std::move(j1));

    switch_to(else_b);
    store_borrow_slot(tmp, expr(*e.orelse));
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));

    switch_to(end);
    return load_slot(tmp, ty);
}

VReg Lowerer::convert_to(VReg v, TypePtr from, TypePtr to) {
    if (!to) return v;
    if (from && type_eq(from, to)) return v;
    if (to->kind == TyKind::Bool && from && from->kind == TyKind::Complex) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexTruth);
        in.args = {v};
        return def(t_bool(), std::move(in));
    }
    if (to->kind == TyKind::Complex) {
        if (from && from->kind == TyKind::Complex) return v;
        VReg f = (from && from->kind == TyKind::Float) ? v
                                                       : convert_to(v, from, t_float());
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexNew);
        in.args = {f, const_float(0.0)};
        return def_heap(t_complex(), std::move(in));
    }
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Complex) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexStr);
        in.args = {v};
        VReg s = def_heap(t_str(), std::move(in));
        return s;
    }
    if (to->kind == TyKind::Str && from && from->kind == TyKind::Exception) {
        VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
        push_temp(s);
        return s;
    }
    Inst in;
    in.op = IrOp::Convert;
    in.a = v;
    VReg r = def(to, std::move(in));
    // 数值转换产出标量；转成字符串才是一个新的堆对象。
    if (to->kind == TyKind::Str && (!from || from->kind != TyKind::Str))
        push_temp(r);
    return r;
}

// 存储位置（槽 / 全局 / 形参 / 列表元素）有静态类型，值可能落在另一个
// 数值域上——典型是 `x: float = 1`。不补这条 convert，整数 1 的位模式
// 会被当成 double 读出来（5e-324），而且错得毫无声响。
// 只在数值域之间转：指针类之间的差异属于类型检查该报的错，这里不猜。
VReg Lowerer::coerce(VReg v, TypePtr from, TypePtr to) {
    if (!from || !to) return v;
    if (type_eq(from, to)) return v;
    auto num = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Float; };
    if (!num(from->kind) || !num(to->kind)) return v;
    bool from_int = from->kind != TyKind::Float;
    bool to_int = to->kind != TyKind::Float;
    if (from_int == to_int) return v;  // int/bool 互换不用动，机器表示一样
    return convert_to(v, from, to);
}

VReg Lowerer::lower_joined(JoinedStr& j) {
    VReg acc = const_str("");
    bool first = true;
    for (auto& part : j.values) {
        VReg piece;
        if (part->kind == ExprKind::Constant) {
            auto& c = static_cast<Constant&>(*part);
            if (c.ck != Constant::CK::Str) {
                err(part->line, part->col, "non-string literal inside f-string");
                continue;
            }
            if (c.s.empty()) continue;
            piece = const_str(c.s);
        } else if (part->kind == ExprKind::FormattedValue) {
            auto& fv = static_cast<FormattedValue&>(*part);
            TypePtr vt = tc_.type_of(fv.value.get());
            VReg raw = expr(*fv.value);
            if (fv.conversion == 's') {
                if (!vt || vt->kind != TyKind::Str) raw = convert_to(raw, vt, t_str());
            } else if (fv.conversion == 'r' || fv.conversion == 'a') {
                if (vt && vt->kind == TyKind::Str) {
                    Inst in;
                    in.op = IrOp::CallRt;
                    in.imm = static_cast<int64_t>(RtFn::ReprStr);
                    in.args = {raw};
                    raw = def_heap(t_str(), std::move(in));
                } else {
                    raw = convert_to(raw, vt, t_str());
                }
            }
            VReg spec = const_str("");
            if (fv.format_spec)
                spec = lower_joined(static_cast<JoinedStr&>(*fv.format_spec));

            auto fmt_call = [&](RtFn fn) {
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(fn);
                in.args = {raw, spec};
                return def_heap(t_str(), std::move(in));
            };
            TypePtr ct = vt;
            if (fv.conversion == 's' || fv.conversion == 'r' || fv.conversion == 'a')
                ct = t_str();
            if (!ct || ct->kind == TyKind::Unknown) {
                VReg s = convert_to(raw, ct, t_str());
                Inst in;
                in.op = IrOp::CallRt;
                in.imm = static_cast<int64_t>(RtFn::FmtStr);
                in.args = {s, spec};
                piece = def_heap(t_str(), std::move(in));
            } else {
                switch (ct->kind) {
                    case TyKind::Bool: piece = fmt_call(RtFn::FmtBool); break;
                    case TyKind::Int: piece = fmt_call(RtFn::FmtI64); break;
                    case TyKind::Float: piece = fmt_call(RtFn::FmtF64); break;
                    case TyKind::Str: piece = fmt_call(RtFn::FmtStr); break;
                    case TyKind::Complex: piece = fmt_call(RtFn::FmtComplex); break;
                    case TyKind::Exception: {
                        VReg ev = call_runtime(RtFn::ExcValueStr, t_str(), {raw});
                        push_temp(ev);
                        Inst in;
                        in.op = IrOp::CallRt;
                        in.imm = static_cast<int64_t>(RtFn::FmtStr);
                        in.args = {ev, spec};
                        piece = def_heap(t_str(), std::move(in));
                        break;
                    }
                    default:
                        err(part->line, part->col,
                            std::format("f-string cannot format '{}'", type_name(ct)));
                        continue;
                }
            }
        } else {
            err(part->line, part->col, "unsupported f-string part");
            continue;
        }

        if (first) {
            acc = piece;
            first = false;
        } else {
            acc = call_runtime(RtFn::StrConcat, t_str(), {acc, piece});
            push_temp(acc);
        }
    }
    return acc;
}

// 按签名重排实参：位置实参、关键字实参、缺省值、形参类型的数值转换。
// info 为空（签名未知）时按源码顺序直传，关键字参数直接报错。
void Lowerer::print_value(VReg v, TypePtr ty, int line, int col) {
    if (!ty) ty = t_unknown();
    switch (ty->kind) {
        case TyKind::None: call_void(RtFn::PrintNone, {}); break;
        case TyKind::Bool: call_void(RtFn::PrintBool, {v}); break;
        case TyKind::Float: call_void(RtFn::PrintF64, {v}); break;
        case TyKind::Str: call_void(RtFn::PrintStr, {v}); break;
        case TyKind::Int: call_void(RtFn::PrintI64, {v}); break;
        case TyKind::Dict: call_void(RtFn::PrintStr, {const_str("<dict object>")}); break;
        case TyKind::Set: call_void(RtFn::PrintStr, {const_str("<set object>")}); break;
        case TyKind::Exception: {
            VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Complex: {
            VReg s = call_runtime(RtFn::ComplexStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Optional: {
            TypePtr inner = ty->sub.empty() ? t_unknown() : ty->sub[0];
            int some = new_block("print.opt.some");
            int none = new_block("print.opt.none");
            int end = new_block("print.opt.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = v;
            br.target = some;
            br.target2 = none;
            emit(std::move(br));
            switch_to(none);
            call_void(RtFn::PrintNone, {});
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(some);
            print_value(v, inner, line, col);
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(end);
            break;
        }
        case TyKind::Class: {
            // 没有 __str__，就按 CPython 的形状打一个 `<Point object>`。
            // 类名进字符串池，和字面量走同一条路。
            std::string name = ty->cls ? ty->cls->name : "object";
            call_void(RtFn::PrintStr, {const_str("<" + name + " object>")});
            break;
        }
        default:
            // 推断不出来时按整数输出。值没有运行时标签，这里没得选；
            // 与其拒绝编译，不如让程序跑起来，类型问题留给 check。
            (void)line;
            (void)col;
            call_void(RtFn::PrintI64, {v});
            break;
    }
}


}  // namespace py23
