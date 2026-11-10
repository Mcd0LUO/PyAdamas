// typecheck_stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "typecheck.h"

#include <set>

#include "module.h"

namespace py23 {

static void pattern_capture_names(const Pattern& p, std::vector<std::string>& out);

bool TypeChecker::block_exits(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto& last = *body.back();
    switch (last.kind) {
        case StmtKind::Return:
        case StmtKind::Raise:
        case StmtKind::Break:
        case StmtKind::Continue:
            return true;
        case StmtKind::If: {
            auto& f = static_cast<If&>(last);
            // 没有 else 的 if 不算离开（可能整条都不执行）
            if (f.orelse.empty()) return false;
            for (auto& b : f.branches)
                if (!block_exits(b.body)) return false;
            return block_exits(f.orelse);
        }
        default: return false;
    }
}

void TypeChecker::stmts(std::vector<StmtPtr>& body) {
    // 提前退出的 None 守卫：`if x is None: return` 之后，x 一定不是 None。
    // 收窄要保持到块末，所以在块级别统一记录、退出时恢复。
    std::vector<Narrowing> active;
    for (auto& s : body) {
        if (s->kind == StmtKind::If) {
            auto& f = static_cast<If&>(*s);
            stmt(*s);
            // 单分支且分支必然离开（return/raise/continue/break），
            // 后面的代码只可能在条件为假时到达
            if (f.orelse.empty() && f.branches.size() == 1 &&
                block_exits(f.branches[0].body)) {
                auto n = narrow_on(*f.branches[0].cond, false);
                if (n.sym) active.push_back(n);
            }
            continue;
        }
        stmt(*s);
    }
    for (auto& n : active) restore_narrowing(n);
}

void TypeChecker::stmt(Stmt& s) {
        ++n_stmt;
        switch (s.kind) {
            case StmtKind::Expr:
                expr(*static_cast<ExprStmt&>(s).value);
                break;
            case StmtKind::Assign: {
                auto& a = static_cast<Assign&>(s);
                // d = globals() 之后 d[k] = v 等价于模块级动态注入，记下别名
                if (a.value->kind == ExprKind::Call) {
                    auto& c = static_cast<Call&>(*a.value);
                    auto* fn = dynamic_cast<Name*>(c.func.get());
                    if (fn && fn->id == "globals")
                        for (auto& t : a.targets)
                            if (auto* tn = dynamic_cast<Name*>(t.get()))
                                globals_aliases_.insert(tn->id);
                }
                auto vt = expr(*a.value);
                for (auto& t : a.targets) assign(*t, vt, a.line, a.col);
                break;
            }
            case StmtKind::AugAssign: {
                auto& a = static_cast<AugAssign&>(s);
                auto cur_ty = expr(*a.target);
                // list += 可迭代对象：+= 走的是 list.__iadd__，吃任意 iterable，
                // 和 list + tuple 是两回事（后者才非法）
                if (a.op == Tok::PLUSEQUAL && cur_ty && cur_ty->kind == TyKind::List) {
                    auto vt = expr(*a.value);
                    auto ie = iter_elem(vt, a.line, a.col);
                    if (!assignable(cur_ty->sub[0], ie)) {
                        if (cur_ty->locked)
                            err(a.line, a.col,
                                std::format("list element type is '{}', got '{}'",
                                            type_name(cur_ty->sub[0]), type_name(ie)));
                        else
                            widen_slot(cur_ty->sub[0], cur_ty, ie);
                    }
                    break;
                }
                auto vt = expr(*a.value);
                Tok bin = aug_to_bin(a.op);
                auto rt = binop_type(bin, cur_ty, vt, a.line, a.col);
                assign(*a.target, rt, a.line, a.col);
                break;
            }
            case StmtKind::AnnAssign: {
                auto& a = static_cast<AnnAssign&>(s);
                auto ann = parse_annotation(a.annotation.get(), a.line);
                TypePtr vt;
                if (a.value) {
                    vt = expr(*a.value);
                    if (ann && !assignable(ann, vt))
                        err(a.line, a.col,
                            std::format("annotated as '{}' but assigned '{}'", type_name(ann),
                                        type_name(vt)));
                }
                if (auto* n = dynamic_cast<Name*>(a.target.get())) {
                    declare(n->id, ann ? ann : t_unknown(), a.line, a.col, true);
                } else if (auto* at = dynamic_cast<Attribute*>(a.target.get());
                           at && is_self_attr(*at)) {
                    // `self.x: int = 0`：字段标注写在方法里，等价于类体标注
                    TypePtr want = (ann && !is_unknown(ann)) ? ann : (vt ? vt : t_unknown());
                    if (!add_implicit_field(*at, want)) {
                        if (auto* f = cur_class_->field(at->attr);
                            f && !is_unknown(want) && !assignable(f->ty, want))
                            err(a.line, a.col,
                                std::format("field '{}' has type '{}', cannot annotate as '{}'",
                                            at->attr, type_name(f->ty), type_name(want)));
                    }
                }
                break;
            }
            case StmtKind::Return: {
                auto& r = static_cast<Return&>(s);
                TypePtr vt = r.value ? expr(*r.value) : t_none();
                if (!cur_fn_) {
                    err(r.line, r.col, "'return' outside function");
                    break;
                }
                if (cur_fn_->ret_declared) {
                    TypePtr want = cur_fn_->is_async ? cur_fn_->async_result : cur_fn_->ret;
                    if (!assignable(want, vt))
                        err(r.line, r.col,
                            std::format("function '{}' is declared to return '{}', got '{}'",
                                        cur_fn_->name, type_name(want), type_name(vt)));
                } else if (rets_) {
                    rets_->push_back({vt, r.line, r.col});
                }
                break;
            }
            case StmtKind::If: {
                auto& f = static_cast<If&>(s);
                // 分支合并：每个分支都从 if 之前的变量状态出发，最后取各分支的
                // 公共上界。直接在原地检查会让 else 分支看到 then 分支的赋值，
                // `if p is None: p = [] else: p = p.split(':')` 就会误判。
                // 只快照「类型」不搬整个 map：外层的收窄持有 Symbol* 指针，
                // 换掉 map 会让这些指针悬空。
                std::unordered_map<std::string, TypePtr> snapshot;
                snapshot.reserve(cur_->vars.size());
                for (auto& [n, sym] : cur_->vars) snapshot.emplace(n, sym.ty);
                auto restore_types = [&]() {
                    for (auto& [n, sym] : cur_->vars) {
                        auto it = snapshot.find(n);
                        if (it != snapshot.end()) sym.ty = it->second;
                    }
                };
                std::unordered_map<std::string, TypePtr> merged;
                auto merge_in = [&]() {
                    for (auto& [n, sym] : cur_->vars) {
                        auto it = merged.find(n);
                        if (it == merged.end()) merged.emplace(n, sym.ty);
                        else if (!type_eq(it->second, sym.ty)) it->second = join(it->second, sym.ty);
                    }
                };
                for (auto& b : f.branches) {
                    restore_types();
                    expr(*b.cond);
                    auto n = narrow_on(*b.cond, true);
                    stmts(b.body);
                    restore_narrowing(n);
                    merge_in();
                }
                restore_types();
                // else 分支等价于「所有条件都为假」，也要收窄：
                // `if x is None: ... else: x.foo()` 里的 x 不是 None
                Narrowing n;
                if (f.branches.size() == 1 && !f.orelse.empty())
                    n = narrow_on(*f.branches[0].cond, false);
                stmts(f.orelse);
                restore_narrowing(n);
                merge_in();
                // 写回：类型取各分支上界，分支里新出现的名字保留
                for (auto& [name, ty] : merged) {
                    auto it = cur_->vars.find(name);
                    if (it == cur_->vars.end())
                        cur_->vars.emplace(name, Symbol{ty, s.line, false, false});
                    else
                        it->second.ty = ty;
                }
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(s);
                expr(*w.cond);
                ++loop_depth_;
                stmts(w.body);
                --loop_depth_;
                stmts(w.orelse);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(s);
                auto it = expr(*f.iter);
                TypePtr elem;
                if (f.is_async) {
                    if (is_unknown(it)) {
                        elem = t_unknown();
                    } else if (it && it->kind == TyKind::AsyncGenerator) {
                        elem = it->sub.empty() ? t_unknown() : it->sub[0];
                    } else {
                        err(f.iter->line, f.iter->col,
                            std::format("async for requires an async generator, got '{}'",
                                        type_name(it)));
                        elem = t_unknown();
                    }
                } else {
                    if (it && it->kind == TyKind::AsyncGenerator)
                        err(f.iter->line, f.iter->col,
                            "'asyncgen' object is not iterable without 'async for'");
                    elem = iter_elem(it, f.line, f.col);
                }
                assign(*f.target, elem, f.line, f.col);
                ++loop_depth_;
                stmts(f.body);
                --loop_depth_;
                stmts(f.orelse);
                break;
            }
            case StmtKind::FunctionDef:
                funcdef(static_cast<FunctionDef&>(s));
                break;
            case StmtKind::ClassDef:
                classdef(static_cast<ClassDef&>(s));
                break;
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(s);
                // 异常可能在任何一条语句上抛出，所以 except 分支看到的变量
                // 类型是「try 入口」与「try 结束时」的公共上界。
                // 不做这个回滚，`v = int(v)` 之后的 `except: v.replace(...)`
                // 会被当成 int 调 str 方法。
                auto snapshot = cur_->vars;
                stmts(t.body);
                for (auto& h : t.handlers) {
                    if (h.type) expr(*h.type);
                    for (auto& [name, sym] : cur_->vars) {
                        if (sym.annotated) continue;
                        auto it = snapshot.find(name);
                        if (it == snapshot.end()) continue;
                        if (type_eq(sym.ty, it->second.ty)) continue;
                        sym.ty = join(sym.ty, it->second.ty);
                    }
                    if (!h.name.empty()) {
                        // as e 绑定异常值对象（args 里是消息）
                        TypePtr et = mk_exception("Exception");
                        if (h.type && h.type->kind == ExprKind::Name) {
                            auto& hn = static_cast<Name&>(*h.type);
                            if (is_exception_name(hn.id)) {
                                et = mk_exception(hn.id);
                            } else if (auto* sym = lookup(hn.id);
                                       sym && sym->ty && sym->ty->kind == TyKind::Class &&
                                       sym->ty->cls && sym->ty->cls->is_exception_class) {
                                et = mk_exception(sym->ty->cls->name);
                            }
                        }
                        auto sc = std::make_unique<Scope>();
                        sc->parent = cur_;
                        cur_ = sc.get();
                        pool_.push_back(std::move(sc));
                        cur_->vars[h.name] = Symbol{et, t.line, false, false};
                        stmts(h.body);
                        cur_ = cur_->parent;
                    } else {
                        stmts(h.body);
                    }
                }
                stmts(t.orelse);
                stmts(t.finalbody);
                break;
            }
            case StmtKind::With: {
                auto& w = static_cast<With&>(s);
                for (auto& item : w.items) {
                    auto t = expr(*item.context);
                    // `with Ctx() as c` 的 c 是 __enter__ 的返回值；能查到
                    // 方法签名就用它，查不到退回 unknown（with 本身不检查协议）。
                    TypePtr vt = t_unknown();
                    TypePtr ct = t;
                    if (ct && ct->kind == TyKind::Optional && !ct->sub.empty()) ct = ct->sub[0];
                    if (ct && ct->kind == TyKind::Class && ct->cls) {
                        const char* enter = w.is_async ? "__aenter__" : "__enter__";
                        if (auto* m = ct->cls->method(enter); m && m->call_sig && m->call_sig->ret)
                            vt = m->call_sig->ret;
                        if (w.is_async) {
                            // 异步上下文管理器：__aenter__/__aexit__ 必须
                            // 是 async def，绑定的是 await 之后的结果。
                            auto* ex = ct->cls->method("__aexit__");
                            if (!vt || vt->kind != TyKind::Coroutine)
                                err(item.context->line, item.context->col,
                                    std::format("'{}' does not define async __aenter__",
                                                ct->cls->name));
                            else
                                vt = vt->sub.empty() ? t_unknown() : vt->sub[0];
                            if (!ex || !ex->call_sig || !ex->call_sig->ret ||
                                ex->call_sig->ret->kind != TyKind::Coroutine)
                                err(item.context->line, item.context->col,
                                    std::format("'{}' does not define async __aexit__",
                                                ct->cls->name));
                        }
                    } else if (w.is_async && ct && ct->kind == TyKind::AsyncLock) {
                        // 内置 Lock 的 __aenter__ 就是 await acquire()。
                        vt = t_bool();
                    } else if (w.is_async && ct && ct->kind == TyKind::AsyncEvent) {
                        vt = t_bool();
                    } else if (w.is_async && ct && ct->kind == TyKind::AsyncSemaphore) {
                        vt = t_bool();
                    } else if (w.is_async && ct && ct->kind == TyKind::AsyncTimeout) {
                        vt = t_none();
                    } else if (w.is_async && ct && ct->kind == TyKind::AsyncTaskGroup) {
                        // __aenter__ 返回 TaskGroup 自身，as tg 绑定的是管理器。
                        vt = ct;
                    } else if (ct && ct->kind == TyKind::File) {
                        // with open(...) as f：f 就是文件对象本身。
                        vt = ct;
                    } else if (w.is_async) {
                        err(item.context->line, item.context->col,
                            "async with requires a class instance with async "
                            "__aenter__/__aexit__");
                    }
                    if (item.optional_vars)
                        assign(*item.optional_vars, vt, w.line, w.col);
                }
                stmts(w.body);
                break;
            }
            case StmtKind::Raise: {
                auto& r = static_cast<Raise&>(s);
                if (r.exc) expr(*r.exc);
                if (r.cause) {
                    auto ct = expr(*r.cause);
                    if (!is_unknown(ct) && ct->kind != TyKind::None &&
                        ct->kind != TyKind::Exception)
                        err(r.cause->line, r.cause->col,
                            "raise from requires an exception or None");
                }
                break;
            }
            case StmtKind::Assert: {
                auto& a = static_cast<Assert&>(s);
                expr(*a.test);
                if (a.msg) expr(*a.msg);
                break;
            }
            case StmtKind::Import: {
                auto& im = static_cast<Import&>(s);
                for (auto& al : im.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
                    if (!module_lookup_) {
                        // 单文件 check：import 没建模，保持前端宽松。
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                        continue;
                    }
                    if (al.name.find('.') != std::string::npos && al.asname.empty()) {
                        err(s.line, s.col,
                            "dotted import needs 'as name' (or use from ... import)");
                        continue;
                    }
                    ModuleInfoPtr info = module_lookup_(al.name);
                    if (info) {
                        cur_->modules[bind] = std::move(info);
                    } else {
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                    }
                }
                break;
            }
            case StmtKind::ImportFrom: {
                auto& im = static_cast<ImportFrom&>(s);
                if (!module_lookup_) {
                    for (auto& al : im.names) {
                        std::string bind = al.asname.empty() ? al.name : al.asname;
                        if (bind == "*") {
                            star_import_ = true;
                            continue;
                        }
                        declare(bind, t_unknown(), s.line);
                    }
                    unsupported.push_back("import");
                    break;
                }
                if (im.level != 0 || im.module.empty()) {
                    err(s.line, s.col, "relative imports are not supported yet");
                    break;
                }
                if (im.names.size() == 1 && im.names[0].name == "*") {
                    ModuleInfoPtr info = module_lookup_(im.module);
                    if (!info) {
                        star_import_ = true;
                        unsupported.push_back("import");
                        break;
                    }
                    for (auto& [n, ty] : info->globals)
                        if (!n.empty() && n[0] != '_') declare(n, ty, s.line);
                    for (auto& [n, ic] : info->classes)
                        if (!n.empty() && n[0] != '_') declare(n, mk_class(ic.cls.get()), s.line);
                    break;
                }
                ModuleInfoPtr info = module_lookup_(im.module);
                for (auto& al : im.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (!info) {
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                        continue;
                    }
                    if (auto ci = info->classes.find(al.name); ci != info->classes.end()) {
                        declare(bind, mk_class(ci->second.cls.get()), s.line);
                        continue;
                    }
                    auto it = info->globals.find(al.name);
                    if (it == info->globals.end()) {
                        if (auto child = module_lookup_(im.module + "." + al.name)) {
                            cur_->modules[bind] = std::move(child);
                            continue;
                        }
                        err(s.line, s.col,
                            std::format("module '{}' has no attribute '{}'", im.module, al.name));
                        declare(bind, t_unknown(), s.line);
                        continue;
                    }
                    declare(bind, it->second, s.line);
                }
                break;
            }
            case StmtKind::Global: {
                for (auto& n : static_cast<Global&>(s).names) global_decl_.insert(n);
                break;
            }
            case StmtKind::Nonlocal: {
                auto& nl = static_cast<Nonlocal&>(s);
                for (auto& n : nl.names) {
                    nonlocal_decl_.insert(n);
                    if (cur_fn_def_) free_refs_[cur_fn_def_].insert(n);
                    bool found = false;
                    if (cur_fn_scope_) {
                        for (Scope* sc = cur_fn_scope_->parent; sc && !sc->is_module; sc = sc->parent)
                            if (sc->vars.count(n)) { found = true; break; }
                    }
                    if (!found) err(s.line, s.col, std::format("no binding for nonlocal '{}' found", n));
                }
                break;
            }
            case StmtKind::Delete: {
                for (auto& t : static_cast<Delete&>(s).targets) expr(*t);
                break;
            }
            case StmtKind::Match: {
                auto& m = static_cast<Match&>(s);
                TypePtr st = expr(*m.subject);
                std::unordered_map<std::string, TypePtr> merged;
                for (auto& c : m.cases) {
                    auto sc = std::make_unique<Scope>();
                    sc->parent = cur_;
                    cur_ = sc.get();
                    pool_.push_back(std::move(sc));
                    match_pattern(*c.pattern, st);
                    if (c.guard) expr(*c.guard);
                    stmts(c.body);
                    std::vector<std::string> names;
                    pattern_capture_names(*c.pattern, names);
                    for (auto& n : names) {
                        auto vit = cur_->vars.find(n);
                        if (vit == cur_->vars.end()) continue;
                        TypePtr t = vit->second.ty;
                        auto mit = merged.find(n);
                        if (mit == merged.end()) merged.emplace(n, t);
                        else mit->second = join(mit->second, t);
                    }
                    cur_ = cur_->parent;
                }
                // Python 的 match 捕获就是普通赋值：case 结束后仍在当前
                // 作用域可见。这里把各分支的类型 join 起来再声明一次。
                for (auto& [name, ty] : merged) declare(name, ty, m.line, m.col);
                match_capture_types_[&m] = std::move(merged);
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Pass:
                break;
        }
    }

Tok TypeChecker::aug_to_bin(Tok t) {
        switch (t) {
            case Tok::PLUSEQUAL: return Tok::PLUS;
            case Tok::MINUSEQUAL: return Tok::MINUS;
            case Tok::STAREQUAL: return Tok::STAR;
            case Tok::SLASHEQUAL: return Tok::SLASH;
            case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
            case Tok::PERCENTEQUAL: return Tok::PERCENT;
            case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
            case Tok::AMPEREQUAL: return Tok::AMPER;
            case Tok::VBAREQUAL: return Tok::VBAR;
            case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
            case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
            case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
            default: return Tok::PLUS;
        }
    }

TypeChecker::Narrowing TypeChecker::narrow_on(Expr& cond, bool truthy) {
        auto non_none = [](const TypePtr& t) -> TypePtr {
            if (!t) return t;
            if (t->kind == TyKind::Optional)
                return t->sub.empty() ? t_unknown() : t->sub[0];
            if (t->kind == TyKind::None) return t_unknown();  // 分支实际不可达
            return t;
        };
        auto is_none_const = [](Expr* e) {
            auto* c = dynamic_cast<Constant*>(e);
            return c && c->ck == Constant::CK::None;
        };

        if (cond.kind == ExprKind::UnaryOp) {
            auto& u = static_cast<UnaryOp&>(cond);
            if (u.op == Tok::KW_NOT) return narrow_on(*u.operand, !truthy);
        }
        if (cond.kind == ExprKind::Compare) {
            auto& c = static_cast<Compare&>(cond);
            if (c.ops.size() == 1) {
                Name* name = dynamic_cast<Name*>(c.left.get());
                bool none_right = is_none_const(c.comparators[0].get());
                if (!name && is_none_const(c.left.get()))
                    name = dynamic_cast<Name*>(c.comparators[0].get());
                if (name && (none_right || is_none_const(c.left.get()))) {
                    // 只做 "is not None" 方向的收窄。
                    // "is None" 分支里若把变量改成 None，`if x is None: x = {}`
                    // 这种合法写法反而会报错，收窄必须尊重原声明。
                    bool is_not = c.ops[0] == CmpOp::IsNot;
                    if (is_not != truthy) return {};
                    Symbol* sym = lookup(name->id);
                    if (!sym) return {};
                    Narrowing n{sym, sym->ty, nullptr};
                    sym->ty = non_none(sym->ty);
                    n.now = sym->ty;
                    return n;
                }
            }
        }
        if (cond.kind == ExprKind::Name && truthy) {
            auto& n = static_cast<Name&>(cond);
            if (Symbol* sym = lookup(n.id)) {
                Narrowing r{sym, sym->ty, nullptr};
                sym->ty = non_none(sym->ty);
                r.now = sym->ty;
                return r;
            }
        }
        return {};
    }

