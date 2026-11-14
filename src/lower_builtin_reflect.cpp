// lower_builtin_reflect.cpp —— 内置函数：反射与属性
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_reflect(const std::string& name, Call& c) {
    if (name == "isinstance") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "isinstance() takes exactly two arguments");
            return const_bool(false);
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        TypePtr tt = tc_.type_of(c.args[1].get());
        if (tt && tt->kind == TyKind::ClassObj && !tt->sub.empty()) tt = tt->sub[0];
        TypePtr st = strip_optional(vt);
        std::string builtin;
        if (tt && tt->kind == TyKind::Func && tt->fn) builtin = tt->fn->name;
        if (!builtin.empty()) {
            if (builtin == "object") return const_bool(true);
            bool known = st && st->kind != TyKind::Unknown;
            auto kind_is = [&](std::initializer_list<TyKind> ks) {
                for (auto k : ks)
                    if (st && st->kind == k) return true;
                return false;
            };
            if (known) {
                bool r = false;
                if (builtin == "int") r = kind_is({TyKind::Int, TyKind::Bool});
                else if (builtin == "bool") r = kind_is({TyKind::Bool});
                else if (builtin == "float") r = kind_is({TyKind::Float});
                else if (builtin == "str") r = kind_is({TyKind::Str});
                else if (builtin == "list") r = kind_is({TyKind::List});
                else if (builtin == "tuple") r = kind_is({TyKind::Tuple});
                else if (builtin == "dict") r = kind_is({TyKind::Dict});
                else if (builtin == "set") r = kind_is({TyKind::Set});
                else if (builtin == "complex") r = kind_is({TyKind::Complex});
                else if (builtin == "range") r = kind_is({TyKind::Range});
                else {
                    err(c.args[1]->line, c.args[1]->col,
                        std::format("isinstance() does not support type '{}' yet", builtin));
                    return const_bool(false);
                }
                return const_bool(r);
            }
            // unknown 值：只有带运行期类型标签的容器/标量能查。
            RtFn fn{};
            if (builtin == "str") fn = RtFn::IsStr;
            else if (builtin == "list" || builtin == "tuple") fn = RtFn::IsSeq;
            else if (builtin == "dict") fn = RtFn::IsDict;
            else if (builtin == "complex") fn = RtFn::IsComplex;
            else {
                err(c.args[1]->line, c.args[1]->col,
                    std::format("isinstance() of an unknown value as '{}' is not supported yet",
                                builtin));
                return const_bool(false);
            }
            return call_runtime(fn, t_bool(), {expr(*c.args[0])});
        }
        ClassInfo* target = tt && tt->kind == TyKind::Class ? tt->cls : nullptr;
        if (!target) {
            err(c.args[1]->line, c.args[1]->col,
                "isinstance() second argument must be a class or builtin type");
            return const_bool(false);
        }
        if (target->is_exception_class) {
            if (st && st->kind != TyKind::Unknown && st->kind != TyKind::Exception)
                return const_bool(false);
            return call_runtime(RtFn::IsException, t_bool(),
                                {expr(*c.args[0]), const_str(target->name)});
        }
        auto cid = class_ids_.find(target);
        if (cid == class_ids_.end()) {
            // 导入类没有本模块的运行期编号，用「模块名.类名」比较；
            // pa_is_named_class 会沿实例的动态 MRO 查，子类也能匹配。
            std::string qual = target->origin_qualname;
            if (qual.empty()) {
                auto scan = [&](const std::unordered_map<std::string, ModuleInfoPtr>& mods) {
                    for (auto& entry : mods) {
                        if (qual.empty() == false) return;
                        for (auto& ce : entry.second->classes)
                            if (ce.second.cls.get() == target) {
                                qual = entry.second->name + "." + ce.first;
                                return;
                            }
                    }
                };
                scan(st_->modules);
                if (qual.empty()) scan(module_aliases_);
            }
            if (qual.empty()) {
                err(c.args[1]->line, c.args[1]->col,
                    "isinstance() with an imported class is not supported yet");
                return const_bool(false);
            }
            return call_runtime(RtFn::IsNamedClass, t_bool(),
                                {expr(*c.args[0]), const_str(qual)});
        }
        bool runtime = true;
        if (st && st->kind == TyKind::Class && st->cls) {
            bool a = st->cls->is_subclass_of(target);
            bool b = target->is_subclass_of(st->cls);
            if (a)
                runtime = vt != st;  // Optional 可能实际是 None
            else if (b)
                runtime = true;      // 值可能是子类
            else
                return const_bool(false);
        } else if (st && st->kind != TyKind::Unknown) {
            return const_bool(false);
        }
        if (!runtime) return const_bool(true);
        // 类编号只在模块内唯一，跨模块对象 MRO 里可能有同号的别的类；
        // 本地类目标也按限定名比较，避免误匹配。
        std::string qual = mod_.runtime_name + "." + target->name;
        return call_runtime(RtFn::IsNamedClass, t_bool(),
                            {expr(*c.args[0]), const_str(qual)});
    }

    if (name == "type") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "type() takes exactly one argument");
            return const_none();
        }
        Expr* arg = c.args[0].get();
        TypePtr t = tc_.type_of(arg);
        bool skip_eval = false;
        if (arg->kind == ExprKind::Name) {
            auto& id = static_cast<Name*>(arg)->id;
            skip_eval = !is_bound_here(id) &&
                        (classes_.count(id) || imported_classes_.count(id) ||
                         TypeChecker::is_builtin_name(id));
        }
        if (!skip_eval) (void)expr(*arg);  // 保留求值副作用
        VReg s = const_str(py_type_name(t));
        return retype(s, mk_typeobj(t ? t : t_unknown()));
    }

    if (name == "id") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "id() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::HashF64, t_int(), {v});
        return retype(v, t_int());
    }

    if (name == "hash") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "hash() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && (t->kind == TyKind::Int || t->kind == TyKind::Bool))
            return call_runtime(RtFn::HashValue, t_int(), {const_int(0), v});
        if (t && t->kind == TyKind::Str)
            return call_runtime(RtFn::HashValue, t_int(), {const_int(1), v});
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::HashF64, t_int(), {v});
        if (t && t->kind == TyKind::Class) {
            VReg h = dunder_call(v, t, "__hash__", {}, t_int());
            if (h != kNoReg) return h;
            // 没有 __hash__：定义了 __eq__ 的类按 CPython 规则不可哈希；
            // 否则退回身份哈希（和 dict/set 对类键的指针哈希一致）。
            const MethodInfo* eq = t->cls ? t->cls->method("__eq__") : nullptr;
            if (eq && eq->kind == MethodKind::Normal) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("unhashable type: '{}' defines __eq__ but no __hash__",
                                type_name(t)));
                return const_int(0);
            }
            return call_runtime(RtFn::HashValue, t_int(), {const_int(3), v});
        }
        err(c.args[0]->line, c.args[0]->col,
            std::format("hash() of '{}' is not supported yet (int/bool/str/float)",
                        type_name(t)));
        return const_int(0);
    }

    if (name == "callable") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "callable() takes exactly one argument");
            return const_bool(false);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        bool is_class = t && t->kind == TyKind::Class;
        bool is_builtin_name = false;
        if (c.args[0]->kind == ExprKind::Name) {
            auto& id = static_cast<Name*>(c.args[0].get())->id;
            is_builtin_name = TypeChecker::is_builtin_name(id) && !is_local(id);
        }
        if (!is_class && !is_builtin_name) (void)expr(*c.args[0]);  // 求值副作用
        if (!t || t->kind == TyKind::Unknown) {
            err(c.args[0]->line, c.args[0]->col,
                "callable() of an unknown value is not supported yet");
            return const_bool(false);
        }
        return const_bool(is_class || is_builtin_name || t->kind == TyKind::Func);
    }

    if (name == "issubclass") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "issubclass() takes exactly two arguments");
            return const_bool(false);
        }
        TypePtr a0 = tc_.type_of(c.args[0].get());
        TypePtr a1 = tc_.type_of(c.args[1].get());
        if (a0 && a0->kind == TyKind::ClassObj && !a0->sub.empty()) a0 = a0->sub[0];
        if (a1 && a1->kind == TyKind::ClassObj && !a1->sub.empty()) a1 = a1->sub[0];
        auto builtin = [](const TypePtr& t) -> std::string {
            if (t && t->kind == TyKind::Func && t->fn && t->fn->unchecked)
                return t->fn->name;
            return std::string();
        };
        ClassInfo* ca = a0 && a0->kind == TyKind::Class ? a0->cls : nullptr;
        ClassInfo* cb = a1 && a1->kind == TyKind::Class ? a1->cls : nullptr;
        std::string ba = builtin(a0);
        std::string bb = builtin(a1);
        if ((!ca && ba.empty()) || (!cb && bb.empty())) {
            err(c.line, c.col,
                "issubclass() arguments must be classes or builtin types");
            return const_bool(false);
        }
        bool r = false;
        if (ca && cb) {
            r = ca->is_subclass_of(cb);
        } else if (ca) {
            r = bb == "object";
        } else if (cb) {
            r = false;
        } else if (bb == "object") {
            r = true;
        } else if (bb == "int") {
            r = ba == "int" || ba == "bool";
        } else {
            r = ba == bb;
        }
        return const_bool(r);
    }

    if (name == "hasattr" || name == "getattr" || name == "setattr") {
        size_t lo = name == "setattr" ? 3 : 2;
        size_t hi = name == "getattr" ? 3 : lo;
        if (c.args.size() < lo || c.args.size() > hi) {
            err(c.line, c.col,
                std::format("{}() takes {} arguments, got {}", name,
                            lo == hi ? std::to_string(lo) : std::format("{}..{}", lo, hi),
                            c.args.size()));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        const std::string* an = nullptr;
        Expr* name_expr = c.args[1].get();
        if (name_expr->kind == ExprKind::Constant) {
            auto& cc = static_cast<Constant&>(*name_expr);
            if (cc.ck == Constant::CK::Str) an = &cc.s;
        }
        if (!an) {
            err(name_expr->line, name_expr->col,
                std::format("{}() attribute name must be a string literal", name));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        TypePtr raw = tc_.type_of(c.args[0].get());
        TypePtr ot = strip_optional(raw);
        if (!ot || ot->kind != TyKind::Class || !ot->cls) {
            // hasattr 对已知的内建/容器类型可以静态判定；Optional 的堆类型
            // 再补一个非空检查（None 就是 0）。
            if (name == "hasattr" && ot && ot->kind != TyKind::Unknown) {
                bool has = TypeChecker::type_has_member(ot, *an);
                VReg v = expr(*c.args[0]);
                if (!has) return const_bool(false);
                if (raw != ot && ir_is_heap(ot)) {
                    Inst cmp;
                    cmp.op = IrOp::Cmp;
                    cmp.cop = CmpKind::Ne;
                    cmp.a = retype(v, t_int());  // 按裸指针判空，避免 str 内容比较
                    cmp.b = const_int(0);
                    return def(t_bool(), std::move(cmp));
                }
                return const_bool(true);
            }
            // getattr 第三参：已知类型上没有这个成员，直接给缺省值。
            if (name == "getattr" && c.args.size() == 3 && ot &&
                ot->kind != TyKind::Unknown && !TypeChecker::type_has_member(ot, *an))
                return expr(*c.args[2]);
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() is only supported on class instances", name));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        const FieldInfo* f = ot->cls->field(*an);
        const PropertyInfo* prop = ot->cls->property(*an);
        auto* meth = ot->cls->method(*an);
        bool optional = ot != raw;
        if (name == "hasattr") {
            if (optional) {
                err(c.line, c.col,
                    "hasattr() on an Optional value is not supported yet");
                return const_bool(false);
            }
            (void)expr(*c.args[0]);  // 保留求值副作用
            return const_bool(f || prop || meth);
        }
        VReg obj = expr(*c.args[0]);
        if (optional) obj = guard_not_none(obj, std::format("attribute '{}'", *an));
        if (name == "getattr") {
            if (prop) {
                if (!prop->owner || prop->getter < 0 ||
                    prop->getter >= static_cast<int>(prop->owner->methods.size())) {
                    err(c.line, c.col, std::format("property '{}' has no getter", *an));
                    return const_none();
                }
                ++side_effect_seq_;
                const MethodInfo& gm = prop->owner->methods[prop->getter];
                Inst vc;
                vc.op = IrOp::VCall;
                vc.a = obj;
                vc.imm = gm.slot;
                TypePtr rt = gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret : t_unknown();
                return def_heap(rt, std::move(vc));
            }
            if (!f) {
                if (meth)
                    err(c.line, c.col,
                        std::format("getattr() of method '{}' must be called with ()", *an));
                else if (c.args.size() == 3)
                    return expr(*c.args[2]);
                else
                    err(c.line, c.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, *an));
                return const_none();
            }
            int off = static_cast<int>(8 * (1 + ot->cls->field_index(*an)));
            return field_load(obj, off, f->ty);
        }
        // setattr
        if (!f) {
            if (prop) {
                if (!prop->owner || prop->setter < 0 ||
                    prop->setter >= static_cast<int>(prop->owner->methods.size())) {
                    err(c.line, c.col, std::format("property '{}' has no setter", *an));
                    return const_none();
                }
                const MethodInfo& sm = prop->owner->methods[prop->setter];
                VReg val = expr(*c.args[2]);
                TypePtr want = sm.call_sig && !sm.call_sig->pos.empty()
                                   ? sm.call_sig->pos[0].ty
                                   : t_unknown();
                val = coerce(val, tc_.type_of(c.args[2].get()), want);
                Inst vc;
                vc.op = IrOp::VCall;
                vc.a = obj;
                vc.imm = sm.slot;
                vc.args = {val};
                emit(std::move(vc));
                return const_none();
            }
            err(c.line, c.col,
                std::format("'{}' object has no field '{}'", ot->cls->name, *an));
            return const_none();
        }
        VReg v = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), f->ty);
        int off = static_cast<int>(8 * (1 + ot->cls->field_index(*an)));
        store_field(f->ty, obj, off, v);
        return const_none();
    }

    if (name == "vars") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "vars() takes exactly one argument");
            return const_none();
        }
        TypePtr ot = tc_.type_of(c.args[0].get());
        if (!ot || ot->kind != TyKind::Class || !ot->cls) {
            err(c.args[0]->line, c.args[0]->col, "vars() argument must be a class instance");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::Dict) ty = mk_dict(t_str(), t_unknown());
        // key 是 str（堆），value 统一 unknown：字段类型各不相同，只能保守存。
        VReg d = call_runtime(RtFn::DictNew, ty,
                              {const_int(1), const_int(1), const_int(2), const_int(-1),
                               const_int(-1)});
        push_temp(d);
        VReg obj = expr(*c.args[0]);
        for (const ClassInfo* p = ot->cls; p; p = p->base) {
            for (auto& f : p->fields) {
                int idx = ot->cls->field_index(f.name);
                if (idx < 0) continue;
                // field_load 的 imm 是字节偏移，跳过对象头的 vtable 指针。
                VReg v = retype(field_load(obj, static_cast<int>(8 * (1 + idx)), f.ty),
                                t_unknown());
                retain_elem(v, 2);
                call_void(RtFn::DictSet, {d, const_str(f.name), v});
            }
        }
        return d;
    }

    if (name == "dir") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "dir() takes at most one argument");
            return const_none();
        }
        std::vector<std::string> names;
        if (c.args.empty()) {
            for (auto& [n, s] : st_->slots) names.push_back(n);
            if (st_->is_module_top)
                for (auto& [n, gi] : global_ids_) names.push_back(n);
        } else {
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                for (const ClassInfo* p = ot->cls; p; p = p->base) {
                    for (auto& f : p->fields) names.push_back(f.name);
                    for (auto& m : p->methods) names.push_back(m.name);
                    for (auto& a : p->class_attrs) names.push_back(a.name);
                    for (auto& [pn, pr] : p->properties) names.push_back(pn);
                }
            } else if (ot && ot->kind != TyKind::Unknown) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("dir() is not supported for '{}'", type_name(ot)));
                return const_none();
            }
            // 实参照常求值一次（可能带副作用）。
            expr(*c.args[0]);
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        TypePtr lt = tc_.type_of(&c);
        if (!lt || lt->kind != TyKind::List) lt = mk_list(t_str());
        VReg l = call_runtime(RtFn::ListNew, lt,
                              {const_int(static_cast<int64_t>(names.size())),
                               const_int(list_kind_filled(lt))});
        push_temp(l);
        for (auto& n : names) {
            VReg s = const_str(n);
            retain_elem(s, 1);
            call_void(RtFn::ListAppend, {l, s});
        }
        return l;
    }

    return kNoReg;
}

}  // namespace pyadamas
