// lower/expr_assign.cpp —— 赋值目标与表达式主分派
#include "lower/impl.h"

namespace pyadamas {

void Lowerer::assign_to(Expr& target, VReg v, TypePtr vty) {
    // 以 vreg 上记录的类型为准：调用方传进来的 vty 有时是目标侧的类型
    TypePtr src_ty = vreg_ty(v);
    if (!src_ty || src_ty->kind == TyKind::Unknown) src_ty = vty;
    // 值语义：`q = p` 必须复制一份 payload，不能共享同一个对象。
    if (src_ty && src_ty->kind == TyKind::Struct && src_ty->st)
        v = struct_copy(v, src_ty->st);
    switch (target.kind) {
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(target);
            if (int cs = comp_slot(n.id); cs >= 0) {
                TypePtr want = st_->fn->slot_ty[cs];
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
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
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
                v = coerce(v, src_ty, want);
                store_global(gi, v);
            } else {
                int slot = st_->slots[n.id];
                TypePtr want = st_->fn->slot_ty[slot];
                if (bad_elem(src_ty, want)) {
                    VReg conv = promote_container(v, src_ty, want);
                    if (conv == kNoReg)
                        err(target.line, target.col,
                            std::format("'{}' cannot be stored into '{}': element type "
                                        "conversion is not supported by the code generator yet",
                                        type_name(src_ty), type_name(want)));
                    else
                        v = conv;
                }
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
                // 元素类型优先取**源序列**里的静态类型。模块级
                // for k, v in d.items() 的目标变量可能因全局预绑定/join 在
                // 符号表里停在 unknown，而源类型是 tuple[str, float]。
                // 0.324 的 unknown->float 解盒把这个不一致暴露成数值错误
                // （v 被当成裸 int 位模式解盒）；按源类型取元素既更精确，
                // 也让后面的 store 直接走静态 float 路径。
                TypePtr et = tc_.type_of(elts[i].get());
                if (src_ty && src_ty->kind == TyKind::Tuple) {
                    if (src_ty->variadic) {
                        if (!src_ty->sub.empty()) et = src_ty->sub[0];
                    } else if (i < src_ty->sub.size()) {
                        et = src_ty->sub[i];
                    }
                } else if (src_ty && src_ty->kind == TyKind::List && !src_ty->sub.empty()) {
                    et = src_ty->sub[0];
                }
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
            // os.environ["K"] = "v"：environ 是只读快照（每次访问重建），
            // 写 dict 不会 setenv —— CPython 语义做不到就显式报错，不静默失效。
            if (auto* at = dynamic_cast<Attribute*>(s.value.get())) {
                if (auto* mn = dynamic_cast<Name*>(at->value.get());
                    mn && at->attr == "environ") {
                    if (auto mod = resolve_module_alias(mn->id);
                        mod && mod->builtin && mod->name == "os") {
                        err(target.line, target.col,
                            "os.environ is a read-only snapshot; setting environment "
                            "variables is not supported by the code generator yet");
                        break;
                    }
                }
            }
            if (s.slice->kind == ExprKind::Slice) {
                ++side_effect_seq_;  // 切片赋值可能改变列表长度
                auto& sl = static_cast<SliceExpr&>(*s.slice);
                TypePtr ot = tc_.type_of(s.value.get());
                if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple &&
                            ot->kind != TyKind::Unknown)) {
                    err(target.line, target.col,
                        std::format("'{}' object does not support item assignment",
                                    type_name(ot)));
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
            if (ot && ot->kind == TyKind::Optional && ot->celem != CKind::None &&
                !ot->sub.empty() && ot->sub[0]->kind == TyKind::Int) {
                VReg base = guard_not_none(obj, "pointer to dereference");
                idx = coerce(idx, tc_.type_of(s.slice.get()), t_int());
                TypePtr et = cptr_elem_type(ot->celem);
                VReg val = coerce(v, src_ty, et);
                Inst in;
                in.op = IrOp::PtrStore;
                in.a = base;
                in.b = idx;
                in.c = val;
                in.imm = ckind_width(ot->celem);
                in.fwidth = static_cast<uint8_t>(in.imm);
                in.ty = et;
                emit(std::move(in));
                break;
            }
            if (ot && ot->kind == TyKind::Dict) {
                TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
                TypePtr vt = ot->sub.size() > 1 ? ot->sub[1] : t_unknown();
                idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
                v = coerce(v, src_ty, vt);
                if (!move_fresh(idx, kt)) retain_elem(idx, own_kind(kt));
                if (!move_fresh(v, vt)) retain_elem(v, own_kind(vt));
                call_void(RtFn::DictSet, {obj, idx, v});
                break;
            }
            if (ot && ot->kind == TyKind::Set) {
                err(target.line, target.col, "'set' object does not support item assignment");
                break;
            }
            // time.struct_time 只读（0.518）：类型检查层已在两处拦截，这里是
            // 降级层的兜底（同一句消息），保证不会走到 IndexStore 把对象当 list 改写。
            if (ot && ot->kind == TyKind::StructTime) {
                err(target.line, target.col,
                    "'time.struct_time' object is immutable (assign to a variable first)");
                break;
            }
            // 用户类：MRO 里有 __setitem__ 时按 dunder 分发，否则报错。
            // 以前直接落到 IndexStore，把对象当 PyAdamasList 解引用 -> 段错误。
            if (ot && ot->kind == TyKind::Class) {
                std::vector<TypePtr> gtys{tc_.type_of(s.slice.get()), src_ty};
                VReg g = dunder_call(obj, ot, "__setitem__", {idx, v}, nullptr, &gtys);
                if (g == kNoReg)
                    err(target.line, target.col,
                        std::format("'{}' object does not support item assignment",
                                    type_name(ot)));
                break;
            }
            // 下标**写**在 unknown 接收者上仍按机器语义（IndexStore）：本轮
            // 只堵了读（见 lower_subscript）。取证：全仓 2 处、stdlib 0 处，
            // 写在 docs/frontend-fixes.md 记为剩余缺口。
            {
                static const bool diag_unknown_index =
                    std::getenv("PYADAMAS_DIAG_UNKNOWN_INDEX") != nullptr;
                if (diag_unknown_index && (!ot || ot->kind == TyKind::Unknown))
                    std::fprintf(stderr, "DIAG unknown-index-store %s:%d\n", file_.c_str(),
                                 target.line);
            }
            // 列表有元素类型，存进去的值要跟着转
            if (ot && ot->kind == TyKind::List && !ot->sub.empty())
                v = coerce(v, src_ty, ot->sub[0]);
            // 运行时 list_set 只负责 dec 旧值；新值的 retain（以及空列表的
            // 元素种类定型）在这里做。
            bool moved = ot && ot->kind == TyKind::List && !ot->sub.empty() &&
                         move_fresh(v, ot->sub[0]);
            store_list_elem(obj, strip_optional(ot), v, moved);
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
                if (const CImportDecl* cd = tc_.cmodule(n->id)) {
                    bool handled = false;
                    for (auto& cv : cd->vars) {
                        if (cv.py_name != at.attr) continue;
                        handled = true;
                        if (cv.kind == CKind::Str || cv.kind == CKind::OptStr) {
                            err(target.line, target.col,
                                std::format("cannot assign to C string variable '{}.{}'",
                                            n->id, at.attr));
                            break;
                        }
                        v = coerce(v, src_ty, cvar_type(cv));
                        Inst si;
                        si.op = IrOp::StoreExtern;
                        si.idx = Operand::ext(extern_var_idx(cv));
                        si.a = v;
                        emit(std::move(si));
                        break;
                    }
                    if (handled) break;
                    err(target.line, target.col,
                        std::format("cannot assign to cimport attribute '{}.{}'", n->id, at.attr));
                    break;
                }
            }
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
            if (ot && (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr)) {
                const StructInfo* s = ot->st;
                const StructFieldInfo* f = s ? s->field(at.attr) : nullptr;
                if (!f) {
                    err(target.line, target.col,
                        std::format("struct '{}' has no field '{}'",
                                    s ? s->name : std::string("?"), at.attr));
                    break;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                v = coerce(v, src_ty, f->ty);
                if (f->is_array()) {
                    err(at.line, at.col,
                        std::format("cannot assign to array field '{}' as a whole; assign its "
                                    "elements instead", at.attr));
                    break;
                }
                field_store(obj, static_cast<int>(f->offset), v,
                            static_cast<uint8_t>(f->size), f->c_unsigned);
                break;
            }
            if (ot && ot->kind == TyKind::ClassObj && !ot->sub.empty()) ot = ot->sub[0];
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                // `C.x = v`：写类属性的共享存储
                if (is_class_receiver(at)) {
                    const ClassInfo* rcls = class_receiver_cls(at, ot);
                    const ClassAttrInfo* ca = rcls ? rcls->class_attr(at.attr) : nullptr;
                    if (!ca) {
                        err(target.line, target.col,
                            std::format("class '{}' has no attribute '{}'",
                                        rcls ? rcls->name : ot->cls->name, at.attr));
                        break;
                    }
                    class_attr_store(rcls, ca, v, src_ty);
                    break;
                }
                if (const PropertyInfo* p = ot->cls->property(at.attr)) {
                    ++side_effect_seq_;  // setter 可能改动容器
                    if (!p->owner || p->setter < 0 ||
                        p->setter >= static_cast<int>(p->owner->methods.size())) {
                        err(target.line, target.col,
                            std::format("property '{}' has no setter", at.attr));
                        break;
                    }
                    const MethodInfo& sm = p->owner->methods[p->setter];
                    VReg obj = expr(*at.value);
                    if (opt) obj = guard_not_none(obj, std::format("property '{}'", at.attr));
                    TypePtr vt = sm.call_sig && !sm.call_sig->pos.empty()
                                     ? sm.call_sig->pos[0].ty
                                     : t_unknown();
                    v = coerce(v, src_ty, vt);
                    Inst vc;
                    vc.op = IrOp::VCall;
                    vc.a = obj;
                    vc.imm = sm.slot;
                    vc.args = {v};
                    emit(std::move(vc));
                    break;
                }
                auto* f = ot->cls->field(at.attr);
                if (!f) {
                    // 静态没有这个字段：写进运行期动态属性表。
                    ot->cls->needs_dict = true;
                    VReg obj2 = expr(*at.value);
                    if (opt) obj2 = guard_not_none(obj2, std::format("attribute '{}'", at.attr));
                    v = coerce(v, src_ty, t_unknown());
                    call_void(RtFn::AttrSet, {obj2, const_str(at.attr), v});
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
    if (e.line > 0) cur_line_ = e.line;
    switch (e.kind) {
        case ExprKind::Constant: return lower_constant(static_cast<Constant&>(e));
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(e);
            // 存储类型与 use-site 类型可能不一致：`try` 里首次赋值的变量，
            // except 分支可能看到「未赋值/旧值」，类型检查器把它的静态类型
            // 并成 unknown（见 TypeChecker::stmt_try）。于是槽/闭包按 unknown
            // 存（float 要装箱成 CPYFLOAT），而 try 体内的使用点仍按具体类型
            // （如 float）降级。读出来必须按存储类型解盒，否则装箱字被当成
            // double 直接参与运算/打印（静默错值：打印出 5e-310 的垃圾）。
            TypePtr ty = name_ty(n.id);
            auto load_typed = [&](VReg v) {
                TypePtr use_ty = tc_.type_of(&e);
                if (ty && ty->kind == TyKind::Unknown && use_ty &&
                    use_ty->kind != TyKind::Unknown && !type_eq(ty, use_ty))
                    v = coerce(v, ty, use_ty);
                return v;
            };
            // @classmethod 的 cls：编译期就绑定成所属类。运行时不需要真值
            // （调用点传的类 id 只是占位），直接给常量再改标类型——这样既
            // 不会按堆类型做 rc，也避开了被内层闭包捕获时的 cell 问题。
            if (!st_->class_self_name.empty() && n.id == st_->class_self_name &&
                st_->class_self_ty) {
                if (st_->class_self_dynamic) {
                    int slot = st_->slots.count(n.id) ? st_->slots[n.id] : -1;
                    VReg raw = slot >= 0 ? load_slot(slot, t_int()) : const_int(0);
                    return retype(raw, mk_class_obj(
                                          mk_class(const_cast<ClassInfo*>(st_->class_self_ty))));
                }
                auto cid = class_ids_.find(st_->class_self_ty);
                VReg raw = const_int(cid == class_ids_.end() ? 0 : cid->second);
                return retype(raw, mk_class(const_cast<ClassInfo*>(st_->class_self_ty)));
            }
            if (int cs = comp_slot(n.id); cs >= 0)
                return load_slot(cs, st_->fn->slot_ty[cs]);
            // 类体命名空间：前面几行定义的类属性
            if (auto it = st_->class_body_attrs.find(n.id); it != st_->class_body_attrs.end())
                return load_global(it->second, mod_.globals[static_cast<size_t>(it->second)].ty);
            // 嵌套函数直接引用自己：取当前闭包，而不是外层绑定。
            if (st_->self_def && n.id == st_->self_def->name && !is_cell(n.id) &&
                !is_env(n.id) && !st_->slots.count(n.id)) {
                if (st_->env_slot >= 0) return load_slot(st_->env_slot, st_->self_type);
                Inst fa;
                fa.op = IrOp::FuncAddr;
                fa.imm = st_->self_idx;
                return def(st_->self_type, std::move(fa));
            }
            if (is_cell(n.id)) return load_typed(read_cell(n.id));
            if (is_env(n.id)) return load_typed(read_env(n.id));
            if (auto it = imported_classes_.find(n.id); it != imported_classes_.end())
                return imported_class_object(it->second);
            if (native_aliases_.count(n.id)) {
                err(e.line, e.col,
                    std::format("native function '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (resolve_module_alias(n.id)) {
                err(e.line, e.col, std::format("module '{}' cannot be used as a value", n.id));
                return const_none();
            }
            // 类名在值位置：取类定义点写好的运行期类对象单例。
            if (!is_local(n.id) && classes_.count(n.id)) return local_class_object(n.id);
            // 类型名在值位置：机器表示是类型名字符串（与 type(x) 同口径），
            // 不能 LoadGlobal 读一个不存在的空槽（那会让 int/float 值全为 0，
            // `t is int` 恒真）。用户自定义的同名绑定优先。
            if (TypeChecker::is_type_name(n.id) && !is_local(n.id) &&
                !st_->globals_decl.count(n.id) &&
                !(st_->scope && st_->scope->vars.count(n.id)) &&
                !(tc_.module_scope() && tc_.module_scope()->vars.count(n.id))) {
                TypePtr tt = tc_.type_of(&e);
                if (!tt || tt->kind != TyKind::TypeObj) tt = mk_typeobj(nullptr);
                return retype(const_str(n.id), tt);
            }
            // P4（lang_gaps）：内建异常类在值位置 → 运行期类对象单例。
            // 以前这里落到 LoadGlobal 读一个不存在的空槽（值退化成 0，
            // contextlib.suppress 的同一性/比较全丢）。现在发 ExcClassObj：
            // 运行期按名字缓存单例（+1 引用），静态类型 ClassObj 带 CLASS tag。
            if (opts_.lang_gaps && TypeChecker::is_exception_type(n.id) && !is_local(n.id) &&
                !st_->globals_decl.count(n.id) &&
                !(st_->scope && st_->scope->vars.count(n.id)) &&
                !(tc_.module_scope() && tc_.module_scope()->vars.count(n.id))) {
                TypePtr tt = tc_.type_of(&e);
                if (tt && tt->kind == TyKind::ClassObj) {
                    VReg obj = call_runtime(RtFn::ExcClassObj, tt, {const_str(n.id)});
                    push_temp(obj);
                    return obj;
                }
            }
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                Inst in;
                in.op = IrOp::LoadGlobal;
                in.idx = Operand::global(global_idx(n.id));
                in.ty = ty;
                return load_typed(def(ty, std::move(in)));
            }
            return load_typed(load_slot(st_->slots[n.id], ty));
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
        case ExprKind::Go: return lower_go(e);
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
                if (const CImportDecl* cd = tc_.cmodule(n->id)) {
                    for (auto& k : cd->consts) {
                        if (k.name != a.attr) continue;
                        switch (k.kind) {
                            case CKind::Float: return const_float(k.f);
                            case CKind::Bool: return const_bool(k.b);
                            case CKind::Str: return const_str(k.s);
                            case CKind::Int8:
                            case CKind::Int16:
                            case CKind::UInt8:
                            case CKind::UInt16:
                            case CKind::UInt64:
                                // 定宽常量带自己的类型（u64 大字面量存位模式），
                                // 与 ckind_type 的静态类型一致。
                                return const_int_ty(k.i, ckind_type(k.kind));
                            default: return const_int(k.i);
                        }
                    }
                    for (auto& v : cd->vars) {
                        if (v.py_name != a.attr) continue;
                        int vidx = extern_var_idx(v);
                        TypePtr vt = cvar_type(v);
                        Inst in;
                        in.op = IrOp::LoadExtern;
                        in.idx = Operand::ext(vidx);
                        in.ty = vt;
                        return def_heap(vt, std::move(in));
                    }
                    for (auto& x : cd->externs)
                        if (x.py_name == a.attr) {
                            err(e.line, e.col,
                                std::format("extern function '{}.{}' must be called", n->id,
                                            a.attr));
                            return const_none();
                        }
                    err(e.line, e.col,
                        std::format("cimport '{}' has no member '{}'", n->id, a.attr));
                    return const_none();
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    VReg bv;
                    TypePtr bty;
                    if (builtin_value(mod->name, a.attr, bv, bty)) return bv;
                    if (!mod->source_globals.count(a.attr)) {
                        err(e.line, e.col,
                            std::format("native function '{}.{}' must be called", n->id, a.attr));
                        return const_none();
                    }
                    // stdlib 扩展的全局：继续走下面的外部全局加载。
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id)) {
                    if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end())
                        return imported_class_object(ci->second);
                }
            }
            if (auto gi = module_member(a))
                return load_global(*gi, mod_.globals[*gi].ty);
            TypePtr raw = tc_.type_of(a.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::ClassObj) {
                if (a.attr == "__name__") {
                    VReg cv = expr(*a.value);
                    VReg s = call_runtime(RtFn::ClassName, t_str(), {cv});
                    push_temp(s);
                    return s;
                }
                if (!ot->sub.empty()) ot = ot->sub[0];
            }
            // P4（lang_gaps）：自定义异常类静态类型仍是 Class（名字机制），
            // 值是类对象单例；__name__ 走运行期类名。
            if (opts_.lang_gaps && ot && ot->kind == TyKind::Class && ot->cls &&
                ot->cls->is_exception_class && a.attr == "__name__") {
                VReg cv = expr(*a.value);
                VReg s = call_runtime(RtFn::ClassName, t_str(), {cv});
                push_temp(s);
                return s;
            }
            if (ot && (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr)) {
                const StructInfo* s = ot->st;
                if (!s) {
                    err(e.line, e.col, std::format("struct has no field '{}'", a.attr));
                    return const_none();
                }
                const StructFieldInfo* f = s->field(a.attr);
                if (!f) {
                    err(e.line, e.col,
                        std::format("struct '{}' has no field '{}'", s->name, a.attr));
                    return const_none();
                }
                VReg obj = expr(*a.value);
                if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                // .cpya 定长数组字段：读属性得到元素地址（不读元素值），
                // 后续 p.buf[i] / p.grid[i][j] / 把 p.buf 当 ptr 传给 C 都从
                // 这个地址出发。
                if (f->is_array()) return retype(field_addr(obj, f->offset), c_array_view_type(*f));
                // 按值嵌套的 struct/union 字段：返回字段地址（不读 8 字节当指针）。
                if (f->ty && f->ty->kind == TyKind::Struct && f->ty->st)
                    return retype(field_addr(obj, f->offset),
                                  mk_optional(mk_struct_ptr(f->ty->st)));
                return field_load(obj, static_cast<int>(f->offset), f->ty,
                                  static_cast<uint8_t>(f->size), f->c_unsigned);
            }
            if (ot && ot->kind == TyKind::Exception) {
                VReg ev = expr(*a.value);
                if (a.attr == "args")
                    return call_runtime(RtFn::ExcValueArgs, mk_tuple({t_str()}), {ev});
                if (a.attr == "__cause__")
                    return call_runtime(RtFn::ExcValueCause,
                                        mk_optional(mk_exception("Exception")), {ev});
                // 异常值只有 args / __cause__ 两个真属性。其它属性以前一律
                // retype 成 unknown 放行，降级后把异常对象指针当值用（实测
                // print(e.__traceback__) 打出 99498760223040），是静默算错。
                // 现在明确拒绝：__traceback__/__context__ 在 CPython 里是
                // 真属性但本项目没实现，消息写「未实现」；其余名字按不存在报。
                if (a.attr == "__traceback__" || a.attr == "__context__" ||
                    a.attr == "__suppress_context__")
                    err(e.line, e.col,
                        std::format("exception attribute '{}' is not implemented yet", a.attr));
                else
                    err(e.line, e.col,
                        std::format("'{}' object has no attribute '{}'", type_name(ot), a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::TypeObj) {
                VReg tv = expr(*a.value);  // interned 类型名
                if (a.attr == "__name__") return retype(tv, t_str());
                err(e.line, e.col,
                    std::format("'type' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::File) {
                VReg fv = expr(*a.value);
                if (opt) fv = guard_not_none(fv, std::format("attribute '{}'", a.attr));
                if (a.attr == "closed")
                    return call_runtime(RtFn::FileClosed, t_bool(), {fv});
                if (a.attr == "name" || a.attr == "mode" || a.attr == "encoding" ||
                    a.attr == "errors") {
                    RtFn fn = a.attr == "name"     ? RtFn::FileName
                              : a.attr == "mode"   ? RtFn::FileMode
                              : a.attr == "encoding" ? RtFn::FileEncoding
                                                     : RtFn::FileErrors;
                    VReg s = call_runtime(fn, t_str(), {fv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col,
                    std::format("'file' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::DateTime) {
                VReg dv = expr(*a.value);
                if (opt) dv = guard_not_none(dv, std::format("attribute '{}'", a.attr));
                static const std::unordered_map<std::string, int64_t> kDtFields = {
                    {"year", 0}, {"month", 1}, {"day", 2},
                    {"hour", 3}, {"minute", 4}, {"second", 5},
                };
                if (auto it = kDtFields.find(a.attr); it != kDtFields.end())
                    return call_runtime(RtFn::DateTimeField, t_int(), {dv, const_int(it->second)});
                err(e.line, e.col,
                    std::format("'datetime' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::TimeDelta) {
                VReg tv = expr(*a.value);
                if (opt) tv = guard_not_none(tv, std::format("attribute '{}'", a.attr));
                static const std::unordered_map<std::string, int64_t> kTdFields = {
                    {"days", 0}, {"seconds", 1}, {"microseconds", 2},
                };
                if (auto it = kTdFields.find(a.attr); it != kTdFields.end())
                    return call_runtime(RtFn::TimeDeltaField, t_int(),
                                        {tv, const_int(it->second)});
                err(e.line, e.col,
                    std::format("'timedelta' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Path) {
                VReg pv = expr(*a.value);
                if (opt) pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "name" || a.attr == "stem" || a.attr == "suffix") {
                    RtFn fn = a.attr == "name"    ? RtFn::PathName
                              : a.attr == "stem"  ? RtFn::PathStem
                                                  : RtFn::PathSuffix;
                    VReg s = call_runtime(fn, t_str(), {pv});
                    push_temp(s);
                    return s;
                }
                if (a.attr == "parent") {
                    VReg s = call_runtime(RtFn::PathParent, new_ty(TyKind::Path), {pv});
                    push_temp(s);
                    return s;
                }
                if (a.attr == "parts") {
                    TypePtr lt = mk_list(t_str());
                    VReg v = call_runtime(RtFn::PathParts, lt, {pv});
                    push_temp(v);
                    return v;
                }
                err(e.line, e.col,
                    std::format("'pathlib.Path' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Deque) {
                VReg dv = expr(*a.value);
                if (opt) dv = guard_not_none(dv, std::format("attribute '{}'", a.attr));
                if (a.attr == "maxlen")
                    return call_runtime(RtFn::DequeMaxlen, t_int(), {dv});
                err(e.line, e.col,
                    std::format("'collections.deque' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::CompletedProcess) {
                VReg pv = expr(*a.value);
                if (opt)
                    pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "returncode")
                    return call_runtime(RtFn::SubprocessReturnCode, t_int(), {pv});
                if (a.attr == "stdout" || a.attr == "stderr") {
                    RtFn fn = a.attr == "stdout" ? RtFn::SubprocessStdout
                                                 : RtFn::SubprocessStderr;
                    VReg s = call_runtime(fn, t_str(), {pv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col,
                    std::format("'CompletedProcess' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::FloatInfo) {
                (void)expr(*a.value);
                // 三个 float 属性是编译期常量（<float.h> 的 DBL_*，与 CPython 同 libc，
                // 十进制字面量就是它们的最短往返表示）——直接发常量，避免 f64 返回值的
                // 运行期调用编组（实测那条路在 asm 里 movsd 操作数不匹配）。
                if (a.attr == "epsilon") return const_float(2.220446049250313e-16);
                if (a.attr == "max") return const_float(1.7976931348623157e+308);
                if (a.attr == "min") return const_float(2.2250738585072014e-308);
                static const char* kFI[] = {"dig",     "mant_dig",   "max_exp", "max_10_exp",
                                            "min_exp", "min_10_exp", "radix",   "rounds"};
                for (int i = 0; i < 8; ++i)
                    if (a.attr == kFI[i])
                        return call_runtime(RtFn::SysFloatInfoI, t_int(), {const_int(i)});
                err(e.line, e.col,
                    std::format("'sys.float_info' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::SysFlags) {
                (void)expr(*a.value);
                static const char* kFL[] = {"debug",         "inspect",       "interactive",
                                            "optimize",      "dont_write_bytecode",
                                            "no_user_site", "no_site",       "ignore_environment",
                                            "verbose",      "bytes_warning", "quiet",
                                            "hash_randomization", "isolated",  "dev_mode",
                                            "utf8_mode"};
                for (int i = 0; i < 15; ++i)
                    if (a.attr == kFL[i])
                        return call_runtime(RtFn::SysFlagField, t_int(), {const_int(i)});
                err(e.line, e.col,
                    std::format("'sys.flags' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::UnameInfo) {
                VReg uv = expr(*a.value);
                if (opt) uv = guard_not_none(uv, std::format("attribute '{}'", a.attr));
                static const char* kUnameFields[] = {"sysname", "nodename", "release",
                                                    "version", "machine"};
                (void)uv;
                for (int i = 0; i < 5; ++i) {
                    if (a.attr == kUnameFields[i]) {
                        VReg s = call_runtime(RtFn::OsUnameField, t_str(), {const_int(i)});
                        push_temp(s);
                        return s;
                    }
                }
                err(e.line, e.col,
                    std::format("'posix.uname_result' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::StructTime) {
                VReg tv = expr(*a.value);
                if (opt) tv = guard_not_none(tv, std::format("attribute '{}'", a.attr));
                static const char* kTmFields[] = {"tm_year", "tm_mon",  "tm_mday", "tm_hour",
                                                 "tm_min",  "tm_sec",  "tm_wday", "tm_yday",
                                                 "tm_isdst"};
                for (int i = 0; i < 9; ++i) {
                    if (a.attr == kTmFields[i])
                        return call_runtime(RtFn::ListGet, t_int(), {tv, const_int(i)});
                }
                err(e.line, e.col,
                    std::format("'time.struct_time' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::StatResult) {
                VReg sv = expr(*a.value);
                if (opt) sv = guard_not_none(sv, std::format("attribute '{}'", a.attr));
                static const char* kStatFields[] = {"st_mode",  "st_ino",  "st_dev",   "st_nlink",
                                                    "st_uid",   "st_gid",  "st_size",  "st_atime",
                                                    "st_mtime", "st_ctime"};
                for (int i = 0; i < 10; ++i) {
                    if (a.attr == kStatFields[i])
                        return call_runtime(RtFn::ListGet, t_int(), {sv, const_int(i)});
                }
                err(e.line, e.col,
                    std::format("'os.stat_result' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Popen) {
                VReg pv = expr(*a.value);
                if (opt) pv = guard_not_none(pv, std::format("attribute '{}'", a.attr));
                if (a.attr == "pid") return call_runtime(RtFn::PopenPid, t_int(), {pv});
                if (a.attr == "returncode")
                    return call_runtime(RtFn::PopenReturnCode, t_int(), {pv});
                if (a.attr == "stdout" || a.attr == "stderr" || a.attr == "stdin") {
                    RtFn fn = a.attr == "stdout"   ? RtFn::PopenStdout
                              : a.attr == "stderr" ? RtFn::PopenStderr
                                                   : RtFn::PopenStdin;
                    VReg f = call_runtime(fn, t_file(), {pv});
                    push_temp(f);
                    return f;
                }
                err(e.line, e.col,
                    std::format("'Popen' object has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Regex) {
                VReg rv = expr(*a.value);
                if (opt) rv = guard_not_none(rv, std::format("attribute '{}'", a.attr));
                if (a.attr == "groups")
                    return call_runtime(RtFn::RegexPatternGroups, t_int(), {rv});
                if (a.attr == "flags")
                    return call_runtime(RtFn::RegexPatternFlags, t_int(), {rv});
                if (a.attr == "pattern") {
                    VReg s = call_runtime(RtFn::RegexPatternStr, t_str(), {rv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col, std::format("re.Pattern has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Match) {
                VReg mv = expr(*a.value);
                if (opt) mv = guard_not_none(mv, std::format("attribute '{}'", a.attr));
                if (a.attr == "lastindex")
                    return call_runtime(RtFn::RegexLastIndex, t_int(), {mv});
                if (a.attr == "lastgroup") {
                    VReg s = call_runtime(RtFn::RegexLastGroup, t_str(), {mv});
                    push_temp(s);
                    return s;
                }
                err(e.line, e.col, std::format("re.Match has no attribute '{}'", a.attr));
                return const_none();
            }
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                // 类接收者：`C.x` / `cls.x` 读的是类属性那份共享存储，
                // 接收者本身不是可求值的值，不能走下面的实例路径。
                if (is_class_receiver(a)) {
                    const ClassInfo* rcls = class_receiver_cls(a, ot);
                    if (rcls) {
                        if (const ClassAttrInfo* ca = rcls->class_attr(a.attr))
                            return class_attr_load(rcls, ca);
                        if (rcls->method(a.attr)) return lower_method_value(a);
                        if (rcls->field(a.attr))
                            err(e.line, e.col,
                                std::format("'{}.{}' is an instance field, not a class attribute",
                                            rcls->name, a.attr));
                        else
                            err(e.line, e.col,
                                std::format("class '{}' has no attribute '{}'", rcls->name,
                                            a.attr));
                        return const_none();
                    }
                }
                if (const PropertyInfo* p = ot->cls->property(a.attr)) {
                    ++side_effect_seq_;  // getter 可能改动容器
                    if (!p->owner || p->getter < 0 ||
                        p->getter >= static_cast<int>(p->owner->methods.size())) {
                        err(e.line, e.col,
                            std::format("property '{}' has no getter", a.attr));
                        return const_none();
                    }
                    const MethodInfo& gm = p->owner->methods[p->getter];
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("property '{}'", a.attr));
                    Inst vc;
                    vc.op = IrOp::VCall;
                    vc.a = obj;
                    vc.imm = gm.slot;
                    TypePtr rt = gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret
                                                                 : t_unknown();
                    return def_heap(rt, std::move(vc));
                }
                if (auto* f = ot->cls->field(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    int off = static_cast<int>(8 * (1 + ot->cls->field_index(a.attr)));
                    return field_load(obj, off, f->ty);
                }
                // 实例字段没有，回退到类属性（Python 的实例查找顺序）。
                // 装饰方法在类属性里也有包装函数，但方法值要绑定接收者，
                // 必须按方法处理。
                if (ot->cls->method(a.attr)) return lower_method_value(a);
                if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    return class_attr_load_dyn(obj, ot->cls, ca);
                }
                // 静态没有这个属性：查运行期动态属性表。
                ot->cls->needs_dict = true;
                VReg obj2 = expr(*a.value);
                if (opt) obj2 = guard_not_none(obj2, std::format("attribute '{}'", a.attr));
                return call_runtime(RtFn::AttrGet, t_unknown(),
                                   {obj2, const_str(a.attr), const_int(0)});
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


}  // namespace pyadamas
