// lower.cpp —— 降级驱动：预扫描、函数编译与模块入口
#include "lower_impl.h"

#include "opt.h"
#include "target_abi.h"

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
                irc.vtable_ext.assign(irc.vtable.size(), "");
                int self_id = static_cast<int>(mod_.classes.size());
                class_ids_[ci.get()] = self_id;
                irc.qualname = mod_.runtime_name + "." + ci->name;
                std::unordered_set<std::string> ext_seen;
                auto add_ext = [&](const ClassAncestor& a) -> int {
                    if (a.module.empty() || a.name.empty()) return -1;
                    std::string key = a.module + "\x1f" + a.name;
                    if (!ext_seen.insert(key).second) return -1;
                    irc.ext_bases.push_back(
                        {module_class_vtable_symbol(a.module, a.name),
                         module_class_own_slots_symbol(a.module, a.name),
                         a.qualname.empty() ? a.module + "." + a.name : a.qualname});
                    return static_cast<int>(irc.ext_bases.size()) - 1;
                };
                for (const ClassInfo* k : ci->mro) {
                    if (!k) continue;
                    auto kit = class_ids_.find(k);
                    if (kit != class_ids_.end()) {
                        irc.mro.push_back(kit->second);
                        continue;
                    }
                    // 导入祖先：mirror 元数据按真实 MRO 顺序交错编码。
                    int idx = add_ext({k->origin_module, k->external_name(),
                                       k->origin_qualname});
                    if (idx >= 0) irc.mro.push_back(-(idx + 1));
                    for (auto& a : k->ext_ancestors) {
                        int ai = add_ext(a);
                        if (ai >= 0) irc.mro.push_back(-(ai + 1));
                    }
                }
                if (irc.mro.empty()) irc.mro.push_back(self_id);
                for (const MethodInfo& m : ci->methods)
                    if (m.slot >= 0) irc.own_slots.push_back(m.slot);
                // 动态属性槽挂在字段之后；需要它的类（含继承链）加一个指针。
                bool need_dict = false;
                for (const ClassInfo* k = ci.get(); k; k = k->base) need_dict |= k->needs_dict;
                irc.dict_off = need_dict ? 8 * (irc.n_fields + 1) : 0;
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
            case StmtKind::FunctionDef:
                // 函数体内可以定义类：先给它们分配类编号，方法体编译时
                // 构造点就能找到布局。
                collect_classes(static_cast<FunctionDef&>(*s).body);
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
    // 跨模块继承：非入口模块的类都可能被导入方继承，不能做「没有子类」
    // 的终结假设，否则基类模块会把方法调用直调化，丢掉子模块覆盖。
    sealed_classes_.clear();
    if (opts_.is_entry) {
        std::unordered_set<const ClassInfo*> has_sub;
        for (auto& [name, ci] : classes_) {
            if (!ci || ci->is_exception_class || !class_ids_.count(ci.get())) continue;
            for (const ClassInfo* k : ci->mro)
                if (k && k != ci.get()) has_sub.insert(k);
        }
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

    // 零开销异常也覆盖模块顶层：__pa_main / __pa_init 是一个合成的函数，
    // 模块级的 try 与函数体内的 try 走同一套形状判定与降级。
    // （顶层不是生成器，也不可能有 yield，所以不需要额外排除。）
    if (opts_.zero_cost_exc && target_abi().gnu_cfi) {
        FunctionDef mod_fn;
        mod_fn.body = std::move(ast.body);
        collect_zero_cost_tries(mod_fn);
        ast.body = std::move(mod_fn.body);
        if (!st_->zero_cost_tries.empty()) entry->has_handlers = true;
    }

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
                st_->class_self_dynamic = !opts_.is_entry;
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

    // 零开销异常（Z2-b2）：--zero-cost-exc 打开时，先按 AST 判定本函数是否
    // 满足最小形状（非可挂起 + 单个 catch-all try + 内部没有 return/break/
    // continue 穿过保护区）。满足时才把唯一那条 try 降级成 LSDA/landing pad
    // 路径；不满足的函数继续走 setjmp 路径，一条指令都不变。
    if (opts_.zero_cost_exc && target_abi().gnu_cfi) {
        collect_zero_cost_tries(f);
        if (!st_->zero_cost_tries.empty()) {
            // 与老路径的 has_handlers 语义保持一致（docs/exc-semantics.md
            // §5.2：Z2/Z3 期间不把「优化差异」混进语义回归）：函数里不再有
            // HandlerPush，但槽位提升 / 序言清零 / 循环优化的门禁照旧。
            irf->has_handlers = true;
        }
    }
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
        // @functools.wraps(f)：PyAdamas 不建模函数元数据（__name__/__doc__），
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
