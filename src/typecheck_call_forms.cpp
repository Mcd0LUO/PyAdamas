// typecheck_call_forms.cpp —— call 按被调表达式形态拆分
#include "typecheck.h"

#include "module.h"

namespace pyadamas {

TypePtr TypeChecker::call_name(Name& n, Call& c) {
        if (n.id == "super" && !lookup("super")) {
            if (!cur_class_) {
                // 类方法上下文丢失（常见于导入基类的类体被延迟/分片检查）
                // 时，super() 静态无法解析；conservative 求值实参后放行。
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            if (c.keywords.empty() && c.args.size() == 2) {
                // super(Cls, self)：Cls 是当前类时等价于零参 super()。
                // 只接受当前类的名字作第一参，展开后按零参 super 处理。
                Expr* fst = c.args[0].get();
                const std::string* cname = nullptr;
                if (fst->kind == ExprKind::Name)
                    cname = &static_cast<Name*>(fst)->id;
                else if (fst->kind == ExprKind::Constant) {
                    auto& cc = static_cast<Constant&>(*fst);
                    if (cc.ck == Constant::CK::Str) cname = &cc.s;
                }
                if (cname && *cname == cur_class_->name) {
                    expr(*c.args[1]);  // 求值 self，保留副作用
                    return t_unknown();
                }
                err(c.line, c.col,
                    "two-argument super() is only supported for the current class");
                for (auto& arg : c.args) expr(*arg);
                return t_unknown();
            }
            if (!c.args.empty() || !c.keywords.empty()) {
                err(c.line, c.col,
                    "only zero-argument super() is supported inside a method");
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            return t_unknown();
        }
        if (!lookup(n.id) && is_exception_name(n.id)) {
            // 内置异常构造：CPython 允许任意多个位置参（生成 args 元组）
            // 和关键字参。这里只负责求值全部实参并返回异常类型。
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return mk_exception(n.id);
        }
        if (auto* sym = lookup(n.id);
            sym && sym->ty && sym->ty->kind == TyKind::Class && sym->ty->cls &&
            sym->ty->cls->is_exception_class) {
            const ClassInfo* cls = sym->ty->cls;
            if (const MethodInfo* init = cls->method("__init__"); init && init->call_sig) {
                for (auto& arg : c.args) expr(*arg);
                check_args(init->call_sig, c);
                return mk_class(const_cast<ClassInfo*>(cls));
            }
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return mk_class(const_cast<ClassInfo*>(cls));
        }
        if (!lookup(n.id) && is_builtin_callable(n.id)) return builtin_call(n.id, c);
        // from asyncio import gather/run/...：别名仍按 asyncio 的调用规则
        // 推断，否则 gather 会退回泛型签名，结果变成 list[unknown]。
        if (auto it = native_aliases_.find(n.id); it != native_aliases_.end()) {
            if (auto* sym = lookup(n.id); sym && sym->ty == it->second.sig &&
                it->second.module == "asyncio")
                if (auto r = asyncio_call(it->second.member, c)) return *r;
        }
        // sizeof / addr 是值类型 struct 的特殊形式：前者是编译期常量，
        // 后者把结构体值变成 ptr[T]，可以交给 C 函数。
        if (!lookup(n.id) && (n.id == "sizeof" || n.id == "addr")) {
            if (!c.keywords.empty()) {
                err(c.line, c.col, std::format("{}() takes no keyword arguments", n.id));
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            if (c.args.size() != 1) {
                err(c.line, c.col, std::format("{}() takes exactly one argument", n.id));
                for (auto& arg : c.args) expr(*arg);
                return n.id == "sizeof" ? t_int() : t_unknown();
            }
            TypePtr at = expr(*c.args[0]);
            if (n.id == "sizeof") {
                if (!at || at->kind != TyKind::Struct)
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("sizeof() expects a struct type or value, got '{}'",
                                    type_name(at)));
                return t_int();
            }
            if (at && at->kind == TyKind::Struct) return mk_struct_ptr(at->st);
            if (at && at->kind == TyKind::StructPtr) return at;
            err(c.args[0]->line, c.args[0]->col,
                std::format("addr() expects a struct value, got '{}'", type_name(at)));
            return t_unknown();
        }
        // 其余名字按普通函数值处理。
        return call_value(c);
    }

TypePtr TypeChecker::call_attr(Attribute& a, Call& c) {
        if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
            if (const CImportDecl* cd = cmodule(vn->id)) {
                for (auto& x : cd->externs) {
                    if (x.py_name != a.attr) continue;
                    auto it = extern_fns_.find(&x);
                    if (it == extern_fns_.end()) break;
                    auto ft = new_ty(TyKind::Func);
                    ft->fn = it->second;
                    TypePtr rt = check_args(ft->fn, c);
                    // str 参数在 C 侧是 char*，Optional[str] 可能是 NULL，
                    // 不能隐式收窄成 str。
                    auto check_str_arg = [&](Expr* arg, size_t pi) {
                        if (pi >= ft->fn->pos.size()) return;
                        const TypePtr& pt = ft->fn->pos[pi].ty;
                        TypePtr at = type_of(arg);
                        if (pt && pt->kind == TyKind::Str && at &&
                            at->kind == TyKind::Optional) {
                            err(arg->line, arg->col,
                                std::format("argument '{}' may be None; declare it as "
                                            "Optional[str] to accept NULL",
                                            ft->fn->pos[pi].name));
                        }
                    };
                    for (size_t i = 0; i < c.args.size(); ++i)
                        check_str_arg(c.args[i].get(), i);
                    for (auto& kw : c.keywords) {
                        for (size_t i = 0; i < ft->fn->pos.size(); ++i)
                            if (ft->fn->pos[i].name == kw.arg) {
                                check_str_arg(kw.value.get(), i);
                                break;
                            }
                    }
                    // 回调参数：实参必须是 PyAdamas 函数，签名按 .cpya 声明校验。
                    auto cb_arg_ty = [](CKind k) -> TypePtr {
                        switch (k) {
                            case CKind::Float: return t_float();
                            case CKind::Bool: return t_bool();
                            default: return t_int();
                        }
                    };
                    auto check_cb = [&](Expr* arg, const CParam& cp) {
                        TypePtr at = type_of(arg);
                        if (!at || at->kind != TyKind::Func || !at->fn) {
                            err(arg->line, arg->col,
                                std::format("callback '{}' expects a function", cp.name));
                            return;
                        }
                        const FuncInfo& fn = *at->fn;
                        if (fn.varargs || fn.pos.size() != cp.cb_params.size()) {
                            err(arg->line, arg->col,
                                std::format("callback '{}' expects {} parameter(s)", cp.name,
                                            cp.cb_params.size()));
                            return;
                        }
                        for (size_t j = 0; j < cp.cb_params.size(); ++j) {
                            TypePtr want = cb_arg_ty(cp.cb_params[j]);
                            TypePtr got = fn.pos[j].ty;
                            if (got && got->kind != TyKind::Unknown && !assignable(want, got)) {
                                err(arg->line, arg->col,
                                    std::format("callback '{}' parameter {} has type '{}', "
                                                "expected '{}'",
                                                cp.name, j + 1, type_name(got),
                                                type_name(want)));
                                return;
                            }
                        }
                        if (cp.cb_ret != CKind::None && fn.ret &&
                            fn.ret->kind != TyKind::Unknown &&
                            !assignable(cb_arg_ty(cp.cb_ret), fn.ret)) {
                            err(arg->line, arg->col,
                                std::format("callback '{}' returns '{}', expected '{}'",
                                            cp.name, type_name(fn.ret),
                                            type_name(cb_arg_ty(cp.cb_ret))));
                        }
                    };
                    for (size_t i = 0; i < c.args.size() && i < x.params.size(); ++i)
                        if (x.params[i].kind == CKind::Callback)
                            check_cb(c.args[i].get(), x.params[i]);
                    for (auto& kw : c.keywords)
                        for (size_t i = 0; i < x.params.size(); ++i)
                            if (x.params[i].name == kw.arg &&
                                x.params[i].kind == CKind::Callback)
                                check_cb(kw.value.get(), x.params[i]);
                    return rt;
                }
                err(c.line, c.col,
                    std::format("cimport '{}' has no function '{}'", vn->id, a.attr));
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            if (auto mod = lookup_module_alias(vn->id)) {
                if (mod->builtin && mod->name == "asyncio") {
                    if (auto r = asyncio_call(a.attr, c)) return *r;
                }
                if (mod->builtin && mod->name == "itertools") {
                    if (auto r = itertools_call(a.attr, c)) return *r;
                }
                if (mod->builtin && mod->name == "operator") {
                    if (auto r = operator_call(a.attr, c)) return *r;
                }
                if (mod->builtin && mod->name == "collections") {
                    if (auto r = collections_call(a.attr, c)) return *r;
                }
                if (mod->builtin && mod->name == "json") {
                    if (auto r = json_call(a.attr, c)) return *r;
                }
                if (mod->builtin && mod->name == "functools") {
                    if (auto r = functools_call(a.attr, c)) return *r;
                }
                if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end()) {
                    auto cls = ci->second.cls;
                    // 跨模块异常类构造：CPython 允许任意位置参（生成 args），
                    // 与 from m import E 的 cname 分支口径一致。
                    if (cls->is_exception_class) {
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                        return mk_class(cls.get());
                    }
                    if (cls->has_init && ci->second.ctor_sig) {
                        check_args(ci->second.ctor_sig, c);
                    } else if (!c.args.empty() || !c.keywords.empty()) {
                        err(c.line, c.col,
                            std::format("'{}' takes no arguments", cls->name));
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                    }
                    return mk_class(cls.get());
                }
                auto it = mod->globals.find(a.attr);
                if (it == mod->globals.end()) {
                    err(c.line, c.col,
                        std::format("module '{}' has no attribute '{}'", vn->id, a.attr));
                    return t_unknown();
                }
                auto mt = it->second;
                if (mt && mt->kind == TyKind::Func) return check_args(mt->fn, c);
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                err(c.line, c.col, "module attribute is not callable");
                return t_unknown();
            }
        }
        // super().m(...)：按定义类的 MRO 找下一实现，至少能检查签名。
        if (auto* sc = dynamic_cast<Call*>(a.value.get())) {
            if (auto* sn = dynamic_cast<Name*>(sc->func.get());
                sn && sn->id == "super" && !lookup("super") && sc->args.empty() &&
                sc->keywords.empty()) {
                if (!cur_class_) {
                    // 类上下文丢失（导入基类等方法分片）时保守放行。
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    return t_unknown();
                }
                // 富异常类的 super().__init__(msg)：基异常把消息存进 args。

                // 没有真实方法表，直接接受（build 端把它降级成记录消息）。

                if (cur_class_->is_exception_class && a.attr == "__init__") {

                    for (auto& arg : c.args) expr(*arg);

                    for (auto& kw : c.keywords) expr(*kw.value);

                    return t_none();

                }

                const MethodInfo* m = super_method(a.attr);

                if (!m) {
                    // 协议 dunder（__new__/__init_subclass__/__setattr__ 等）
                    // 常由 object/基类提供，静态找不到是正常的，保守放行。
                    if (a.attr.size() > 4 && a.attr.starts_with("__") &&
                        a.attr.ends_with("__")) {
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                        return t_unknown();
                    }
                    // 定义类的 MRO 里有导入基类时，这个方法的实现可能在
                    // 另一份接口里，check 阶段看不见。这不是错误——build
                    // 会沿运行期 MRO 找。只在纯本地继承链都找不到时报错。
                    if (cur_class_->is_open() || imported_base_in_mro(cur_class_)) {
                        for (auto& arg : c.args) expr(*arg);
                        for (auto& kw : c.keywords) expr(*kw.value);
                        return t_unknown();
                    }
                    err(c.line, c.col,
                        std::format("super() has no method '{}' after class '{}'",
                                    a.attr, cur_class_->name));
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    return t_unknown();
                }
                if (m->kind != MethodKind::Normal || !m->call_sig) {
                    err(c.line, c.col,
                        std::format("super().{} is not a plain method", a.attr));
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    return t_unknown();
                }
                return check_args(m->call_sig, c);
            }
        }
        // int.bit_length / type.__new__ 这类「类型上的未绑定方法」：
        // 接收者不是值，没有可检查的方法表
        if (auto* vn = dynamic_cast<Name*>(a.value.get()); vn && is_type_name(vn->id)) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        auto ot = expr(*a.value);
        // cdll 句柄的属性调用：lib.sym(...) 免写直调，符号经 dlsym(句柄) 解析。
        if (ot && ot->kind == TyKind::CDLL) return check_c_lazy(c, a.attr);
        if (ot && ot->kind == TyKind::TypeObj) {
            // type(self).method(...)：类型对象上的动态方法调用（__lt__ 等），
            // 静态没有方法表，conservative 返回 unknown。
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        if (is_unknown(ot)) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        // 类接收者 + 装饰方法：类属性里存的是包装函数，self 显式传。
        if (is_class_receiver(a) && ot && ot->kind == TyKind::ClassObj && !ot->sub.empty())
            ot = ot->sub[0];
        if (is_class_receiver(a) && ot->kind == TyKind::Class && ot->cls) {
            const MethodInfo* m = ot->cls->method(a.attr);
            if (m && m->decorated) {
                if (!m->decorated_ty || m->decorated_ty->kind != TyKind::Func ||
                    !m->decorated_ty->fn) {
                    // 装饰器返回类型未知：没有签名可检查，实参仍要
                    // 求值，调用结果按 unknown 处理。
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    return t_unknown();
                }
                return check_args(m->decorated_ty->fn, c);
            }
        }
        // 类接收者的普通方法：C.m(obj, ...) 显式传 self，按含 self 的签名检查。
        if (is_class_receiver(a) && ot->kind == TyKind::Class && ot->cls) {
            const MethodInfo* m = ot->cls->method(a.attr);
            if (m && m->kind == MethodKind::Normal && !m->decorated && m->sig)
                return check_args(m->sig, c);
        }
        return method_call(ot, a.attr, c, is_self_attr(a));
    }

TypePtr TypeChecker::call_value(Call& c) {
    auto ft = expr(*c.func);
    if (is_unknown(ft)) {
        for (auto& arg : c.args) expr(*arg);
        for (auto& kw : c.keywords) expr(*kw.value);
        return t_unknown();
    }
    if (ft->kind == TyKind::BoundMethod) return method_call(ft->sub[0], ft->name, c);
    if (ft->kind == TyKind::Struct) {
        // 值类型构造：Point(1.0, 2.0)，按字段声明顺序逐位置传参。
        auto st = ft->st;
        if (!st) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        if (!c.keywords.empty())
            err(c.line, c.col, "struct constructor does not take keyword arguments");
        for (auto& kw : c.keywords) expr(*kw.value);
        if (c.args.size() != st->fields.size()) {
            err(c.line, c.col,
                std::format("'{}' takes {} positional argument{} but {} were given",
                            st->name, st->fields.size(),
                            st->fields.size() == 1 ? "" : "s", c.args.size()));
        }
        for (size_t i = 0; i < c.args.size() && i < st->fields.size(); ++i) {
            TypePtr at = expr(*c.args[i]);
            const auto& f = st->fields[i];
            if (!assignable(f.ty, at))
                err(c.args[i]->line, c.args[i]->col,
                    std::format("field '{}' of '{}' expects '{}', got '{}'", f.name,
                                st->name, type_name(f.ty), type_name(at)));
        }
        for (size_t i = st->fields.size(); i < c.args.size(); ++i) expr(*c.args[i]);
        return mk_struct(st);
    }
    bool class_value = ft->kind == TyKind::ClassObj;
    if (class_value && !ft->sub.empty() && ft->sub[0]) ft = ft->sub[0];
    if (ft->kind == TyKind::Class) {
        // 类类型既可能是类本身，也可能是实例值：类名以外的来源
        // （变量、调用结果）都是实例，调用走 __call__。
        bool is_instance = !class_value && c.func->kind == ExprKind::Call;
        if (!class_value)
            if (auto* cn = dynamic_cast<Name*>(c.func.get()))
                is_instance = !name_is_class(*cn);
        if (is_instance) {
            const MethodInfo* cm = ft->cls ? ft->cls->method("__call__") : nullptr;
            if (!ft->cls || !cm || !cm->call_sig) {
                // 类实例无 __call__：可能是「类名当值」被误推成实例（
                // Fraction(...) 这类），保守放行返回 unknown。
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            return check_args(cm->call_sig, c);
        }
        // 构造：分配 + 字段初值 + 调 __init__。参数按 __init__ 的
        // 调用签名检查（已去掉 self），返回值是实例。
        auto cls = ft->cls;
        if (!cls) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        if (!cls->complete && cls != cur_class_) {
            // 类体还没检查到：按未知放行，别把「先使用后定义」判死
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return mk_class(cls);
        }
        if (cls->has_init) {
            if (auto* im = cls->method("__init__"); im && im->call_sig)
                check_args(im->call_sig, c);
            else
                for (auto& arg : c.args) expr(*arg);
        } else {
            // 没有 __init__：CPython 对带实参的实例化报 TypeError。以前
            // 为了 uuid.SafeUUID 一律放过——但 SafeUUID 其实是 @simple_enum
            // 生成的枚举，SafeUUID(0) 是 Enum 的值查找，根本不是「无
            // __init__ 带参实例化」。真正的来源是 @dataclass（装饰器在
            // 运行期合成 __init__，静态看不到）。所以恢复检查，只对
            // dataclass 和基类来源未知（is_open）的类放行。
            if ((!c.args.empty() || !c.keywords.empty()) && !cls->is_open() &&
                !cls->is_dataclass)
                err(c.line, c.col,
                    std::format("'{}' takes no arguments (no __init__)", cls->name));
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        return mk_class(cls);
    }
    if (ft->kind == TyKind::TypeObj) {
        // type(self)(...)：动态类型实例化。类型对象当构造器调用，
        // 结果类型静态未知，conservative 返回 unknown。
        for (auto& arg : c.args) expr(*arg);
        for (auto& kw : c.keywords) expr(*kw.value);
        return t_unknown();
    }
    if (ft->kind != TyKind::Func) {
        err(c.line, c.col, std::format("'{}' object is not callable", type_name(ft)));
        for (auto& arg : c.args) expr(*arg);
        for (auto& kw : c.keywords) expr(*kw.value);
        return t_unknown();
    }
    return check_args(ft->fn, c);
    }

}  // namespace pyadamas
