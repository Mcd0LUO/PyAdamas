// typecheck_func.cpp —— 函数与类：定义体、方法、隐式字段
#include "typecheck.h"

#include "module.h"

namespace py23 {

// C3 线性化：反复取各序列中「不在任何序列尾部」的头部。失败说明
// 继承关系自相矛盾（典型是菱形里父类顺序冲突）。
static bool c3_merge(std::vector<std::vector<ClassInfo*>>& seqs,
                     std::vector<ClassInfo*>& out) {
    for (;;) {
        bool any_left = false;
        for (auto& s : seqs)
            if (!s.empty()) any_left = true;
        if (!any_left) return true;
        bool picked = false;
        for (auto& seq : seqs) {
            if (seq.empty()) continue;
            ClassInfo* cand = seq.front();
            bool in_tail = false;
            for (auto& other : seqs) {
                for (size_t i = 1; i < other.size(); ++i)
                    if (other[i] == cand) {
                        in_tail = true;
                        break;
                    }
                if (in_tail) break;
            }
            if (in_tail) continue;
            out.push_back(cand);
            for (auto& other : seqs)
                if (!other.empty() && other.front() == cand) other.erase(other.begin());
            picked = true;
            break;
        }
        if (!picked) return false;
    }
}

void TypeChecker::funcdef(FunctionDef& f) { funcdef_impl(f, nullptr); }

void TypeChecker::funcdef_impl(FunctionDef& f, const ClassInfoPtr& owner) {
        if (!func_owners_.count(&f)) {
            func_owners_[&f] = owner;
            // 只登记模块级函数和方法：嵌套函数（含 lambda 脱糖产物）的
            // 词法父作用域在重查时要重新挂接，收益不值这个复杂度。
            // 生成器表达式的合成函数例外：它的形参就是最外层 iterable，
            // 拿不到实参类型就没法在降级层选迭代路径。
            if (cur_ == module_ || owner) {
                func_defs_.push_back(&f);
            } else if (f.name.rfind("__py23$gen_", 0) == 0) {
                nested_recheck_parent_[&f] = cur_;
                func_defs_.push_back(&f);
            }
        }
        FuncInfoPtr info;
        if (owner) {
            // 方法名属于类命名空间，不进外层作用域；签名在 classdef 里
            // 已经建好（方法之间要能互相调用），这里复用同一份对象。
            auto fit = func_infos_.find(&f);
            if (fit != func_infos_.end() && fit->second) {
                info = fit->second;
            } else {
                info = build_sig(f);
                bool is_static = false;
                for (auto& d : f.decorators)
                    if (auto* dn = dynamic_cast<Name*>(d.get()); dn && dn->id == "staticmethod")
                        is_static = true;
                if (!is_static && !info->pos.empty() && is_unknown(info->pos[0].ty))
                    info->pos[0].ty = mk_class(owner.get());
            }
        } else if (auto* sym = lookup(f.name); sym && sym->ty && sym->ty->kind == TyKind::Func &&
                                       sym->ty->fn && sym->ty->fn->name == f.name) {
            info = sym->ty->fn;
        } else {
            info = build_sig(f);
            if (f.decorators.empty()) {
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                declare(f.name, t, f.line, f.col, true);
            } else {
                // 装饰器的返回值未知；函数体仍然按自己的签名检查
                declare(f.name, t_unknown(), f.line, f.col, true);
            }
        }

        if (!owner)
            if (auto* sym = lookup(f.name)) sym->from_def = true;

        // 默认值和装饰器在外层作用域求值 —— Python 是在 def 执行时算的，
        // 此时形参还不存在。先检查再进函数作用域，`def f(a, b=a)` 里的 a
        // 才不会被误解析成形参。
        for (auto& d : f.args.defaults) expr(*d);
        for (auto& d : f.args.kw_defaults)
            if (d) expr(*d);
        for (auto& dec : f.decorators)
            if (!owner) expr(*dec);

        // 装饰器的返回值常常就是包装函数：从下往上套，每一层用返回类型
        // 替换当前类型。这样调用点能按包装签名检查实参并做数值转换，
        // 而不是退化成 unknown 后按错误的调用约定传参。
        if (!owner && !f.decorators.empty()) {
            TypePtr cur = new_ty(TyKind::Func);
            cur->fn = info;
            bool known = true;
            for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
                TypePtr dt = type_of(it->get());
                if (dt && dt->kind == TyKind::Func && dt->fn && dt->fn->ret) {
                    cur = dt->fn->ret;
                } else {
                    known = false;
                    break;
                }
            }
            if (known && cur && cur->kind == TyKind::Func) {
                if (auto* sym = lookup(f.name)) sym->ty = cur;
            }
        }

