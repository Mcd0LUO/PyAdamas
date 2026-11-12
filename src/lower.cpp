// lower.cpp —— 降级驱动：预扫描、入口、函数/类编译
#include "lower_impl.h"

#include "opt.h"

namespace pyadamas {

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
                            irc.field_heap.push_back(ir_is_heap(f.ty)
                                                          ? 1
                                                          : (f.ty && f.ty->kind == TyKind::Unknown ? 2 : 0));
                }
                irc.vtable.assign(static_cast<size_t>(tc_.method_slot_count()), -1);
                int self_id = static_cast<int>(mod_.classes.size());
                class_ids_[ci.get()] = self_id;
                irc.qualname = mod_.runtime_name + "." + ci->name;
                for (const ClassInfo* k : ci->mro) {
                    auto kit = class_ids_.find(k);
                    if (kit != class_ids_.end()) irc.mro.push_back(kit->second);
                }
                if (irc.mro.empty()) irc.mro.push_back(self_id);
                for (const MethodInfo& m : ci->methods)
                    if (m.slot >= 0) irc.own_slots.push_back(m.slot);
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
    mod_.opt_level = opts_.opt_level;

    hoist_globals(ast.body);
    collect_funcdefs(ast.body);
    for (auto* root : func_roots_) compute_closure_meta(root);
    classes_ = tc_.classes();
    collect_classes(ast.body);
    // 跨模块继承不支持，所有子类都在本模块可见：没被任何本模块类继承的
    // 类就是终态。方法调用点可以据此把 vtable 间接跳转换成直接调用。
    {
        std::unordered_set<const ClassInfo*> has_sub;
        for (auto& [name, ci] : classes_) {
            if (!ci || ci->is_exception_class || !class_ids_.count(ci.get())) continue;
            for (const ClassInfo* k : ci->mro)
                if (k && k != ci.get()) has_sub.insert(k);
        }
        sealed_classes_.clear();
        for (auto& [name, ci] : classes_)
            if (ci && !ci->is_exception_class && class_ids_.count(ci.get()) &&
                !has_sub.count(ci.get()))
                sealed_classes_.insert(ci.get());
    }

    IRFunction* entry = mod_.add_func(opts_.is_entry ? "__pa_main" : "__pa_init");
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

int Lowerer::compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                                   const ClassInfo* owner) {
    if (f.is_async) {
        int kind = (info && info->is_async_generator) ? 2 : 1;
        return compile_suspendable_function(f, info, label, kind, owner);
    }
    if (info && info->is_generator) return compile_suspendable_function(f, info, label, 0, owner);

    IRFunction* irf = mod_.add_func(label);
    int fidx = static_cast<int>(mod_.funcs.size()) - 1;
    if (owner) fn_owner_class_[irf] = owner;

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
    // @classmethod 的 cls 槽位按整数存类 id：它是编译期类型标记，不是
    // 真实对象（调用点传的是小整数），按堆类型做 rc 会直接崩。
    const MethodInfo* self_m = owner ? owner->method(f.name) : nullptr;
    bool is_cmethod = self_m && self_m->kind == MethodKind::ClassMethod;
    auto add_param = [&](const Param& p, bool as_int = false) {
        TypePtr T = as_int ? t_int() : (p.ty ? p.ty : t_unknown());
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
        for (size_t i = 0; i < info->pos.size(); ++i) {
            bool is_cls = is_cmethod && i == 0;
            if (is_cls) {
                st_->class_self_name = info->pos[i].name;
                st_->class_self_ty = owner;
            }
            add_param(info->pos[i], is_cls);
        }
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
    finalize_param_borrows();
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);

    pop_frame();
    auto cmit = closure_meta_.find(&f);
    bool simple_closure =
        cmit == closure_meta_.end() ||
        (cmit->second.env.empty() && cmit->second.cells.empty());
    irf->can_inline = simple_closure && !irf->has_handlers && f.decorators.empty() &&
                      !(info && (info->varargs || info->kwargs));
    return fidx;
}

