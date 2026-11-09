// lower.cpp —— 降级驱动：预扫描、入口、函数/类编译
#include "lower_impl.h"

namespace py23 {

// ────────────────────────── 预扫描 ──────────────────────────

void Lowerer::bind_name(Expr& target) {
    switch (target.kind) {
        case ExprKind::Name:
            global_idx(static_cast<Name&>(target).id);
            break;
        case ExprKind::Tuple:
        case ExprKind::List:
            for (auto& e : (target.kind == ExprKind::Tuple
                                ? static_cast<TupleExpr&>(target).elts
                                : static_cast<ListExpr&>(target).elts))
                if (e) bind_name(*e);
            break;
        case ExprKind::Starred:
            bind_name(*static_cast<Starred&>(target).value);
            break;
        default:
            break;
    }
}

void Lowerer::scan_globals(Stmt& s, bool enter_functions) {
    switch (s.kind) {
        case StmtKind::Assign:
            for (auto& t : static_cast<Assign&>(s).targets) bind_name(*t);
            break;
        case StmtKind::AnnAssign:
        case StmtKind::AugAssign:
            bind_name(*static_cast<AnnAssign&>(s).target);
            break;
        case StmtKind::For: {
            auto& f = static_cast<For&>(s);
            bind_name(*f.target);
            for (auto& x : f.body) scan_globals(*x, enter_functions);
            for (auto& x : f.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<While&>(s);
            for (auto& x : w.body) scan_globals(*x, enter_functions);
            for (auto& x : w.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::If: {
            auto& f = static_cast<If&>(s);
            for (auto& b : f.branches)
                for (auto& x : b.body) scan_globals(*x, enter_functions);
            for (auto& x : f.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::Try: {
            auto& t = static_cast<Try&>(s);
            for (auto& x : t.body) scan_globals(*x, enter_functions);
            for (auto& h : t.handlers) {
                if (!h.name.empty()) global_idx(h.name);
                for (auto& x : h.body) scan_globals(*x, enter_functions);
            }
            for (auto& x : t.orelse) scan_globals(*x, enter_functions);
            for (auto& x : t.finalbody) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::With: {
            auto& w = static_cast<With&>(s);
            for (auto& it : w.items)
                if (it.optional_vars) bind_name(*it.optional_vars);
            for (auto& x : w.body) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::FunctionDef: {
            auto& f = static_cast<FunctionDef&>(s);
            global_idx(f.name);
            if (enter_functions)
                for (auto& x : f.body) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::ClassDef:
            global_idx(static_cast<ClassDef&>(s).name);
            break;
        case StmtKind::Import:
            // import m 只绑定编译期模块别名，不占运行时全局槽。
            break;
        case StmtKind::ImportFrom:
            for (auto& a : static_cast<ImportFrom&>(s).names)
                if (a.name != "*") global_idx(a.asname.empty() ? a.name : a.asname);
            break;
        default:
            break;
    }
}

void Lowerer::hoist_globals(std::vector<StmtPtr>& body) {
    for (auto& s : body) scan_globals(*s, false);
}

void Lowerer::collect_funcdefs(std::vector<StmtPtr>& body, FunctionDef* parent) {
    for (auto& s : body) {
        if (s->kind == StmtKind::FunctionDef) {
            auto& f = static_cast<FunctionDef&>(*s);
            func_defs_[f.name] = &f;
            func_parent_[&f] = parent;
            if (parent) func_children_[parent].push_back(&f);
            else func_roots_.push_back(&f);
            collect_funcdefs(f.body, &f);
        } else if (s->kind == StmtKind::ClassDef) {
            // 方法不是词法嵌套函数（类作用域不参与捕获），但方法体里的
            // def 仍是其嵌套函数，需要单独建树。
            for (auto& m : static_cast<ClassDef&>(*s).body) {
                if (m->kind != StmtKind::FunctionDef) continue;
                auto& f = static_cast<FunctionDef&>(*m);
                func_parent_[&f] = nullptr;
                func_roots_.push_back(&f);
                collect_funcdefs(f.body, &f);
            }
        } else if (s->kind == StmtKind::If) {
            for (auto& b : static_cast<If&>(*s).branches) collect_funcdefs(b.body, parent);
            collect_funcdefs(static_cast<If&>(*s).orelse, parent);
        } else if (s->kind == StmtKind::While) {
            collect_funcdefs(static_cast<While&>(*s).body, parent);
        } else if (s->kind == StmtKind::For) {
            collect_funcdefs(static_cast<For&>(*s).body, parent);
        } else if (s->kind == StmtKind::Try) {
            auto& t = static_cast<Try&>(*s);
            collect_funcdefs(t.body, parent);
            for (auto& h : t.handlers) collect_funcdefs(h.body, parent);
            collect_funcdefs(t.finalbody, parent);
        } else if (s->kind == StmtKind::With) {
            collect_funcdefs(static_cast<With&>(*s).body, parent);
        }
    }
}

TypePtr Lowerer::capture_type(const FunctionDef* f, const std::string& name) {
    const Scope* s = tc_.scope_of(f);
    for (s = s ? s->parent : nullptr; s; s = s->parent) {
        auto it = s->vars.find(name);
        if (it != s->vars.end()) return it->second.ty;
    }
    return t_unknown();
}

// 自底向上传播捕获集：子函数需要的名字，如果在本函数里有绑定，就变成
// 本函数的 cell；否则继续往上要。这样多层嵌套闭包也能拿到正确的 cell。
void Lowerer::compute_closure_meta(FunctionDef* f) {
    if (closure_meta_.count(f)) return;

    std::unordered_set<std::string> local_names;
    if (auto* sc = tc_.scope_of(f))
        for (auto& [n, sym] : sc->vars) local_names.insert(n);

    std::unordered_set<std::string> needed;
    for (auto& n : tc_.free_refs(f))
        if (n != f->name) needed.insert(n);  // 自引用走 env/静态闭包，不建 cell 环
    std::set<std::string> cell_names;
    for (auto* c : func_children_[f]) {
        compute_closure_meta(c);
        for (auto& n : closure_meta_[c].needed) {
            if (local_names.count(n)) cell_names.insert(n);
            else needed.insert(n);
        }
    }

    ClosureMeta meta;
    for (auto& n : cell_names) {
        TypePtr t = t_unknown();
        if (auto* sc = tc_.scope_of(f)) {
            auto it = sc->vars.find(n);
            if (it != sc->vars.end()) t = it->second.ty;
        }
        meta.cell_index[n] = static_cast<int>(meta.cells.size());
        meta.cells.push_back({n, t});
    }
    std::set<std::string> sorted_env(needed.begin(), needed.end());
    for (auto& n : sorted_env) {
        meta.env_index[n] = static_cast<int>(meta.env.size());
        meta.env.push_back({n, capture_type(f, n)});
    }
    meta.needed = std::move(needed);
    closure_meta_[f] = std::move(meta);
}

// 类表必须在编译任何方法体之前建好：方法里可能构造另一个类，
// 而构造指令只带一个类下标，对象大小和 vtable 位置都得先确定。
// 只扫语句块，不进函数体——函数内定义的类暂不支持。
void Lowerer::collect_classes(std::vector<StmtPtr>& body) {
    for (auto& s : body) {
        switch (s->kind) {
            case StmtKind::ClassDef: {
                auto& c = static_cast<ClassDef&>(*s);
                auto it = classes_.find(c.name);
                if (it == classes_.end()) break;
                const ClassInfoPtr& ci = it->second;
                if (class_ids_.count(ci.get())) break;

                IRClass irc;
                irc.name = c.name;
                irc.n_fields = ci->n_fields();
                // 字段偏移顺序 = 父类字段 + 本类字段；析构器按这个顺序 dec。
                {
                    std::vector<const ClassInfo*> chain;
                    for (const ClassInfo* k = ci.get(); k; k = k->base) chain.push_back(k);
                    for (auto kit = chain.rbegin(); kit != chain.rend(); ++kit)
                        for (auto& f : (*kit)->fields)
                            irc.field_heap.push_back(ir_is_heap(f.ty) ? 1 : 0);
                }
                irc.vtable.assign(static_cast<size_t>(ci->n_slots()), -1);
                class_ids_[ci.get()] = static_cast<int>(mod_.classes.size());
                mod_.classes.push_back(std::move(irc));
                break;
            }
            case StmtKind::If: {
                auto& f = static_cast<If&>(*s);
                for (auto& b : f.branches) collect_classes(b.body);
                collect_classes(f.orelse);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(*s);
                collect_classes(w.body);
                collect_classes(w.orelse);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(*s);
                collect_classes(f.body);
                collect_classes(f.orelse);
                break;
            }
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(*s);
                collect_classes(t.body);
                for (auto& h : t.handlers) collect_classes(h.body);
                collect_classes(t.orelse);
                collect_classes(t.finalbody);
                break;
            }
            case StmtKind::With:
                collect_classes(static_cast<With&>(*s).body);
                break;
            default:
                break;
        }
    }
}

// ────────────────────────── 入口 ──────────────────────────

void Lowerer::run(Module& ast) {
    mod_.module_symbol = opts_.module_symbol;
    mod_.runtime_name = opts_.runtime_name;
    mod_.source_path = opts_.source_path.empty() ? file_ : opts_.source_path;
    mod_.is_entry = opts_.is_entry;

    hoist_globals(ast.body);
    collect_funcdefs(ast.body);
    for (auto* root : func_roots_) compute_closure_meta(root);
    classes_ = tc_.classes();
    collect_classes(ast.body);

    IRFunction* entry = mod_.add_func(opts_.is_entry ? "__py23_main" : "__py23_init");
    entry->ret = opts_.is_entry ? t_int() : t_none();
    mod_.entry = static_cast<int>(mod_.funcs.size()) - 1;

    push_frame(entry, tc_.module_scope());
    st_->is_module_top = true;
    int b = new_block("entry");
    switch_to(b);
    st_->is_entry = opts_.is_entry;
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    // __name__ / __file__：导入方也可能读它们，作为普通 str 全局存放。
    int name_gi = global_idx_as("__name__", t_str());
    store_global(name_gi, const_str(mod_.runtime_name));
    int file_gi = global_idx_as("__file__", t_str());
    store_global(file_gi, const_str(mod_.source_path));

    stmts(ast.body);

    if (!terminated())
        emit_return(opts_.is_entry ? const_int(0) : const_none(),
                    opts_.is_entry ? t_int() : t_none());
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);

    pop_frame();
}

int Lowerer::compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label) {
    if (f.is_async) {
        int kind = (info && info->is_async_generator) ? 2 : 1;
        return compile_suspendable_function(f, info, label, kind);
    }
    if (info && info->is_generator) return compile_suspendable_function(f, info, label, 0);

    IRFunction* irf = mod_.add_func(label);
    int fidx = static_cast<int>(mod_.funcs.size()) - 1;

    TypePtr ret = t_none();
    if (info && info->ret) ret = info->ret;
    else if (info) ret = t_unknown();
    irf->ret = ret;

    const Scope* scope = tc_.scope_of(&f);
    push_frame(irf, scope);
    if (auto it = closure_meta_.find(&f); it != closure_meta_.end()) st_->closure = &it->second;

    int entry = new_block("entry");
    switch_to(entry);

    const ClosureMeta* meta = st_->closure;

    // 被内层捕获的变量不占普通值槽，槽里放的是 cell（单元素 list[T]）。
    // 形参例外：IRParam 需要原始值槽接实参，入口再 retain 进 cell。
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = irf->new_slot(T);
        irf->params.push_back(IRParam{p.name, T, s});
        if (meta && meta->cell_index.count(p.name)) {
            int cs = irf->new_slot(mk_list(T));
            st_->cell_slots[p.name] = cs;
            st_->param_slots[p.name] = s;
            st_->slots[p.name] = cs;
        } else {
            st_->slots[p.name] = s;
        }
    };
    if (info) {
        for (auto& p : info->pos) add_param(p);
        for (auto& p : info->kwonly) add_param(p);
        if (info->varargs) {
            TypePtr et = info->vararg_ty ? info->vararg_ty : t_unknown();
            Param p;
            p.name = info->vararg_name;
            p.ty = mk_list(et);
            add_param(p);
        }
        if (info->kwargs) {
            TypePtr vt = info->kwarg_ty ? info->kwarg_ty : t_unknown();
            Param p;
            p.name = info->kwarg_name;
            p.ty = mk_dict(t_str(), vt);
            add_param(p);
        }
    }

    // 其余局部名按符号表分配。符号表是 typecheck 的最终结果，
    // 这里不再自己推断，避免两套规则分叉。
    if (scope) {
        for (auto& [name, sym] : scope->vars) {
            if (st_->slots.count(name)) continue;
            if (st_->globals_decl.count(name)) continue;
            TypePtr T = sym.ty ? sym.ty : t_unknown();
            if (meta && meta->cell_index.count(name)) {
                int cs = irf->new_slot(mk_list(T));
                st_->cell_slots[name] = cs;
                st_->slots[name] = cs;
            } else {
                st_->slots[name] = irf->new_slot(T);
            }
        }
    }

    // 闭包环境指针由 codegen 在入口从 %r10 存进这个槽；只是借用，
    // 不参与引用计数，出口也不释放。
    if (meta && !meta->env.empty()) {
        int es = irf->new_slot(t_unknown());
        st_->env_slot = es;
        irf->env_slot = es;
        st_->borrow_slots.insert(es);
    }

    // 嵌套函数里的自引用：如果它需要闭包（有其它捕获），env_slot 里就是
    // 自己；没有捕获则是静态闭包。两种情况都不需要 self cell。
    if (auto pit = func_parent_.find(&f); pit != func_parent_.end() && pit->second) {
        st_->self_def = &f;
        st_->self_idx = fidx;
        st_->self_type = new_ty(TyKind::Func);
        st_->self_type->fn = info;
    }

    // 形参按值借用，入口统一 retain 一份归形参槽所有；出口 release_slots
    // 负责释放。这样函数体内覆盖形参时也能按普通槽的规则 dec 旧值。
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);
    inc_params();
    create_cells();

    stmts(f.body);
    if (!terminated()) emit_return(kNoReg, nullptr);
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);

    pop_frame();
    return fidx;
}

void Lowerer::lower_funcdef(FunctionDef& f) {
    FuncInfoPtr info = tc_.func_info(&f);
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = info;

    std::string label = std::format("{}_{}", f.name, mod_.funcs.size());
    int fidx = compile_function_body(f, info, label);

    auto mit = closure_meta_.find(&f);
    const ClosureMeta* meta = mit == closure_meta_.end() ? nullptr : &mit->second;

    // 无捕获：函数值指向静态闭包（rc = INT64_MIN，永生）。
    // 有捕获：运行时分配闭包对象，填 code 和各 cell 指针。
    VReg fv;
    if (!meta || meta->env.empty()) {
        Inst fa;
        fa.op = IrOp::FuncAddr;
        fa.imm = fidx;
        fv = def(fty, std::move(fa));
    } else {
        Inst fc;
        fc.op = IrOp::FuncCode;
        fc.imm = fidx;
        VReg code = def(t_int(), std::move(fc));
        fv = call_runtime(RtFn::ClosureNew, fty,
                          {code, const_int(static_cast<int64_t>(meta->env.size()))});
        push_temp(fv);
    }

    // def 执行时才绑定名字。先绑定再填 env：自递归闭包要靠 cell 里
    // 已经写好的闭包指针找到自己（这会形成环，环目前靠泄漏处理）。
    bind_name_value(f.name, fv, fty);

    if (meta && !meta->env.empty()) {
        for (size_t i = 0; i < meta->env.size(); ++i) {
            const auto& cv = meta->env[i];
            VReg cell = cell_for_capture(cv.name);
            if (cell == kNoReg) {
                err(f.line, f.col,
                    std::format("closure capture of '{}' is not available here", cv.name));
                continue;
            }
            rc_inc(cell);
            field_store(fv, 16 + 8 * static_cast<int>(i), cell);
        }
    }

    // 装饰器按从下到上的顺序调用：f = d1(d2(f))。函数值已经绑定，
    // 每一步都重新绑定同名，后一个装饰器拿到前一步的结果。
    for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
        ExprPtr dec = std::move(*it);
        int line = dec->line;
        int col = dec->col;
        VReg callee = expr(*dec);
        TypePtr dt = tc_.type_of(dec.get());
        FuncInfoPtr dinfo = (dt && dt->kind == TyKind::Func) ? dt->fn : nullptr;
        if (!dinfo) {
            err(line, col,
                "decorator must be a function value with a known signature");
            continue;
        }
        Call dc;
        dc.line = line;
        dc.col = col;
        auto arg = std::make_unique<Name>(f.name);
        arg->line = line;
        arg->col = col;
        dc.args.push_back(std::move(arg));
        std::vector<VReg> dargs = bind_args(dc, dinfo, nullptr);
        TypePtr rty = dinfo->ret ? dinfo->ret : t_unknown();
        Inst in;
        in.op = IrOp::Call;
        in.a = callee;
        in.args = std::move(dargs);
        VReg dv = def_heap(rty, std::move(in));
        bind_name_value(f.name, dv, rty);
    }
}

void Lowerer::lower_classdef(ClassDef& c) {
    if (!c.decorators.empty()) {
        err(c.line, c.col, "class decorators are not supported by the code generator yet");
        return;
    }
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
        // 自定义异常类不参与对象布局；注册父子关系供 except 匹配。
        call_void(RtFn::ExcRegister, {const_str(ci->name), const_str(ci->exc_parent)});
        return;
    }
    if (ci->base && !class_ids_.count(ci->base)) {
        err(c.line, c.col, "inheriting from an imported class is not supported yet");
        return;
    }
    for (auto* eb : ci->extra_bases) {
        if (!class_ids_.count(eb)) {
            err(c.line, c.col, "inheriting from an imported class is not supported yet");
            return;
        }
    }

    // 类体语句在下面按执行语义降级；类命名空间未建模，赋值走模块全局。

    // 先把父类的 vtable 抄过来。父类一定在本类之前定义，此时它的槽
    // 已经填好；子类覆盖同名方法只改自己那几个槽，前缀保持不变。
    if (ci->base) {
        auto bit = class_ids_.find(ci->base);
        if (bit != class_ids_.end()) {
            const auto& bv = mod_.classes[bit->second].vtable;
            auto& cv = mod_.classes[cid].vtable;
            for (size_t i = 0; i < bv.size() && i < cv.size(); ++i)
                cv[i] = bv[i];
        }
    }

    for (auto& m : ci->methods) {
        if (!m.def) continue;
        if (!m.def->decorators.empty()) {
            err(m.def->line, m.def->col, "decorators on methods are not supported yet");
            continue;
        }
        std::string label = std::format("{}_{}_{}", c.name, m.name, mod_.funcs.size());
        int fidx = compile_function_body(*m.def, m.sig, label);
        if (m.slot >= 0 && m.slot < static_cast<int>(mod_.classes[cid].vtable.size()))
            mod_.classes[cid].vtable[static_cast<size_t>(m.slot)] = fidx;
    }

    if (!opts_.module_symbol.empty() && !opts_.is_entry)
        compile_ctor_wrapper(c, ci, cid);

    // 类体里的非声明语句：在类定义点执行（赋值按模块全局处理）。
    for (auto& s : c.body) {
        if (s->kind == StmtKind::FunctionDef || s->kind == StmtKind::AnnAssign ||
            s->kind == StmtKind::Pass)
            continue;
        if (s->kind == StmtKind::Expr) {
            auto& e = static_cast<ExprStmt&>(*s);
            if (e.value->kind == ExprKind::Constant &&
                static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                continue;
        }
        stmt(*s);
    }
}

void Lowerer::compile_ctor_wrapper(ClassDef& clsdef, const ClassInfoPtr& ci, int cid) {
    (void)clsdef;
    const MethodInfo* init = ci->method("__init__");
    FuncInfoPtr sig;
    if (init && init->call_sig) {
        if (init->call_sig->varargs || init->call_sig->kwargs) return;
        sig = init->call_sig;
    }
    auto ctor = std::make_shared<FuncInfo>();
    ctor->name = "__new_" + ci->name;
    if (sig) {
        ctor->pos = sig->pos;
        ctor->n_posonly = sig->n_posonly;
        ctor->kwonly = sig->kwonly;
    }
    TypePtr cls_ty = mk_class(ci.get());
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ctor;

    std::string label = std::format("__py23_new_{}_{}", ci->name, mod_.funcs.size());
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
            if (!f.has_default || !f.default_expr) continue;
            VReg v = expr(*f.default_expr);
            v = coerce(v, vreg_ty(v), f.ty);
            int off = static_cast<int>(8 * (1 + ci->field_index(f.name)));
            store_field(f.ty, obj, off, v);
        }
    }