        auto prev_fn = cur_fn_;
        bool prev_yield = saw_yield_;
        auto* prev_yields = yields_;
        auto* prev_scope = cur_;   // 必须回到进入函数前的作用域，不能一律挂到 module_
        auto sc = std::make_unique<Scope>();
        sc->parent = prev_scope;
        sc->fn_owner = &f;
        cur_ = sc.get();
        func_scopes_[&f] = sc.get();
        func_infos_[&f] = info;
        auto* prev_fn_def = cur_fn_def_;
        auto* prev_fn_scope = cur_fn_scope_;
        cur_fn_ = info;
        cur_fn_def_ = &f;
        cur_fn_scope_ = sc.get();
        loop_depth_ = 0;
        auto saved_globals = std::move(global_decl_);
        auto saved_nonlocals = std::move(nonlocal_decl_);
        global_decl_.clear();
        nonlocal_decl_.clear();
        std::vector<RetInfo> rets;
        auto* prev_rets = rets_;
        rets_ = &rets;
        std::vector<TypePtr> yields;
        yields_ = &yields;
        saw_yield_ = false;

        for (auto& p : info->pos)
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, true, false};
        for (auto& p : info->kwonly)
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, true, false};
        if (info->varargs)
            // *args 在 Python 里是 tuple，不是 list：`(v,) + args` 合法
            cur_->vars[info->vararg_name] =
                Symbol{mk_tuple_rest(info->vararg_ty ? info->vararg_ty : t_unknown()), f.line, true,
                       false};
        if (info->kwargs)
            cur_->vars[info->kwarg_name] =
                Symbol{mk_dict(t_str(), info->kwarg_ty ? info->kwarg_ty : t_unknown()),
                       f.line, true, false};

        // global / nonlocal 作用于整个函数，先收集，再让 hoist 跳过这些名字。
        collect_fn_decls(f.body, global_decl_, nonlocal_decl_);
        hoist_scope_bindings(f.body, cur_);
        for (auto& n : global_decl_) cur_->vars.erase(n);
        for (auto& n : nonlocal_decl_) cur_->vars.erase(n);
        auto saved_self_name = cur_self_name_;  // 拷贝：下面要让嵌套函数看见它
        bool owner_is_static = false;
        if (owner) {
            for (auto& d : f.decorators)
                if (auto* dn = dynamic_cast<Name*>(d.get()); dn && dn->id == "staticmethod")
                    owner_is_static = true;
        }
        if (owner && !owner_is_static)
            cur_self_name_ = info->pos.empty() ? std::string() : info->pos[0].name;
        // @classmethod：首参是类不是实例，`cls.x` 要走类属性查找
        auto saved_cls_name = cur_cls_name_;
        bool owner_is_cmethod = false;
        if (owner) {
            if (const MethodInfo* om = owner->method(f.name))
                owner_is_cmethod = om->kind == MethodKind::ClassMethod;
        }
        if (owner_is_cmethod && !info->pos.empty()) cur_cls_name_ = info->pos[0].name;
        // 嵌套函数不改它：闭包捕获的 self 还是外层那个名字
        stmts(f.body);
        cur_self_name_ = std::move(saved_self_name);
        cur_cls_name_ = std::move(saved_cls_name);

        // 无标注函数：从所有 return 处推断返回类型
        TypePtr inferred = nullptr;
        if (!info->ret_declared) {
            for (auto& r : rets) inferred = inferred ? join(inferred, r.ty) : r.ty;
        }

        if (f.is_async) {
            // async def 调用得到协程对象，不是 return 标注里的值；
            // `_c = _c()` 这类「函数名复用成结果」的写法全靠这条才不误报。
            if (saw_yield_) {
                // 异步生成器：调用得到 asyncgen[T]，只供 async for 消费。
                TypePtr elem = nullptr;
                for (auto& y : yields) elem = elem ? join(elem, y) : y;
                info->is_async_generator = true;
                info->ret = mk_async_generator(elem ? elem : t_unknown());
                info->async_result = t_none();
            } else {
                if (!info->ret_declared)
                    info->async_result = inferred ? inferred : t_unknown();
                if (!info->async_result) info->async_result = t_unknown();
                info->ret = mk_coroutine(info->async_result);
            }
        } else if (saw_yield_) {
            // 生成器：调用得到 Generator[所有 yield 值的公共上界]
            TypePtr elem = nullptr;
            for (auto& y : yields) elem = elem ? join(elem, y) : y;
            info->ret = mk_generator(elem ? elem : t_unknown());
            info->is_generator = true;
        } else if (!info->ret_declared) {
            info->ret = inferred;
        }
        info->ret_known = true;

        cur_ = prev_scope;
        cur_fn_ = prev_fn;
        cur_fn_def_ = prev_fn_def;
        cur_fn_scope_ = prev_fn_scope;
        saw_yield_ = prev_yield;
        yields_ = prev_yields;
        rets_ = prev_rets;
        global_decl_ = std::move(saved_globals);
        nonlocal_decl_ = std::move(saved_nonlocals);
        // 保留函数作用域：IR 降级要按名字查最终类型
        pool_.push_back(std::move(sc));
    }

