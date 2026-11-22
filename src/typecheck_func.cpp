// typecheck_func.cpp —— 函数与类：定义体、方法、隐式字段
#include "typecheck.h"

#include "module.h"

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
            // 模块级函数与方法直接进重查表。
            //
            // **嵌套函数也要进**（0.382）：它们的未标注形参同样靠调用点回填，
            // 以前不登记，于是「函数里定义的 def 拿到实参类型」这条路径完全
            // 不存在 —— 形参停在 unknown，降级层看到 unknown 就报
            // 「method 'x' is not supported」。
            //
            // 重查时词法父作用域靠 `nested_recheck_parent_` 重新挂接（工作队列
            // 里已经这么做了），作用域对象由 pool_ 持有，重查期间仍然有效。
            if (cur_ == module_ || owner) {
                func_defs_.push_back(&f);
            } else {
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
        if (!owner && !f.decorators.empty()) {
            TypePtr cur = new_ty(TyKind::Func);
            cur->fn = info;
            bool known = true;
            for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
                // lru_cache 包装后签名与被装饰函数相同，类型上按恒等处理。
                if (is_lru_cache_expr(it->get())) continue;
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
        if (base) {
            cls->base_fields = base->n_fields();
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
                                   m.sig->pos[1].ty->kind != TyKind::Class) {
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
                case StmtKind::FunctionDef: {
                    // 自定义方法装饰器：在类定义点求值装饰器，把链的结果
                    // 注册成类属性；方法体仍按自己的签名检查。
                    auto& f = static_cast<FunctionDef&>(*s);
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

        // 方法体最后检查：签名、字段、self 类型都已就位。
        ClassInfo* saved_cls = cur_class_;
        cur_class_ = cls.get();
        for (auto& m : cls->methods) funcdef_impl(*m.def, cls);
        cur_class_ = saved_cls;
        // 无标注方法的返回类型是检查完才推断出来的，调用签名要重新切一份
        for (auto& m : cls->methods) rebuild_call_sig(m);
        if (cls->is_exception_class) {
            // 有自有字段或方法（含 __init__/__str__）的异常类在 except as e
            // 时给真实实例；否则保持扁平 Exception 值。
            if (!cls->fields.empty()) cls->is_rich_exception = true;
            if (!cls->methods.empty()) cls->is_rich_exception = true;
        }
        cls->complete = true;



    }


// 协议 dunder 的返回类型校验。必须在所有函数体检查完之后跑：未标注的
// 方法在定义处 ret 还是空的，只有 body 检查完才回填 ret_known。
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
