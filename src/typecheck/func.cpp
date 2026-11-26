// typecheck/func.cpp —— 函数与类：定义体、方法、隐式字段
#include "typecheck/typecheck.h"

#include "ir/module.h"

namespace pyadamas {

// Align tuple elements correctly: a variadic annotation has one element
// type, not a slot corresponding to the first item of a fixed tuple.
static TypePtr refine_return_holes(const TypePtr& to, const TypePtr& from, int depth = 0) {
    if (!to || !from || depth > 6) return to;
    if (to->kind == TyKind::Unknown)
        return to != t_unknown() && !is_unknown(from) ? from : to;
    if (to->kind != from->kind || to->variadic != from->variadic ||
        to->sub.size() != from->sub.size() || !ty_contains_hole(to, depth)) return to;
    auto result = std::make_shared<Type>(*to);
    // A bare mutable container permits widening after the call, even if
    // we now know which element type this invocation starts with.
    if (std::ranges::all_of(to->sub, [](const TypePtr& sub) {
            return is_unknown(sub) && sub != t_unknown();
        })) result->locked = false;
    for (size_t i = 0; i < to->sub.size(); ++i)
        result->sub[i] = refine_return_holes(to->sub[i], from->sub[i], depth + 1);
    return result;
}

// Scalar return coercion does not rewrite elements of an existing container.
// Don't publish a newly refined container signature if a fixed annotation
// would make the caller reinterpret an integer slot as float (or vice versa).
static bool return_slot_mismatch(const TypePtr& want, const TypePtr& actual, int depth = 0) {
    if (depth > 6 || is_unknown(want) || is_unknown(actual)) return false;
    if (want->kind == TyKind::Optional && !want->sub.empty() && actual->kind != TyKind::Optional)
        return actual->kind != TyKind::None && return_slot_mismatch(want->sub[0], actual, depth + 1);
    if (depth > 0 && (want->kind == TyKind::Float) != (actual->kind == TyKind::Float))
        return true;
    if (want->kind != actual->kind || want->variadic != actual->variadic ||
        want->sub.size() != actual->sub.size()) return false;
    for (size_t i = 0; i < want->sub.size(); ++i)
        if (return_slot_mismatch(want->sub[i], actual->sub[i], depth + 1)) return true;
    return false;
}

void TypeChecker::funcdef(FunctionDef& f) { funcdef_impl(f, nullptr); }

bool TypeChecker::reserve_imported_method_slots(const ClassInfo* cls, int line, int col) {
    bool ok = true;
    for (const MethodInfo& m : cls->methods) {
        std::string key = method_slot_key(m.name, m.kind);
        std::vector<int> slots{m.slot};
        for (int a : m.aliases) slots.push_back(a);
        for (int slot : slots) {
            if (slot < 0) continue;
            auto owner = method_slot_owner_.find(slot);
            if (owner != method_slot_owner_.end() && owner->second != key) {
                err(line, col,
                    std::format("slot {} of imported class '{}' is already used by method '{}' in "
                                "this module",
                                slot, cls->name, owner->second));
                ok = false;
                continue;
            }
            method_slot_owner_.emplace(slot, key);
        }
        if (m.slot >= 0 && method_slots_.find(key) == method_slots_.end())
            method_slots_.emplace(key, m.slot);
        if (m.slot >= 0) method_slot_count_ = std::max(method_slot_count_, m.slot + 1);
        for (int a : m.aliases)
            if (a >= 0) method_slot_count_ = std::max(method_slot_count_, a + 1);
    }
    return ok;
}

void TypeChecker::funcdef_impl(FunctionDef& f, const ClassInfoPtr& owner) {
        if (!func_owners_.count(&f)) {
            func_owners_[&f] = owner;
            // 嵌套函数也要进重查表（0.382）：未标注形参靠调用点回填，不登记就停在
            // unknown；重查时词法父作用域经 nested_recheck_parent_ 重新挂接。
            if (cur_ == module_ || owner) {
                func_defs_.push_back(&f);
            } else {
                nested_recheck_parent_[&f] = cur_;
                func_defs_.push_back(&f);
            }
        }
        FuncInfoPtr info;  // owner：方法；否则模块级/嵌套函数
        if (owner) {
            // 方法签名 classdef 里已建好（方法间要能互调），这里复用同一份对象。
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
                // 0.568：def 不是标注 —— 同名重定义（两支各定义一个）合法，
                // 按「标注」登记会被 ann_seen 判成类型冲突。
                declare(f.name, t, f.line, f.col, false);
            } else {
                // 装饰器的返回值未知；函数体仍然按自己的签名检查
                declare(f.name, t_unknown(), f.line, f.col, true);
            }
        }

