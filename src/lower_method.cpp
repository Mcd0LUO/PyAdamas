// lower_method.cpp —— 方法与 super 调用降级
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_super_call(Attribute& at, Call& c) {
    auto fit = fn_owner_class_.find(st_->fn);
    const ClassInfo* owner = fit == fn_owner_class_.end() ? nullptr : fit->second;
    if (!owner) {
        err(c.line, c.col, "super() is only supported directly inside a method body");
        return const_none();
    }
    if (st_->fn->params.empty()) {
        err(c.line, c.col, "super() requires a method receiver");
        return const_none();
    }
    const MethodInfo* m = nullptr;
    const ClassInfo* declaring = nullptr;
    bool after = false;
    for (const ClassInfo* k : owner->mro) {
        if (!after) {
            if (k == owner) after = true;
            continue;
        }
        if (auto* km = k->own_method(at.attr)) {
            m = km;
            declaring = k;
            break;
        }
    }
    if (!m) {
        err(c.line, c.col,
            std::format("super() has no method '{}' after class '{}'", at.attr, owner->name));
        return const_none();
    }
    if (m->kind == MethodKind::Normal && m->decorated) {
        // 装饰方法的实现是声明类类属性里的包装函数：显式补上 self。
        const ClassAttrInfo* ca = declaring ? declaring->class_attr(at.attr) : nullptr;
        FuncInfoPtr winfo =
            (ca && ca->ty && ca->ty->kind == TyKind::Func) ? ca->ty->fn : nullptr;
        if (!ca || !winfo || !m->call_sig) {
            err(c.line, c.col,
                std::format("decorated method '{}.{}' is not available here",
                            declaring ? declaring->name : owner->name, at.attr));
            return const_none();
        }
        VReg fv = class_attr_load(declaring, ca);
        std::vector<VReg> args = bind_args(c, m->call_sig, nullptr);
        const IRParam& selfp = st_->fn->params.front();
        args.insert(args.begin(), load_slot(selfp.slot, selfp.ty));
        Inst in;
        in.op = IrOp::Call;
        in.a = fv;
        in.args = std::move(args);
        TypePtr rt = winfo->ret ? winfo->ret : t_unknown();
        return def_heap(rt, std::move(in));
    }
    if (m->kind != MethodKind::Normal || m->decorated) {
        err(c.line, c.col,
            std::format("super().{} is not a plain method", at.attr));
        return const_none();
    }
    if (!m->call_sig) {
        err(c.line, c.col, std::format("super().{} has no known signature", at.attr));
        return const_none();
    }
    auto cid = class_ids_.find(owner);
    if (cid == class_ids_.end()) {
        err(c.line, c.col, "super() is not available for this class");
        return const_none();
    }
    std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
    const IRParam& selfp = st_->fn->params.front();
    VReg self = load_slot(selfp.slot, selfp.ty);
    Inst sv;
    sv.op = IrOp::SuperVCall;
    sv.a = self;
    // 类编号只在模块内唯一；after 类按运行期限定名定位，跨模块继承
    // （基类方法在对象上执行 super()）才能找对位置。
    sv.c = const_str(owner->origin_qualname.empty()
                         ? mod_.runtime_name + "." + owner->name
                         : owner->origin_qualname);
    sv.idx = cid->second;
    sv.imm = m->slot;
    sv.b = const_str(at.attr);
    sv.args = std::move(args);
    TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
    return def_heap(rt, std::move(sv));
}

