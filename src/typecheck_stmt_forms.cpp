// typecheck_stmt_forms.cpp —— stmt 按语句形态拆分
#include "typecheck.h"

#include <filesystem>
#include <set>

#include "module.h"

namespace pyadamas {

void TypeChecker::stmt_assign(Assign& a) {
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
        return;
    }

void TypeChecker::stmt_aug_assign(AugAssign& a) {
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
            return;
        }
        auto vt = expr(*a.value);
        // x += y：优先原地 dunder，没有再看普通 __add__ / __radd__。
        if (TypePtr irt = dunder_aug_ret(a.op, cur_ty, vt)) {
            assign(*a.target, irt, a.line, a.col);
            return;
        }
        Tok bin = aug_to_bin(a.op);
        auto rt = binop_type(bin, cur_ty, vt, a.line, a.col);
        assign(*a.target, rt, a.line, a.col);
        return;
    }

void TypeChecker::stmt_ann_assign(AnnAssign& a) {
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
        return;
    }

void TypeChecker::stmt_return(Return& r) {
        TypePtr vt = r.value ? expr(*r.value) : t_none();
        if (!cur_fn_) {
            err(r.line, r.col, "'return' outside function");
            return;
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
        return;
    }

void TypeChecker::stmt_if(If& f) {
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
                cur_->vars.emplace(name, Symbol{ty, f.line, false, false});
            else
                it->second.ty = ty;
        }
        return;
    }

void TypeChecker::stmt_while(While& w) {
        expr(*w.cond);
        ++loop_depth_;
        stmts(w.body);
        --loop_depth_;
        stmts(w.orelse);
        return;
    }

