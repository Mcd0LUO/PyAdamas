// lower/builtin_reflect.cpp —— 内置函数：反射与属性
#include "lower/impl.h"
#include "backend/tag_defs.h"
#include "ir/module.h"  // fnv1a64：isinstance 的稳定类 id（0.531）

namespace pyadamas {

VReg Lowerer::lower_builtin_reflect(const std::string& name, Call& c) {
    if (name == "isinstance") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "isinstance() takes exactly two arguments");
            return const_bool(false);
        }
        TypePtr vt0 = tc_.type_of(c.args[0].get());
        TypePtr tt0 = tc_.type_of(c.args[1].get());
        if (tt0 && tt0->kind == TyKind::ClassObj && !tt0->sub.empty()) tt0 = tt0->sub[0];
        bool obj_evaled = false;
        VReg objreg = kNoReg;
        auto obj = [&]() -> VReg {
            if (!obj_evaled) {
                objreg = expr(*c.args[0]);
                obj_evaled = true;
            }
            return objreg;
        };
        auto class_of_target = [&](TypePtr t) -> ClassInfo* {
            if (!t) return nullptr;
            if (t->kind == TyKind::ClassObj && !t->sub.empty()) t = t->sub[0];
            if (t->kind == TyKind::TypeObj && !t->sub.empty()) t = t->sub[0];
            if (t->kind == TyKind::ClassObj && !t->sub.empty()) t = t->sub[0];
            return t->kind == TyKind::Class ? t->cls : nullptr;
        };
        // 单个目标类型的检查；tuple 目标是它反复调用后 OR。
        std::function<VReg(const TypePtr&)> single =
            [&](const TypePtr& tt) -> VReg {
            TypePtr st = strip_optional(vt0);
            std::string builtin;
            if (tt && tt->kind == TyKind::Func && tt->fn) builtin = tt->fn->name;
            // 类型名现在是 TypeObj 值（不再是 Func 签名）：名字优先取
            // 字面量（frozenset/bytearray 等保留原名），否则按内部类型名。
            if (builtin.empty() && tt && tt->kind == TyKind::TypeObj) {
                if (c.args[1]->kind == ExprKind::Name)
                    builtin = static_cast<Name&>(*c.args[1]).id;
                else if (!tt->sub.empty() && tt->sub[0])
                    builtin = py_type_name(tt->sub[0]);
            }
            if (!builtin.empty()) {
                if (builtin == "object") return const_bool(true);
                // bytearray / memoryview 在 pyadamas 里根本没有独立表示
                // （bytearray 被当成 list、memoryview 是 unknown、frozenset
                // 被当成 set），任何静态答案都是编的。以前把这些名字映射
                // 到内部等价类型，导致 isinstance([1,2,3], bytearray) 返回
                // True —— 为一个不存在的类型作假证。宁可报不支持。
                if (builtin == "bytearray" || builtin == "memoryview" ||
                    builtin == "frozenset") {
                    err(c.args[1]->line, c.args[1]->col,
                        std::format("isinstance() does not support type '{}' yet",
                                    builtin));
                    return const_bool(false);
                }
                bool known = st && st->kind != TyKind::Unknown;
                auto kind_is = [&](std::initializer_list<TyKind> ks) {
                    for (auto k : ks)
                        if (st && st->kind == k) return true;
                    return false;
                };
                if (known) {
                    bool r = false;
                    // 定宽整数（P1）在 Python 语义里也是 int。
                    if (builtin == "int")
                        r = kind_is({TyKind::Int, TyKind::Bool, TyKind::I8, TyKind::I16,
                                     TyKind::I32, TyKind::U8, TyKind::U16, TyKind::U32,
                                     TyKind::U64, TyKind::Big});
                    else if (builtin == "bool") r = kind_is({TyKind::Bool});
                    else if (builtin == "float") r = kind_is({TyKind::Float});
                    else if (builtin == "str") r = kind_is({TyKind::Str});
                    else if (builtin == "list") r = kind_is({TyKind::List});
                    else if (builtin == "tuple") r = kind_is({TyKind::Tuple});
                    else if (builtin == "dict") r = kind_is({TyKind::Dict});
                    else if (builtin == "set") r = kind_is({TyKind::Set});
                    else if (builtin == "complex") r = kind_is({TyKind::Complex});
                    else if (builtin == "range") r = kind_is({TyKind::Range});
                    else if (builtin == "bytes") r = kind_is({TyKind::Bytes});
                    else if (builtin == "type")
                        r = kind_is({TyKind::Class, TyKind::TypeObj, TyKind::ClassObj});
                    else {
                        err(c.args[1]->line, c.args[1]->col,
                            std::format("isinstance() does not support type '{}' yet", builtin));
                        return const_bool(false);
                    }
                    return const_bool(r);
                }
                // P2（PYADAMAS_TYPE_TAGS）：unknown 值带运行期类型标签后，
                // 内置类型的判定改交运行期 pa_any_type_is(v, tag, code)。
                // 静态类型是 unknown 时这是唯一能分辨 None/bool/tuple/set/
                // bytes 的途径（0.339 的探针口径只能认出 str/list/dict）。
                if (opts_.type_tags) {
                    int64_t code = -1;
                    if (builtin == "int") code = kTypeInt;
                    else if (builtin == "bool") code = kTypeBool;
                    else if (builtin == "float") code = kTypeFloat;
                    else if (builtin == "str") code = kTypeStr;
                    else if (builtin == "bytes") code = kTypeBytes;
                    else if (builtin == "list") code = kTypeList;
                    else if (builtin == "dict") code = kTypeDict;
                    else if (builtin == "tuple") code = kTypeTuple;
                    else if (builtin == "set") code = kTypeSet;
                    else if (builtin == "object") code = kTypeObject;
                    if (code >= 0) {
                        Inst in;
                        in.op = IrOp::CallRt;
                        in.imm = static_cast<int64_t>(RtFn::AnyTypeIs);
                        in.args = {obj(), const_int(code)};
                        in.ty = t_bool();
                        return def(t_bool(), std::move(in));
                    }
                    // 其余内置（complex/range/type/...）保守走下面的旧口径。
                }
                // unknown 值：只有带运行期类型标签的容器/标量能查。
                RtFn fn{};
                bool has_tag = true;
                if (builtin == "str") fn = RtFn::IsStr;
                else if (builtin == "list" || builtin == "tuple") fn = RtFn::IsSeq;
                else if (builtin == "dict") fn = RtFn::IsDict;
                else if (builtin == "complex") fn = RtFn::IsComplex;
                // 0.324：float 在 unknown 里有 CPYFLOAT 标签，可精确判定。
                else if (builtin == "float") fn = RtFn::AnyIsFloat;
                else has_tag = false;
                // 没有运行期类型标签的内置（int/float/bool/bytes 等）在
                // unknown 值上无法判定：保守返回 false（tuple 的 OR 场景
                // 下其余元素仍可继续检查）。
                if (!has_tag) return const_bool(false);
                return call_runtime(fn, t_bool(), {obj()});
            }
            // P4（lang_gaps）：异常类对象当 isinstance 目标（isinstance(e,
            // ValueError)，ValueError 是 ClassObj[Exception] 值）。运行期按
            // 异常值的类型名走 exc_is_a 父子链；富异常实例由 pa_is_exc 的
            // INSTANCE 分支按 MRO 类名判。静态已知非异常类型直接 false。
            {
                TypePtr et = tt;
                if (et && et->kind == TyKind::ClassObj && !et->sub.empty()) et = et->sub[0];
                if (et && et->kind == TyKind::Exception) {
                    if (st && st->kind != TyKind::Unknown && st->kind != TyKind::Exception &&
                        st->kind != TyKind::Class)
                        return const_bool(false);
                    return call_runtime(RtFn::IsException, t_bool(),
                                        {obj(), const_str(et->name)});
                }
            }
            ClassInfo* target = class_of_target(tt);
            if (!target) {
                err(c.args[1]->line, c.args[1]->col,
                    "isinstance() second argument must be a class or builtin type");
                return const_bool(false);
            }
            if (target->is_exception_class) {
                if (st && st->kind != TyKind::Unknown && st->kind != TyKind::Exception)
                    return const_bool(false);
                return call_runtime(RtFn::IsException, t_bool(),
                                    {obj(), const_str(target->name)});
            }
            auto cid = class_ids_.find(target);
            if (cid == class_ids_.end()) {
                std::string qual = target->origin_qualname;
                if (qual.empty()) {
                    auto scan = [&](const std::unordered_map<std::string, ModuleInfoPtr>& mods) {
                        for (auto& entry : mods) {
                            if (!qual.empty()) return;
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
                // 0.531：按限定名的 FNV-1a 64 做整数 MRO 比较（元数据第 8 个字），
                // 比 pa_is_named_class 的 strlen+memcmp 便宜一个数量级。
                // 导入类：跨模块实例的 vtable 元数据由定义模块发射，导入方的镜像
                // 元数据不一定是实例实际用的那份 —— 这里保留限定名比较（0.531 实测：
                // 走 cid 会在 `from value import Value` 的 isinstance 上返回 False）。
                return call_runtime(RtFn::IsNamedClass, t_bool(),
                                    {obj(), const_str(qual)});
            }
            bool runtime = true;
            if (st && st->kind == TyKind::Class && st->cls) {
                bool a = st->cls->is_subclass_of(target);
                bool b = target->is_subclass_of(st->cls);
                if (a)
                    runtime = vt0 != st;
                else if (b)
                    runtime = true;
                else
                    return const_bool(false);
            } else if (st && st->kind != TyKind::Unknown) {
                return const_bool(false);
            }
            if (!runtime) return const_bool(true);
            std::string qual = mod_.runtime_name + "." + target->name;
            return call_runtime(RtFn::IsInstanceOf, t_bool(),
                                {obj(), const_int(static_cast<int64_t>(fnv1a64(qual)))});
        };
        if (tt0 && tt0->kind == TyKind::Tuple) {
            const auto& subs = tt0->sub;
            if (subs.size() == 1) return single(subs.empty() ? nullptr : subs[0]);
            int end = new_block("isinst.end");
            int res_slot = new_borrow_slot(t_bool());
            int cur = -1;
            for (size_t i = 0; i < subs.size(); ++i) {
                if (i > 0) switch_to(cur);
                VReg r = single(subs[i]);
                store_borrow_slot(res_slot, r);
                if (i + 1 < subs.size()) {
                    int next = new_block("isinst.n");
                    Inst br;
                    br.op = IrOp::Branch;
                    br.a = load_slot(res_slot, t_bool());
                    br.target = end;
                    br.target2 = next;
                    emit(std::move(br));
                    cur = next;
                } else {
                    Inst j;
                    j.op = IrOp::Jump;
                    j.target = end;
                    emit(std::move(j));
                }
            }
            switch_to(end);
            return load_slot(res_slot, t_bool());
        }
        return single(tt0);

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
        // 异常值：type(e) 的类名是运行期的（except Exception 捕获到的可能
        // 是任意子类）。求值一次 e，再从异常值对象里取真实类名。
        if (t && t->kind == TyKind::Exception) {
            VReg ev = skip_eval ? kNoReg : expr(*arg);
            if (ev == kNoReg) ev = expr(*arg);
            VReg s = call_runtime(RtFn::ExcValueTypeName, t_str(), {ev});
            push_temp(s);
            return retype(s, mk_typeobj(t));
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
        if (!t || t->kind == TyKind::Unknown) {
            // unknown 值：运行期按种类分派（str 内容 / 实例 __hash__ / big / 机器字）。
            return call_runtime(RtFn::HashValue, t_int(), {const_int(-1), v});
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
        // P4（lang_gaps）：两边都是异常类（内建 ClassObj[Exception] 值或
        // 自定义异常类）→ 运行期按名字判父子（含变量持有的类值；静态名
        // 可能因形参回填过期，不能编译期定死）。用户类名取限定名末段，
        // 与 pa_exc_register 的裸名登记口径一致（rt_lang.c）。
        {
            auto exc_like = [](const TypePtr& t) {
                return t && (t->kind == TyKind::Exception ||
                             (t->kind == TyKind::Class && t->cls && t->cls->is_exception_class));
            };
            if (exc_like(a0) && exc_like(a1)) {
                VReg ca = expr(*c.args[0]);
                VReg cb = expr(*c.args[1]);
                return call_runtime(RtFn::ExcSubclass, t_bool(), {ca, cb});
            }
        }
        auto builtin = [](const TypePtr& t, Expr* e) -> std::string {
            if (t && t->kind == TyKind::Func && t->fn && t->fn->unchecked)
                return t->fn->name;
            if (t && t->kind == TyKind::TypeObj) {
                if (e && e->kind == ExprKind::Name) return static_cast<Name&>(*e).id;
                if (!t->sub.empty() && t->sub[0]) return py_type_name(t->sub[0]);
            }
            return std::string();
        };
        ClassInfo* ca = a0 && a0->kind == TyKind::Class ? a0->cls : nullptr;
        ClassInfo* cb = a1 && a1->kind == TyKind::Class ? a1->cls : nullptr;
        std::string ba = builtin(a0, c.args[0].get());
        std::string bb = builtin(a1, c.args[1].get());
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
        // 属性名不是字面量：只能走运行期动态属性表。只有类实例能办，
        // 其它接收者（模块/函数/常量）没有 dict 槽，直接拒绝。
        if (!an) {
            TypePtr r = strip_optional(tc_.type_of(c.args[0].get()));
            if (r && r->kind == TyKind::Class && r->cls) {
                r->cls->needs_dict = true;
                VReg obj = expr(*c.args[0]);
                VReg nm = expr(*c.args[1]);
                if (name == "hasattr")
                    return call_runtime(RtFn::AttrHas, t_bool(), {obj, nm});
                if (name == "setattr") {
                    // 与字面量名路径同口径：float 装箱进 unknown，堆值 retain。
                    VReg v = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_unknown());
                    call_void(RtFn::AttrSet, {obj, nm, v});
                    return const_none();
                }
                VReg dflt = c.args.size() == 3 ? expr(*c.args[2]) : const_int(0);
                return call_runtime(RtFn::AttrGet, t_unknown(), {obj, nm, dflt});
            }
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
            (void)expr(*c.args[0]);  // 保留求值副作用
            if (f || prop || meth) return const_bool(true);
            // 静态字段没有：查运行期动态属性表（类带槽时）。
            ot->cls->needs_dict = true;
            VReg obj2 = expr(*c.args[0]);
            if (optional) obj2 = guard_not_none(obj2, *an);
            return call_runtime(RtFn::AttrHas, t_bool(),
                               {obj2, const_str(*an)});
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
                else {
                    // 静态字段没有：查运行期动态属性表。
                    ot->cls->needs_dict = true;
                    VReg dflt = c.args.size() == 3 ? expr(*c.args[2]) : const_int(0);
                    return call_runtime(RtFn::AttrGet, t_unknown(),
                                       {obj, const_str(*an), dflt});
                }
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
            // 静态没有这个字段：写进运行期动态属性表。
            ot->cls->needs_dict = true;
            VReg val = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_unknown());
            call_void(RtFn::AttrSet, {obj, const_str(*an), val});
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
        // 动态属性（setattr 挂的成员）也要进 vars()：合并运行期 dict。
        // AttrDict 返回借用引用（可能 NULL＝从没 setattr 过），DictUpdate
        // 自己 retain 元素，这里不 push_temp、不 dec。
        bool dyn = false;
        for (const ClassInfo* p = ot->cls; p; p = p->base)
            if (p->needs_dict) { dyn = true; break; }
        if (dyn) {
            VReg ad = call_runtime(RtFn::AttrDict, mk_dict(t_str(), t_unknown()), {obj});
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Ne;
            cmp.a = retype(ad, t_int());
            cmp.b = const_int(0);
            VReg nz = def(t_bool(), std::move(cmp));
            int merge_bb = new_block("vars.dyn");
            int end_bb = new_block("vars.dyn.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = nz;
            br.target = merge_bb;
            br.target2 = end_bb;
            emit(std::move(br));
            switch_to(merge_bb);
            call_void(RtFn::DictUpdate, {d, ad});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end_bb;
            emit(std::move(j));
            switch_to(end_bb);
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
            call_void(RtFn::ListAppend, {l, s, const_int(1)});
        }
        return l;
    }

    // P4 内部 intrinsics（stdlib/contextlib.py 的 suppress/closing）。
    if (name == "__pa_exc_is_a") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "__pa_exc_is_a() takes exactly two arguments");
            return const_bool(false);
        }
        VReg nm = expr(*c.args[0]);
        VReg cls = expr(*c.args[1]);
        return call_runtime(RtFn::ExcClassIsA, t_bool(), {nm, cls});
    }
    if (name == "__pa_close") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "__pa_close() takes exactly one argument");
            return const_none();
        }
        VReg obj = expr(*c.args[0]);
        call_void(RtFn::MethodCall0, {obj, const_str("close")});
        return const_none();
    }
    // P4 块3（stdlib/traceback.py）：tb 帧链物化。返回 list[(str,int,str)]，
    // 堆返回值带 +1，push_temp 接管（与 ExcInfo 同口径）。
    if (name == "__pa_tb_frames") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "__pa_tb_frames() takes exactly one argument");
            return const_none();
        }
        VReg tb = expr(*c.args[0]);
        TypePtr rt = mk_list(mk_tuple({t_str(), t_int(), t_str()}));
        VReg v = call_runtime(RtFn::TbFrames, rt, {tb});
        push_temp(v);
        return v;
    }

    return kNoReg;
}

}  // namespace pyadamas
