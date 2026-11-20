// lower_class.cpp —— 类属性、类定义与构造包装
#include "lower_impl.h"

#include "opt.h"

namespace pyadamas {

int Lowerer::class_attr_global(const ClassInfo* owner, const std::string& name, TypePtr ty) {
    auto& m = class_attr_globals_[owner];
    auto it = m.find(name);
    if (it != m.end()) {
        // 先用到后定义（方法体里读类属性）：把后来才知道的类型补上
        int gi = it->second;
        if (ty && mod_.globals[static_cast<size_t>(gi)].ty &&
            mod_.globals[static_cast<size_t>(gi)].ty->kind == TyKind::Unknown)
            mod_.globals[static_cast<size_t>(gi)].ty = ty;
        return gi;
    }
    std::string sym = std::format("__pa_cattr_{}_{}", owner->name, name);
    int gi = global_idx_as(sym, ty);
    m.emplace(name, gi);
    return gi;
}

// 类属性的存储位置。
//   声明者：自己就是唯一那份，没有标志位。
//   赋过值的子类：有自己的槽 + 「已拥有」标志；标志为 0 时读父类那份。
//   其余子类：直接读声明者那份（Python 的类属性继承就是这个语义）。
Lowerer::ClassAttrStorage Lowerer::class_attr_storage(const ClassInfo* recv,
                                                      const ClassAttrInfo* ca) {
    ClassAttrStorage st;
    if (!recv || recv == ca->owner) {
        st.slot = class_attr_global(ca->owner, ca->name, ca->ty);
        return st;
    }
    if (recv->cowed_attrs.count(ca->name)) {
        st.slot = class_attr_global(recv, ca->name, ca->ty);
        std::string sym = std::format("__pa_cattr_own_{}_{}", recv->name, ca->name);
        st.flag = global_idx_as(sym, t_int());
        st.parent = class_attr_storage(ca->owner, ca).slot;
        return st;
    }
    return class_attr_storage(ca->owner, ca);
}

int Lowerer::class_attr_slot(const ClassInfo* recv, const ClassAttrInfo* ca) {
    return class_attr_storage(recv, ca).slot;
}

// 读类属性：有标志位的类要先看「自己那份」是否已经存在。
VReg Lowerer::class_attr_load(const ClassInfo* recv, const ClassAttrInfo* ca) {
    ClassAttrStorage st = class_attr_storage(recv, ca);
    if (st.flag < 0) return load_global(st.slot, ca->ty);

    int tmp = new_borrow_slot(ca->ty);
    int own = new_block("cattr.own");
    int parent = new_block("cattr.parent");
    int end = new_block("cattr.end");

    VReg f = load_global(st.flag, t_int());
    Inst br;
    br.op = IrOp::Branch;
    br.a = f;
    br.target = own;
    br.target2 = parent;
    emit(std::move(br));

    switch_to(own);
    store_borrow_slot(tmp, load_global(st.slot, ca->ty));
    Inst j1;
    j1.op = IrOp::Jump;
    j1.target = end;
    emit(std::move(j1));

    switch_to(parent);
    store_borrow_slot(tmp, load_global(st.parent, ca->ty));
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));

    switch_to(end);
    return load_slot(tmp, ca->ty);
}

// 写类属性：通过子类第一次写时置位标志，从此读的就是本类这一份。
void Lowerer::class_attr_store(const ClassInfo* recv, const ClassAttrInfo* ca, VReg v,
                               TypePtr vty) {
    ClassAttrStorage st = class_attr_storage(recv, ca);
    v = coerce(v, vty, ca->ty);
    store_global(st.slot, v);
    if (st.flag >= 0) store_global(st.flag, const_int(1));
}