        if (f.returns && info->ret_known && !info->is_async && !info->is_generator)
            info->ret = parse_annotation(f.returns.get(), f.line);

        if (!owner)
            if (auto* sym = lookup(f.name)) sym->from_def = true;

        // 默认值和装饰器在外层作用域求值 —— Python 是在 def 执行时算的，
        // 此时形参还不存在。先检查再进函数作用域，`def f(a, b=a)` 里的 a
        // 才不会被误解析成形参。
        for (auto& d : f.args.defaults) expr(*d);
        for (auto& d : f.args.kw_defaults)
            if (d) expr(*d);
        // u64 大字面量默认值在 def 点消费（函数可能永远不被调用，等不到
        // 调用点的 bind_default）。只认 u64 字面量——普通定宽默认值的既有
        // 路径（调用点 int_literal_fits）逐字不动。
        auto accept_u64_default = [&](Param& p) {
            if (!p.has_default || !p.default_expr || !p.ty) return;
            if (p.ty->kind != TyKind::U64 && p.ty->kind != TyKind::Big) return;
            // 改写 CK::Big 需要去 const：与 accept_literal_into 同口径，
            // 字面量定型是类型检查期的合法 AST 改写。
            auto* c = const_cast<Constant*>(dynamic_cast<const Constant*>(p.default_expr));
            if (c && c->ck == Constant::CK::Int && c->u64_lit) {
                if (p.ty->kind == TyKind::Big) {
                    // P3：big 形参默认值消费 u64 大字面量（按无符号值转写）。
                    c->ck = Constant::CK::Big;
                    c->s = std::to_string(static_cast<uint64_t>(c->i));
                    c->u64_lit = false;
                }
                expr_types_[p.default_expr] = p.ty;
                u64_lit_ok_.insert(p.default_expr);
            }
        };
        for (auto& p : info->pos) accept_u64_default(p);
        for (auto& p : info->kwonly) accept_u64_default(p);
        for (auto& dec : f.decorators)
            if (!owner) expr(*dec);