    void TypeChecker::restore_narrowing(const Narrowing& n) {
        // 块内可能已经重新赋值（类型变了），这时不该把类型恢复成收窄前的
        if (!n.sym || n.sym->ty != n.now) return;
        n.sym->ty = n.old;
    }

bool TypeChecker::target_touches_globals(Expr& target) const {
    const Expr* p = &target;
    while (p) {
        switch (p->kind) {
            case ExprKind::Subscript: p = static_cast<const Subscript*>(p)->value.get(); break;
            case ExprKind::Attribute: p = static_cast<const Attribute*>(p)->value.get(); break;
            case ExprKind::Call: {
                auto& c = *static_cast<const Call*>(p);
                auto* n = dynamic_cast<const Name*>(c.func.get());
                return n && n->id == "globals";
            }
            case ExprKind::Name: {
                auto* n = static_cast<const Name*>(p);
                return globals_aliases_.count(n->id) > 0;
            }
            default: return false;
        }
    }
    return false;
}

void TypeChecker::assign(Expr& target, TypePtr value, int line, int col) {
        // globals()[name] = ... 之后模块级名字集合就不可知了
        if ((target.kind == ExprKind::Subscript || target.kind == ExprKind::Attribute) &&
            target_touches_globals(target))
            dynamic_globals_ = true;
        if (auto* n = dynamic_cast<Name*>(&target)) {
            declare(n->id, value, line, col);
            return;
        }
        if (auto* tup = dynamic_cast<TupleExpr*>(&target)) {
            unpack_assign(tup->elts, value, line, col);
            return;
        }
        if (auto* lst = dynamic_cast<ListExpr*>(&target)) {
            unpack_assign(lst->elts, value, line, col);
            return;
        }
        if (auto* st = dynamic_cast<Starred*>(&target)) {
            assign(*st->value, value, line, col);
            return;
        }
        if (auto* sub = dynamic_cast<Subscript*>(&target)) {
            auto ot = expr(*sub->value);
            auto st = expr(*sub->slice);
            if (sub->slice->kind == ExprKind::Slice) {
                // 切片赋值在运行时才展开，类型层面只确认对象是可变的
                auto& sl = static_cast<SliceExpr&>(*sub->slice);
                if (sl.lower) expr(*sl.lower);
                if (sl.upper) expr(*sl.upper);
                if (sl.step) expr(*sl.step);
                if (!is_unknown(ot) && ot->kind != TyKind::List && ot->kind != TyKind::Optional)
                    err(line, col,
                        std::format("'{}' does not support slice assignment", type_name(ot)));
                return;
            }
            if (ot->kind == TyKind::List) {
                if (!merge_elem(ot->sub[0], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (ot->kind == TyKind::Dict) {
                if (!merge_elem(ot->sub[0], ot, st, ot->locked))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(ot->sub[0]),
                                    type_name(st)));
                if (!merge_elem(ot->sub[1], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (!is_unknown(ot) && ot->kind != TyKind::Optional &&
                       ot->kind != TyKind::Class) {
                err(line, col, std::format("'{}' does not support item assignment",
                                           type_name(ot)));
            }
            return;
        }
        if (auto* at = dynamic_cast<Attribute*>(&target)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
                    auto it = mod->globals.find(at->attr);
                    if (it == mod->globals.end()) {
                        err(line, col,
                            std::format("module '{}' has no attribute '{}'", vn->id, at->attr));
                        return;
                    }
                    if (!assignable(it->second, value))
                        err(line, col,
                            std::format("module attribute '{}.{}' has type '{}', cannot assign '{}'",
                                        vn->id, at->attr, type_name(it->second),
                                        type_name(value)));
                    return;
                }
            }
            auto ot = expr(*at->value);
            if (ot->kind == TyKind::Func) return;
            if (ot->kind == TyKind::Optional && !ot->sub.empty() &&
                ot->sub[0]->kind == TyKind::Class)
                ot = ot->sub[0];
            if (ot->kind == TyKind::Class) {
                if (!ot->cls) return;
                if (!ot->cls->complete && ot->cls != cur_class_) return;  // 骨架，无从判断
                if (const PropertyInfo* p = ot->cls->property(at->attr)) {
                    if (!p->owner || p->setter < 0 ||
                        p->setter >= static_cast<int>(p->owner->methods.size())) {
                        err(line, col,
                            std::format("property '{}' has no setter", at->attr));
                        return;
                    }
                    const MethodInfo& sm = p->owner->methods[p->setter];
                    TypePtr vt = sm.call_sig && !sm.call_sig->pos.empty()
                                     ? sm.call_sig->pos[0].ty
                                     : t_unknown();
                    if (!is_unknown(vt) && !assignable(vt, value))
                        err(line, col,
                            std::format("property '{}' setter cannot accept '{}'", at->attr,
                                        type_name(value)));
                    return;
                }
                // `self.x = ...` 且 x 没声明过：就地补成字段，类型不再苛求
                if (add_implicit_field(*at, value)) return;
                if (auto* f = ot->cls->field(at->attr)) {
                    if (!assignable(f->ty, value))
                        err(line, col,
                            std::format("field '{}' has type '{}', cannot assign '{}'", at->attr,
                                        type_name(f->ty), type_name(value)));
                    return;
                }
                if (ot->cls->method(at->attr)) {
                    // `c.__annotations__ = d` 这类：dunder 是协议，静态不管
                    if (at->attr.size() > 4 && at->attr.starts_with("__") &&
                        at->attr.ends_with("__"))
                        return;
                    // `self.x = v` 可能走 @property 的 setter，静态分不清
                    if (is_self_attr(*at)) {
                        unsupported.push_back("assignment to a property setter");
                        return;
                    }
                    err(line, col, std::format("cannot assign to method '{}'", at->attr));
                    return;
                }
                // 外部基类可能带来任意属性
                if (ot->cls->is_open()) return;
                err(line, col,
                    std::format("'{}' object has no attribute '{}'", ot->cls->name, at->attr));
                return;
            }
            if (!is_unknown(ot))
                err(line, col,
                    std::format("'{}' has no assignable attribute '{}'", type_name(ot), at->attr));
            return;
        }
        err(line, col, "cannot assign to this expression");
    }

void TypeChecker::unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col) {
        size_t n = targets.size();
        size_t star_at = n;  // n 表示没有星号目标
        for (size_t i = 0; i < n; ++i)
            if (targets[i]->kind == ExprKind::Starred) { star_at = i; break; }

        auto assign_one = [&](Expr& t, TypePtr ty) {
            if (t.kind == ExprKind::Starred) {
                assign(*static_cast<Starred&>(t).value, ty, line, col);
            } else {
                assign(t, ty, line, col);
            }
        };

        if (value->kind == TyKind::Tuple) {
            if (value->variadic) {
                // 长度不定：每个目标拿元素类型，星号目标拿 list[元素]
                TypePtr e = value->sub.empty() ? t_unknown() : value->sub[0];
                for (auto& t : targets) {
                    if (t->kind == ExprKind::Starred)
                        assign_one(*t, mk_list(e));
                    else
                        assign_one(*t, e);
                }
                return;
            }
            auto& elems = value->sub;
            if (star_at == n) {
                if (!elems.empty() && elems.size() != n) {
                    err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                    return;
                }
                for (size_t i = 0; i < n; ++i)
                    assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
                return;
            }
            // a, *b, c = (...)：星号吃中间，剩余目标与末尾元素对齐
            size_t tail = n - star_at - 1;
            if (elems.size() + 1 < n) {
                err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                return;
            }
            for (size_t i = 0; i < star_at; ++i)
                assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
            TypePtr rest = nullptr;
            for (size_t i = star_at; i + tail < elems.size(); ++i)
                rest = rest ? join(rest, elems[i]) : elems[i];
            assign_one(*targets[star_at], mk_list(rest ? rest : t_unknown()));
            for (size_t j = 0; j < tail; ++j)
                assign_one(*targets[star_at + 1 + j], elems[elems.size() - tail + j]);
            return;
        }

        if (value->kind == TyKind::List || value->kind == TyKind::Set) {
            // list[int] 只记录元素类型，不记录长度，所以逐个赋元素类型；
            // 老代码把 sub（元素类型数组）当元素列表，`[a, b] = [1, 2]` 会误报。
            TypePtr elem = value->sub.empty() ? t_unknown() : value->sub[0];
            for (auto& t : targets) {
                if (t->kind == ExprKind::Starred)
                    assign_one(*t, mk_list(elem));
                else
                    assign_one(*t, elem);
            }
            return;
        }

        if (is_unknown(value) || value->kind == TyKind::None) {
            for (auto& t : targets) assign_one(*t, t_unknown());
            return;
        }
        if (value->kind == TyKind::Class) return;  // 可能实现了 __iter__
        err(line, col, std::format("cannot unpack non-sequence '{}'", type_name(value)));
    }



static void pattern_capture_names(const Pattern& p, std::vector<std::string>& out) {
    switch (p.kind) {
        case PatternKind::Capture:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            return;
        case PatternKind::As:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            for (auto& it : p.items) pattern_capture_names(*it, out);
            return;
        case PatternKind::Sequence:
        case PatternKind::Or:
            for (auto& it : p.items) pattern_capture_names(*it, out);
            return;
        case PatternKind::Mapping:
            for (auto& kv : p.pairs) pattern_capture_names(*kv.second, out);
            if (!p.rest.empty() && p.rest != "_") out.push_back(p.rest);
            return;
        case PatternKind::Class:
            for (auto& it : p.positional) pattern_capture_names(*it, out);
            for (auto& kv : p.attrs) pattern_capture_names(*kv.second, out);
            return;
        default:
            return;
    }
}

void TypeChecker::match_pattern(Pattern& p, TypePtr subject,
                                const std::unordered_set<std::string>* or_names) {
    auto declare_capture = [&](const std::string& name, TypePtr ty) {
        if (name.empty() || name == "_") return;
        if (or_names && or_names->count(name)) return;  // 已由 OR 统一声明
        if (cur_->vars.count(name)) return;             // 同一模式重复捕获
        declare(name, std::move(ty), p.line, p.col);
    };
    switch (p.kind) {
        case PatternKind::Literal:
            expr(*p.lit);
            return;
        case PatternKind::Value:
            expr(*p.lit);
            return;
        case PatternKind::Capture:
            declare_capture(p.name, subject);
            return;
        case PatternKind::Wildcard:
            return;
        case PatternKind::As: {
            for (auto& it : p.items) match_pattern(*it, subject, or_names);
            declare_capture(p.name, subject);
            return;
        }
        case PatternKind::Or: {
            std::vector<std::set<std::string>> sets;
            for (auto& it : p.items) {
                std::vector<std::string> names;
                pattern_capture_names(*it, names);
                sets.emplace_back(names.begin(), names.end());
            }
            for (size_t i = 1; i < sets.size(); ++i) {
                if (sets[i] != sets[0]) {
                    err(p.line, p.col,
                        "alternative patterns bind different names");
                    break;
                }
            }
            std::unordered_set<std::string> names(sets[0].begin(), sets[0].end());
            for (auto& n : names)
                if (!cur_->vars.count(n)) declare(n, t_unknown(), p.line, p.col);
            for (auto& it : p.items) match_pattern(*it, subject, &names);
            return;
        }
        case PatternKind::Sequence: {
            TypePtr t = subject;
            if (t && t->kind == TyKind::Optional && !t->sub.empty()) t = t->sub[0];
            if (t && t->kind == TyKind::Optional) {
                err(p.line, p.col, "sequence pattern cannot match Optional");
                return;
            }
            TypePtr elem = t_unknown();
            bool str_seq = t && t->kind == TyKind::Str;
            if (t && t->kind == TyKind::List) elem = t->sub.empty() ? t_unknown() : t->sub[0];
            else if (t && t->kind == TyKind::Str) {
                err(p.line, p.col, "sequence pattern cannot match str");
                return;
            } else if (t && t->kind == TyKind::Tuple) elem = t_unknown();
            else if (t && t->kind != TyKind::Unknown) {
                err(p.line, p.col,
                    std::format("sequence pattern cannot match '{}'", type_name(t)));
                return;
            }
            for (size_t i = 0; i < p.items.size(); ++i) {
                if (static_cast<int>(i) == p.star) {
                    if (p.items[i]->kind == PatternKind::Capture)
                        declare_capture(p.items[i]->name, mk_list(elem));
                    continue;
                }
                TypePtr et = elem;
                if (t && t->kind == TyKind::Tuple && !str_seq) {
                    et = i < t->sub.size() ? t->sub[i] : t_unknown();
                }
                match_pattern(*p.items[i], et, or_names);
            }
            return;
        }
        case PatternKind::Mapping: {
            TypePtr t = subject;
            if (t && t->kind == TyKind::Optional && !t->sub.empty()) t = t->sub[0];
            TypePtr kt = t_unknown();
            TypePtr val = t_unknown();
            if (t && t->kind == TyKind::Dict) {
                if (!t->sub.empty()) kt = t->sub[0];
                if (t->sub.size() > 1) val = t->sub[1];
            } else if (t && t->kind != TyKind::Unknown) {
                err(p.line, p.col,
                    std::format("mapping pattern cannot match '{}'", type_name(t)));
                return;
            }
            for (auto& kv : p.pairs) {
                expr(*kv.first);
                match_pattern(*kv.second, val, or_names);
            }
            if (!p.rest.empty() && p.rest != "_")
                declare_capture(p.rest, mk_dict(kt, val));
            return;
        }
        case PatternKind::Class: {
            if (p.cls.find('.') != std::string::npos) {
                size_t dot = p.cls.rfind('.');
                std::string mod_name = p.cls.substr(0, dot);
                std::string cls_name = p.cls.substr(dot + 1);
                auto mod = lookup_module_alias(mod_name);
                if (!mod) {
                    err(p.line, p.col,
                        std::format("unknown module '{}' in class pattern", mod_name));
                    return;
                }
                auto ci = mod->classes.find(cls_name);
                if (ci == mod->classes.end()) {
                    err(p.line, p.col,
                        std::format("module '{}' has no class '{}'", mod_name, cls_name));
                    return;
                }
                p.qualname = mod->name + "." + cls_name;
                if (!p.positional.empty() || !p.attrs.empty())
                    err(p.line, p.col,
                        "imported class patterns support only the class check "
                        "(field sub-patterns are not available)");
                return;
            }
            auto it = classes_.find(p.cls);
            if (it == classes_.end()) {
                if (is_exception_name(p.cls)) {
                    if (!p.positional.empty() || !p.attrs.empty())
                        err(p.line, p.col,
                            "exception class patterns cannot have sub-patterns");
                    return;
                }
                if (is_type_name(p.cls)) {
                    if (!p.positional.empty() || !p.attrs.empty())
                        err(p.line, p.col,
                            "builtin type patterns cannot have sub-patterns");
                    return;
                }
                err(p.line, p.col,
                    std::format("unknown class '{}' in class pattern", p.cls));
                return;
            }
            ClassInfo* pc = it->second.get();
            if (pc->is_exception_class) {
                if (!p.positional.empty() || !p.attrs.empty())
                    err(p.line, p.col,
                        "exception class patterns cannot have sub-patterns");
                return;
            }
            if (!pc->complete && pc != cur_class_ &&
                (!p.positional.empty() || !p.attrs.empty())) {
                err(p.line, p.col,
                    std::format("class '{}' must be fully defined before a class pattern "
                                "uses its fields", p.cls));
                return;
            }
            if (!p.positional.empty()) {
                const auto* margs = pc->match_args_of();
                if (!margs) {
                    err(p.line, p.col,
                        std::format("'{}' has no __match_args__ for positional patterns",
                                    pc->name));
                } else {
                    if (p.positional.size() > margs->size())
                        err(p.line, p.col,
                            std::format("'{}' accepts {} positional sub-pattern{}, got {}",
                                        pc->name, margs->size(),
                                        margs->size() == 1 ? "" : "s",
                                        p.positional.size()));
                    size_t n = std::min(p.positional.size(), margs->size());
                    for (size_t i = 0; i < n; ++i) {
                        const FieldInfo* f = pc->field((*margs)[i]);
                        if (!f) {
                            err(p.line, p.col,
                                std::format("__match_args__ of '{}' names unknown field '{}'",
                                            pc->name, (*margs)[i]));
                            continue;
                        }
                        match_pattern(*p.positional[i], f->ty, or_names);
                    }
                }
            }
            for (auto& kv : p.attrs) {
                const FieldInfo* f = pc->field(kv.first);
                if (!f) {
                    err(p.line, p.col,
                        std::format("'{}' object has no field '{}'", pc->name, kv.first));
                    continue;
                }
                match_pattern(*kv.second, f->ty, or_names);
            }
            return;
        }
    }
}

}  // namespace py23
