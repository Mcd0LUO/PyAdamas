// typecheck/func.cpp —— 函数与类：定义体、方法、隐式字段
#include "typecheck/typecheck.h"

#include "ir/module.h"


namespace pyadamas {
bool c3_merge(std::vector<std::vector<ClassInfo*>>& seqs,
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

void TypeChecker::classdef(ClassDef& c) {
        if (!c.decorators.empty()) {
            // 类装饰器在类定义点拿到运行期类对象；降级时按副作用执行。
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
                    } else if (auto* sym = lookup(bn->id);
                               sym && sym->ty && sym->ty->kind == TyKind::Class && sym->ty->cls &&
                               sym->ty->cls->is_exception_class) {
                        // 本模块定义或跨模块导入的异常类作基类。父名用定义
                        // 模块里的原名：运行期注册表按名字串匹配，本地别名
                        //（from lib import LibError as LE）不能拿来注册。
                        is_exc_class = true;
                        exc_parent = sym->ty->cls->external_name();
                    }
                }
            }
            if (!is_exc_class) {
                for (auto& b : c.bases) {
                    TypePtr bt = expr(*b);
                    if (bt && bt->kind == TyKind::ClassObj && !bt->sub.empty()) bt = bt->sub[0];
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
        // @dataclass / @dataclasses.dataclass / @dataclass(...)：装饰器会在
        // 运行期按字段标注合成 __init__，静态看不见。记下来供实例化检查用。
        for (auto& d : c.decorators) {
            const Expr* e = d.get();
            if (!e) continue;
            const std::string* nm = nullptr;
            if (e->kind == ExprKind::Name) nm = &static_cast<const Name*>(e)->id;
            else if (e->kind == ExprKind::Attribute)
                nm = &static_cast<const Attribute*>(e)->attr;
            else if (e->kind == ExprKind::Call) {
                const Expr* f = static_cast<const Call*>(e)->func.get();
                if (f && f->kind == ExprKind::Name) nm = &static_cast<const Name*>(f)->id;
                else if (f && f->kind == ExprKind::Attribute)
                    nm = &static_cast<const Attribute*>(f)->attr;
            }
            if (nm && *nm == "dataclass") cls->is_dataclass = true;
        }
        cls->base = base;
        // enum.Enum 子类（stdlib/enum.py 的 Enum）：类体赋值变成枚举成员。
        cls->is_enum = base != nullptr && base->name == "Enum" && base->imported() &&
                       base->origin_module == "enum";
        cls->open = open;
        cls->is_exception_class = is_exc_class;
        cls->exc_parent = exc_parent;
        cls->extra_bases = extra_bases;
        if (!is_exc_class && base && base->imported())
            reserve_imported_method_slots(base, c.line, c.col);
        for (auto* eb : extra_bases)
            if (eb->imported()) reserve_imported_method_slots(eb, c.line, c.col);
        for (auto* eb : extra_bases) {
            // 附加基类不能引入主基类布局里没有的字段；共享祖先（菱形
            // 的 A）只要偏移一致就允许，否则继承方法里的字段偏移会错位。
            for (const ClassInfo* k = eb; k; k = k->base) {
                for (const FieldInfo& f : k->fields) {
                    int eidx = eb->field_index(f.name);
                    int pidx = base ? base->field_index(f.name) : -1;
                    if (pidx != eidx)
                        // 附加基类引入主基类布局没有的字段：CPython 允许任意
                        // 布局，pyadamas 编译期布局不支持。check 只记 unsupported，
                        // 别把合法 Python 判死。
                        unsupported.push_back(
                            "multiple-inheritance field layout '" + f.name + "'");
                }
            }
            if (eb->is_open()) cls->open = true;
        }
        // layout base（0.504）：MRO 顺序里第一个有字段的基类提供前缀（mi_guard.cpp）。
        set_layout_base(*cls, base, extra_bases);
        // 布局实体化（0.506）：非 layout base 的字段块追加进子类布局（mi_guard.cpp）。
        // 偏移修正（thunk / 调用点 delta）不在本步，护栏继续拒绝。
        materialize_base_fields(*cls, base, extra_bases);
        dump_layout(c, *cls);
        // 3.9：非 layout base 的方法碰字段 + 字段确实挪位 → 类定义处明确报错。
        check_mi_layout(c, cls.get(), base, extra_bases, open);
        if (base) {
            cls->has_field_defaults = base->has_field_defaults;
            if (base->is_open()) cls->open = true;
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
                it->second.is_class_name = true;
            } else {
                sc->vars[c.name] = Symbol{mk_class(cls.get()), c.line, true, false};
                sc->vars[c.name].is_class_name = true;
            }
        }
        class_obj_names_.insert(c.name);
        auto prev = classes_.find(c.name);
        classes_[c.name] = cls;
        if (!c.decorators.empty()) cls->value_used = true;
        all_classes_.push_back(cls);
        (void)prev;  // 同名类重定义：后面的引用拿新类，旧实例仍按旧布局走
        if (is_exc_class) {
            // 0.167 起自定义异常类按普通类建模（字段/方法/__init__），
            // 实例是 vtable 对象。is_exception_class 仍标记它，raise/except
            // 走专门路径；这里继续走下面的字段/方法处理，不再提前 return。
        }

        // 0.569：类体作用域（class C: x = 5; y = x + 1），字段预收集前生效。
        const ClassInfo* saved_body_cls = std::exchange(class_body_cls_, cls.get());
        class_body_pre_.clear();
        class_body_attrs_.clear();
        prescan_class_body(c);

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
            if (a.value) fi.default_lit = literal_of_expr(a.value.get());
            cls->fields.push_back(std::move(fi));
        }