// 实例读类属性：静态类型可能只是基类，值却是子类，所以要按运行期类号
// 派发。没有任何子类覆盖这个属性时退化成一次直接读，不产生额外开销。
VReg Lowerer::class_attr_load_dyn(VReg obj, const ClassInfo* stat, const ClassAttrInfo* ca) {
    std::vector<std::pair<const ClassInfo*, const ClassAttrInfo*>> overrides;
    for (auto& [name, ptr] : classes_) {
        const ClassInfo* k = ptr.get();
        if (!k || k == stat) continue;
        bool derived = false;
        for (const ClassInfo* m : k->mro)
            if (m == stat) { derived = true; break; }
        if (!derived) continue;
        const ClassAttrInfo* kca = k->class_attr(ca->name);
        if (!kca) continue;
        // 覆盖了才算：自己声明，或者通过子类赋过值（有独立的槽）
        if (kca->owner == ca->owner && !k->cowed_attrs.count(ca->name)) continue;
        overrides.push_back({k, kca});
    }
    if (overrides.empty()) return class_attr_load(stat, ca);

    int tmp = new_borrow_slot(ca->ty);
    int end = new_block("cattr.dyn.end");
    VReg cid = call_runtime(RtFn::ObjClassId, t_int(), {obj});
    for (auto& [k, kca] : overrides) {
        auto idit = class_ids_.find(k);
        if (idit == class_ids_.end()) continue;
        int hit = new_block("cattr.dyn.hit");
        int next = new_block("cattr.dyn.next");

        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        cmp.a = cid;
        cmp.b = const_int(idit->second);
        VReg eq = def(t_bool(), std::move(cmp));

        Inst br;
        br.op = IrOp::Branch;
        br.a = eq;
        br.target = hit;
        br.target2 = next;
        emit(std::move(br));

        switch_to(hit);
        store_borrow_slot(tmp, class_attr_load(k, kca));
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));

        switch_to(next);
    }
    // 所有子类都没命中：用静态类型那一份
    store_borrow_slot(tmp, class_attr_load(stat, ca));
    Inst jd;
    jd.op = IrOp::Jump;
    jd.target = end;
    emit(std::move(jd));

    switch_to(end);
    return load_slot(tmp, ca->ty);
}

// `C.x` / `cls.x`：接收者是类本身。类名不是可求值的值，属性访问必须
// 在降级阶段就分流，不能先 expr(接收者)。
bool Lowerer::is_class_receiver(const Attribute& a) {
    auto* n = dynamic_cast<Name*>(a.value.get());
    if (!n) return false;
    if (!st_->class_self_name.empty() && n->id == st_->class_self_name) return true;
    if (is_bound_here(n->id)) return false;
    return classes_.count(n->id) > 0;
}

// 类接收者对应的类。@classmethod 的 cls 要用本帧静态绑定的那个类，
// 不能信类型检查记下的「声明它的类」——子类特化副本里那已经过时了。
const ClassInfo* Lowerer::class_receiver_cls(const Attribute& a, TypePtr ot) {
    if (auto* n = dynamic_cast<Name*>(a.value.get())) {
        if (!st_->class_self_name.empty() && n->id == st_->class_self_name)
            return st_->class_self_ty;
        if (!is_bound_here(n->id)) {
            auto it = classes_.find(n->id);
            if (it != classes_.end() && it->second) return it->second.get();
        }
    }
    return (ot && ot->kind == TyKind::Class) ? ot->cls : nullptr;
}