VReg Lowerer::lower_method_call(Attribute& at, Call& c) {
    ++side_effect_seq_;
    // super().m(...)：沿运行期 MRO 找下一个实现。
    if (auto* sup = dynamic_cast<Call*>(at.value.get())) {
        if (auto* sn = dynamic_cast<Name*>(sup->func.get());
            sn && sn->id == "super" && !is_bound_here("super")) {
            if (!sup->args.empty() || !sup->keywords.empty()) {
                err(c.line, c.col, "only zero-argument super() is supported");
                return const_none();
            }
            return lower_super_call(at, c);
        }
    }
    // cimport 外部函数：alias.fname(...) 直接生成 CallExtern。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (tc_.cmodule(n->id)) return lower_cimport_call(at, c);
    }
    // 内置模块的原生函数/常量。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
            if (mod->name == "asyncio") return lower_asyncio(at.attr, c);
            if (mod->name == "itertools") return lower_itertools(at.attr, c);
            if (mod->name == "operator") return lower_operator(at.attr, c);
            if (mod->name == "collections") return lower_collections(at.attr, c);
            if (mod->name == "functools") return lower_functools(at.attr, c);
            if (mod->name == "datetime") return lower_datetime(at.attr, c);
            if (mod->name == "pathlib") return lower_pathlib(at.attr, c);
            if (mod->name == "json") return lower_json(at.attr, c);
            auto it = mod->globals.find(at.attr);
            if (it != mod->globals.end() && it->second && it->second->kind == TyKind::Func)
                return call_native(mod->name, at.attr, c, it->second->fn);
            err(c.line, c.col,
                std::format("module attribute '{}.{}' is not callable", n->id, at.attr));
            return const_none();
        }
    }
    // m.C()：导入类的构造包装。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id)) {
            if (auto ci = mod->classes.find(at.attr); ci != mod->classes.end())
                return construct_imported(ci->second, c, mk_class(ci->second.cls.get()));
        }
    }
    // m.f()：外部全局里存的是函数闭包，加载后按普通 call 调用。
    if (auto gi = module_member(at)) {
        TypePtr ft = mod_.globals[*gi].ty;
        FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;
        if (!info) {
            err(c.line, c.col, "module attribute is not callable");
            return const_none();
        }
        VReg callee = load_global(*gi, ft);
        std::vector<VReg> args = bind_args(c, info, nullptr);
        Inst in;
        in.op = IrOp::Call;
        in.a = callee;
        in.args = std::move(args);
        TypePtr rt = info->ret ? info->ret : t_unknown();
        return def_heap(rt, std::move(in));
    }
    // 非入口模块编译的 classmethod 把 cls 当运行期类对象：入口模块仍用
    // 静态类 id 直调；导入方法 / 非入口模块方法走类对象 vtable。
    auto cmethod_dynamic = [&](const MethodInfo* m) {
        return m->kind == MethodKind::ClassMethod && (!opts_.is_entry || !m->def);
    };
    auto class_object_for = [&](const ClassInfo* klass) -> VReg {
        if (st_->class_self_dynamic && st_->class_self_ty == klass &&
            !st_->class_self_name.empty()) {
            int slot =
                st_->slots.count(st_->class_self_name) ? st_->slots[st_->class_self_name] : -1;
            return slot >= 0 ? load_slot(slot, t_int()) : const_int(0);
        }
        if (class_ids_.count(klass)) return local_class_object(klass->name);
        for (auto& [alias, ic] : imported_classes_)
            if (ic.cls.get() == klass) return imported_class_object(ic);
        for (auto& [alias, mod] : st_->modules)
            for (auto& ce : mod->classes)
                if (ce.second.cls.get() == klass) return imported_class_object(ce.second);
        for (auto& [alias, mod] : module_aliases_)
            for (auto& ce : mod->classes)
                if (ce.second.cls.get() == klass) return imported_class_object(ce.second);
        err(at.line, at.col,
            std::format("class '{}' is not available for method dispatch", klass->name));
        return const_none();
    };
    auto emit_dynamic_method = [&](VReg cls_obj, const MethodInfo* m, bool pass_cls,
                                   std::vector<VReg> args) -> VReg {
        if (!m->call_sig || m->slot < 0) return const_none();
        VReg fn = call_runtime(RtFn::ClassMethod, t_int(),
                               {cls_obj, const_int(m->slot)});
        if (pass_cls) args.insert(args.begin(), cls_obj);
        Inst cp;
        cp.op = IrOp::CallPtr;
        cp.a = fn;
        cp.args = std::move(args);
        TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
        return def_heap(rt, std::move(cp));
    };

    // C.f(...) / cls.f(...)：类接收者的静态方法、类方法直接调用；
    // 普通方法需要实例。@classmethod 里的 cls 编译期就绑定成所属类，
    // 和写类名完全等价。
    const ClassInfo* class_recv = nullptr;
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (!st_->class_self_name.empty() && n->id == st_->class_self_name &&
            st_->class_self_ty) {
            class_recv = st_->class_self_ty;
        } else if (!is_bound_here(n->id) && !imported_classes_.count(n->id)) {
            auto cit = classes_.find(n->id);
            if (cit != classes_.end() && cit->second && !cit->second->is_exception_class)
                class_recv = cit->second.get();
        }
    }
    if (class_recv) {
        {
            const ClassInfo* cls = class_recv;
            const MethodInfo* m = cls->method(at.attr);
            if (!m) {
                err(c.line, c.col,
                    std::format("'{}' has no method '{}'", cls->name, at.attr));
                return const_none();
            }
            if (m->decorated) {
                // 装饰器链的结果存在类属性里，签名是包装函数的（含 self）。
                const ClassAttrInfo* ca = cls->class_attr(at.attr);
                FuncInfoPtr winfo =
                    (ca && ca->ty && ca->ty->kind == TyKind::Func) ? ca->ty->fn : nullptr;
                if (!ca) {
                    err(c.line, c.col,
                        std::format("decorated method '{}.{}' is not available here",
                                    cls->name, at.attr));
                    return const_none();
                }
                VReg fv = class_attr_load(cls, ca);
                std::vector<VReg> args;
                TypePtr rt = t_unknown();
                if (winfo) {
                    std::vector<VReg> bound = bind_args(c, winfo, nullptr);
                    args.insert(args.end(), bound.begin(), bound.end());
                    rt = winfo->ret ? winfo->ret : t_unknown();
                } else {
                    if (!c.keywords.empty()) {
                        err(c.line, c.col,
                            "decorated method with unknown signature does not take keyword arguments");
                        return const_none();
                    }
                    for (auto& a : c.args) args.push_back(expr(*a));
                }
                Inst in;
                in.op = IrOp::Call;
                in.a = fv;
                in.args = std::move(args);
                return def_heap(rt, std::move(in));
            }
            if (m->kind == MethodKind::Normal) {
                // C.m(obj, ...)：self 由调用方显式传入，直调最终实现。
                if (!m->sig) return const_none();
                auto ncid = class_ids_.find(cls);
                if (ncid == class_ids_.end()) {
                    err(c.line, c.col,
                        std::format("method '{}.{}' is not available here", cls->name,
                                    at.attr));
                    return const_none();
                }
                const auto& nvt = mod_.classes[static_cast<size_t>(ncid->second)].vtable;
                if (m->slot < 0 || m->slot >= static_cast<int>(nvt.size()) ||
                    nvt[static_cast<size_t>(m->slot)] < 0) {
                    err(c.line, c.col,
                        std::format("method '{}.{}' was not compiled", cls->name, at.attr));
                    return const_none();
                }
                std::vector<VReg> args = bind_args(c, m->sig, m->def);
                Inst cd;
                cd.op = IrOp::CallDirect;
                cd.idx = nvt[static_cast<size_t>(m->slot)];
                for (VReg a : args) cd.args.push_back(a);
                TypePtr rt = m->sig->ret ? m->sig->ret : t_none();
                return def_heap(rt, std::move(cd));
            }
            if (m->kind == MethodKind::ClassMethod && cmethod_dynamic(m)) {
                std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
                VReg obj = class_object_for(cls);
                return emit_dynamic_method(obj, m, true, std::move(args));
            }
            if (m->kind == MethodKind::Static && (!opts_.is_entry || !m->def)) {
                std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
                VReg obj = class_object_for(cls);
                return emit_dynamic_method(obj, m, false, std::move(args));
            }
            if (m->kind != MethodKind::Static && m->kind != MethodKind::ClassMethod) {
                err(c.line, c.col,
                    std::format("'{}.{}' is an instance method; call it on an instance",
                                cls->name, at.attr));
                return const_none();
            }
            if (!m->call_sig) return const_none();
            // 用接收者类的 vtable：类方法在子类里会重新编一份，槽位指向
            // 子类特化的实现；静态方法继承时也应当按 MRO 解析到最终覆盖。
            auto cid = class_ids_.find(cls);
            if (cid == class_ids_.end()) {
                err(c.line, c.col,
                    std::format("static method '{}.{}' is not available here", cls->name,
                                at.attr));
                return const_none();
            }
            const auto& vt = mod_.classes[cid->second].vtable;
            if (m->slot < 0 || m->slot >= static_cast<int>(vt.size()) || vt[m->slot] < 0) {
                err(c.line, c.col,
                    std::format("static method '{}.{}' was not compiled", cls->name, at.attr));
                return const_none();
            }
            std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
            Inst cd;
            cd.op = IrOp::CallDirect;
            cd.idx = vt[m->slot];
            // 类方法的 cls 占第一个实参位：传所属类的 id，纯整数不参与 rc
            if (m->kind == MethodKind::ClassMethod) cd.args.push_back(const_int(cid->second));
            for (VReg a : args) cd.args.push_back(a);
            TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
            return def_heap(rt, std::move(cd));
        }
    }
    // from m import C 后 C.method(...)：导入类的类接收者。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        auto iit = imported_classes_.find(n->id);
        if (iit != imported_classes_.end()) {
            const ImportedClass& ic = iit->second;
            const MethodInfo* m = ic.cls->method(at.attr);
            if (!m) {
                err(c.line, c.col,
                    std::format("'{}' has no method '{}'", ic.cls->name, at.attr));
                return const_none();
            }
            if (m->kind == MethodKind::Normal) {
                err(c.line, c.col,
                    std::format("'{}.{}' is an instance method; call it on an instance",
                                ic.cls->name, at.attr));
                return const_none();
            }
            std::vector<VReg> args = bind_args(c, m->call_sig, nullptr);
            VReg obj = imported_class_object(ic);
            return emit_dynamic_method(obj, m, m->kind == MethodKind::ClassMethod,
                                       std::move(args));
        }
    }
    TypePtr raw = tc_.type_of(at.value.get());
    TypePtr recv_ty = strip_optional(raw);
    bool opt = recv_ty != raw;
    VReg recv = expr(*at.value);

    // 用户类的方法调用：从对象的 vtable 里取函数地址。槽号在编译期
    // 就定了，运行时只有一次间接跳转，不需要按名字查表。
    if (recv_ty && recv_ty->kind == TyKind::Class && recv_ty->cls) {
        if (opt) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
        auto* m = recv_ty->cls->method(at.attr);
        if (!m) {
            err(c.line, c.col,
                std::format("'{}' object has no method '{}'", recv_ty->cls->name, at.attr));
            return const_none();
        }
        if (m->decorated) {
            // 装饰后的方法值存在类属性里：加载出来，把 self 作为第一个
            // 实参传进去（装饰器按普通函数值被调用）。
            const ClassAttrInfo* ca = recv_ty->cls->class_attr(at.attr);
            if (!ca) {
                err(c.line, c.col,
                    std::format("decorated method '{}' is not available here", at.attr));
                return const_none();
            }
            VReg fv = class_attr_load_dyn(recv, recv_ty->cls, ca);
            std::vector<VReg> args;
            args.push_back(recv);
            TypePtr rt = t_unknown();
            if (m->call_sig) {
                std::vector<VReg> bound = bind_args(c, m->call_sig, nullptr);
                args.insert(args.end(), bound.begin(), bound.end());
                rt = m->call_sig->ret ? m->call_sig->ret : t_none();
            } else {
                // 装饰器返回类型未知：没有签名可重排，只接受纯位置实参，
                // 求值后按机器字原样转发，结果也按 unknown 处理。
                if (!c.keywords.empty()) {
                    err(c.line, c.col,
                        "decorated method with unknown signature does not take keyword arguments");
                    return const_none();
                }
                for (auto& a : c.args) args.push_back(expr(*a));
            }
            Inst in;
            in.op = IrOp::Call;
            in.a = fv;
            in.args = std::move(args);
            return def_heap(rt, std::move(in));
        }
        if (!m->call_sig) return const_none();
        std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
        if (m->kind == MethodKind::ClassMethod && cmethod_dynamic(m)) {
            VReg obj = class_object_for(recv_ty->cls);
            return emit_dynamic_method(obj, m, true, std::move(args));
        }
        // 类方法在实例上调用时，cls 仍占第一个实参位；传静态类型对应的类 id
        if (m->kind == MethodKind::ClassMethod) {
            auto cid = class_ids_.find(recv_ty->cls);
            args.insert(args.begin(), const_int(cid == class_ids_.end() ? 0 : cid->second));
        }
        // 终态类（本模块没有任何类继承它）：vtable 槽在编译期就能解析成
        // 具体实现，直接调用，省掉两次查表和间接跳转，也让内联器能接上。
        if ((m->kind == MethodKind::Normal || m->kind == MethodKind::Static ||
             m->kind == MethodKind::ClassMethod) &&
            sealed_classes_.count(recv_ty->cls)) {
            auto cid = class_ids_.find(recv_ty->cls);
            if (cid != class_ids_.end() && m->slot >= 0) {
                const auto& vt = mod_.classes[static_cast<size_t>(cid->second)].vtable;
                if (m->slot < static_cast<int>(vt.size())) {
                    int fidx = vt[static_cast<size_t>(m->slot)];
                    const IRFunction* callee =
                        (fidx >= 0 && fidx < static_cast<int>(mod_.funcs.size()))
                            ? mod_.funcs[static_cast<size_t>(fidx)].get()
                            : nullptr;
                    if (callee && callee->env_slot < 0) {
                        Inst cd;
                        cd.op = IrOp::CallDirect;
                        cd.idx = fidx;
                        // 只有普通方法传 self；静态方法和类方法都不占这个位置
                        if (m->kind == MethodKind::Normal) cd.args.push_back(recv);
                        for (VReg a : args) cd.args.push_back(a);
                        TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
                        return def_heap(rt, std::move(cd));
                    }
                }
            }
        }
        Inst vc;
        vc.op = (m->kind == MethodKind::Static || m->kind == MethodKind::ClassMethod)
                    ? IrOp::VCallStatic
                    : IrOp::VCall;
        vc.a = recv;
        vc.imm = m->slot;
        vc.args = std::move(args);
        TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
        return def_heap(rt, std::move(vc));
    }

    if (recv_ty && (recv_ty->kind == TyKind::AsyncEvent ||
                    recv_ty->kind == TyKind::AsyncLock ||
                    recv_ty->kind == TyKind::AsyncQueue ||
                    recv_ty->kind == TyKind::AsyncTimeout ||
                    recv_ty->kind == TyKind::AsyncTaskGroup ||
                    recv_ty->kind == TyKind::AsyncSemaphore ||
                    recv_ty->kind == TyKind::DateTime ||
                    recv_ty->kind == TyKind::TimeDelta ||
                    recv_ty->kind == TyKind::Path ||
                    recv_ty->kind == TyKind::Popen ||
                    recv_ty->kind == TyKind::Deque)) {
        if (opt) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
        // Path.mkdir / Popen.wait / Popen.communicate 收关键字参数，下面单独处理。
        bool kw_ok = (recv_ty->kind == TyKind::Path && at.attr == "mkdir") ||
                     (recv_ty->kind == TyKind::Popen &&
                      (at.attr == "wait" || at.attr == "communicate"));
        if (!c.keywords.empty() && !kw_ok)
            err(c.line, c.col, std::format("{}() does not take keyword arguments", at.attr));
        auto count = [&](size_t n) {
            if (c.args.size() == n) return true;
            err(c.line, c.col,
                std::format("{}() takes exactly {} argument{}", at.attr, n, n == 1 ? "" : "s"));
            return false;
        };
        if (recv_ty->kind == TyKind::AsyncEvent) {
            if (at.attr == "wait") {
                if (!count(0)) return const_none();
                VReg f = call_runtime(RtFn::EventWait, mk_future(t_bool()), {recv});
                push_temp(f);
                return f;
            }
            if (at.attr == "set") {
                if (!count(0)) return const_none();
                call_void(RtFn::EventSet, {recv});
                return const_none();
            }
            if (at.attr == "is_set") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::EventIsSet, t_bool(), {recv});
            }
        }
        if (recv_ty->kind == TyKind::AsyncLock) {
            if (at.attr == "acquire") {
                if (!count(0)) return const_none();
                VReg f = call_runtime(RtFn::LockAcquire, mk_future(t_bool()), {recv});
                push_temp(f);
                return f;
            }
            if (at.attr == "release") {
                if (!count(0)) return const_none();
                call_void(RtFn::LockRelease, {recv});
                return const_none();
            }
            if (at.attr == "locked") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::LockLocked, t_bool(), {recv});
            }
        }
        if (recv_ty->kind == TyKind::AsyncQueue) {
            if (at.attr == "put") {
                if (!count(1)) return const_none();
                TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
                VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
                // put 可能阻塞（满队列），运行时返回 Future；lowering 直接
                // 把它当可等待值返回，await 与忽略都行。
                VReg f = call_runtime(RtFn::QueuePut, mk_future(t_none()), {recv, v});
                push_temp(f);
                return f;
            }
            if (at.attr == "get") {
                if (!count(0)) return const_none();
                TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
                VReg f = call_runtime(RtFn::QueueGet, mk_future(et), {recv});
                push_temp(f);
                return f;
            }
            if (at.attr == "qsize") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::QueueQsize, t_int(), {recv});
            }
            if (at.attr == "empty") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::QueueEmpty, t_bool(), {recv});
            }
            if (at.attr == "full") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::QueueFull, t_bool(), {recv});
            }
            if (at.attr == "join") {
                if (!count(0)) return const_none();
                VReg f = call_runtime(RtFn::QueueJoin, mk_future(t_none()), {recv});
                push_temp(f);
                return f;
            }
            if (at.attr == "task_done") {
                if (!count(0)) return const_none();
                call_void(RtFn::QueueTaskDone, {recv});
                return const_none();
            }
            if (at.attr == "put_nowait") {
                if (!count(1)) return const_none();
                TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
                VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
                call_void(RtFn::QueuePutNowait, {recv, v});
                return const_none();
            }
            if (at.attr == "get_nowait") {
                if (!count(0)) return const_none();
                TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
                VReg v = call_runtime(RtFn::QueueGetNowait, t_unknown(), {recv});
                return retype(v, et);
            }
        }
        if (recv_ty->kind == TyKind::DateTime) {
            if (at.attr == "isoformat") {
                if (!count(0)) return const_none();
                VReg s = call_runtime(RtFn::DateTimeIsoformat, t_str(), {recv});
                push_temp(s);
                return s;
            }
            if (at.attr == "strftime") {
                if (!count(1)) return const_none();
                VReg f = expr(*c.args[0]);
                f = coerce(f, tc_.type_of(c.args[0].get()), t_str());
                VReg s = call_runtime(RtFn::DateTimeStrftime, t_str(), {recv, f});
                push_temp(s);
                return s;
            }
            if (at.attr == "weekday") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::DateTimeField, t_int(), {recv, const_int(6)});
            }
            err(c.line, c.col, std::format("datetime has no method '{}'", at.attr));
            return const_none();
        }
        if (recv_ty->kind == TyKind::TimeDelta) {
            if (at.attr == "total_seconds") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::TimeDeltaTotalSeconds, t_float(), {recv});
            }
            err(c.line, c.col, std::format("timedelta has no method '{}'", at.attr));
            return const_none();
        }
        if (recv_ty->kind == TyKind::Path) {
            if (at.attr == "exists" || at.attr == "is_file" || at.attr == "is_dir") {
                if (!count(0)) return const_none();
                RtFn fn = at.attr == "exists"    ? RtFn::PathExists
                          : at.attr == "is_file" ? RtFn::PathIsFile
                                                 : RtFn::PathIsDir;
                return call_runtime(fn, t_bool(), {recv});
            }
            if (at.attr == "read_text") {
                if (!count(0)) return const_none();
                VReg s = call_runtime(RtFn::PathReadText, t_str(), {recv});
                push_temp(s);
                return s;
            }
            if (at.attr == "write_text") {
                if (!count(1)) return const_none();
                VReg s = expr(*c.args[0]);
                s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
                return call_runtime(RtFn::PathWriteText, t_int(), {recv, s});
            }
            if (at.attr == "iterdir") {
                if (!count(0)) return const_none();
                TypePtr rt = tc_.type_of(&c);
                if (!rt || rt->kind != TyKind::List) rt = mk_list(new_ty(TyKind::Path));
                VReg v = call_runtime(RtFn::PathIterdir, rt, {recv});
                push_temp(v);
                return v;
            }
            if (at.attr == "glob") {
                if (!count(1)) return const_none();
                VReg pat = expr(*c.args[0]);
                pat = coerce(pat, tc_.type_of(c.args[0].get()), t_str());
                TypePtr rt = tc_.type_of(&c);
                if (!rt || rt->kind != TyKind::List) rt = mk_list(new_ty(TyKind::Path));
                VReg v = call_runtime(RtFn::PathGlob, rt, {recv, pat});
                push_temp(v);
                return v;
            }
            if (at.attr == "with_suffix") {
                if (!count(1)) return const_none();
                VReg suf = expr(*c.args[0]);
                suf = coerce(suf, tc_.type_of(c.args[0].get()), t_str());
                VReg v = call_runtime(RtFn::PathWithSuffix, new_ty(TyKind::Path),
                                      {recv, suf});
                push_temp(v);
                return v;
            }
            if (at.attr == "unlink") {
                if (!count(0)) return const_none();
                VReg s = call_runtime(RtFn::PathStr, t_str(), {recv});
                push_temp(s);
                call_void(RtFn::OsRemove, {s});
                return const_none();
            }
            if (at.attr == "rmdir") {
                if (!count(0)) return const_none();
                VReg s = call_runtime(RtFn::PathStr, t_str(), {recv});
                push_temp(s);
                call_void(RtFn::OsRmdir, {s});
                return const_none();
            }
            if (at.attr == "mkdir") {
                if (!c.args.empty()) {
                    err(c.line, c.col, "pathlib.Path.mkdir() takes only keyword arguments");
                    return const_none();
                }
                VReg mode = const_int(0777);
                VReg parents = const_int(0);
                VReg exist_ok = const_int(0);
                for (auto& kw : c.keywords) {
                    if (kw.arg == "mode") {
                        mode = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
                    } else if (kw.arg == "parents") {
                        parents = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
                    } else if (kw.arg == "exist_ok") {
                        exist_ok = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("pathlib.Path.mkdir() got an unexpected keyword "
                                        "argument '{}'",
                                        kw.arg));
                    }
                }
                call_void(RtFn::PathMkdir, {recv, mode, parents, exist_ok});
                return const_none();
            }
            err(c.line, c.col, std::format("pathlib.Path has no method '{}'", at.attr));
            return const_none();
        }
        if (recv_ty->kind == TyKind::Popen) {
            if (at.attr == "wait") {
                if (!c.args.empty()) {
                    err(c.line, c.col, "Popen.wait() takes only the timeout keyword");
                    return const_none();
                }
                VReg to = const_float(-1.0);
                for (auto& kw : c.keywords) {
                    if (kw.arg == "timeout") {
                        to = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_float());
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("Popen.wait() got an unexpected keyword argument '{}'",
                                        kw.arg));
                    }
                }
                return call_runtime(RtFn::PopenWait, t_int(), {recv, to});
            }
            if (at.attr == "communicate") {
                if (!c.args.empty()) {
                    err(c.line, c.col, "Popen.communicate() takes only the input keyword");
                    return const_none();
                }
                VReg in = const_str("");
                for (auto& kw : c.keywords) {
                    if (kw.arg == "input") {
                        in = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_str());
                    } else {
                        err(kw.value->line, kw.value->col,
                            std::format("Popen.communicate() got an unexpected keyword "
                                        "argument '{}'",
                                        kw.arg));
                    }
                }
                TypePtr rt = tc_.type_of(&c);
                if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({t_str(), t_str()});
                VReg v = call_runtime(RtFn::PopenCommunicate, rt, {recv, in});
                push_temp(v);
                return v;
            }
            if (at.attr == "kill" || at.attr == "terminate") {
                if (!count(0)) return const_none();
                call_void(at.attr == "kill" ? RtFn::PopenKill : RtFn::PopenTerminate, {recv});
                return const_none();
            }
            if (at.attr == "send_signal") {
                if (!count(1)) return const_none();
                VReg s = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
                call_void(RtFn::PopenSendSignal, {recv, s});
                return const_none();
            }
            err(c.line, c.col, std::format("Popen has no method '{}'", at.attr));
            return const_none();
        }
        if (recv_ty->kind == TyKind::Deque) {
            TypePtr et = t_int();
            if (at.attr == "append" || at.attr == "appendleft") {
                if (!count(1)) return const_none();
                VReg v = expr(*c.args[0]);
                call_void(at.attr == "append" ? RtFn::DequeAppend : RtFn::DequeAppendLeft,
                          {recv, v});
                return const_none();
            }
            if (at.attr == "pop" || at.attr == "popleft") {
                if (!count(0)) return const_none();
                TypePtr rt = tc_.type_of(&c);
                if (!rt || rt->kind == TyKind::Unknown) rt = et;
                VReg v = call_runtime(
                    at.attr == "pop" ? RtFn::DequePop : RtFn::DequePopleft, rt, {recv});
                if (ir_is_heap(rt)) push_temp(v);
                return v;
            }
            if (at.attr == "rotate") {
                if (!count(1)) return const_none();
                VReg n = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
                call_void(RtFn::DequeRotate, {recv, n});
                return const_none();
            }
            if (at.attr == "clear") {
                if (!count(0)) return const_none();
                call_void(RtFn::DequeClear, {recv});
                return const_none();
            }
            if (at.attr == "extend" || at.attr == "extendleft") {
                if (!count(1)) return const_none();
                VReg xs = expr(*c.args[0]);
                call_void(at.attr == "extend" ? RtFn::DequeExtend : RtFn::DequeExtendLeft,
                          {recv, xs});
                return const_none();
            }
            err(c.line, c.col, std::format("collections.deque has no method '{}'", at.attr));
            return const_none();
        }
        if (recv_ty->kind == TyKind::AsyncTimeout) {
            if (at.attr == "expired") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::TimeoutExpired, t_bool(), {recv});
            }
        }
        if (recv_ty->kind == TyKind::AsyncSemaphore) {
            if (at.attr == "acquire") {
                if (!count(0)) return const_none();
                VReg f = call_runtime(RtFn::SemAcquire, mk_future(t_bool()), {recv});
                push_temp(f);
                return f;
            }
            if (at.attr == "release") {
                if (!count(0)) return const_none();
                call_void(RtFn::SemRelease, {recv});
                return const_none();
            }
            if (at.attr == "locked") {
                if (!count(0)) return const_none();
                return call_runtime(RtFn::SemLocked, t_bool(), {recv});
            }
        }
        if (recv_ty->kind == TyKind::AsyncTaskGroup) {
            if (at.attr == "create_task") {
                if (!count(1)) return const_none();
                VReg coro = expr(*c.args[0]);
                VReg t = call_runtime(RtFn::TaskGroupAdd, mk_task(t_unknown()),
                                      {recv, coro});
                push_temp(t);
                return t;
            }
        }
        err(c.line, c.col,
            std::format("'{}' has no method '{}'", type_name(recv_ty), at.attr));
        return const_none();
    }

    // 内置类型的方法表是静态的，Optional 上没有；走到这里说明没做
    // None 检查。报错比让 pa_list_append 拿到 0 更可查。
    if (opt) {
        err(c.line, c.col,
            std::format("method '{}' called on a maybe-None value; add a None check first",
                        at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Generator) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "__next__") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.__next__() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenNext);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "send") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "generator.send() takes exactly one argument");
                return const_none();
            }
            VReg sv = expr(*c.args[0]);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenSend);
            in.args = {recv, sv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "throw") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "generator.throw() takes exactly one argument");
                return const_none();
            }
            VReg ev = kNoReg;
            std::string cn;
            if (exc_class_name(*c.args[0], cn)) {
                // throw(ValueError) 等价于 throw(ValueError())。
                Inst ctor;
                ctor.op = IrOp::CallRt;
                ctor.imm = static_cast<int64_t>(RtFn::ExcValueNew);
                ctor.args = {const_str(cn), const_int(0)};
                ev = def_heap(mk_exception(cn), std::move(ctor));
            } else {
                ev = expr(*c.args[0]);
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenThrow);
            in.args = {recv, ev};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "close") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.close() takes no arguments");
            call_void(RtFn::GenClose, {recv});
            return const_none();
        }
        err(c.line, c.col, std::format("'generator' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::AsyncGenerator) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "__anext__") {
            if (!c.args.empty())
                err(c.line, c.col, "async generator.__anext__() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
            in.args = {recv};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "asend") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "async generator.asend() takes exactly one argument");
                return const_none();
            }
            VReg sv = expr(*c.args[0]);
            TypePtr vt = vreg_ty(sv);
            if (ir_is_heap(vt)) rc_inc(sv);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(sv);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAsend);
            in.args = {recv, sv};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "athrow") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "async generator.athrow() takes exactly one argument");
                return const_none();
            }
            VReg ev = kNoReg;
            std::string cn;
            if (exc_class_name(*c.args[0], cn)) {
                // athrow(ValueError) 等价于 athrow(ValueError())。
                Inst ctor;
                ctor.op = IrOp::CallRt;
                ctor.imm = static_cast<int64_t>(RtFn::ExcValueNew);
                ctor.args = {const_str(cn), const_int(0)};
                ev = def_heap(mk_exception(cn), std::move(ctor));
            } else {
                ev = expr(*c.args[0]);
            }
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAthrow);
            in.args = {recv, ev};
            return def_heap(mk_coroutine(et), std::move(in));
        }
        if (at.attr == "aclose") {
            if (!c.args.empty())
                err(c.line, c.col, "async generator.aclose() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncGenAClose);
            in.args = {recv};
            return def_heap(mk_coroutine(t_none()), std::move(in));
        }
        err(c.line, c.col,
            std::format("'async generator' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Task) {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("task.{}() takes no arguments", at.attr));
        }
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            RtFn fn = at.attr == "done"       ? RtFn::AsyncioTaskDone
                      : at.attr == "cancel"   ? RtFn::AsyncioTaskCancel
                                              : RtFn::AsyncioTaskCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "result") {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncioTaskResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            VReg v = call_runtime(RtFn::AsyncioTaskException,
                                  mk_optional(mk_exception("Exception")), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'task' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Future) {
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            if (!c.args.empty())
                err(c.line, c.col, std::format("future.{}() takes no arguments", at.attr));
            RtFn fn = at.attr == "done"       ? RtFn::FutureDone
                      : at.attr == "cancel"   ? RtFn::FutureCancel
                                              : RtFn::FutureCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "set_result") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.set_result() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            TypePtr vt = vreg_ty(v);
            if (ir_is_heap(vt)) rc_inc(v);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
            call_void(RtFn::FutureSetResult, {recv, v});
            return const_none();
        }
        if (at.attr == "set_exception") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.set_exception() takes exactly one argument");
                return const_none();
            }
            VReg msg = expr(*c.args[0]);
            TypePtr mt = vreg_ty(msg);
            if (mt && mt->kind == TyKind::Exception)
                call_void(RtFn::FutureSetExceptionValue, {recv, msg});
            else
                call_void(RtFn::FutureSetException, {recv, msg});
            return const_none();
        }
        if (at.attr == "add_done_callback") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.add_done_callback() takes exactly one argument");
                return const_none();
            }
            VReg cb = expr(*c.args[0]);
            TypePtr ct = vreg_ty(cb);
            if (ir_is_heap(ct)) rc_inc(cb);
            else if (!ct || ct->kind == TyKind::Unknown) rc_inc_unknown(cb);
            call_void(RtFn::FutureAddCallback, {recv, cb});
            return const_none();
        }
        if (at.attr == "result") {
            if (!c.args.empty()) err(c.line, c.col, "future.result() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::FutureResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            if (!c.args.empty()) err(c.line, c.col, "future.exception() takes no arguments");
            VReg v = call_runtime(RtFn::FutureException,
                                  mk_optional(mk_exception("Exception")), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'future' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Dict) {
        TypePtr kt = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        TypePtr vt = recv_ty->sub.size() > 1 ? recv_ty->sub[1] : t_unknown();
        if (at.attr == "get") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "dict.get() does not take keyword arguments");
                return const_none();
            }
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.get() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            if (c.args.size() == 1)
                // 缺失时返回 None（pyadamas 里 None 就是 0）
                return call_runtime(RtFn::DictGetOpt, mk_optional(vt), {recv, key});
            VReg def = expr(*c.args[1]);
            def = coerce(def, tc_.type_of(c.args[1].get()), vt);
            return call_runtime(RtFn::DictGetOr, vt, {recv, key, def});
        }
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("dict.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "most_common") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "dict.most_common() takes at most one argument");
                return const_none();
            }
            VReg n = const_int(-1);
            if (c.args.size() == 1)
                n = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            TypePtr rt = mk_list(mk_tuple({kt, t_int()}));
            VReg v = call_runtime(RtFn::CollMostCommon, rt, {recv, n});
            push_temp(v);
            return v;
        }
        if (at.attr == "keys" || at.attr == "values" || at.attr == "items") {
            if (!c.args.empty()) {
                err(c.line, c.col, std::format("dict.{}() takes no arguments", at.attr));
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::List) {
                rt = at.attr == "keys" ? mk_list(kt)
                     : at.attr == "values" ? mk_list(vt)
                                           : mk_list(mk_tuple({kt, vt}));
            }
            RtFn fn = at.attr == "keys" ? RtFn::DictKeys
                      : at.attr == "values" ? RtFn::DictValues
                                            : RtFn::DictItems;
            VReg v = call_runtime(fn, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "dict.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Dict && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col,
                    "dict.update() expects a dict argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "pop") {
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.pop() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            VReg has = const_int(0);
            VReg def = const_int(0);
            if (c.args.size() == 2) {
                has = const_int(1);
                def = expr(*c.args[1]);
                def = coerce(def, tc_.type_of(c.args[1].get()), vt);
                // 运行时把默认值原样返回；补一份引用，让返回值统一按
                // 「新引用」进临时栈。
                retain_elem(def, own_kind(vt));
            }
            VReg v = call_runtime(RtFn::DictPop, vt, {recv, key, has, def});
            if (ir_is_heap(vt)) push_temp(v);
            return v;
        }
        if (at.attr == "popitem") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.popitem() takes no arguments");
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({kt, vt});
            VReg v = call_runtime(RtFn::DictPopItem, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        if (at.attr == "setdefault") {
            if (c.args.size() != 2) {
                err(c.line, c.col, "dict.setdefault() takes exactly two arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            VReg def = expr(*c.args[1]);
            def = coerce(def, tc_.type_of(c.args[1].get()), vt);
            int add_b = new_block("dict.setdefault.add");
            int done_b = new_block("dict.setdefault.done");
            VReg has = call_runtime(RtFn::DictContains, t_bool(), {recv, key});
            Inst br;
            br.op = IrOp::Branch;
            br.a = has;
            br.target = done_b;
            br.target2 = add_b;
            emit(std::move(br));
            switch_to(add_b);
            retain_elem(key, own_kind(kt));
            retain_elem(def, own_kind(vt));
            call_void(RtFn::DictSet, {recv, key, def});
            Inst j;
            j.op = IrOp::Jump;
            j.target = done_b;
            emit(std::move(j));
            switch_to(done_b);
            VReg v = call_runtime(RtFn::DictGet, vt, {recv, key});
            if (ir_is_heap(vt)) push_temp(v);
            return v;
        }
        if (at.attr == "copy") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.copy() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::DictCopy, recv_ty, {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col,
            std::format("dict method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Set) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("set.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "add") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.add() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            retain_elem(v, own_kind(et));
            call_void(RtFn::DictSet, {recv, v, const_int(0)});
            return const_none();
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Set && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col, "set.update() expects a set argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "remove" || at.attr == "discard") {
            if (c.args.size() != 1) {
                err(c.line, c.col,
                    std::format("set.{}() takes exactly one argument", at.attr));
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            call_void(RtFn::DictRemove,
                      {recv, v, const_int(at.attr == "discard" ? 1 : 0)});
            return const_none();
        }
        if (at.attr == "pop") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.pop() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::DictPopLast, et, {recv});
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        if (at.attr == "copy") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.copy() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::DictCopy, recv_ty, {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col,
            std::format("set method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Random) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("random.Random.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        auto int_at = [&](size_t i) -> VReg {
            return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
        };
        auto float_at = [&](size_t i) -> VReg {
            return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_float());
        };
        if (at.attr == "random") {
            if (!c.args.empty()) {
                err(c.line, c.col, "random() takes no arguments");
                return const_none();
            }
            return call_runtime(RtFn::RandNext, t_float(), {recv});
        }
        if (at.attr == "seed") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "seed() takes at most one argument");
                return const_none();
            }
            VReg seed = const_int(-9223372036854775807LL - 1);
            if (!c.args.empty()) seed = int_at(0);
            call_void(RtFn::RandSeed, {recv, seed});
            return const_none();
        }
        if (at.attr == "randint") {
            if (c.args.size() != 2) {
                err(c.line, c.col, "randint() takes exactly two arguments");
                return const_none();
            }
            VReg a = int_at(0);
            VReg b = int_at(1);
            return call_runtime(RtFn::RandRandint, t_int(), {recv, a, b});
        }
        if (at.attr == "uniform") {
            if (c.args.size() != 2) {
                err(c.line, c.col, "uniform() takes exactly two arguments");
                return const_none();
            }
            VReg a = float_at(0);
            VReg b = float_at(1);
            return call_runtime(RtFn::RandUniform, t_float(), {recv, a, b});
        }
        if (at.attr == "choice") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "choice() takes exactly one argument");
                return const_none();
            }
            TypePtr st = tc_.type_of(c.args[0].get());
            VReg seq = expr(*c.args[0]);
            TypePtr elem = tc_.type_of(&c);
            if (st && st->kind == TyKind::Str) {
                seq = call_runtime(RtFn::StrToList, mk_list(t_str()), {seq});
                push_temp(seq);
                elem = t_str();
            }
            VReg v = call_runtime(RtFn::RandChoice, elem ? elem : t_unknown(), {recv, seq});
            if (ir_is_heap(elem)) {
                rc_inc(v);
                push_temp(v);
            }
            return v;
        }
        if (at.attr == "shuffle") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "shuffle() takes exactly one argument");
                return const_none();
            }
            VReg l = expr(*c.args[0]);
            call_void(RtFn::RandShuffle, {recv, l});
            return const_none();
        }
        err(c.line, c.col,
            std::format("random.Random method '{}' is not supported", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Regex) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("re.Pattern.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        auto int_at = [&](size_t i) -> VReg {
            return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
        };
        if (at.attr == "search" || at.attr == "match" || at.attr == "fullmatch") {
            if (c.args.size() != 1) {
                err(c.line, c.col,
                    std::format("{}() takes exactly one argument", at.attr));
                return const_none();
            }
            int64_t mode = at.attr == "search" ? 0 : at.attr == "match" ? 1 : 2;
            VReg text = expr(*c.args[0]);
            TypePtr mt = mk_optional(t_match());
            VReg m = call_runtime(RtFn::RegexSearch, mt, {recv, text, const_int(mode)});
            if (ir_is_heap(mt)) push_temp(m);
            return m;
        }
        if (at.attr == "findall") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "findall() takes exactly one argument");
                return const_none();
            }
            VReg text = expr(*c.args[0]);
            VReg l = call_runtime(RtFn::RegexFindAll, mk_list(t_str()), {recv, text});
            push_temp(l);
            return l;
        }
        if (at.attr == "finditer") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "finditer() takes exactly one argument");
                return const_none();
            }
            VReg text = expr(*c.args[0]);
            VReg l = call_runtime(RtFn::RegexFindIter, mk_list(t_match()), {recv, text});
            push_temp(l);
            return l;
        }
        if (at.attr == "sub") {
            if (c.args.size() < 2 || c.args.size() > 3) {
                err(c.line, c.col, "sub() takes two or three arguments");
                return const_none();
            }
            TypePtr rty0 = tc_.type_of(c.args[0].get());
            VReg text = expr(*c.args[1]);
            VReg count = c.args.size() == 3 ? int_at(2) : const_int(0);
            VReg s;
            if (rty0 && rty0->kind == TyKind::Func) {
                // repl 是函数：运行时每匹配一次调一次闭包。
                VReg fnv = expr(*c.args[0]);
                s = call_runtime(RtFn::RegexSubFn, t_str(), {recv, fnv, text, count});
            } else {
                VReg repl = expr(*c.args[0]);
                s = call_runtime(RtFn::RegexSub, t_str(), {recv, repl, text, count});
            }
            push_temp(s);
            return s;
        }
        if (at.attr == "split") {
            if (c.args.size() < 1 || c.args.size() > 2) {
                err(c.line, c.col, "split() takes one or two arguments");
                return const_none();
            }
            VReg text = expr(*c.args[0]);
            VReg maxsplit = c.args.size() == 2 ? int_at(1) : const_int(0);
            VReg l = call_runtime(RtFn::RegexSplit, mk_list(t_str()),
                                  {recv, text, maxsplit});
            push_temp(l);
            return l;
        }
        err(c.line, c.col,
            std::format("re.Pattern method '{}' is not supported", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Match) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("re.Match.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        auto int_at = [&](size_t i) -> VReg {
            return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
        };
        if (at.attr == "group") {
            if (c.args.size() > 1) {
                // group(1, 2, ...) 返回元组；先按 list 建，再改标成 tuple。
                VReg l = call_runtime(RtFn::ListNew, mk_list(t_str()),
                                      {const_int(static_cast<int64_t>(c.args.size())),
                                       const_int(1)});
                push_temp(l);
                for (size_t i = 0; i < c.args.size(); ++i) {
                    TypePtr a0 = tc_.type_of(c.args[i].get());
                    VReg s;
                    if (a0 && a0->kind == TyKind::Str) {
                        VReg sv = expr(*c.args[i]);
                        s = call_runtime(RtFn::RegexGroupStr, t_str(), {recv, sv});
                    } else {
                        VReg n = int_at(i);
                        s = call_runtime(RtFn::RegexGroup, t_str(), {recv, n});
                    }
                    call_void(RtFn::ListAppend, {l, s});
                }
                return retype(l, mk_tuple_rest(t_str()));
            }
            if (c.args.empty()) {
                VReg s = call_runtime(RtFn::RegexGroup, t_str(), {recv, const_int(0)});
                push_temp(s);
                return s;
            }
            TypePtr a0 = tc_.type_of(c.args[0].get());
            if (a0 && a0->kind == TyKind::Str) {
                VReg sv = expr(*c.args[0]);
                VReg s = call_runtime(RtFn::RegexGroupStr, t_str(), {recv, sv});
                push_temp(s);
                return s;
            }
            VReg n = int_at(0);
            VReg s = call_runtime(RtFn::RegexGroup, t_str(), {recv, n});
            push_temp(s);
            return s;
        }
        if (at.attr == "expand") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "expand() takes exactly one argument");
                return const_none();
            }
            VReg t = expr(*c.args[0]);
            VReg s = call_runtime(RtFn::RegexExpand, t_str(), {recv, t});
            push_temp(s);
            return s;
        }
        if (at.attr == "groups") {
            if (!c.args.empty()) {
                err(c.line, c.col, "groups() takes no arguments");
                return const_none();
            }
            VReg l = call_runtime(RtFn::RegexGroups, mk_list(t_str()), {recv});
            push_temp(l);
            return l;
        }
        if (at.attr == "groupdict") {
            if (!c.args.empty()) {
                err(c.line, c.col, "groupdict() takes no arguments");
                return const_none();
            }
            VReg d = call_runtime(RtFn::RegexGroupDict, mk_dict(t_str(), t_str()), {recv});
            push_temp(d);
            return d;
        }
        if (at.attr == "start" || at.attr == "end") {
            if (c.args.size() > 1) {
                err(c.line, c.col,
                    std::format("{}() takes at most one argument", at.attr));
                return const_none();
            }
            VReg n = c.args.empty() ? const_int(0) : int_at(0);
            return call_runtime(at.attr == "start" ? RtFn::RegexStart : RtFn::RegexEnd,
                                t_int(), {recv, n});
        }
        if (at.attr == "span") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "span() takes at most one argument");
                return const_none();
            }
            VReg n = c.args.empty() ? const_int(0) : int_at(0);
            VReg t = call_runtime(RtFn::RegexSpan, mk_tuple({t_int(), t_int()}),
                                  {recv, n});
            push_temp(t);
            return t;
        }
        err(c.line, c.col, std::format("re.Match method '{}' is not supported", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::File) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("file.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        auto int_arg = [&](size_t i, int64_t def) -> VReg {
            if (i >= c.args.size()) return const_int(def);
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        if (at.attr == "read") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "file.read() takes at most one argument");
                return const_none();
            }
            VReg s = call_runtime(RtFn::FileRead, t_str(), {recv, int_arg(0, -1)});
            push_temp(s);
            return s;
        }
        if (at.attr == "readline") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "file.readline() takes at most one argument");
                return const_none();
            }
            VReg s = call_runtime(RtFn::FileReadLine, t_str(), {recv, int_arg(0, -1)});
            push_temp(s);
            return s;
        }
        if (at.attr == "readlines") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "file.readlines() takes at most one argument");
                return const_none();
            }
            VReg l = call_runtime(RtFn::FileReadLines, mk_list(t_str()),
                                  {recv, int_arg(0, -1)});
            push_temp(l);
            return l;
        }
        if (at.attr == "write") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "file.write() takes exactly one argument");
                return const_none();
            }
            TypePtr wt = tc_.type_of(c.args[0].get());
            VReg v = expr(*c.args[0]);
            if (wt && wt->kind != TyKind::Str && wt->kind != TyKind::Unknown) {
                err(c.args[0]->line, c.args[0]->col, "file.write() expects a string");
                return const_int(0);
            }
            return call_runtime(RtFn::FileWrite, t_int(), {recv, v});
        }
        if (at.attr == "writelines") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "file.writelines() takes exactly one argument");
                return const_none();
            }
            TypePtr lt = tc_.type_of(c.args[0].get());
            if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
                err(c.args[0]->line, c.args[0]->col,
                    "file.writelines() expects a list of strings");
                return const_none();
            }
            VReg l = expr(*c.args[0]);
            call_void(RtFn::FileWritelines, {recv, l});
            return const_none();
        }
        if (at.attr == "seek") {
            if (c.args.empty() || c.args.size() > 2) {
                err(c.line, c.col, "file.seek() takes 1 or 2 arguments");
                return const_none();
            }
            VReg off = int_arg(0, 0);
            VReg wh = int_arg(1, 0);
            return call_runtime(RtFn::FileSeek, t_int(), {recv, off, wh});
        }
        if (at.attr == "tell" || at.attr == "fileno") {
            if (!c.args.empty()) {
                err(c.line, c.col, std::format("file.{}() takes no arguments", at.attr));
                return const_none();
            }
            return call_runtime(at.attr == "tell" ? RtFn::FileTell : RtFn::FileFileno,
                                t_int(), {recv});
        }
        if (at.attr == "truncate") {
            if (c.args.size() > 1) {
                err(c.line, c.col, "file.truncate() takes at most one argument");
                return const_none();
            }
            return call_runtime(RtFn::FileTruncate, t_int(), {recv, int_arg(0, -1)});
        }
        if (at.attr == "isatty" || at.attr == "readable" || at.attr == "writable" ||
            at.attr == "seekable") {
            if (!c.args.empty()) {
                err(c.line, c.col, std::format("file.{}() takes no arguments", at.attr));
                return const_none();
            }
            RtFn fn = at.attr == "isatty"     ? RtFn::FileIsatty
                      : at.attr == "readable" ? RtFn::FileReadable
                      : at.attr == "writable" ? RtFn::FileWritable
                                              : RtFn::FileSeekable;
            return call_runtime(fn, t_bool(), {recv});
        }
        if (at.attr == "close") {
            if (!c.args.empty()) {
                err(c.line, c.col, "file.close() takes no arguments");
                return const_none();
            }
            call_void(RtFn::FileClose, {recv});
            return const_none();
        }
        if (at.attr == "flush") {
            if (!c.args.empty()) {
                err(c.line, c.col, "file.flush() takes no arguments");
                return const_none();
            }
            call_void(RtFn::FileFlush, {recv});
            return const_none();
        }
        err(c.line, c.col,
            std::format("file method '{}' is not supported (use 'with open(...)')", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Bytes) {
        if (at.attr == "decode") {
            // bytes 和 str 同一套运行时表示，解码就是改标类型；
            // 编码参数（utf-8 之类）在按字节的世界里没有意义，忽略。
            for (auto& a : c.args) expr(*a);
            return recv;
        }
        // 其余方法在字节层面和 str 完全一致，借道 StrMethod；区别只在结果标成 bytes。
        // 注意：操作码要按方法名选，不能写死——写死成 0（= Upper）会让所有方法都变 upper。
        auto need_bytes = [&](size_t i) -> VReg {
            VReg v = expr(*c.args[i]);
            TypePtr t = tc_.type_of(c.args[i].get());
            if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Bytes))
                err(c.args[i]->line, c.args[i]->col,
                    std::format("bytes.{}() expects a string argument", at.attr));
            return v;
        };
        auto need_int = [&](size_t i, int64_t dv) -> VReg {
            if (i >= c.args.size()) return const_int(dv);
            return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
        };
        auto arg_count = [&](size_t lo, size_t hi) {
            if (c.args.size() >= lo && c.args.size() <= hi) return true;
            err(c.line, c.col,
                std::format("bytes.{}() takes {} argument(s)", at.attr,
                            lo == hi ? std::to_string(lo)
                                     : std::format("{}..{}", lo, hi)));
            return false;
        };
        auto emit = [&](StrOp op, TypePtr ret, VReg a0, VReg a1, VReg a2) -> VReg {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::StrMethod);
            in.args = {recv, const_int(static_cast<int64_t>(op)),
                       a0 == kNoReg ? const_int(0) : a0,
                       a1 == kNoReg ? const_int(0) : a1,
                       a2 == kNoReg ? const_int(0) : a2};
            in.ty = ret;
            VReg v = def(ret, std::move(in));
            if (ir_is_heap(ret)) push_temp(v);
            return v;
        };
        if (at.attr == "upper" || at.attr == "lower") {
            if (!arg_count(0, 0)) return const_none();
            return emit(at.attr == "upper" ? StrOp::Upper : StrOp::Lower, t_bytes(),
                        kNoReg, kNoReg, kNoReg);
        }
        if (at.attr == "strip" || at.attr == "lstrip" || at.attr == "rstrip") {
            if (!arg_count(0, 1)) return const_none();
            bool has_chars = c.args.size() == 1;
            StrOp op;
            if (at.attr == "strip") op = has_chars ? StrOp::StripChars : StrOp::Strip;
            else if (at.attr == "lstrip") op = has_chars ? StrOp::LStripChars : StrOp::LStrip;
            else op = has_chars ? StrOp::RStripChars : StrOp::RStrip;
            return emit(op, t_bytes(), has_chars ? need_bytes(0) : kNoReg, kNoReg, kNoReg);
        }
        if (at.attr == "replace") {
            if (!arg_count(2, 3)) return const_none();
            VReg old_s = need_bytes(0);
            VReg nw = need_bytes(1);
            VReg cnt = need_int(2, -1);
            return emit(StrOp::Replace, t_bytes(), old_s, nw, cnt);
        }
        if (at.attr == "split") {
            if (!arg_count(0, 2)) return const_none();
            TypePtr lt = mk_list(t_bytes());
            if (c.args.empty())
                return emit(StrOp::SplitWs, lt, kNoReg, kNoReg, kNoReg);
            VReg sep = need_bytes(0);
            VReg maxsplit = need_int(1, -1);
            return emit(StrOp::SplitSep, lt, sep, maxsplit, kNoReg);
        }
        if (at.attr == "find" || at.attr == "rfind") {
            if (!arg_count(1, 3)) return const_none();
            VReg sub = need_bytes(0);
            VReg start = need_int(1, 0);
            VReg end = need_int(2, std::numeric_limits<int64_t>::max());
            return emit(at.attr == "find" ? StrOp::Find : StrOp::RFind, t_int(),
                        sub, start, end);
        }
        if (at.attr == "count") {
            if (!arg_count(1, 1)) return const_none();
            VReg sub = need_bytes(0);
            return emit(StrOp::Count, t_int(), sub, kNoReg, kNoReg);
        }
        if (at.attr == "startswith" || at.attr == "endswith") {
            if (!arg_count(1, 1)) return const_none();
            VReg prefix = need_bytes(0);
            return emit(at.attr == "startswith" ? StrOp::StartsWith : StrOp::EndsWith,
                        t_bool(), prefix, kNoReg, kNoReg);
        }
        if (at.attr == "join") {
            if (!arg_count(1, 1)) return const_none();
            TypePtr lt = tc_.type_of(c.args[0].get());
            VReg parts;
            if (lt && lt->kind == TyKind::Generator) {
                // 生成器没有下标：先物化成列表再拼
                TypePtr se = t_unknown();
                parts = seq_as_list(c.args[0].get(), se);
            } else if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
                err(c.args[0]->line, c.args[0]->col,
                    "bytes.join() expects a list of bytes");
                return const_none();
            } else {
                parts = expr(*c.args[0]);
            }
            return emit(StrOp::Join, t_bytes(), parts, kNoReg, kNoReg);
        }
        err(c.line, c.col, std::format("bytes method '{}' is not supported", at.attr));
        return const_none();
    }
    if (recv_ty && recv_ty->kind == TyKind::Str) {
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("str.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        auto need_str = [&](size_t i) -> VReg {
            VReg v = expr(*c.args[i]);
            TypePtr t = tc_.type_of(c.args[i].get());
            if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Bytes))
                err(c.args[i]->line, c.args[i]->col,
                    std::format("str.{}() expects a string argument", at.attr));
            return v;
        };
        auto need_int = [&](size_t i, int64_t def) -> VReg {
            if (i >= c.args.size()) return const_int(def);
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        if (at.attr == "encode") {
            // 按字节的世界里编码就是改标类型；编码名参数忽略。
            for (auto& a : c.args) expr(*a);
            return recv;
        }
        auto arg_count = [&](size_t lo, size_t hi) {
            if (c.args.size() >= lo && c.args.size() <= hi) return true;
            err(c.line, c.col,
                std::format("str.{}() takes {} argument(s)", at.attr,
                            lo == hi ? std::to_string(lo)
                                     : std::format("{}..{}", lo, hi)));
            return false;
        };
        auto emit_op = [&](StrOp op, TypePtr ret, VReg a0, VReg a1, VReg a2) -> VReg {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::StrMethod);
            in.args = {recv, const_int(static_cast<int64_t>(op)),
                       a0 == kNoReg ? const_int(0) : a0,
                       a1 == kNoReg ? const_int(0) : a1,
                       a2 == kNoReg ? const_int(0) : a2};
            in.ty = ret;
            VReg v = def(ret, std::move(in));
            if (ir_is_heap(ret)) push_temp(v);
            return v;
        };
        TypePtr rty = tc_.type_of(&c);
        if (at.attr == "upper" || at.attr == "lower" || at.attr == "capitalize" ||
            at.attr == "title" || at.attr == "casefold") {
            if (!arg_count(0, 0)) return const_none();
            StrOp op = at.attr == "upper" ? StrOp::Upper
                       : at.attr == "lower" || at.attr == "casefold" ? StrOp::Lower
                       : at.attr == "capitalize" ? StrOp::Capitalize
                                                   : StrOp::Title;
            return emit_op(op, t_str(), kNoReg, kNoReg, kNoReg);
        }
        if (at.attr == "strip" || at.attr == "lstrip" || at.attr == "rstrip") {
            if (!arg_count(0, 1)) return const_none();
            bool has_chars = c.args.size() == 1;
            StrOp op;
            if (at.attr == "strip") op = has_chars ? StrOp::StripChars : StrOp::Strip;
            else if (at.attr == "lstrip") op = has_chars ? StrOp::LStripChars : StrOp::LStrip;
            else op = has_chars ? StrOp::RStripChars : StrOp::RStrip;
            VReg chars = has_chars ? need_str(0) : kNoReg;
            return emit_op(op, t_str(), chars, kNoReg, kNoReg);
        }
        if (at.attr == "split") {
            if (!arg_count(0, 2)) return const_none();
            TypePtr lt = mk_list(t_str());
            if (c.args.empty())
                return emit_op(StrOp::SplitWs, lt, kNoReg, kNoReg, kNoReg);
            VReg sep = need_str(0);
            VReg maxsplit = need_int(1, -1);
            return emit_op(StrOp::SplitSep, lt, sep, maxsplit, kNoReg);
        }
        if (at.attr == "splitlines") {
            if (!arg_count(0, 0)) return const_none();
            return emit_op(StrOp::SplitLines, mk_list(t_str()), kNoReg, kNoReg, kNoReg);
        }
        if (at.attr == "join") {
            if (!arg_count(1, 1)) return const_none();
            TypePtr lt = tc_.type_of(c.args[0].get());
            VReg parts;
            if (lt && lt->kind == TyKind::Generator) {
                // 生成器没有下标：先物化成列表再拼。
                TypePtr se = t_unknown();
                parts = seq_as_list(c.args[0].get(), se);
            } else if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
                err(c.args[0]->line, c.args[0]->col,
                    "str.join() expects a list of strings");
                return const_none();
            } else {
                parts = expr(*c.args[0]);
            }
            return emit_op(StrOp::Join, t_str(), parts, kNoReg, kNoReg);
        }
        if (at.attr == "replace") {
            if (!arg_count(2, 3)) return const_none();
            VReg old = need_str(0);
            VReg nw = need_str(1);
            VReg cnt = need_int(2, -1);
            return emit_op(StrOp::Replace, t_str(), old, nw, cnt);
        }
        if (at.attr == "find" || at.attr == "rfind" || at.attr == "index") {
            if (!arg_count(1, 3)) return const_none();
            VReg sub = need_str(0);
            VReg start = need_int(1, 0);
            VReg end = need_int(2, std::numeric_limits<int64_t>::max());
            StrOp op = at.attr == "find" ? StrOp::Find
                       : at.attr == "rfind" ? StrOp::RFind : StrOp::Find;
            VReg pos = emit_op(op, t_int(), sub, start, end);
            if (at.attr == "index") {
                int ok_b = new_block("str.index.ok");
                int bad_b = new_block("str.index.bad");
                Inst c0;
                c0.op = IrOp::Cmp;
                c0.cop = CmpKind::Ge;
                c0.a = pos;
                c0.b = const_int(0);
                VReg ok = def(t_bool(), std::move(c0));
                Inst br;
                br.op = IrOp::Branch;
                br.a = ok;
                br.target = ok_b;
                br.target2 = bad_b;
                emit(std::move(br));
                switch_to(bad_b);
                call_void(RtFn::Raise,
                          {const_str("ValueError"), const_str("substring not found")});
                Inst j;
                j.op = IrOp::Jump;
                j.target = ok_b;
                emit(std::move(j));
                switch_to(ok_b);
            }
            return pos;
        }
        if (at.attr == "startswith" || at.attr == "endswith") {
            if (!arg_count(1, 1)) return const_none();
            VReg prefix = need_str(0);
            return emit_op(at.attr == "startswith" ? StrOp::StartsWith : StrOp::EndsWith,
                           t_bool(), prefix, kNoReg, kNoReg);
        }
        if (at.attr == "count") {
            if (!arg_count(1, 1)) return const_none();
            VReg sub = need_str(0);
            return emit_op(StrOp::Count, t_int(), sub, kNoReg, kNoReg);
        }
        if (at.attr == "zfill") {
            if (!arg_count(1, 1)) return const_none();
            VReg width = need_int(0, 0);
            return emit_op(StrOp::ZFill, t_str(), width, kNoReg, kNoReg);
        }
        if (at.attr == "ljust" || at.attr == "rjust" || at.attr == "center") {
            if (!arg_count(1, 2)) return const_none();
            VReg width = need_int(0, 0);
            VReg fill = c.args.size() == 2 ? need_str(1) : kNoReg;
            StrOp op = at.attr == "ljust" ? StrOp::LJust
                       : at.attr == "rjust" ? StrOp::RJust : StrOp::Center;
            return emit_op(op, t_str(), width, fill, kNoReg);
        }
        if (at.attr == "removeprefix" || at.attr == "removesuffix") {
            if (!arg_count(1, 1)) return const_none();
            VReg affix = need_str(0);
            return emit_op(at.attr == "removeprefix" ? StrOp::RemovePrefix
                                                      : StrOp::RemoveSuffix,
                           t_str(), affix, kNoReg, kNoReg);
        }
        if (at.attr == "isdigit" || at.attr == "isalpha" || at.attr == "isspace" ||
            at.attr == "isupper" || at.attr == "islower" || at.attr == "isalnum") {
            if (!arg_count(0, 0)) return const_none();
            StrOp op = at.attr == "isdigit" ? StrOp::IsDigit
                       : at.attr == "isalpha" ? StrOp::IsAlpha
                       : at.attr == "isspace" ? StrOp::IsSpace
                       : at.attr == "isupper" ? StrOp::IsUpper
                       : at.attr == "islower" ? StrOp::IsLower : StrOp::IsAlphaNum;
            return emit_op(op, t_bool(), kNoReg, kNoReg, kNoReg);
        }
        err(c.line, c.col,
            std::format("str method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && (recv_ty->kind == TyKind::List || recv_ty->kind == TyKind::Tuple)) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        int ek = key_kind(et);        // in/index/count 的比较种类
        int lk = list_kind(recv_ty);  // 容器元素的堆种类
        bool is_list = recv_ty->kind == TyKind::List;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("{}.{}() does not take keyword arguments",
                            is_list ? "list" : "tuple", at.attr));
            return const_none();
        }
        auto elem_arg = [&](size_t i) {
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), et);
        };
        if (at.attr == "append" && is_list) {
            if (c.args.size() != 1) {
                err(c.line, c.col, "list.append() takes exactly one argument");
                return const_none();
            }
            // 运行时不再做隐式 retain：容器元素的所有权由降级器按静态类型决定。
            VReg v = expr(*c.args[0]);
            retain_elem(v, lk);
            call_void(RtFn::ListAppend, {recv, v});
            return const_none();
        }
        if (at.attr == "extend" && is_list) {
            if (c.args.size() != 1) {
                err(c.line, c.col, "list.extend() takes exactly one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple)) {
                err(c.args[0]->line, c.args[0]->col, "list.extend() expects a list");
                return const_none();
            }
            call_void(RtFn::ListExtend, {recv, other});
            return const_none();
        }
        if (at.attr == "insert" && is_list) {
            if (c.args.size() != 2) {
                err(c.line, c.col, "list.insert() takes exactly two arguments");
                return const_none();
            }
            VReg idx = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            VReg v = elem_arg(1);
            retain_elem(v, lk);
            call_void(RtFn::ListInsert, {recv, idx, v});
            return const_none();
        }
        if (at.attr == "remove" && is_list) {
            if (c.args.size() != 1) {
                err(c.line, c.col, "list.remove() takes exactly one argument");
                return const_none();
            }
            VReg v = elem_arg(0);
            VReg idx = call_runtime(RtFn::ListIndex, t_int(), {recv, v, const_int(ek)});
            call_void(RtFn::ListDelIndex, {recv, idx});
            return const_none();
        }
        if (at.attr == "pop" && is_list) {
            if (c.args.size() > 1) {
                err(c.line, c.col, "list.pop() takes at most one argument");
                return const_none();
            }
            VReg idx;
            if (c.args.empty()) {
                Inst len;
                len.op = IrOp::Len;
                len.a = recv;
                VReg n = def(t_int(), std::move(len));
                Inst sub;
                sub.op = IrOp::Bin;
                sub.bop = BinKind::Sub;
                sub.a = n;
                sub.b = const_int(1);
                idx = def(t_int(), std::move(sub));
            } else {
                idx = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            }
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = recv;
            get.b = idx;
            VReg v = def(et, std::move(get));
            // kind 1 的列表持有元素引用，删除时会 dec；把返回值领出来，
            // 否则返回值会随删除变成悬空指针。
            if (lk == 1 && ir_is_heap(et)) {
                rc_inc(v);
                push_temp(v);
            }
            call_void(RtFn::ListDelIndex, {recv, idx});
            return v;
        }
        if (at.attr == "clear" && is_list) {
            if (!c.args.empty()) {
                err(c.line, c.col, "list.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::ListSetLen, {recv, const_int(0)});
            return const_none();
        }
        if (at.attr == "index") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "index() takes exactly one argument");
                return const_none();
            }
            VReg v = elem_arg(0);
            return call_runtime(RtFn::ListIndex, t_int(), {recv, v, const_int(ek)});
        }
        if (at.attr == "count") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "count() takes exactly one argument");
                return const_none();
            }
            VReg v = elem_arg(0);
            return call_runtime(RtFn::ListCount, t_int(), {recv, v, const_int(ek)});
        }
        if (at.attr == "copy" && is_list) {
            if (!c.args.empty()) {
                err(c.line, c.col, "list.copy() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::ListCopy, recv_ty, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "reverse" && is_list) {
            if (!c.args.empty()) {
                err(c.line, c.col, "list.reverse() takes no arguments");
                return const_none();
            }
            call_void(RtFn::ListReverse, {recv});
            return const_none();
        }
        if (at.attr == "sort" && is_list) {
            if (!c.args.empty() || !c.keywords.empty()) {
                err(c.line, c.col, "list.sort() key=/reverse= are not supported yet");
                return const_none();
            }
            call_void(RtFn::ListSort, {recv, const_int(sort_kind(et))});
            return const_none();
        }
        err(c.line, c.col, std::format("method '{}' is not supported by the code generator yet",
                                       at.attr));
        return const_none();
    }

    if (recv_ty && (recv_ty->kind == TyKind::Int || recv_ty->kind == TyKind::Bool) &&
        at.attr == "bit_length") {
        if (!c.args.empty()) {
            err(c.line, c.col, "bit_length() takes no arguments");
            return const_none();
        }
        return call_runtime(RtFn::IntBitLength, t_int(), {recv});
    }

    err(c.line, c.col, std::format("method '{}' is not supported by the code generator yet",
                                   at.attr));
    return const_none();
}