        // P4 块4（lang_gaps）：@dataclass 编译期展开——字段已收集，合成
        // __init__/__repr__/__eq__ 注入类体，下面的方法登记/槽分配/体检查
        // 与降级全部复用既有机制。门控关不调用（逐字旧样）。重查轮里已注入
        // 的方法按名字判重，不会重复注入。
        if (lang_gaps_enabled() && cls->is_dataclass)
            expand_dataclass(c, cls);

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
            if (m.decorated) {
                // 装饰器链的返回值就是调用点看到的函数；没有已知的函数
                // 类型时调用点不检查实参，降级阶段会明确报错。
                if (!m.decorated_ty || m.decorated_ty->kind != TyKind::Func ||
                    !m.decorated_ty->fn) {
                    m.call_sig = nullptr;
                    return;
                }
                m.call_sig = std::make_shared<FuncInfo>(*m.decorated_ty->fn);
                m.call_sig->name = m.name;
                m.call_sig->outer = m.sig.get();
                m.call_sig->outer_off = 0;
                if (m.call_sig->pos.empty()) {
                    m.call_sig = nullptr;
                    return;
                }
                m.call_sig->pos.erase(m.call_sig->pos.begin());
                m.call_sig->outer_off = 1;
                if (m.call_sig->n_posonly > 0) --m.call_sig->n_posonly;
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
            MethodInfo m;
            m.name = f.name;
            m.kind = kind;
            // abc：@abstractmethod 标记抽象方法（编译期事实）。
            mark_abstract_methods(f, m);
            // 没认出来的方法装饰器：装饰器链的结果存成类属性，调用走它。
            m.decorated = !decor_ok;
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
                bool fmt_d = f.name == "__format__";
                bool one_arg = one_of(
                    {"__str__", "__repr__", "__len__", "__hash__", "__iter__",
                     "__bool__", "__reversed__"});
                if (two_arg || one_arg || fmt_d) {
                    if (kind != MethodKind::Normal) {
                        bad("must be a normal method");
                    } else if (m.sig->varargs || m.sig->kwargs) {
                        bad("does not accept *args / **kwargs");
                    } else if (fmt_d) {
                        if (m.sig->pos.size() != 2)
                            bad("takes 'self' and a format spec");
                        else if (!is_unknown(m.sig->pos[1].ty) &&
                                 m.sig->pos[1].ty->kind != TyKind::Str)
                            bad("format spec must be a str");
                    } else if (two_arg) {
                        if (m.sig->pos.size() != 2) {
                            bad("takes 'self' and one other parameter");
                        } else if (bool_ret && !is_unknown(m.sig->pos[1].ty) &&
                                   m.sig->pos[1].ty->kind != TyKind::Class &&
                                   // 联合形参（0.488）：比较 dunder 收 int/同类，
                                   // 用来表达「与 int/big 混比」，不该判死。
                                   !is_union_ty(m.sig->pos[1].ty)) {
                            bad("second parameter must be unannotated or a class type");
                        }
                    } else if (m.sig->pos.size() != 1) {
                        bad("takes only 'self'");
                    }
                    // 返回类型不在这里查：未标注的方法此刻还看不到 return 的
                    // 类型，按 unknown != str 判会把返回 self.key 这类正当写法
                    // 全判死。check_dunder_returns() 在函数体推理完之后统一
                    // 校验；仍是 unknown 就放过——证明不了错就不报错。
                    // __iter__ 不限制返回类型：生成器方法的标注是元素类型。
                }
            }
            // 方法体检查复用这份签名，返回类型才会回填到同一个对象上
            func_infos_[&f] = m.sig;
            signature_defs_[m.sig.get()] = &f;
            m.owner = cls.get(), cls->methods.push_back(std::move(m));  // owner=声明类
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

