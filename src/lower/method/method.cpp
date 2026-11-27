// lower/method.cpp —— 方法与 super 调用降级
#include "lower/impl.h"
#include <cstdio>

#include "cimport/native_sigs.h"  // 子模块属性链（os.path.*）的声明式签名

namespace pyadamas {

// 多继承（0.510）：target 的字段块在 cls 的布局里从哪个下标开始？
// 子类（单继承 layout base）没有自己的 block 表，但它的布局前缀就是 layout base
// 的布局 → 沿 layout_base 链往下找，偏移对子类同样成立。
static int mi_block_offset(const ClassInfo* cls, const ClassInfo* target) {
    for (const ClassInfo* k = cls; k; k = k->layout_base) {
        auto it = k->base_field_offsets.find(target);
        if (it != k->base_field_offsets.end() && it->second > 0) return it->second;
    }
    return -1;
}

// 显式直调 C.m(obj, ...) 里 self 实参的静态类型：位置实参优先，其次同名的
// self= 关键字。*解包或没有实参时返回 nullptr（拿不到布局起点，保持旧行为）。
// 这个类型决定 self 现在是哪一套字段布局。
static TypePtr explicit_self_type(const Call& c, const FuncInfo& sig,
                                  const TypeChecker& tc) {
    if (sig.pos.empty()) return nullptr;
    const Expr* se = nullptr;
    if (!c.args.empty() && c.args[0] && c.args[0]->kind != ExprKind::Starred)
        se = c.args[0].get();
    else
        for (auto& k : c.keywords)
            if (!k.arg.empty() && k.arg == sig.pos[0].name && k.value) {
                se = k.value.get();
                break;
            }
    if (!se) return nullptr;
    TypePtr t = tc.type_of(se);
    // Optional[T] 的表示就是同一根指针，剥掉外壳再按类布局算。
    if (t && t->kind == TyKind::Optional && !t->sub.empty()) t = t->sub[0];
    return t;
}

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
    // 富异常类的 super().__init__(msg)：基异常没有真实方法表。这里只求值
    // 实参（保持副作用）并记录为消息；字段已由 lower_construct 初始化。
    if (owner->is_exception_class && at.attr == "__init__") {
        VReg self = load_slot(st_->fn->params.front().slot,
                            st_->fn->params.front().ty);
        if (!c.args.empty()) expr(*c.args[0]);
        for (auto& kw : c.keywords) expr(*kw.value);
        (void)self;
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
    // 多继承（0.510，方案 2）：super 的静态解法。declaring 是 MRO 里「owner 之后
    // 第一个定义该方法的类」，编译期就能定；若它是带字段的非 layout 基类（块偏移
    // > 0），直接在调用点把 self 挪到该块起点再**直调**它的真实实现。
    // 不能走「先挪 self 再 SuperVCall」：运行期分派用 self 指针查对象所属类。
    if (declaring && declaring != owner) {
        int block = mi_block_offset(owner, declaring);
        auto dcid = class_ids_.find(declaring);
        if (block > 0 && dcid != class_ids_.end() && m->slot >= 0 &&
            m->slot < static_cast<int>(mod_.classes[static_cast<size_t>(dcid->second)].vtable.size())) {
            int tfidx = mod_.classes[static_cast<size_t>(dcid->second)].vtable[static_cast<size_t>(m->slot)];
            if (tfidx >= 0 && tfidx < static_cast<int>(mod_.funcs.size()) &&
                !mod_.funcs[static_cast<size_t>(tfidx)]->mi_thunk &&
                mod_.funcs[static_cast<size_t>(tfidx)]->env_slot < 0) {
                Inst ad;
                ad.op = IrOp::Bin;
                ad.bop = BinKind::Add;
                ad.a = self;
                ad.b = const_int(8 * block);
                VReg adj = def(t_int(), std::move(ad));
                Inst cd;
                cd.op = IrOp::CallDirect;
                cd.idx = Operand::func(tfidx);
                cd.args.push_back(adj);
                for (VReg av : args) cd.args.push_back(av);
                TypePtr srt = m->call_sig->ret ? m->call_sig->ret : t_none();
                return def_heap(srt, std::move(cd));
            }
        }
    }
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
    // C3（0.534）：complex.conjugate() -> 新 complex（无参数）。
    {
        TypePtr rt0 = tc_.type_of(at.value.get());
        if (rt0 && rt0->kind == TyKind::Complex && at.attr == "conjugate") {
            if (!c.args.empty() || !c.keywords.empty()) {
                err(c.line, c.col, "conjugate() takes no arguments");
                return const_none();
            }
            VReg zv = expr(*at.value);
            VReg r = call_runtime(RtFn::ComplexConj, t_complex(), {zv});
            return r;
        }
    }
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
    // dylib 句柄的属性调用：lib.sym(...) 免写直调，符号经 dlsym(句柄) 解析。
    if (TypePtr vt = tc_.type_of(at.value.get()); vt && vt->kind == TyKind::DYLIB) {
        VReg h = expr(*at.value);
        return lower_c_lazy_call(c, at.attr, h);
    }
    // cimport 外部函数：alias.fname(...) 直接生成 CallExtern。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (tc_.cmodule(n->id)) return lower_cimport_call(at, c);
    }
    // cpyos.path.basename(...)：CPython 代理 + 点子模块链，编译期拼成
    // 完整模块名（python.os.path），与单层代理同一条 IPC 降级路径。
    {
        Expr* base = at.value.get();
        std::vector<std::string> chain;
        while (auto* ba = dynamic_cast<Attribute*>(base)) {
            chain.push_back(ba->attr);
            base = ba->value.get();
        }
        auto* bn = dynamic_cast<Name*>(base);
        if (bn && !chain.empty()) {
            if (auto mod = resolve_module_alias(bn->id); mod && mod->cpy_proxy) {
                std::string name = mod->name;
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    name += "." + *it;
                return lower_cpy_call(name, at.attr, c);
            }
        }
    }
    // 子模块属性链（0.484，pya-lsp 缺口 2）：os.path.join(...) 里的 os.path 是
    // 独立原生模块。把链展开成完整模块名，命中就按 from os.path import join 同
    // 口径降级（native_fn 里已有声明式签名的 (module, member) 映射）。
    {
        Expr* base = at.value.get();
        std::vector<std::string> chain;
        while (auto* ba = dynamic_cast<Attribute*>(base)) {
            chain.push_back(ba->attr);
            base = ba->value.get();
        }
        auto* bn = dynamic_cast<Name*>(base);
        if (bn && !chain.empty()) {
            if (auto mod = resolve_module_alias(bn->id); mod && !mod->cpy_proxy) {
                std::string full = mod->name;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit)
                    full += "." + *rit;
                auto sub = pyadamas::nativesig::module_info(full);
                if (sub) {
                    auto it = sub->globals.find(at.attr);
                    if (it != sub->globals.end() && it->second &&
                        it->second->kind == TyKind::Func)
                        return call_native(full, at.attr, c, it->second->fn);
                }
            }
        }
    }
    // 内置模块的原生函数/常量。stdlib 扩展补进来的名字不在原生名单里，
    // 跳过这段，落到下面按普通外部全局函数调用。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id);
            mod && mod->builtin && !mod->source_globals.count(at.attr)) {
            if (mod->cpy_proxy) return lower_cpy_call(mod->name, at.attr, c);
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
    } else if (at.value && at.value->kind == ExprKind::Attribute) {
        // 0.648：跨模块类对象 `mod.K` 不是裸 Name，静态类型就是 ClassObj[K]。
        // 不认它的话 `mod.K.cm()` / `mod.K.sm()` 会一路掉到「方法不支持」
        // （审计 A8-2）。类接收者的类从这里取，后面的分派逻辑完全复用。
        TypePtr vt = strip_optional(tc_.type_of(at.value.get()));
        if (vt && vt->kind == TyKind::ClassObj && !vt->sub.empty() && vt->sub[0] &&
            vt->sub[0]->kind == TyKind::Class && vt->sub[0]->cls)
            class_recv = vt->sub[0]->cls;
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
                // 多继承字段块平移（BUG-1）：显式传 self 的直调同样要挪。被调
                // 实现按**声明类**的布局算 self.<字段> 偏移；实参的静态类若把
                // 那个类当非主基类、把它的字段块追加到了别处
                // （base_field_offsets），self 必须先挪到块起点。
                // 走 thunk 时被调的位移是相对**接收者类 cls**的（thunk 装在它的
                // vtable 里），所以起点用 cls 而不是 m->owner。
                // 以前这条路只做 bind_args，于是
                //   class C(A, B): __init__: A.__init__(self); B.__init__(self)
                // 里 B 的字段写到了 A 的槽上（静默错值）。
                {
                    const IRFunction* callee =
                        mod_.funcs[static_cast<size_t>(nvt[static_cast<size_t>(m->slot)])].get();
                    const ClassInfo* want = (callee && callee->mi_thunk) ? cls : m->owner;
                    TypePtr self_ty = explicit_self_type(c, *m->sig, tc_);
                    if (want && !args.empty() && self_ty && self_ty->kind == TyKind::Class &&
                        self_ty->cls && self_ty->cls != want) {
                        int block = mi_block_offset(self_ty->cls, want);
                        if (block > 0) {
                            Inst ad;
                            ad.op = IrOp::Bin;
                            ad.bop = BinKind::Add;
                            ad.a = args[0];
                            ad.b = const_int(8 * block);
                            args[0] = def(t_int(), std::move(ad));
                        }
                    }
                }
                Inst cd;
                cd.op = IrOp::CallDirect;
                cd.idx = Operand::func(nvt[static_cast<size_t>(m->slot)]);
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
            cd.idx = Operand::func(vt[m->slot]);
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
    // 联合接收者（0.529 P0-D；0.680 收紧）：类型检查已按成员表放行
    // （`if isinstance(o, int): return` 之后的 o 是 str），但降级只看到声明的
    // `int | str`。**降级层只有在「非 None 成员恰好一个」时才静态选型** ——
    // 那正是 Optional[T]/None 的形态（运行期 guard_not_none 兜底，表示一致）。
    //
    // 真·多类联合（≥2 个非 None 成员）一律不选、保持联合类型，最后由本函数
    // 末尾报「method not supported by the code generator yet」：
    //   * 只有部分成员有该方法 → 选中有方法的那个会给另一类的实例直调它的
    //     实现（0.679 前静默错值：读错字段偏移、越界读）；
    //   * 即使所有成员都有该方法，静态选第一个仍然错——各成员的实现按各自
    //     布局/返回类型编译（实测 r11/r12：返回类型不同时打出垃圾值，字段
    //     偏移不同时读到错字段）。多类联合要支持得靠运行期按 tag 分派。
    //
    // 旧判据 `hits == 1`：多类联合里选唯一命中者（r13/r15 静默错值）；0.678
    // 类型检查侧已收紧，这里漏改，0.680 补齐。
    if (recv_ty && recv_ty->kind == TyKind::Unknown && !recv_ty->sub.empty()) {
        TypePtr pick;
        int hits = 0;
        int classes = 0;   // 非 None 的成员数
        for (auto& m : recv_ty->sub) {
            if (!m || m->kind == TyKind::None) continue;
            ++classes;
            const bool ok = m->kind == TyKind::Class ? (m->cls && m->cls->method(at.attr))
                                                    : TypeChecker::type_has_method(m, at.attr);
            if (ok) { pick = m; ++hits; }
        }
        if (classes == 1 && hits == 1) recv_ty = pick;
    }

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
                        cd.idx = Operand::func(fidx);
                        // 只有普通方法传 self；静态方法和类方法都不占这个位置。
                        // 多继承（0.508）：目标方法声明类不是接收者静态类时，self 要
                        // 先挪到那个基类的字段块起点（调用点 delta，thunk 的直调形态）。
                        if (m->kind == MethodKind::Normal) {
                            VReg self = recv;
                            if (m->owner && m->owner != recv_ty->cls && !callee->mi_thunk) {
                                int block = mi_block_offset(recv_ty->cls, m->owner);
                                if (block > 0) {
                                    Inst ad;
                                    ad.op = IrOp::Bin;
                                    ad.bop = BinKind::Add;
                                    ad.a = recv;
                                    // 位移 = 8 * 块起始下标（字段下标到字节偏移的
                                    // 换算是「下标差」，不是绝对偏移）。
                                    ad.b = const_int(8 * block);
                                    self = def(t_int(), std::move(ad));
                                }
                            }
                            cd.args.push_back(self);
                        }
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
                    recv_ty->kind == TyKind::Deque))
        return lower_method_native(at, c, recv, recv_ty, opt);

    // 内置类型的方法表是静态的，Optional 上没有；走到这里说明没做
    // None 检查。报错比让 pa_list_append 拿到 0 更可查。
    if (opt) {
        err(c.line, c.col,
            std::format("method '{}' called on a maybe-None value; add a None check first",
                        at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Generator) return lower_method_generator(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::AsyncGenerator) return lower_method_async_generator(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Task) return lower_method_task(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Future) return lower_method_future(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Chan) return lower_method_chan(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Dict) return lower_method_dict(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Set) return lower_method_set(at, c, recv, recv_ty);

    if (recv_ty && recv_ty->kind == TyKind::Random) return lower_method_random(at, c, recv);

    if (recv_ty && recv_ty->kind == TyKind::Regex) return lower_method_regex(at, c, recv);

    if (recv_ty && recv_ty->kind == TyKind::Match) return lower_method_match(at, c, recv);

    if (recv_ty && recv_ty->kind == TyKind::File) return lower_method_file(at, c, recv);

    if (recv_ty && recv_ty->kind == TyKind::Bytes) return lower_method_bytes(at, c, recv);
    if (recv_ty && recv_ty->kind == TyKind::Str) return lower_method_str(at, c, recv);

    if (recv_ty && (recv_ty->kind == TyKind::List || recv_ty->kind == TyKind::Tuple)) return lower_method_seq(at, c, recv, recv_ty);

    if (recv_ty && is_int_like(recv_ty) && at.attr == "bit_length") return lower_method_int(c, recv);

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
        bool fp = f.ty && f.ty->kind == TyKind::Float;
        if (!fp) continue;
        size_t b = static_cast<size_t>(f.offset / 8);
        size_t e = static_cast<size_t>((f.offset + f.size - 1) / 8);
        for (size_t c = b; c <= e && c < n; ++c) abi.chunk_fp[c] = 1;
    }
    // float32 只有 4 字节，可能与整数字段共用同一个 chunk；double 天然 8 字节
    // 对齐不会共享。共用时 SysV 会把这个 chunk 拆到两个寄存器类，这里标记出来。
    for (auto& f : st->fields) {
        if (!f.ty || f.ty->kind != TyKind::Float || f.size != 4) continue;
        size_t chunk = static_cast<size_t>(f.offset / 8);
        if (chunk >= n) continue;
        for (auto& g : st->fields) {
            if (g.name == f.name) continue;
            if (g.offset / 8 != static_cast<int64_t>(chunk)) continue;
            if (g.ty && g.ty->kind == TyKind::Float) continue;
            abi.mixed = 1;
        }
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
        // 0.562：.cpya 的 C struct 构造（c.pair() / c.pair(1.0, 2.0)）：
        // 值语义 + C 布局，直接复用值类型 struct 的构造路径。
        for (auto& cs : decl->structs) {
            if (cs.name != at.attr) continue;
            if (StructInfo* st = tc_.struct_info(cs.name))
                return lower_struct_construct(c, st);
            break;
        }
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
            // varargs 额外实参：按 PyAdamas 实参类型选择 C 编组。
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
            } else if (aty->kind == TyKind::Buffer) {
                k = CKind::BufBuffer;
            }
        }
        if (k == CKind::BufF64 || k == CKind::BufI64 || k == CKind::BufBytes ||
            k == CKind::BufBuffer) {
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        // 按值结构体已经是等布局的 payload 指针，不需要转换。
        if (k == CKind::Struct) {
            if (StructInfo* st = tc_.struct_info(ex->params[i].ptr_struct)) {
                if (struct_abi(st).mixed) {
                    err(srcs[i]->line, srcs[i]->col,
                        std::format("struct '{}' 的同一个 8 字节块里混有 float32 与整数字段，"
                                    "SysV 按值传递要拆成两个寄存器类，暂不支持；"
                                    "改用它指针形态 ptr[{}]",
                                    st->name, st->name));
                    return const_none();
                }
            }
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        // 回调实参是 PyAdamas 函数值；codegen 负责登记闭包并换成跳板地址。
        if (k == CKind::Callback) {
            cargs.push_back(static_cast<uint8_t>(k));
            args.push_back(v);
            continue;
        }
        switch (k) {
            case CKind::Float:
            case CKind::Float32: v = coerce(v, aty, t_float()); break;
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
    in.idx = Operand::ext(idx);
    in.args = std::move(args);
    in.cargs = std::move(cargs);
    TypePtr rty = ckind_type(ex->ret);
    if (ex->ret == CKind::Struct) {
        if (StructInfo* st = tc_.struct_info(ex->ret_struct)) {
            if (struct_abi(st).mixed) {
                err(c.line, c.col,
                    std::format("struct '{}' 的同一个 8 字节块里混有 float32 与整数字段，"
                                "按值返回的 ABI 尚未支持；请改用 ptr[{}] 返回",
                                st->name, st->name));
                return const_none();
            }
            rty = mk_struct(st);
        }
    }
    // **先把类型取出来再 move in。** 同一实参列表里 in.ty 与 std::move(in)
    // 的求值顺序未指定；GCC 会先构造 by-value 的 Inst 参数（移走 in.ty），
    // 再读 in.ty 就得到空指针，vreg 被记成 unknown。cimport 返回 float 的
    // 调用点因此长期是「机器值在 xmm、静态类型 unknown」，0.324 的
    // unknown -> float 解盒第一次把它暴露成数值错误。
    TypePtr out_ty = rty;
    in.ty = out_ty;
    return def_heap(out_ty, std::move(in));
}


// 方法值：把 `obj.m` / `C.m` 合成成一个普通函数值的闭包。闭包的签名由
// typecheck 的 mk_method_value 决定，这里只负责在调用时把捕获的接收者
// 补回去：
//   - 实例上的普通方法：VCall，运行期按对象 vtable 分派；
//   - 实例上的静态/类方法：VCallStatic（类方法补 cls id）；
//   - 类上的静态/类方法：CallDirect 到该类 vtable 的最终实现；
//   - 装饰后的普通方法：捕获实例，调用类属性里的包装函数并补 self。
// B1-min：接收者类型是否足够确定（列表/字典/集合/字符串/具体类实例）。
bool Lowerer::bm_receiver_ok(const TypePtr& t) {
    TypePtr r = strip_optional(t);
    if (!r) return false;
    switch (r->kind) {
        case TyKind::List:
        case TyKind::Dict:
        case TyKind::Set:
        case TyKind::Str:
        case TyKind::Class: return true;
        default: return false;
    }
}

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
            cd.idx = Operand::func(direct);
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