bool TypeChecker::is_self_attr(const Attribute& a) const {
    if (!cur_class_ || cur_self_name_.empty()) return false;
    if (a.value->kind != ExprKind::Name) return false;
    return static_cast<const Name&>(*a.value).id == cur_self_name_;
}

// `C.x` / `cls.x`：接收者是一个类，不是实例。区分不了的时候按实例处理——
// 实例侧多了字段回退，宁可宽松也不要误判。
bool TypeChecker::is_class_receiver(const Attribute& a) {
    if (a.value->kind != ExprKind::Name) return false;
    auto& n = static_cast<const Name&>(*a.value);
    if (!cur_cls_name_.empty() && n.id == cur_cls_name_) return true;
    if (!cur_self_name_.empty() && n.id == cur_self_name_) return false;
    // 类名绑定在模块作用域里；被函数局部或形参遮住就不是类了
    for (Scope* s = cur_; s; s = s->parent) {
        if (s->vars.find(n.id) == s->vars.end()) continue;
        return s->is_module && classes_.count(n.id) > 0;
    }
    return false;
}

// `self.x = ...` 里 x 没在类体里声明过：补成字段。Python 的实例属性本来
// 就是在 __init__ 里长出来的，强制先写标注会把标准库挡在门外。
// 返回 true 表示这次赋值已经处理完（隐式字段一律放宽，不再做类型检查）；
// 返回 false 表示字段是显式声明的，调用方仍要按标注检查类型。
bool TypeChecker::add_implicit_field(const Attribute& a, TypePtr ty) {
    if (!is_self_attr(a)) return false;
    // 只动本类声明的字段；父类的字段不能被子类的赋值悄悄改类型
    for (auto& f : cur_class_->fields) {
        if (f.name != a.attr) continue;
        if (!f.implicit) return false;
        // 任一边 unknown 就保持 unknown：不知道就是不知道，别拿后来那个
        // 具体类型当准（`self.x = make()` 之后 `self.x = None` 会把类型
        // 覆盖成 None，后面每次 self.x.anything 全被判死）
        if (is_unknown(ty) || is_unknown(f.ty)) f.ty = t_unknown();
        else f.ty = join(f.ty, ty);
        return true;
    }
    if (cur_class_->field(a.attr)) return false;   // 继承来的字段，按标注检查
    if (cur_class_->method(a.attr)) return false;  // 同名方法：报「不能给方法赋值」
    FieldInfo fi;
    fi.name = a.attr;
    // `self.x = None` 只说明「还没赋值」，拿 None 当字段类型会让后面
    // 每一次 self.x.anything 都被判死
    fi.ty = (ty && ty->kind != TyKind::None) ? ty : t_unknown();
    fi.implicit = true;
    cur_class_->fields.push_back(std::move(fi));
    return true;
}