        // 装饰器的返回值常常就是包装函数：从下往上套，每一层用返回类型
        // 替换当前类型。这样调用点能按包装签名检查实参并做数值转换，
        // 而不是退化成 unknown 后按错误的调用约定传参。
        //
        // 试过「未知装饰器 → 保留被装饰函数签名」，在 628 文件语料上 **500 →
        // 496**（asyncio/tasks、importlib/resources/_common、pkgutil、
        // test/support/import_helper）：contextmanager / types.coroutine /
        // singledispatch 这些装饰器**确实改变**签名与返回类型，保留签名会误杀。
        // 判负，保持「装饰后的符号是 unknown」。详见 docs/frontend-gaps.md §8.3。
        if (!owner && !f.decorators.empty()) {
            TypePtr cur = new_ty(TyKind::Func);
            cur->fn = info;
            bool known = true;
            for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
                // lru_cache 包装后签名与被装饰函数相同，类型上按恒等处理。
                if (is_lru_cache_expr(it->get())) continue;
                // 0.540: @contextmanager 的精确签名（实现见 call_modules.cpp）。
                if (is_contextmanager_expr(it->get()))
                    if (TypePtr cm = contextmanager_func_type(f.name)) { cur = cm; continue; }
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
        signature_defs_[info.get()] = &f;
        auto* prev_fn_def = cur_fn_def_;
        auto* prev_fn_scope = cur_fn_scope_;
        cur_fn_ = info;
        cur_fn_def_ = &f;
        cur_fn_scope_ = sc.get();
        int saved_loop_depth = std::exchange(loop_depth_, 0);
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
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, !p.inferred, false};
        for (auto& p : info->kwonly)
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, !p.inferred, false};
        if (info->varargs)
            // *args 在 Python 里是 tuple，不是 list：`(v,) + args` 合法
            cur_->vars[info->vararg_name] =
                Symbol{mk_tuple_rest(info->vararg_ty ? info->vararg_ty : t_unknown()), f.line, false,
                       false};
        if (info->kwargs)
            cur_->vars[info->kwarg_name] =
                Symbol{mk_dict(t_str(), info->kwarg_ty ? info->kwarg_ty : t_unknown()),
                       f.line, false, false};

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
        if (type_stats_on_) {
            std::set<std::string> pnames;
            auto note_param = [&](const Param& p) {
                pnames.insert(p.name);
                if (p.inferred && is_unknown(p.ty))
                    type_stat("param_unannotated", f.name + "." + p.name);
            };
            for (auto& p : info->pos) note_param(p);
            for (auto& p : info->kwonly) note_param(p);
            if (!info->vararg_name.empty()) pnames.insert(info->vararg_name);
            if (!info->kwarg_name.empty()) pnames.insert(info->kwarg_name);
            if (info->ret && is_unknown(info->ret)) type_stat("return_unknown", f.name);
            for (auto& [n, sym] : cur_->vars) {
                if (pnames.count(n)) continue;
                if (is_unknown(sym.ty) && !sym.annotated)
                    type_stat("local_unknown", f.name + "." + n);
                else if (type_contains_unknown_elem(sym.ty))
                    type_stat("container_elem", f.name + "." + n);
            }
        }
        // **用函数体的实际用法回填参数标注里的类型洞。**
        //
        // 裸 `list` / `set` / `dict` / `tuple` 标注解析成带 fresh_hole 的类型。
        // 返回位置的洞已经由 `inferred`（实际 return 值）填上了，但**参数位置
        // 没有赋值来源** —— 洞永远填不上，参数就停在 `list[unknown]`。
        //
        // 而完全不标注时反而能推出精确类型：参数以 t_unknown() 进作用域，
        // 函数体里的用法（`for x in xs` 把元素并进 xs、`total + x` 收窄 x）
        // 会通过 merge_elem / join 把它收窄成 `list[int]`。
        //
        // 也就是说「标注裸 list」比「不标注」还差 —— 这是结构性缺陷，不是
        // 某个具体分支的问题。修法：函数体检查完后，把作用域里参数的**实际
        // 类型**回填到标注的洞上。这样裸标注至少和「不标注」等价，而不是更差。
        //
        // 实测 bench/compare/nqueens.py：`def place(..., cols: list, d1: list,
        // d2: list)` 三个参数全是 list[unknown]，热循环里 3 处 index.get 走
        // unknown 路径。
        for (auto& p : info->pos) {
            if (p.inferred) continue;   // 没标注的本来就是 unknown，不用回填
            auto it = cur_->vars.find(p.name);
            if (it == cur_->vars.end() || !it->second.ty) continue;
            fill_holes_recursive(p.ty, it->second.ty, 0);
        }
        for (auto& p : info->kwonly) {
            if (p.inferred) continue;
            auto it = cur_->vars.find(p.name);
            if (it == cur_->vars.end() || !it->second.ty) continue;
            fill_holes_recursive(p.ty, it->second.ty, 0);
        }
        cur_self_name_ = std::move(saved_self_name);
        cur_cls_name_ = std::move(saved_cls_name);

        // 无标注函数：从所有 return 处推断返回类型
        // 从所有 return 处推断返回值类型。
        //
        // **标注了也要算**：裸 `list` / `set` / `dict` 标注解析成含 hole 的
        // 类型，需要这个推断结果去填（见下面的收窄逻辑）。原来只在
        // `!ret_declared` 时计算，导致 `-> list:` 的 hole 永远填不上。
        TypePtr inferred = nullptr;
        for (auto& r : rets) inferred = inferred ? join(inferred, r.ty) : r.ty;

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
            // 生成器：标注了 Generator[Y, S, R] 就以标注为准，Y 未知时用
            // 推导出的 yield 上界补；否则调用得到 Generator[yield 上界]。
            TypePtr elem = nullptr;
            for (auto& y : yields) elem = elem ? join(elem, y) : y;
            TypePtr gen = (info->ret_declared && info->ret &&
                           info->ret->kind == TyKind::Generator)
                              ? info->ret
                              : nullptr;
            if (gen) {
                if (gen->sub.empty()) gen->sub.push_back(elem ? elem : t_unknown());
                else if (!gen->sub[0] || gen->sub[0]->kind == TyKind::Unknown)
                    gen->sub[0] = elem ? elem : t_unknown();
                info->ret = gen;
            } else {
                info->ret = mk_generator(elem ? elem : t_unknown());
            }
            info->is_generator = true;
        } else if (!info->ret_declared) {
            info->ret = inferred;
        } else if (ty_contains_hole(info->ret, 0) && inferred) {
            // Refine only holes; explicit Any and concrete siblings are not holes.
            const auto& decl = info->ret;
            for (auto& r : rets) {
                if (return_slot_mismatch(decl, r.ty))
                    err(r.line, r.col, std::format(
                        "return of '{}' has incompatible float storage: declared '{}', got '{}'",
                        f.name, type_name(decl), type_name(r.ty)));
            }
            if (decl->kind == TyKind::Tuple && decl->variadic &&
                inferred->kind == TyKind::Tuple && !inferred->variadic &&
                decl->sub.size() == 1 && is_unknown(decl->sub[0]) &&
                decl->sub[0] != t_unknown()) {
                // A bare tuple does not fix the arity. Keep a known shape.
                info->ret = inferred;
            } else {
                info->ret = refine_return_holes(decl, inferred);
            }
        }
        info->ret_known = true;

        cur_ = prev_scope;
        cur_fn_ = prev_fn;
        cur_fn_def_ = prev_fn_def;
        cur_fn_scope_ = prev_fn_scope;
        saw_yield_ = prev_yield;
        yields_ = prev_yields;
        rets_ = prev_rets;
        loop_depth_ = saved_loop_depth;
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
bool TypeChecker::name_is_class(const Name& n) {
    if (!cur_cls_name_.empty() && n.id == cur_cls_name_) return true;
    if (!cur_self_name_.empty() && n.id == cur_self_name_) return false;
    // 类名绑定在模块作用域里；被函数局部或形参遮住就不是类了。
    // class_obj_names_ 里没有的名字（模块级的实例变量）按实例处理。
    for (Scope* s = cur_; s; s = s->parent) {
        auto it = s->vars.find(n.id);
        if (it == s->vars.end()) continue;
        // class 语句绑定的名字（含函数内定义的类）。
        if (it->second.is_class_name) return true;
        // `from m import Class` 也是类对象绑定 —— **不管绑在哪一层作用域**。
        // 原来这里写 `s->is_module && ...`，于是函数内的 from-import 被当成
        // 实例：`TarInfo("a.txt")` 返回 unknown，读属性直接报
        // 「attribute access is only supported」。判据改成看静态类型是不是
        // 类对象，比「变量在哪一层」准。
        if (it->second.ty && it->second.ty->kind == TyKind::Class &&
            class_obj_names_.count(n.id) > 0)
            return true;
        return false;
    }
    return false;
}