void Lowerer::lower_funcdef(FunctionDef& f) {
    // 装饰器在 def 执行时立即调用，可能改动容器。
    if (!f.decorators.empty()) ++side_effect_seq_;
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
    VReg cur_func = fv;
    TypePtr cur_fty = fty;

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
        // @functools.wraps(f)：pyadamas 不建模函数元数据（__name__/__doc__），
        // 这一层没有可观察效果，直接跳过——否则它要求一个运行期的
        // identity 装饰器，而那是当前架构里没有的东西。
        if (is_wraps_decorator(it->get())) continue;
        if (is_lru_cache_decorator(it->get())) {
            // @lru_cache / @lru_cache(maxsize=...)：参数只求值，包装闭包
            // 在降级期合成，签名与被装饰函数完全相同。
            int line = (*it)->line;
            int col = (*it)->col;
            if (auto* lc = dynamic_cast<Call*>(it->get())) {
                for (auto& a : lc->args) expr(*a);
                for (auto& kw : lc->keywords) expr(*kw.value);
            }
            VReg wrapped = build_lru_wrapper(f.name, cur_func, cur_fty, line, col);
            cur_func = wrapped;
            bind_name_value(f.name, cur_func, cur_fty);
            continue;
        }
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
        cur_func = dv;
        cur_fty = rty;
    }
}

// @functools.wraps(...) / @wraps(...)：装饰器链里可以整层跳过。
bool Lowerer::is_wraps_decorator(const Expr* e) const {
    auto* call = dynamic_cast<const Call*>(e);
    if (!call) return false;
    if (auto* at = dynamic_cast<const Attribute*>(call->func.get())) {
        if (at->attr != "wraps") return false;
        auto* rn = dynamic_cast<const Name*>(at->value.get());
        if (!rn) return false;
        auto mod = resolve_module_alias(rn->id);
        return mod && mod->builtin && mod->name == "functools";
    }
    if (auto* fn = dynamic_cast<const Name*>(call->func.get())) {
        auto it = native_aliases_.find(fn->id);
        return it != native_aliases_.end() && it->second.module == "functools" &&
               it->second.member == "wraps";
    }
    return false;
}

// @functools.lru_cache / @lru_cache / @lru_cache(...)：
// 装饰器本身不是运行期调用，包装闭包由 build_lru_wrapper 合成。
bool Lowerer::is_lru_cache_decorator(const Expr* e) const {
    const Expr* target = e;
    if (auto* call = dynamic_cast<const Call*>(e)) target = call->func.get();
    if (auto* at = dynamic_cast<const Attribute*>(target)) {
        if (at->attr != "lru_cache") return false;
        auto* rn = dynamic_cast<const Name*>(at->value.get());
        if (!rn) return false;
        auto mod = resolve_module_alias(rn->id);
        return mod && mod->builtin && mod->name == "functools";
    }
    if (auto* fn = dynamic_cast<const Name*>(target)) {
        auto it = native_aliases_.find(fn->id);
        return it != native_aliases_.end() && it->second.module == "functools" &&
               it->second.member == "lru_cache";
    }
    return false;
}

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

    // 按 C3 MRO 从前向后填 vtable：MRO 中越靠前的类优先级越高，
    // 每个槽只由最先声明它的类占用。本类自己的方法稍后编译时覆盖。
    {
        auto& cv = mod_.classes[cid].vtable;
        for (size_t mi = 1; mi < ci->mro.size(); ++mi) {
            const ClassInfo* k = ci->mro[mi];
            auto kit = class_ids_.find(k);
            if (kit == class_ids_.end()) continue;
            const auto& kv = mod_.classes[kit->second].vtable;
            for (const MethodInfo& km : k->methods) {
                if (km.slot < 0 || km.slot >= static_cast<int>(cv.size())) continue;
                if (cv[static_cast<size_t>(km.slot)] != -1) continue;
                if (km.slot < static_cast<int>(kv.size()) &&
                    kv[static_cast<size_t>(km.slot)] >= 0)
                    cv[static_cast<size_t>(km.slot)] = kv[static_cast<size_t>(km.slot)];
            }
        }
    }

    // 自定义装饰器方法：方法体照常编译，但 vtable 槽不填；类创建时把
    // 装饰器链的结果写进类属性，调用点走类属性的间接调用。
    std::unordered_map<std::string, int> decorated_methods;
    for (auto& m : ci->methods) {
        if (!m.def) continue;
        if (m.decorated) {
            if (!m.decorated_ty || m.decorated_ty->kind != TyKind::Func) {
                err(m.def->line, m.def->col,
                    std::format("decorated method '{}': the decorator must return a function "
                                "with a known signature",
                                m.name));
                continue;
            }
        } else if (!m.def->decorators.empty() && m.kind == MethodKind::Normal) {
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
        }
    }

    if (!opts_.module_symbol.empty() && !opts_.is_entry)
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
    finalize_param_borrows();
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
}

LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file,
                         const LowerOptions& options) {
    LowerResult res;
    Lowerer lw(res.mod, tc, file, options);
    lw.run(ast);
    res.errors = std::move(lw.errors());
    if (res.errors.empty() && options.opt_level >= 2) optimize_ir(res.mod, options.opt_level);
    return res;
}


}  // namespace pyadamas