    if (sig && init->slot >= 0) {
        std::vector<VReg> args;
        for (int s : arg_slots) args.push_back(load_slot(s, wf->slot_ty[s]));
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = obj;
        vc.imm = init->slot;
        vc.args = std::move(args);
        TypePtr rt = sig->ret ? sig->ret : t_none();
        VReg r = def_heap(rt, std::move(vc));
        (void)r;
    }
    emit_return(obj, cls_ty);
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);
    pop_frame();

    // 模块顶层绑定隐藏全局：导入方按 ctor_symbol 加载闭包。
    IRGlobal g;
    g.name = "__py23$new$" + ci->name;
    g.symbol = module_class_new_symbol(opts_.module_symbol, ci->name);
    g.ty = fty;
    int gi = static_cast<int>(mod_.globals.size());
    mod_.globals.push_back(std::move(g));
    global_ids_["__py23$new$" + ci->name] = gi;
    Inst fa;
    fa.op = IrOp::FuncAddr;
    fa.imm = widx;
    VReg fv = def(fty, std::move(fa));
    store_global(gi, fv);
}

LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file,
                         const LowerOptions& options) {
    LowerResult res;
    Lowerer lw(res.mod, tc, file, options);
    lw.run(ast);
    res.errors = std::move(lw.errors());
    return res;
}


}  // namespace py23