void TypeChecker::stmt_for(For& f) {
        auto it = expr(*f.iter);
        TypePtr elem;
        if (f.is_async) {
            // 自定义 __aiter__：先静态解析成异步迭代器类型。
            TypePtr iter_ty = it;
            bool via_aiter = false;
            if (it && it->kind == TyKind::Class && it->cls) {
                if (auto* ai = it->cls->method("__aiter__")) {
                    via_aiter = true;
                    iter_ty = t_unknown();
                    if (ai->call_sig && ai->call_sig->ret) iter_ty = ai->call_sig->ret;
                    if (ai->decorated && ai->decorated_ty &&
                        ai->decorated_ty->kind == TyKind::Func &&
                        ai->decorated_ty->fn && ai->decorated_ty->fn->ret)
                        iter_ty = ai->decorated_ty->fn->ret;
                }
            }
            if (is_unknown(it) || (via_aiter && is_unknown(iter_ty))) {
                elem = t_unknown();
            } else if (iter_ty && iter_ty->kind == TyKind::AsyncGenerator) {
                elem = iter_ty->sub.empty() ? t_unknown() : iter_ty->sub[0];
            } else if (via_aiter && iter_ty && iter_ty->kind == TyKind::Class &&
                       iter_ty->cls && iter_ty->cls->method("__anext__")) {
                // __aiter__ 返回自身（或另一个异步迭代器）：每轮
                // await __anext__()，元素类型是协程的结果。
                auto* ne = iter_ty->cls->method("__anext__");
                TypePtr nt = ne->call_sig ? ne->call_sig->ret : nullptr;
                if (ne->decorated && ne->decorated_ty &&
                    ne->decorated_ty->kind == TyKind::Func && ne->decorated_ty->fn)
                    nt = ne->decorated_ty->fn->ret;
                if (nt && nt->kind == TyKind::Coroutine)
                    elem = nt->sub.empty() ? t_unknown() : nt->sub[0];
                else {
                    err(f.iter->line, f.iter->col,
                        std::format("async for requires __anext__ to be async on '{}'",
                                    iter_ty->cls->name));
                    elem = t_unknown();
                }
            } else {
                err(f.iter->line, f.iter->col,
                    std::format("async for requires an async generator or a class "
                                "with __aiter__, got '{}'",
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
        return;
    }

void TypeChecker::stmt_try(Try& t) {
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
                // as e 绑定异常值对象。富异常类（有字段/方法）绑定成真实例，
                // 字段/方法走普通类访问；pass-only 或内置异常仍是 Exception 值。
                TypePtr et = mk_exception("Exception");
                if (h.type && h.type->kind == ExprKind::Name) {
                    auto& hn = static_cast<Name&>(*h.type);
                    if (is_exception_name(hn.id)) {
                        et = mk_exception(hn.id);
                    } else if (auto* sym = lookup(hn.id);
                               sym && sym->ty && sym->ty->kind == TyKind::Class &&
                               sym->ty->cls && sym->ty->cls->is_exception_class &&
                               sym->ty->cls->is_rich_exception) {
                        et = mk_class(sym->ty->cls);
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
        return;
    }

void TypeChecker::stmt_with(With& w) {
        for (auto& item : w.items) {
            auto t = expr(*item.context);
            // `with Ctx() as c` 的 c 是 __enter__ 的返回值；能查到
            // 方法签名就用它，查不到退回 unknown（with 本身不检查协议）。
            TypePtr vt = t_unknown();
            TypePtr ct = t;
            if (ct && ct->kind == TyKind::Optional && !ct->sub.empty()) ct = ct->sub[0];
            if (ct && ct->kind == TyKind::Class && ct->cls) {
                const char* enter = w.is_async ? "__aenter__" : "__enter__";
                if (auto* m = ct->cls->method(enter); m && m->call_sig && m->call_sig->ret) {
                    vt = m->call_sig->ret;
                    // 装饰后的方法调用的是类属性里的包装函数，
                    // 绑定值按包装函数的返回类型算。
                    if (m->decorated && m->decorated_ty &&
                        m->decorated_ty->kind == TyKind::Func && m->decorated_ty->fn &&
                        m->decorated_ty->fn->ret)
                        vt = m->decorated_ty->fn->ret;
                }
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
        return;
    }

void TypeChecker::stmt_raise(Raise& r) {
        if (r.exc) expr(*r.exc);
        if (r.cause) {
            auto ct = expr(*r.cause);
            if (!is_unknown(ct) && ct->kind != TyKind::None &&
                ct->kind != TyKind::Exception)
                err(r.cause->line, r.cause->col,
                    "raise from requires an exception or None");
        }
        return;
    }

void TypeChecker::stmt_assert(Assert& a) {
        expr(*a.test);
        if (a.msg) expr(*a.msg);
        return;
    }

void TypeChecker::stmt_import(Import& im) {
        for (auto& al : im.names) {
            std::string bind = al.asname.empty() ? al.name : al.asname;
            if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
            if (!module_lookup_) {
                // 单文件 check：import 没建模，保持前端宽松。
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
                continue;
            }
            if (al.name.find('.') != std::string::npos && al.asname.empty()) {
                err(im.line, im.col,
                    "dotted import needs 'as name' (or use from ... import)");
                continue;
            }
            ModuleInfoPtr info = module_lookup_(al.name);
            if (info) {
                cur_->modules[bind] = std::move(info);
            } else {
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
            }
        }
        return;
    }

void TypeChecker::stmt_cimport(CImport& ci) {
        std::string dir = std::filesystem::path(file_).parent_path().string();
        for (auto& al : ci.names) {
            std::string bind = al.asname;
            if (bind.empty()) {
                auto dot = al.name.rfind('.');
                bind = dot == std::string::npos ? al.name : al.name.substr(dot + 1);
            }
            if (lookup(bind)) {
                err(ci.line, ci.col,
                    std::format("name '{}' is already defined in this module", bind));
                continue;
            }
            if (cmodules_.count(bind)) {
                err(ci.line, ci.col,
                    std::format("cimport name '{}' is already defined", bind));
                continue;
            }
            std::string load_err;
            auto decl = load_cpya(al.name, dir, cinclude_dirs_, load_err);
            if (!decl) {
                err(ci.line, ci.col, load_err);
                continue;
            }
            auto shared = std::make_shared<CImportDecl>(std::move(*decl));
            std::unordered_map<std::string, StructInfo*> cmap;
            for (auto& cs : shared->structs) {
                auto it = structs_.find(cs.name);
                if (it != structs_.end()) {
                    // 同名 struct 必须来自另一个 cimport 且布局一致；
                    // 与 .pya struct 冲突时不能悄悄用错布局。
                    if (!it->second->c_struct || it->second->size != cs.size ||
                        it->second->align != cs.align) {
                        err(ci.line, ci.col,
                            std::format("struct '{}' is already defined", cs.name));
                    }
                    cmap[cs.name] = it->second;
                    continue;
                }
                StructInfo* st = build_cstruct(cs);
                structs_[cs.name] = st;
                // 允许在 PyAdamas 里按字段构造 C 结构体（DivT(7, 3)）；
                // 已有同名符号时不覆盖。
                if (!lookup(cs.name))
                    module_->vars[cs.name] = Symbol{mk_struct(st), ci.line, true, false};
                cmap[cs.name] = st;
            }
            auto cty = [&](CKind k, const std::string& sname) -> TypePtr {
                if (!sname.empty()) {
                    auto it = cmap.find(sname);
                    if (it != cmap.end()) {
                        if (k == CKind::Ptr) return mk_optional(mk_struct_ptr(it->second));
                        if (k == CKind::Struct) return mk_struct(it->second);
                    }
                }
                return ckind_type(k);
            };
            for (auto& ex : shared->externs) {
                auto fi = std::make_shared<FuncInfo>();
                fi->name = ex.py_name;
                fi->is_extern = true;
                fi->ret_declared = true;
                fi->ret_known = true;
                fi->ret = cty(ex.ret, ex.ret_struct);
                if (ex.ret == CKind::Ptr && ex.ret_elem != CKind::None && fi->ret)
                    fi->ret->celem = ex.ret_elem;
                if (ex.varargs) {
                    fi->varargs = true;
                    fi->vararg_ty = t_unknown();
                }
                for (auto& p : ex.params) {
                    Param pm;
                    pm.name = p.name;
                    pm.ty = cty(p.kind, p.ptr_struct);
                    if (p.kind == CKind::Ptr && p.ptr_elem != CKind::None && pm.ty)
                        pm.ty->celem = p.ptr_elem;
                    if (p.kind == CKind::Ptr) {
                        pm.cptr = true;
                        pm.cptr_elem = p.ptr_elem;
                        // 所有结构体指针都不接受 list/bytes 缓冲区。
                        pm.cptr_opaque = p.opaque || !p.ptr_struct.empty();
                    }
                    fi->pos.push_back(std::move(pm));
                }
                extern_by_fn_[fi.get()] = &ex;
                extern_fns_[&ex] = std::move(fi);
            }
            cmodules_[bind] = std::move(shared);
        }
        return;
    }

void TypeChecker::stmt_import_from(ImportFrom& im) {
        if (!module_lookup_) {
            for (auto& al : im.names) {
                std::string bind = al.asname.empty() ? al.name : al.asname;
                if (bind == "*") {
                    star_import_ = true;
                    continue;
                }
                declare(bind, t_unknown(), im.line);
            }
            unsupported.push_back("import");
            return;
        }
        if (im.level != 0 || im.module.empty()) {
            err(im.line, im.col, "relative imports are not supported yet");
            return;
        }
        if (im.names.size() == 1 && im.names[0].name == "*") {
            ModuleInfoPtr info = module_lookup_(im.module);
            if (!info) {
                star_import_ = true;
                unsupported.push_back("import");
                return;
            }
            for (auto& [n, ty] : info->globals)
                if (!n.empty() && n[0] != '_') declare(n, ty, im.line);
            for (auto& [n, ic] : info->classes) {
                if (n.empty() || n[0] == '_') continue;
                declare(n, mk_class(ic.cls.get()), im.line);
                class_obj_names_.insert(n);
            }
            return;
        }
        ModuleInfoPtr info = module_lookup_(im.module);
        for (auto& al : im.names) {
            std::string bind = al.asname.empty() ? al.name : al.asname;
            if (!info) {
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
                continue;
            }
            if (auto ci = info->classes.find(al.name); ci != info->classes.end()) {
                declare(bind, mk_class(ci->second.cls.get()), im.line);
                class_obj_names_.insert(bind);
                continue;
            }
            auto it = info->globals.find(al.name);
            if (it == info->globals.end()) {
                if (auto child = module_lookup_(im.module + "." + al.name)) {
                    cur_->modules[bind] = std::move(child);
                    continue;
                }
                err(im.line, im.col,
                    std::format("module '{}' has no attribute '{}'", im.module, al.name));
                declare(bind, t_unknown(), im.line);
                continue;
            }
            if (info->builtin && info->name == "asyncio")
                native_aliases_[bind] = NativeAlias{info->name, al.name, it->second};
            declare(bind, it->second, im.line);
        }
        return;
    }

void TypeChecker::stmt_delete(Delete& a) {
        for (auto& t : static_cast<Delete&>(a).targets) expr(*t);
        return;
    }

}  // namespace pyadamas