bool TypeChecker::is_class_receiver(const Attribute& a) {
    if (a.value->kind != ExprKind::Name) return false;
    return name_is_class(static_cast<const Name&>(*a.value));
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
// 模块里的隐藏全局），其余（if / for / with …）在类定义点执行后按模块
// 全局处理，类标记成 open。不能在这里报错，真实代码太常见。
void TypeChecker::check_dunder_returns() {
    for (auto& ci : all_classes_) {
        if (!ci) continue;
        for (auto& mi : ci->methods) {
            if (!mi.def || !mi.sig || mi.kind != MethodKind::Normal) continue;
            if (mi.sig->varargs || mi.sig->kwargs) continue;
            const std::string& n = mi.name;
            TyKind want;
            if (n == "__len__" || n == "__hash__")
                want = TyKind::Int;
            else if (n == "__str__" || n == "__repr__" || n == "__format__")
                want = TyKind::Str;
            else if (n == "__bool__" || n == "__eq__" || n == "__ne__" ||
                     n == "__contains__" || n == "__lt__" || n == "__le__" ||
                     n == "__gt__" || n == "__ge__")
                want = TyKind::Bool;
            else
                continue;
            TypePtr r = mi.sig->ret;
            if (!r || is_unknown(r) || !mi.sig->ret_known) continue;
            bool ok = r->kind == want;
            // bool 是 int 的子类，整型返回也不算错。
            if (!ok && want == TyKind::Int && r->kind == TyKind::Bool) ok = true;
            if (!ok && want == TyKind::Bool && r->kind == TyKind::Int) ok = true;
            if (ok) continue;
            const char* want_s = want == TyKind::Int   ? "int"
                                 : want == TyKind::Str ? "str"
                                                       : "bool";
            err(mi.def->line, mi.def->col,
                std::format("'{}' must return {}", n, want_s));
        }
    }
}

}  // namespace pyadamas