        // 类体其他语句在类定义点执行；普通赋值是类属性，其余语句标记 open 后按模块全局处理。
        auto saved_body_attrs = std::move(class_body_attrs_);
        auto saved_body_methods = std::move(class_body_methods_);
        class_body_methods_.clear();
        for (auto& s : c.body) {
            switch (s->kind) {
                case StmtKind::FunctionDef: {
                    // 自定义方法装饰器：在类定义点求值装饰器，把链的结果
                    // 注册成类属性；方法体仍按自己的签名检查。
                    auto& f = static_cast<FunctionDef&>(*s);
                    // 类体顺序求值：def 当场把名字绑进类命名空间
                    // （`__str__ = short` 这类别名只看得到前面的 def）。
                    class_body_register_def(cls, f);
                    if (f.decorators.empty()) break;
                    MethodInfo* m = nullptr;
                    for (auto& cand : cls->methods)
                        if (cand.def == &f) {
                            m = &cand;
                            break;
                        }
                    if (!m || !m->decorated || m->kind != MethodKind::Normal) break;
                    if (f.name.size() > 4 && f.name.starts_with("__") &&
                        f.name.ends_with("__")) {
                        // 只有走 dunder_call 的那批 dunder 能装饰：调用点会从
                        // 类属性里取包装函数并补 self。构造、with/async with、
                        // 由 vtable 槽按名字分派的 dunder 在调用点显式走类属性
                        // 里的包装函数，装饰后仍能工作。
                        static const std::unordered_set<std::string> kDecorOk = {
                            "__str__", "__repr__", "__eq__", "__ne__", "__lt__",
                            "__le__", "__gt__", "__ge__", "__add__", "__sub__",
                            "__mul__", "__truediv__", "__floordiv__", "__mod__",
                            "__pow__", "__radd__", "__rsub__", "__rmul__",
                            "__rtruediv__", "__rfloordiv__", "__rmod__", "__rpow__",
                            "__iadd__", "__isub__", "__imul__", "__itruediv__",
                            "__ifloordiv__", "__imod__", "__ipow__", "__contains__",
                            "__len__", "__hash__", "__iter__", "__init__",
                            "__enter__", "__exit__", "__aenter__", "__aexit__",
                            "__call__", "__format__", "__bool__", "__reversed__"};
                        if (!kDecorOk.count(f.name)) {
                            err(f.line, f.col,
                                std::format(
                                    "decorating dunder method '{}' is not supported yet",
                                    f.name));
                            break;
                        }
                    }
                    for (auto& d : f.decorators) {
                        Expr* de = d.get();
                        if (auto* n = dynamic_cast<Name*>(de)) {
                            if (n->id == "staticmethod" || n->id == "classmethod" ||
                                n->id == "property")
                                continue;
                        }
                        if (auto* at = dynamic_cast<Attribute*>(de)) {
                            if (auto* bn = dynamic_cast<Name*>(at->value.get());
                                bn && bn->id == f.name &&
                                (at->attr == "setter" || at->attr == "deleter" ||
                                 at->attr == "getter"))
                                continue;
                        }
                        expr(*de);
                        // 装饰器的第一个形参就是被装饰的方法。没标注时把
                        // 方法类型回填进去，包装体里的 f(self, ...) 才能按
                        // 真实签名检查；sig_dirty 让重查循环重查装饰器
                        // 函数体（含内层 wrapper）。
                        TypePtr dt = type_of(de);
                        if (dt && dt->kind == TyKind::Func && dt->fn &&
                            !dt->fn->pos.empty()) {
                            Param& p0 = dt->fn->pos[0];
                            if (is_unknown(p0.ty) && m->sig) {
                                auto ft = new_ty(TyKind::Func);
                                ft->fn = m->sig;
                                p0.ty = ft;
                                p0.inferred = true;
                                dt->fn->sig_dirty = true;
                            }
                        }
                    }
                    TypePtr cur = new_ty(TyKind::Func);
                    cur->fn = m->sig;
                    bool known = true;
                    for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
                        TypePtr dt = type_of(it->get());
                        if (dt && dt->kind == TyKind::Func && dt->fn && dt->fn->ret)
                            cur = dt->fn->ret;
                        else {
                            known = false;
                            break;
                        }
                    }
                    m->decorated_ty = known ? cur : t_unknown();
                    if (known && cur && cur->kind == TyKind::Func) {
                        ClassAttrInfo ai;
                        ai.name = f.name;
                        ai.ty = cur;
                        ai.init = nullptr;
                        ai.owner = cls.get();
                        cls->class_attrs.push_back(std::move(ai));
                    }
                    break;
                }
                case StmtKind::AnnAssign:
                case StmtKind::Pass:
                    break;
                case StmtKind::Assign: {
                    auto& as = static_cast<Assign&>(*s);
                    // 方法别名（`__str__ = short`）：建成共享实现的两个方法槽。
                    if (class_body_method_alias(cls, *s)) break;
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
                        if (cls->is_enum) {
                            enum_member_assign(cls.get(), nm.id, as.value.get(), nm.line,
                                               nm.col);
                            break;
                        }
                        TypePtr ty = expr(*as.value);
                        ClassAttrInfo ai;
                        ai.name = nm.id;
                        // 类体里的 `name = None` 是占位/清空，不是「这个类
                        // 属性就是 NoneType」。钉成 None 会让子类实例上的
                        // self.name[k] = v 误报（wsgiref 的 headers = None
                        // 就是这种写法）。和模块级 None 占位同一套处理。
                        ai.ty = (ty && ty->kind != TyKind::None) ? ty : t_unknown();
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
        class_body_methods_ = std::move(saved_body_methods);

        // 方法体最后检查：签名、字段、self 类型都已就位。
        ClassInfo* saved_cls = cur_class_;
        cur_class_ = cls.get();
        // 别名条目和原方法共用同一个 FunctionDef：同一个函数体只检查一次，
        // 否则一条错误会报两遍。
        {
            std::unordered_set<const FunctionDef*> done;
            for (auto& m : cls->methods) {
                if (!m.def) continue;
                if (!done.insert(m.def).second) continue;
                funcdef_impl(*m.def, cls);
            }
        }
        cur_class_ = saved_cls;
        class_body_cls_ = saved_body_cls;  // 方法默认值已在外层检查完
        // 无标注方法的返回类型是检查完才推断出来的，调用签名要重新切一份
        for (auto& m : cls->methods) rebuild_call_sig(m);
        if (cls->is_exception_class) {
            // 有自有字段或方法（含 __init__/__str__）的异常类在 except as e
            // 时给真实实例；否则保持扁平 Exception 值。
            if (!cls->fields.empty()) cls->is_rich_exception = true;
            if (!cls->methods.empty()) cls->is_rich_exception = true;
        }
        // enum 成员收集完：加 value/name 字段并检查冲突。
        if (cls->is_enum) finalize_enum(c, cls.get());
        cls->complete = true;



    }

// 协议 dunder 的返回类型校验。必须在所有函数体检查完之后跑：未标注的
// 方法在定义处 ret 还是空的，只有 body 检查完才回填 ret_known。

}  // namespace pyadamas