namespace {

// SysV x86-64 聚合分类：每个 8 字节 chunk 只含 double 字段时走 SSE，
// 否则走整数寄存器。C 结构体字段都是自然对齐的定宽标量，double 不会
// 跨 chunk，因此按 chunk 求并集就够。
IRStructABI struct_abi(const StructInfo* st) {
    IRStructABI abi;
    if (!st) return abi;
    abi.size = st->size;
    size_t n = static_cast<size_t>((st->size + 7) / 8);
    abi.chunk_fp.assign(n, 0);
    for (auto& f : st->fields) {
        if (!f.ty || f.ty->kind != TyKind::Float) continue;
        size_t b = static_cast<size_t>(f.offset / 8);
        size_t e = static_cast<size_t>((f.offset + f.size - 1) / 8);
        for (size_t c = b; c <= e && c < n; ++c) abi.chunk_fp[c] = 1;
    }
    return abi;
}

}  // namespace

VReg Lowerer::lower_cimport_call(Attribute& at, Call& c) {
    auto* n = static_cast<Name*>(at.value.get());
    const CImportDecl* decl = tc_.cmodule(n->id);
    const CExtern* ex = nullptr;
    for (auto& e : decl->externs)
        if (e.py_name == at.attr) ex = &e;
    if (!ex) {
        err(c.line, c.col,
            std::format("cimport '{}' has no function '{}'", n->id, at.attr));
        return const_none();
    }
    if (c.args.size() > ex->params.size() && !ex->varargs) {
        err(c.line, c.col,
            std::format("'{}' takes {} arguments but {} were given", at.attr,
                        ex->params.size(), c.args.size()));
        return const_none();
    }

    // 按 .cpya 的参数顺序收集实参；位置参数在前，关键字参数按名字补齐。
    size_t nparams = ex->params.size();
    size_t nsrcs = c.args.size() > nparams ? c.args.size() : nparams;
    std::vector<Expr*> srcs(nsrcs, nullptr);
    for (size_t i = 0; i < c.args.size(); ++i) srcs[i] = c.args[i].get();
    for (auto& kw : c.keywords) {
        bool found = false;
        for (size_t i = 0; i < ex->params.size(); ++i) {
            if (ex->params[i].name != kw.arg) continue;
            if (srcs[i]) {
                err(c.line, c.col,
                    std::format("'{}' got multiple values for argument '{}'", at.attr, kw.arg));
                return const_none();
            }
            srcs[i] = kw.value.get();
            found = true;
            break;
        }
        if (!found) {
            err(c.line, c.col,
                std::format("'{}' got an unexpected keyword argument '{}'", at.attr, kw.arg));
            return const_none();
        }
    }
    for (size_t i = 0; i < srcs.size(); ++i) {
        if (!srcs[i]) {
            err(c.line, c.col,
                std::format("'{}' missing required argument '{}'", at.attr,
                            ex->params[i].name));
            return const_none();
        }
    }

    std::vector<VReg> args;
    std::vector<uint8_t> cargs;
    args.reserve(srcs.size());
    cargs.reserve(srcs.size());
    for (size_t i = 0; i < srcs.size(); ++i) {
        TypePtr aty = tc_.type_of(srcs[i]);
        VReg v = expr(*srcs[i]);
        if (i >= ex->params.size()) {
            // varargs 额外实参：按 py23 实参类型选择 C 编组。
            CKind k = CKind::Int;
            if (aty && aty->kind == TyKind::Float) {
                k = CKind::Float;
            } else if (aty && aty->kind == TyKind::Str) {
                k = CKind::Str;
            } else if (aty && aty->kind == TyKind::Optional && !aty->sub.empty() &&
                       aty->sub[0]->kind == TyKind::Str) {
                k = CKind::OptStr;
            } else if (aty && aty->kind == TyKind::Optional) {
                k = CKind::Ptr;
            } else if (aty && (aty->kind == TyKind::Int || aty->kind == TyKind::Bool)) {
                k = CKind::Int;
            } else {
                err(srcs[i]->line, srcs[i]->col,
                    std::format("varargs argument type '{}' is not supported", type_name(aty)));
            }
            if (k == CKind::OptStr) v = coerce(v, aty, t_str());
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        CKind k = ex->params[i].kind;
        // ptr/ptr[T] 接 list/bytes 时不转换，直接把对象指针交给 codegen；
        // codegen 按 Buf* 取 data 字段。
        if (k == CKind::Ptr && aty && !ex->params[i].opaque) {
            if (aty->kind == TyKind::List && !aty->sub.empty() && aty->sub[0]) {
                TyKind e = aty->sub[0]->kind;
                if (e == TyKind::Float) k = CKind::BufF64;
                else if (e == TyKind::Int) k = CKind::BufI64;
            } else if (aty->kind == TyKind::Bytes) {
                k = CKind::BufBytes;
            }
        }
        if (k == CKind::BufF64 || k == CKind::BufI64 || k == CKind::BufBytes) {
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        // 按值结构体已经是等布局的 payload 指针，不需要转换。
        if (k == CKind::Struct) {
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        // 回调实参是 py23 函数值；codegen 负责登记闭包并换成跳板地址。
        if (k == CKind::Callback) {
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        switch (k) {
            case CKind::Float: v = coerce(v, aty, t_float()); break;
            case CKind::Str: v = coerce(v, aty, t_str()); break;
            case CKind::Bool: v = coerce(v, aty, t_bool()); break;
            case CKind::OptStr:
                if (aty && aty->kind != TyKind::None) v = coerce(v, aty, t_str());
                break;
            case CKind::Ptr:
                if (aty && aty->kind != TyKind::None) v = coerce(v, aty, t_int());
                break;
            case CKind::None: break;
            default: v = coerce(v, aty, t_int()); break;
        }
        cargs.push_back(static_cast<uint8_t>(k));
        args.push_back(v);
    }

    int idx = -1;
    for (size_t i = 0; i < mod_.externs.size(); ++i) {
        if (mod_.externs[i].symbol == ex->symbol) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) {
        IRExtern ent;
        ent.symbol = ex->symbol;
        ent.ret = ex->ret;
        ent.varargs = ex->varargs;
        ent.param_abi.reserve(ex->params.size());
        ent.param_cb.reserve(ex->params.size());
        for (auto& p : ex->params) {
            ent.params.push_back(p.kind);
            ent.param_abi.push_back(
                p.kind == CKind::Struct ? struct_abi(tc_.struct_info(p.ptr_struct))
                                        : IRStructABI{});
            if (p.kind == CKind::Callback) {
                IRCallbackSig sig;
                sig.params = p.cb_params;
                sig.ret = p.cb_ret;
                mod_.cb_sigs.push_back(std::move(sig));
                ent.param_cb.push_back(static_cast<int>(mod_.cb_sigs.size()) - 1);
            } else {
                ent.param_cb.push_back(-1);
            }
        }
        if (ex->ret == CKind::Struct)
            ent.ret_abi = struct_abi(tc_.struct_info(ex->ret_struct));
        mod_.externs.push_back(std::move(ent));
        idx = static_cast<int>(mod_.externs.size()) - 1;
    }

    Inst in;
    in.op = IrOp::CallExtern;
    in.idx = idx;
    in.args = std::move(args);
    in.cargs = std::move(cargs);
    TypePtr rty = ckind_type(ex->ret);
    if (ex->ret == CKind::Struct) {
        if (StructInfo* st = tc_.struct_info(ex->ret_struct)) rty = mk_struct(st);
    }
    in.ty = rty;
    return def_heap(in.ty, std::move(in));
}


// 方法值：把 `obj.m` / `C.m` 合成成一个普通函数值的闭包。闭包的签名由
// typecheck 的 mk_method_value 决定，这里只负责在调用时把捕获的接收者
// 补回去：
//   - 实例上的普通方法：VCall，运行期按对象 vtable 分派；
//   - 实例上的静态/类方法：VCallStatic（类方法补 cls id）；
//   - 类上的静态/类方法：CallDirect 到该类 vtable 的最终实现；
//   - 装饰后的普通方法：捕获实例，调用类属性里的包装函数并补 self。
VReg Lowerer::lower_method_value(Attribute& at) {
    ++side_effect_seq_;

    TypePtr out_ty = tc_.type_of(&at);
    FuncInfoPtr sig = (out_ty && out_ty->kind == TyKind::Func) ? out_ty->fn : nullptr;
    if (!sig) {
        err(at.line, at.col, std::format("method '{}' has no known signature", at.attr));
        return const_none();
    }

    bool class_recv = is_class_receiver(at);
    const ClassInfo* cls = nullptr;
    VReg recv = kNoReg;
    TypePtr recv_ty = nullptr;
    if (class_recv) {
        cls = class_receiver_cls(at, strip_optional(tc_.type_of(at.value.get())));
        if (!cls) {
            err(at.line, at.col,
                std::format("method value '{}.{}' is not available here", at.attr, at.attr));
            return const_none();
        }
    } else {
        TypePtr raw = tc_.type_of(at.value.get());
        recv_ty = strip_optional(raw);
        if (!recv_ty || recv_ty->kind != TyKind::Class || !recv_ty->cls) {
            err(at.line, at.col,
                std::format("method value '{}' requires a class instance", at.attr));
            return const_none();
        }
        cls = recv_ty->cls;
        recv = expr(*at.value);
        if (recv_ty != raw) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
    }
    const MethodInfo* m = cls->method(at.attr);
    if (!m) {
        err(at.line, at.col, std::format("'{}' has no method '{}'", cls->name, at.attr));
        return const_none();
    }
    if (m->kind == MethodKind::ClassMethod && (!opts_.is_entry || !m->def)) {
        err(at.line, at.col,
            "method value of a cross-module/dynamic classmethod is not supported yet");
        return const_none();
    }
    bool decorated = m->kind == MethodKind::Normal && m->decorated;
    if (class_recv && m->kind == MethodKind::Normal) {
        err(at.line, at.col,
            std::format("'{}.{}' is an instance method; take the value on an instance",
                        cls->name, at.attr));
        return const_none();
    }
    const ClassAttrInfo* ca = decorated ? cls->class_attr(at.attr) : nullptr;
    if (decorated && !ca) {
        err(at.line, at.col,
            std::format("decorated method '{}.{}' is not available here", cls->name, at.attr));
        return const_none();
    }

    // 类接收者直接派发到该类 vtable 的最终实现；槽缺失说明没编到。
    int direct = -1;
    int cls_id = -1;
    if (class_recv) {
        auto cid = class_ids_.find(cls);
        if (cid == class_ids_.end() || m->slot < 0) {
            err(at.line, at.col,
                std::format("method value '{}.{}' is not available here", cls->name, at.attr));
            return const_none();
        }
        const auto& vt = mod_.classes[static_cast<size_t>(cid->second)].vtable;
        if (m->slot >= static_cast<int>(vt.size()) || vt[static_cast<size_t>(m->slot)] < 0) {
            err(at.line, at.col,
                std::format("method '{}.{}' was not compiled", cls->name, at.attr));
            return const_none();
        }
        direct = vt[static_cast<size_t>(m->slot)];
        cls_id = cid->second;
    }

    int ncap = class_recv ? 0 : 1;
    TypePtr rty = sig->ret ? sig->ret : t_unknown();
    IRFunction* wf = mod_.add_func(std::format("__pa_bound_{}_{}", m->name, mod_.funcs.size()));
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = rty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<std::pair<int, TypePtr>> ps;
    for (auto& p : sig->pos) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        ps.emplace_back(s, T);
    }
    for (auto& p : sig->kwonly) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        ps.emplace_back(s, T);
    }
    int es = -1;
    if (ncap) {
        es = wf->new_slot(t_unknown());
        st_->env_slot = es;
        wf->env_slot = es;
        st_->borrow_slots.insert(es);
    }
    inc_params();

    VReg self = kNoReg;
    if (ncap) {
        // 环境槽 16 起是捕获值：每个捕获是一个单元素 cell。
        TypePtr cell_ty = mk_list(recv_ty);
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        self = def(recv_ty, std::move(in));
    }

    std::vector<VReg> args;
    for (auto& [s, T] : ps) args.push_back(load_slot(s, T));

    VReg r = kNoReg;
    if (decorated) {
        VReg fv = class_attr_load_dyn(self, cls, ca);
        Inst in;
        in.op = IrOp::Call;
        in.a = fv;
        in.args.push_back(self);
        for (VReg a : args) in.args.push_back(a);
        r = def_heap(rty, std::move(in));
    } else if (m->kind != MethodKind::Normal) {
        std::vector<VReg> all;
        if (m->kind == MethodKind::ClassMethod) {
            if (class_recv) {
                all.push_back(const_int(cls_id));
            } else {
                auto cid = class_ids_.find(cls);
                all.push_back(const_int(cid == class_ids_.end() ? 0 : cid->second));
            }
        }
        for (VReg a : args) all.push_back(a);
        if (class_recv) {
            Inst cd;
            cd.op = IrOp::CallDirect;
            cd.idx = direct;
            cd.args = std::move(all);
            r = def_heap(rty, std::move(cd));
        } else {
            Inst vc;
            vc.op = IrOp::VCallStatic;
            vc.a = self;
            vc.imm = m->slot;
            vc.args = std::move(all);
            r = def_heap(rty, std::move(vc));
        }
    } else {
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = self;
        vc.imm = m->slot;
        vc.args = std::move(args);
        r = def_heap(rty, std::move(vc));
    }
    emit_return(r, rty);
    pop_frame();

    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, out_ty, {code, const_int(ncap)});
    push_temp(closure);
    if (ncap) {
        VReg cell = make_cell_value(recv, recv_ty);
        rc_inc(cell);
        field_store(closure, 16, cell);
    }
    return closure;
}

}  // namespace pyadamas