// 类体里的语句：方法定义和字段标注之外，普通赋值按类属性处理（存进
// 模块里的隐藏全局），其余（if / for / with …）在 Python 里是「类体在
// 独立命名空间里执行」，py23 没有运行期类对象，标记成「没建模」，
// 由编译阶段拒绝——不能在这里报错，真实代码太常见。
void TypeChecker::classdef(ClassDef& c) {
        if (!c.decorators.empty()) {
            unsupported.push_back("class decorator");
            for (auto& d : c.decorators) expr(*d);
        }
        if (!c.keywords.empty()) {
            // metaclass=... 之类的类关键字：语法合法，语义没建模。
            // 按「解析了但还没支持」处理，不误杀真实代码。
            unsupported.push_back("class keyword arguments");
            for (auto& kw : c.keywords)
                if (kw.value) expr(*kw.value);
        }
        ClassInfo* base = nullptr;
        std::vector<ClassInfo*> extra_bases;
        bool is_exc_class = false;
        std::string exc_parent;
        // 多继承：第一个基类提供布局，后续基类只贡献方法（字段必须为空）。
        bool open = false;
        if (!c.bases.empty()) {
            // 自定义异常类：只有单基类且是内置异常名/自定义异常类时成立。
            if (c.bases.size() == 1) {
                if (auto* bn = dynamic_cast<Name*>(c.bases[0].get())) {
                    if (!lookup(bn->id) && is_exception_name(bn->id)) {
                        is_exc_class = true;
                        exc_parent = bn->id;
                    } else if (auto it = classes_.find(bn->id);
                               it != classes_.end() && it->second->is_exception_class) {
                        is_exc_class = true;
                        exc_parent = bn->id;
                    }
                }
            }
            if (!is_exc_class) {
                for (auto& b : c.bases) {
                    TypePtr bt = expr(*b);
                    if (bt && bt->kind == TyKind::Class) {
                        if (!base) base = bt->cls;
                        else extra_bases.push_back(bt->cls);
                    } else if (is_unknown(bt)) {
                        open = true;
                    } else if (auto* bn = dynamic_cast<Name*>(b.get());
                               bn && is_type_name(bn->id)) {
                        open = true;
                    } else {
                        err(b->line, b->col,
                            std::format("base class must be a class, got '{}'",
                                        type_name(bt)));
                    }
                }
            }
        }

        // 复用 collect_toplevel 建的骨架（函数签名已经引用了它的指针），
        // 只有同名类重定义时才新建。
        ClassInfoPtr cls;
        if (auto it = classes_.find(c.name); it != classes_.end() && !it->second->complete) {
            cls = it->second;
            cls->fields.clear();
            cls->methods.clear();
            cls->extra_bases.clear();
            cls->properties.clear();
            cls->mro.clear();
        } else {
            cls = std::make_shared<ClassInfo>();
        }
        cls->name = c.name;
        cls->base = base;
        cls->open = open;
        cls->is_exception_class = is_exc_class;
        cls->exc_parent = exc_parent;
        cls->extra_bases = extra_bases;
        for (auto* eb : extra_bases) {
            // 附加基类不能引入主基类布局里没有的字段；共享祖先（菱形
            // 的 A）只要偏移一致就允许，否则继承方法里的字段偏移会错位。
            for (const ClassInfo* k = eb; k; k = k->base) {
                for (const FieldInfo& f : k->fields) {
                    int eidx = eb->field_index(f.name);
                    int pidx = base ? base->field_index(f.name) : -1;
                    if (pidx != eidx)
                        err(c.line, c.col,
                            std::format("multiple inheritance base '{}' introduces field '{}' "
                                        "that is not in the primary base layout", eb->name,
                                        f.name));
                }
            }
            if (eb->is_open()) cls->open = true;
        }
        if (base) {
            cls->base_fields = base->n_fields();
            cls->has_field_defaults = base->has_field_defaults;
        }

        // C3 MRO。方法查找、super() 和 vtable 填充都按这条链走。
        {
            std::vector<ClassInfo*> known;
            if (base) known.push_back(base);
            for (ClassInfo* e : extra_bases) known.push_back(e);
            for (size_t i = 0; i < known.size(); ++i)
                for (size_t j = i + 1; j < known.size(); ++j)
                    if (known[i] == known[j])
                        err(c.line, c.col,
                            std::format("duplicate base class '{}'", known[i]->name));
            std::vector<std::vector<ClassInfo*>> seqs;
            for (ClassInfo* b : known) {
                if (b->mro.empty()) seqs.push_back({b});
                else seqs.push_back(b->mro);
            }
            seqs.push_back(known);
            std::vector<ClassInfo*> linear;
            cls->mro.clear();
            cls->mro.push_back(cls.get());
            if (c3_merge(seqs, linear)) {
                for (ClassInfo* b : linear) cls->mro.push_back(b);
            } else {
                err(c.line, c.col,
                    std::format("cannot create a consistent method resolution order "
                                "for class '{}'", c.name));
                for (ClassInfo* b : known) cls->mro.push_back(b);
            }
        }

        // 先把类名绑定好：字段标注和字符串注解可能引用类自身（`next: "Node"`）。
        // collect_toplevel 已经占了个 unknown 的坑，这里要真的换成类类型，
        // 不能走 declare —— 它会因为「已标注」而拒绝改类型。
        {
            Scope* sc = assign_target_scope(c.name);
            auto it = sc->vars.find(c.name);
            if (it != sc->vars.end()) {
                it->second.ty = mk_class(cls.get());
                it->second.annotated = true;
            } else {
                sc->vars[c.name] = Symbol{mk_class(cls.get()), c.line, true, false};
            }
        }
        auto prev = classes_.find(c.name);
        classes_[c.name] = cls;
        all_classes_.push_back(cls);
        (void)prev;  // 同名类重定义：后面的引用拿新类，旧实例仍按旧布局走
        if (!cur_->is_module) unsupported.push_back("nested class definition");
        if (is_exc_class) {
            // 自定义异常类目前只支持继承 + pass/文档字符串。
            for (auto& s : c.body) {
                if (s->kind == StmtKind::Pass) continue;
                if (s->kind == StmtKind::Expr) {
                    auto& e = static_cast<ExprStmt&>(*s);
                    if (e.value->kind == ExprKind::Constant &&
                        static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                        continue;
                }
                err(s->line, s->col,
                    "custom exception classes cannot define fields or methods yet");
            }
            cls->complete = true;
            return;
        }

        // 字段先处理：方法体里 `self.x` 的类型要靠它。
        for (auto& s : c.body) {
            if (s->kind != StmtKind::AnnAssign) continue;
            auto& a = static_cast<AnnAssign&>(*s);
            if (a.target->kind != ExprKind::Name) {
                err(s->line, s->col, "class field must be a plain name");
                continue;
            }
            auto& fn = static_cast<Name&>(*a.target);
            TypePtr ty = parse_annotation(a.annotation.get(), s->line);
            if (is_unknown(ty)) {
                // 标注引用了解析不出来的名字（典型：类变量 mytype = int 之后
                // 写 x: mytype）。类体赋值我们不建模，这里跟着降级，别误杀。
                unsupported.push_back("unresolved class field annotation");
            }
            if (cls->field_index(fn.id) >= 0) {
                err(s->line, s->col, std::format("duplicate field '{}'", fn.id));
                continue;
            }
            bool has_default = false;
            if (a.value) {
                auto vt = expr(*a.value);
                if (!assignable(ty, vt))
                    err(a.value->line, a.value->col,
                        std::format("cannot initialize field '{}' of type '{}' with '{}'", fn.id,
                                    type_name(ty), type_name(vt)));
                has_default = true;
                cls->has_field_defaults = true;
            }
            FieldInfo fi;
            fi.name = fn.id;
            fi.ty = ty ? ty : t_unknown();
            fi.has_default = has_default;
            fi.default_expr = a.value.get();
            cls->fields.push_back(std::move(fi));
        }

        // __match_args__：位置形式类模式的字段顺序。class 体里的赋值仍然
        // 按模块全局执行（没有类命名空间），这里只额外抽一份编译期元数据。
        for (auto& s : c.body) {
            if (s->kind != StmtKind::Assign) continue;
            auto& a = static_cast<Assign&>(*s);
            if (a.targets.size() != 1 || a.targets[0]->kind != ExprKind::Name) continue;
            if (static_cast<Name&>(*a.targets[0]).id != "__match_args__") continue;
            std::vector<ExprPtr>* elts = nullptr;
            if (a.value->kind == ExprKind::Tuple)
                elts = &static_cast<TupleExpr&>(*a.value).elts;
            else if (a.value->kind == ExprKind::List)
                elts = &static_cast<ListExpr&>(*a.value).elts;
            cls->match_args.clear();
            bool ok = elts != nullptr;
            if (ok) {
                for (auto& e : *elts) {
                    auto* cc = dynamic_cast<Constant*>(e.get());
                    if (!cc || cc->ck != Constant::CK::Str) {
                        ok = false;
                        break;
                    }
                    cls->match_args.push_back(cc->s);
                }
            }
            if (!ok) {
                cls->match_args.clear();
                unsupported.push_back("non-constant __match_args__");
            }
        }

        // 方法签名先全体登记，方法之间可以互相调用（包括调用本类的其他
        // 方法，以及被子类覆盖的方法名）。
        auto decor_kind = [](const FunctionDef& f, bool& ok) -> MethodKind {
            ok = true;
            if (f.decorators.empty()) return MethodKind::Normal;
            if (f.decorators.size() != 1) {
                ok = false;
                return MethodKind::Normal;
            }
            Expr* d = f.decorators[0].get();
            if (auto* n = dynamic_cast<Name*>(d)) {
                if (n->id == "staticmethod") return MethodKind::Static;
                if (n->id == "classmethod") return MethodKind::ClassMethod;
                if (n->id == "property") return MethodKind::PropertyGetter;
                ok = false;
                return MethodKind::Normal;
            }
            if (auto* a = dynamic_cast<Attribute*>(d)) {
                if (auto* bn = dynamic_cast<Name*>(a->value.get()); bn && bn->id == f.name) {
                    if (a->attr == "setter") return MethodKind::PropertySetter;
                    if (a->attr == "deleter") return MethodKind::PropertyDeleter;
                }
            }
            ok = false;
            return MethodKind::Normal;
        };
        auto rebuild_call_sig = [](MethodInfo& m) {
            if (!m.sig) return;
            if (m.kind == MethodKind::Normal && m.def && !m.def->decorators.empty()) {
                // 未识别的方法装饰器：语义不确定，调用点不检查实参。
                m.call_sig = nullptr;
                return;
            }
            m.call_sig = std::make_shared<FuncInfo>(*m.sig);
            m.call_sig->name = m.name;
            m.call_sig->outer = m.sig.get();
            m.call_sig->outer_off = 0;
            if (m.kind == MethodKind::Static) return;  // 静态方法没有隐式 self
            if (m.call_sig->pos.empty()) {
                m.call_sig = nullptr;
                return;
            }
            m.call_sig->pos.erase(m.call_sig->pos.begin());
            m.call_sig->outer_off = 1;
            if (m.call_sig->n_posonly > 0) --m.call_sig->n_posonly;
        };
        for (auto& s : c.body) {
            if (s->kind != StmtKind::FunctionDef) continue;
            auto& f = static_cast<FunctionDef&>(*s);
            bool decor_ok = true;
            MethodKind kind = decor_kind(f, decor_ok);
            if (!decor_ok) unsupported.push_back("method decorator");
            MethodInfo m;
            m.name = f.name;
            m.kind = kind;
            m.sig = build_sig(f);
            int midx = static_cast<int>(cls->methods.size());
            if (kind == MethodKind::Normal || kind == MethodKind::ClassMethod || !decor_ok) {
                if (m.sig->pos.empty()) {
                    err(f.line, f.col,
                        std::format("method '{}' must take 'self' as first parameter", f.name));
                } else if (is_unknown(m.sig->pos[0].ty)) {
                    m.sig->pos[0].ty = mk_class(cls.get());
                }
                m.slot = intern_method_slot(f.name);
            } else if (kind == MethodKind::Static) {
                // 静态方法按书写顺序接收实参，没有隐式 self。
                m.slot = intern_method_slot(f.name);
            } else {
                if (m.sig->pos.empty()) {
                    err(f.line, f.col,
                        std::format("property '{}' must take 'self' as first parameter",
                                    f.name));
                } else if (is_unknown(m.sig->pos[0].ty)) {
                    m.sig->pos[0].ty = mk_class(cls.get());
                }
                if (kind == MethodKind::PropertyGetter) {
                    if (cls->field_index(f.name) >= 0)
                        err(f.line, f.col,
                            std::format("property '{}' conflicts with a field", f.name));
                    m.slot = intern_method_slot("@get:" + f.name);
                    PropertyInfo pi;
                    pi.owner = cls.get();
                    pi.getter = midx;
                    cls->properties[f.name] = pi;
                } else {
                    auto pit = cls->properties.find(f.name);
                    if (pit == cls->properties.end()) {
                        err(f.line, f.col,
                            std::format("@{} requires a @property getter named '{}' in the "
                                        "same class",
                                        kind == MethodKind::PropertySetter ? "setter" : "deleter",
                                        f.name));
                    } else if (kind == MethodKind::PropertySetter) {
                        pit->second.setter = midx;
                    } else {
                        pit->second.deleter = midx;
                    }
                    if (kind == MethodKind::PropertySetter && m.sig->pos.size() < 2)
                        err(f.line, f.col,
                            std::format("property setter '{}' must take self and value", f.name));
                    m.slot = intern_method_slot(
                        (kind == MethodKind::PropertySetter ? "@set:" : "@del:") + f.name);
                }
            }
            if (f.name == "__init__" && kind != MethodKind::Normal)
                err(f.line, f.col, "__init__ cannot be a static method or property");
            m.def = &f;
            rebuild_call_sig(m);
            // 协议 dunder 的签名固定，写错在定义处报出来，避免调用点
            // 拿到不可用的 vtable 槽。
            {
                auto bad = [&](const std::string& msg) {
                    err(f.line, f.col, std::format("'{}' {}", f.name, msg));
                };
                auto one_of = [&](std::initializer_list<const char*> names) {
                    for (const char* n : names)
                        if (f.name == n) return true;
                    return false;
                };
                bool cmp_d = one_of({"__lt__", "__le__", "__gt__", "__ge__"});
                bool arith_d = one_of({"__add__", "__sub__", "__mul__", "__truediv__",
                                       "__floordiv__", "__mod__", "__pow__", "__radd__",
                                       "__rsub__", "__rmul__", "__rtruediv__", "__rfloordiv__",
                                       "__rmod__", "__rpow__", "__iadd__", "__isub__", "__imul__",
                                       "__itruediv__", "__ifloordiv__", "__imod__", "__ipow__"});
                bool bool_ret = cmp_d || f.name == "__eq__" || f.name == "__ne__" ||
                                f.name == "__contains__";
                bool two_arg = bool_ret || arith_d;
                bool one_arg = one_of(
                    {"__str__", "__repr__", "__len__", "__hash__", "__iter__"});
                if (two_arg || one_arg) {
                    if (kind != MethodKind::Normal) {
                        bad("must be a normal method");
                    } else if (m.sig->varargs || m.sig->kwargs) {
                        bad("does not accept *args / **kwargs");
                    } else if (two_arg) {
                        if (m.sig->pos.size() != 2) {
                            bad("takes 'self' and one other parameter");
                        } else if (bool_ret && !is_unknown(m.sig->pos[1].ty) &&
                                   m.sig->pos[1].ty->kind != TyKind::Class) {
                            bad("second parameter must be unannotated or a class type");
                        } else if (bool_ret &&
                                   (!m.sig->ret || m.sig->ret->kind != TyKind::Bool)) {
                            bad("must return bool");
                        }
                    } else if (m.sig->pos.size() != 1) {
                        bad("takes only 'self'");
                    } else if (f.name == "__len__" || f.name == "__hash__") {
                        if (!m.sig->ret || m.sig->ret->kind != TyKind::Int)
                            bad("must return int");
                    } else if (f.name == "__str__" || f.name == "__repr__") {
                        if (!m.sig->ret || m.sig->ret->kind != TyKind::Str)
                            bad("must return str");
                    }
                    // __iter__ 不限制返回类型：生成器方法的标注是元素类型。
                }
            }
            // 方法体检查复用这份签名，返回类型才会回填到同一个对象上
            func_infos_[&f] = m.sig;
            cls->methods.push_back(std::move(m));
        }
        cls->has_init = cls->method("__init__") != nullptr;

        // 沿 MRO 检查同名方法的种类是否一致。普通方法/静态方法的调用 ABI
        // 不同，属性又走独立槽；混用会让基类静态类型上的调用点拿错约定。
        {
            std::unordered_map<std::string, MethodKind> kinds;
            std::unordered_set<std::string> prop_names;
            for (const ClassInfo* k : cls->mro) {
                for (const MethodInfo& m : k->methods) {
                    if (m.is_accessor()) {
                        prop_names.insert(m.name);
                        continue;
                    }
                    auto it = kinds.find(m.name);
                    if (it == kinds.end()) kinds.emplace(m.name, m.kind);
                    else if (it->second != m.kind)
                        err(c.line, c.col,
                            std::format("incompatible method kinds for '{}' in the MRO of "
                                        "'{}'", m.name, c.name));
                }
                for (const auto& [pn, pi] : k->properties) {
                    (void)pi;
                    prop_names.insert(pn);
                }
            }
            for (const auto& [n, k] : kinds) {
                (void)k;
                if (prop_names.count(n))
                    err(c.line, c.col,
                        std::format("cannot mix property and method '{}' in the MRO of '{}'",
                                    n, c.name));
            }
        }

        // 类体里的其他语句：在类定义点执行。普通赋值是类属性（存进
        // 模块里的隐藏全局，所有实例共享）；其他语句（if/for/with…）
        // 仍然没法建模，把类标记成 open 后按模块全局处理。
        auto saved_body_attrs = std::move(class_body_attrs_);
        class_body_attrs_.clear();
        for (auto& s : c.body) {
            switch (s->kind) {
                case StmtKind::FunctionDef:
                case StmtKind::AnnAssign:
                case StmtKind::Pass:
                    break;
                case StmtKind::Assign: {
                    auto& as = static_cast<Assign&>(*s);
                    if (as.targets.size() == 1 &&
                        as.targets[0]->kind == ExprKind::Name) {
                        auto& nm = static_cast<Name&>(*as.targets[0]);
                        if (nm.id.starts_with("__") && nm.id.ends_with("__") &&
                            nm.id != "__match_args__") {
                            // 双下划线类变量（__slots__ 之类）没有语义模型
                            cls->open = true;
                            stmt(*s);
                            break;
                        }
                        TypePtr ty = expr(*as.value);
                        ClassAttrInfo ai;
                        ai.name = nm.id;
                        ai.ty = ty ? ty : t_unknown();
                        ai.init = as.value.get();
                        ai.owner = cls.get();
                        class_body_attrs_[nm.id] = ai.ty;
                        cls->class_attrs.push_back(std::move(ai));
                        break;
                    }
                    cls->open = true;
                    stmt(*s);
                    break;
                }
                case StmtKind::Expr: {
                    auto& e = static_cast<ExprStmt&>(*s);
                    if (e.value->kind == ExprKind::Constant &&
                        static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                        break;
                    cls->open = true;
                    stmt(*s);
                    break;
                }
                default:
                    cls->open = true;
                    stmt(*s);
                    break;
            }
        }
        class_body_attrs_ = std::move(saved_body_attrs);

        // 方法体最后检查：签名、字段、self 类型都已就位。
        ClassInfo* saved_cls = cur_class_;
        cur_class_ = cls.get();
        for (auto& m : cls->methods) funcdef_impl(*m.def, cls);
        cur_class_ = saved_cls;
        // 无标注方法的返回类型是检查完才推断出来的，调用签名要重新切一份
        for (auto& m : cls->methods) rebuild_call_sig(m);
        cls->complete = true;



    }


}  // namespace py23