void Lowerer::lower_classdef(ClassDef& c) {
    auto it = classes_.find(c.name);
    if (it == classes_.end()) return;
    const ClassInfoPtr& ci = it->second;
    auto cid_it = class_ids_.find(ci.get());
    if (cid_it == class_ids_.end()) {
        unsupported(c, "'class' inside a function");
        return;
    }
    int cid = cid_it->second;
    if (ci->is_exception_class) {
        // 注册父子关系供 except 按类型匹配。富异常（有字段/方法）另外走
        // 下面的普通类布局建 vtable/字段；pass-only 保持扁平。
        if (!c.decorators.empty()) {
            err(c.line, c.col, "class decorators on exception classes are not supported yet");
            return;
        }
        call_void(RtFn::ExcRegister, {const_str(ci->name), const_str(ci->exc_parent)});
        if (!ci->is_rich_exception) return;
        // 富异常：继续建 vtable 实例布局。
    }
    // 类体语句在下面按执行语义降级；类命名空间未建模，赋值走模块全局。

    // 按 (name, kind) 汇总 MRO 上所有槽：不同模块可能给同名方法分了不同
    // 槽，一个实现要写进该 key 的全部槽；不同 key 抢同一个槽才是冲突。
    struct SlotPlan {
        std::vector<int> slots;
        bool own = false;
        bool has_winner = false;
        int fidx = -1;
        std::string ext;
    };
    std::map<std::string, SlotPlan> plans;
    std::map<int, std::string> slot_owner;
    auto& cv = mod_.classes[cid].vtable;
    auto& cext = mod_.classes[cid].vtable_ext;
    auto add_slot = [&](SlotPlan& p, int slot, const std::string& key) {
        if (slot < 0 || slot >= static_cast<int>(cv.size())) return;
        auto oit = slot_owner.find(slot);
        if (oit != slot_owner.end() && oit->second != key) {
            err(c.line, c.col,
                std::format("method slot {} is used by both '{}' and '{}'", slot, oit->second,
                            key));
            return;
        }
        slot_owner[slot] = key;
        if (std::find(p.slots.begin(), p.slots.end(), slot) == p.slots.end())
            p.slots.push_back(slot);
    };
    for (size_t mi = 1; mi < ci->mro.size(); ++mi) {
        const ClassInfo* k = ci->mro[mi];
        if (!k) continue;
        auto kit = class_ids_.find(k);
        for (const MethodInfo& km : k->methods) {
            if (km.slot < 0) continue;
            std::string key = method_slot_key(km.name, km.kind);
            SlotPlan& p = plans[key];
            add_slot(p, km.slot, key);
            for (int a : km.aliases) add_slot(p, a, key);
            if (p.has_winner) continue;
            if (kit != class_ids_.end()) {
                const auto& kv = mod_.classes[kit->second].vtable;
                const auto& ke = mod_.classes[kit->second].vtable_ext;
                if (km.slot < static_cast<int>(kv.size()) && kv[km.slot] >= 0) {
                    p.fidx = kv[km.slot];
                    p.has_winner = true;
                } else if (km.slot < static_cast<int>(ke.size()) && !ke[km.slot].empty()) {
                    p.ext = ke[km.slot];
                    p.has_winner = true;
                }
            } else if (k->imported()) {
                p.ext = module_class_method_symbol(k->origin_module, k->external_name(),
                                                   km.slot);
                p.has_winner = true;
            }
        }
    }
    // 本类声明的方法：同一 key 的全部槽稍后都写自己的实现。
    for (auto& m : ci->methods) {
        if (m.slot < 0 || m.decorated) continue;
        std::string key = method_slot_key(m.name, m.kind);
        SlotPlan& p = plans[key];
        add_slot(p, m.slot, key);
        for (int a : m.aliases) add_slot(p, a, key);
        p.own = true;
    }
    // 先填继承来的 winner。
    for (auto& [key, p] : plans) {
        if (p.own || !p.has_winner) continue;
        for (int slot : p.slots) {
            if (p.fidx >= 0) {
                cv[slot] = p.fidx;
                if (slot < static_cast<int>(cext.size())) cext[slot].clear();
            } else if (!p.ext.empty()) {
                cext[slot] = p.ext;
            }
        }
    }

    // 自定义装饰器方法：方法体照常编译，但 vtable 槽不填；类创建时把
    // 装饰器链的结果写进类属性，调用点走类属性的间接调用。
    std::unordered_map<std::string, int> decorated_methods;
    std::map<std::string, int> own_fidx;
    for (auto& m : ci->methods) {
        if (!m.def) continue;
        if (!m.decorated && !m.def->decorators.empty() && m.kind == MethodKind::Normal) {
            err(m.def->line, m.def->col,
                "only @staticmethod / @property method decorators are supported");
            continue;
        }
        std::string label = std::format("{}_{}_{}", c.name, m.name, mod_.funcs.size());
        int fidx = compile_function_body(*m.def, m.sig, label, ci.get());
        if (m.decorated) {
            decorated_methods[m.name] = fidx;
        } else if (m.slot >= 0 &&
                   m.slot < static_cast<int>(mod_.classes[cid].vtable.size())) {
            mod_.classes[cid].vtable[static_cast<size_t>(m.slot)] = fidx;
            if (m.slot < static_cast<int>(mod_.classes[cid].vtable_ext.size()))
                mod_.classes[cid].vtable_ext[static_cast<size_t>(m.slot)].clear();
            own_fidx[method_slot_key(m.name, m.kind)] = fidx;
        }
    }

    // 继承来的 @classmethod 按本类再编一份：cls 静态绑定到本类，所以
    // Sub.make() 构造的是 Sub 而不是 Base。不这么做的话，所有子类
    // 共用基类那份实现，工厂方法就永远返回基类实例。
    for (const ClassInfo* k : ci->mro) {
        if (k == ci.get()) continue;
        for (const MethodInfo& km : k->methods) {
            if (km.kind != MethodKind::ClassMethod || !km.def || km.slot < 0) continue;
            // 被更靠前的类（含本类自己）覆盖了，不重复编
            if (ci->method(km.name) != &km) continue;
            if (km.slot >= static_cast<int>(mod_.classes[cid].vtable.size())) continue;
            std::string label = std::format("{}_{}_{}", c.name, km.name, mod_.funcs.size());
            int fidx = compile_function_body(*km.def, km.sig, label, ci.get());
            mod_.classes[cid].vtable[static_cast<size_t>(km.slot)] = fidx;
            if (km.slot < static_cast<int>(mod_.classes[cid].vtable_ext.size()))
                mod_.classes[cid].vtable_ext[static_cast<size_t>(km.slot)].clear();
            std::string key = method_slot_key(km.name, km.kind);
            SlotPlan& p = plans[key];
            add_slot(p, km.slot, key);
            p.own = true;
            own_fidx[key] = fidx;
        }
    }
    // 本类实现写到该 key 的全部别名槽，并刷新 own_slots（super 依赖它）。
    for (auto& [key, p] : plans) {
        if (!p.own) continue;
        auto fit = own_fidx.find(key);
        if (fit == own_fidx.end()) continue;
        for (int slot : p.slots) {
            cv[slot] = fit->second;
            if (slot < static_cast<int>(cext.size())) cext[slot].clear();
        }
    }
    {
        auto& os = mod_.classes[cid].own_slots;
        os.clear();
        for (auto& [key, p] : plans) {
            if (!p.own || !own_fidx.count(key)) continue;
            for (int slot : p.slots) os.push_back(slot);
        }
    }

    bool need_class_obj = ci->value_used || !c.decorators.empty();
    // classmethod/staticmethod 调用可能要把类对象当接收者传；即使本类
    // 没有覆盖，也从 MRO 里继承到了这些方法。
    if (!need_class_obj) {
        for (const ClassInfo* k : ci->mro) {
            if (!k) continue;
            for (const MethodInfo& m : k->methods)
                if (m.kind == MethodKind::ClassMethod || m.kind == MethodKind::Static) {
                    need_class_obj = true;
                    break;
                }
            if (need_class_obj) break;
        }
    }
    if ((!opts_.module_symbol.empty() && !opts_.is_entry) || need_class_obj)
        compile_ctor_wrapper(c, ci, cid);

    // 类体里的非声明语句：在类定义点执行。普通赋值是类属性，写进隐藏
    // 全局（方法体里读到的也是这一份）；其余语句仍然按模块全局处理。
    st_->class_body_attrs.clear();
    for (auto& s : c.body) {
        if (s->kind == StmtKind::FunctionDef) {
            auto& f = static_cast<FunctionDef&>(*s);
            auto dit = decorated_methods.find(f.name);
            if (dit == decorated_methods.end()) continue;
            // 自定义装饰器方法：生成函数值（含闭包捕获），按从下到上的
            // 顺序应用装饰器，结果写进类属性槽；调用点通过类属性间接调用。
            const MethodInfo* m = ci->own_method(f.name);
            const ClassAttrInfo* ca = ci->class_attr(f.name);
            if (!m || !ca) continue;
            TypePtr fty = new_ty(TyKind::Func);
            fty->fn = m->sig;
            auto cmit = closure_meta_.find(&f);
            const ClosureMeta* meta = cmit == closure_meta_.end() ? nullptr : &cmit->second;
            VReg cur;
            if (!meta || meta->env.empty()) {
                Inst fa;
                fa.op = IrOp::FuncAddr;
                fa.imm = dit->second;
                cur = def(fty, std::move(fa));
            } else {
                Inst fc;
                fc.op = IrOp::FuncCode;
                fc.imm = dit->second;
                VReg code = def(t_int(), std::move(fc));
                cur = call_runtime(RtFn::ClosureNew, fty,
                                   {code, const_int(static_cast<int64_t>(meta->env.size()))});
                push_temp(cur);
                for (size_t i = 0; i < meta->env.size(); ++i) {
                    VReg cell = cell_for_capture(meta->env[i].name);
                    if (cell == kNoReg) {
                        err(f.line, f.col,
                            std::format("closure capture of '{}' is not available here",
                                        meta->env[i].name));
                        continue;
                    }
                    rc_inc(cell);
                    field_store(cur, 16 + 8 * static_cast<int>(i), cell);
                }
            }
            TypePtr cur_ty = fty;
            std::string hidden = "__pyadamas$deco$" + f.name;
            bind_name_value(hidden, cur, cur_ty);
            for (auto dit2 = f.decorators.rbegin(); dit2 != f.decorators.rend(); ++dit2) {
                if (is_wraps_decorator(dit2->get())) continue;
                Expr* de = dit2->get();
                int line = de->line;
                int col = de->col;
                VReg callee = expr(*de);
                TypePtr dt = tc_.type_of(de);
                FuncInfoPtr dinfo = (dt && dt->kind == TyKind::Func) ? dt->fn : nullptr;
                if (!dinfo) {
                    err(line, col, "decorator must be a function value with a known signature");
                    continue;
                }
                Call dc;
                dc.line = line;
                dc.col = col;
                auto arg = std::make_unique<Name>(hidden);
                arg->line = line;
                arg->col = col;
                dc.args.push_back(std::move(arg));
                std::vector<VReg> dargs = bind_args(dc, dinfo, nullptr);
                TypePtr rty = dinfo->ret ? dinfo->ret : t_unknown();
                Inst in;
                in.op = IrOp::Call;
                in.a = callee;
                in.args = std::move(dargs);
                cur = def_heap(rty, std::move(in));
                cur_ty = rty;
                bind_name_value(hidden, cur, cur_ty);
            }
            class_attr_store(ci.get(), ca, cur, cur_ty);
            continue;
        }
        if (s->kind == StmtKind::AnnAssign || s->kind == StmtKind::Pass) continue;
        if (s->kind == StmtKind::Expr) {
            auto& e = static_cast<ExprStmt&>(*s);
            if (e.value->kind == ExprKind::Constant &&
                static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                continue;
        }
        if (s->kind == StmtKind::Assign) {
            auto& as = static_cast<Assign&>(*s);
            if (as.targets.size() == 1 && as.targets[0]->kind == ExprKind::Name) {
                auto& nm = static_cast<Name&>(*as.targets[0]);
                const ClassAttrInfo* ca = ci->class_attr(nm.id);
                if (ca && ca->owner == ci.get()) {
                    st_->class_body_attrs[nm.id] = class_attr_slot(ci.get(), ca);
                    class_attr_store(ci.get(), ca, expr(*as.value),
                                     tc_.type_of(as.value.get()));
                    continue;
                }
            }
        }
        stmt(*s);
    }
    st_->class_body_attrs.clear();

    if (need_class_obj) {
        TypePtr cls_ty = mk_class(ci.get());
        TypePtr obj_ty = mk_class_obj(cls_ty);
        int ngi = global_idx("__pyadamas$new$" + ci->name);
        VReg ctor = load_global(ngi, mod_.globals[static_cast<size_t>(ngi)].ty);
        Inst ca;
        ca.op = IrOp::ClassAddr;
        ca.imm = cid;
        VReg vtable = def(t_int(), std::move(ca));
        VReg obj = call_runtime(RtFn::ClassNew, obj_ty,
                                {const_int(cid), ctor, vtable,
                                 const_str(mod_.runtime_name + "." + ci->name)});
        push_temp(obj);
        if (!c.decorators.empty()) {
            std::string hidden = "__pyadamas$classdeco$" + ci->name;
            bind_name_value(hidden, obj, obj_ty);
            for (auto dit = c.decorators.rbegin(); dit != c.decorators.rend(); ++dit) {
                VReg callee = expr(**dit);
                TypePtr dt = tc_.type_of(dit->get());
                FuncInfoPtr dinfo = (dt && dt->kind == TyKind::Func) ? dt->fn : nullptr;
                Inst in;
                in.op = IrOp::Call;
                in.a = callee;
                if (dinfo) {
                    Call dc;
                    dc.line = (*dit)->line;
                    dc.col = (*dit)->col;
                    auto arg = std::make_unique<Name>(hidden);
                    arg->line = dc.line;
                    arg->col = dc.col;
                    dc.args.push_back(std::move(arg));
                    in.args = bind_args(dc, dinfo, nullptr);
                } else {
                    in.args.push_back(obj);
                }
                TypePtr rty = dinfo && dinfo->ret ? dinfo->ret : t_unknown();
                if (rty && rty->kind != TyKind::ClassObj && rty->kind != TyKind::Unknown) {
                    err((*dit)->line, (*dit)->col, "class decorator must return a class");
                    continue;
                }
                obj = def_heap(rty, std::move(in));
                bind_name_value(hidden, obj, rty);
            }
        }
        int gi = global_idx_as("__pyadamas$class$" + ci->name, obj_ty);
        store_global(gi, obj);
    }
}

int Lowerer::compile_ctor_wrapper(ClassDef& clsdef, const ClassInfoPtr& ci, int cid) {
    (void)clsdef;
    const MethodInfo* init = ci->method("__init__");
    FuncInfoPtr sig;
    if (init && init->call_sig) sig = init->call_sig;
    auto ctor = std::make_shared<FuncInfo>();
    ctor->name = "__new_" + ci->name;
    if (sig) {
        ctor->pos = sig->pos;
        ctor->n_posonly = sig->n_posonly;
        ctor->kwonly = sig->kwonly;
        ctor->varargs = sig->varargs;
        ctor->vararg_name = sig->vararg_name;
        ctor->vararg_ty = sig->vararg_ty;
        ctor->kwargs = sig->kwargs;
        ctor->kwarg_name = sig->kwarg_name;
        ctor->kwarg_ty = sig->kwarg_ty;
    }
    TypePtr cls_ty = mk_class(ci.get());
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ctor;

    std::string label = std::format("__pa_new_{}_{}", ci->name, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = cls_ty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<int> arg_slots;
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        st_->slots[p.name] = s;
        arg_slots.push_back(s);
    };
    if (sig) {
        for (auto& p : sig->pos) add_param(p);
        for (auto& p : sig->kwonly) add_param(p);
        // *args/**kwargs 在 ABI 上是额外的 list/dict 形参，导入方
        // bind_args 已经把它们物化好，包装层只负责原样转发。
        if (sig->varargs) {
            Param p;
            p.name = sig->vararg_name.empty() ? "__args" : sig->vararg_name;
            p.ty = mk_list(sig->vararg_ty ? sig->vararg_ty : t_unknown());
            add_param(p);
        }
        if (sig->kwargs) {
            Param p;
            p.name = sig->kwarg_name.empty() ? "__kwargs" : sig->kwarg_name;
            p.ty = mk_dict(t_str(), sig->kwarg_ty ? sig->kwarg_ty : t_unknown());
            add_param(p);
        }
    }
    inc_params();

    Inst on;
    on.op = IrOp::ObjNew;
    on.imm = cid;
    VReg obj = def_heap(cls_ty, std::move(on));

    std::vector<const ClassInfo*> chain;
    for (auto* p = ci.get(); p; p = p->base) chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (auto& f : (*it)->fields) {
            if (!f.has_default) continue;
            VReg v;
            if (f.default_lit)
                v = const_literal(*f.default_lit);
            else if (f.default_expr)
                v = expr(*f.default_expr);
            else {
                err(clsdef.line, clsdef.col,
                    std::format("non-literal default of imported field '{}' is not supported yet",
                                f.name));
                continue;
            }
            v = coerce(v, vreg_ty(v), f.ty);
            int off = static_cast<int>(8 * (1 + ci->field_index(f.name)));
            store_field(f.ty, obj, off, v);
        }
    }

    if (sig && init->slot >= 0) {
        std::vector<VReg> args;
        for (int s : arg_slots) args.push_back(load_slot(s, wf->slot_ty[s]));
        TypePtr rt = sig->ret ? sig->ret : t_none();
        if (init->decorated) {
            // 装饰后的 __init__ 存在类属性里：走与普通构造一致的分派。
            VReg r = dunder_call(obj, cls_ty, "__init__", std::move(args), rt);
            if (r == kNoReg) {
                // 构造包装函数没有源码位置，错误挂到类定义处。
                err(clsdef.line, clsdef.col,
                    std::format("decorated __init__ of '{}' has an incompatible signature",
                                ci->name));
            }
        } else {
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = obj;
            vc.imm = init->slot;
            vc.args = std::move(args);
            VReg r = def_heap(rt, std::move(vc));
            (void)r;
        }
    }
    emit_return(obj, cls_ty);
    finalize_param_borrows();
    finalize_borrow_slots();
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);
    pop_frame();

    // 模块顶层绑定隐藏全局：导入方按 ctor_symbol 加载闭包。
    IRGlobal g;
    g.name = "__pyadamas$new$" + ci->name;
    g.symbol = module_class_new_symbol(opts_.module_symbol, ci->name);
    g.ty = fty;
    int gi = static_cast<int>(mod_.globals.size());
    mod_.globals.push_back(std::move(g));
    global_ids_["__pyadamas$new$" + ci->name] = gi;
    Inst fa;
    fa.op = IrOp::FuncAddr;
    fa.imm = widx;
    VReg fv = def(fty, std::move(fa));
    store_global(gi, fv);
    return widx;
}


}  // namespace pyadamas
