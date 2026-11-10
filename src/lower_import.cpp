// lower_import.cpp —— import / 模块别名与外部符号绑定
#include "lower_impl.h"

namespace py23 {

ModuleInfoPtr Lowerer::resolve_module_alias(const std::string& name) const {
    // 显式登记的模块别名优先：from pkg import sub 会把 sub 放进别名表，
    // 类型检查的 hoist 也可能留下同名 unknown 变量，不能让它遮住别名。
    if (st_) {
        auto it = st_->modules.find(name);
        if (it != st_->modules.end()) return it->second;
    }
    auto it = module_aliases_.find(name);
    if (it != module_aliases_.end()) return it->second;
    // 真正的局部变量 / 推导式变量仍然遮蔽模块别名。
    if (st_ && st_->slots.count(name)) return nullptr;
    if (comp_slot(name) >= 0) return nullptr;
    return nullptr;
}

void Lowerer::ensure_module_init(const ModuleInfoPtr& mod) {
    if (!mod) return;
    int idx = -1;
    for (size_t i = 0; i < mod_.imports.size(); ++i)
        if (mod_.imports[i] == mod->name) {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0) {
        idx = static_cast<int>(mod_.imports.size());
        mod_.imports.push_back(mod->name);
    }
    Inst in;
    in.op = IrOp::ImportModule;
    in.imm = idx;
    emit(std::move(in));
}

int Lowerer::extern_global(const std::string& symbol, const std::string& debug_name, TypePtr ty) {
    auto it = extern_globals_.find(symbol);
    if (it != extern_globals_.end()) {
        auto& g = mod_.globals[it->second];
        if (ty && g.ty && g.ty->kind == TyKind::Unknown) g.ty = ty;
        return it->second;
    }
    IRGlobal g;
    g.name = debug_name;
    g.ty = ty ? ty : t_unknown();
    g.symbol = symbol;
    g.external = true;
    int id = static_cast<int>(mod_.globals.size());
    mod_.globals.push_back(std::move(g));
    extern_globals_[symbol] = id;
    return id;
}

std::optional<int> Lowerer::module_member(Attribute& at, ModuleInfoPtr* owner) {    auto* n = dynamic_cast<Name*>(at.value.get());
    if (!n) return std::nullopt;
    auto mod = resolve_module_alias(n->id);
    if (!mod) return std::nullopt;
    if (owner) *owner = mod;
    auto it = mod->globals.find(at.attr);
    if (it == mod->globals.end()) {
        err(at.line, at.col,
            std::format("module '{}' has no attribute '{}'", n->id, at.attr));
        return std::nullopt;
    }
    if (it->second && it->second->kind == TyKind::Class) {
        err(at.line, at.col, std::format("class '{}' cannot be used as a value", at.attr));
        return std::nullopt;
    }
    return extern_global(module_global_symbol(mod->name, at.attr),
                         std::format("{}.{}", mod->name, at.attr), it->second);
}

VReg Lowerer::construct_imported(const ImportedClass& ic, Call& c, TypePtr cls_ty) {
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ic.ctor_sig;
    int gi = extern_global(ic.ctor_symbol, ic.ctor_symbol, fty);
    VReg callee = load_global(gi, fty);
    std::vector<VReg> args = bind_args(c, ic.ctor_sig, nullptr);
    Inst in;
    in.op = IrOp::Call;
    in.a = callee;
    in.args = std::move(args);
    return def_heap(cls_ty, std::move(in));
}

std::optional<RtFn> Lowerer::native_fn(const std::string& module, const std::string& member) {
    if (module == "asyncio") {
        // 只为 from-import 登记别名；真正降级走 lower_asyncio。
        if (member == "run") return RtFn::AsyncioRun;
        if (member == "sleep") return RtFn::AsyncioSleep;
        if (member == "gather") return RtFn::AsyncioGather;
        if (member == "create_task") return RtFn::AsyncioCreateTask;
        if (member == "Future") return RtFn::AsyncioFutureNew;
        if (member == "Event") return RtFn::EventNew;
        if (member == "Lock") return RtFn::LockNew;
        if (member == "Queue") return RtFn::QueueNew;
        if (member == "wait_for") return RtFn::AsyncioWaitFor;
        if (member == "wait_readable" || member == "wait_writable")
            return RtFn::AsyncioWaitFd;
        return std::nullopt;
    }
    if (module == "math") {
        if (member == "sqrt") return RtFn::MathSqrt;
        if (member == "floor") return RtFn::MathFloor;
        if (member == "ceil") return RtFn::MathCeil;
        if (member == "fabs") return RtFn::MathFabs;
        if (member == "sin") return RtFn::MathSin;
        if (member == "cos") return RtFn::MathCos;
        if (member == "tan") return RtFn::MathTan;
        if (member == "log") return RtFn::MathLog;
        if (member == "log2") return RtFn::MathLog2;
        if (member == "log10") return RtFn::MathLog10;
        if (member == "exp") return RtFn::MathExp;
        if (member == "pow") return RtFn::MathPow;
        if (member == "fmod") return RtFn::MathFmod;
        if (member == "atan2") return RtFn::MathAtan2;
        if (member == "hypot") return RtFn::MathHypot;
        if (member == "degrees") return RtFn::MathDegrees;
        if (member == "radians") return RtFn::MathRadians;
        if (member == "trunc") return RtFn::MathTrunc;
        if (member == "copysign") return RtFn::MathCopysign;
        if (member == "isnan") return RtFn::MathIsnan;
        if (member == "isinf") return RtFn::MathIsinf;
        if (member == "isfinite") return RtFn::MathIsfinite;
        if (member == "factorial") return RtFn::MathFactorial;
        if (member == "gcd") return RtFn::MathGcd;
        if (member == "lcm") return RtFn::MathLcm;
        if (member == "fsum") return RtFn::MathFsum;
        if (member == "acos") return RtFn::MathAcos;
        if (member == "asin") return RtFn::MathAsin;
        if (member == "atan") return RtFn::MathAtan;
        if (member == "acosh") return RtFn::MathAcosh;
        if (member == "asinh") return RtFn::MathAsinh;
        if (member == "atanh") return RtFn::MathAtanh;
        if (member == "cbrt") return RtFn::MathCbrt;
        if (member == "cosh") return RtFn::MathCosh;
        if (member == "sinh") return RtFn::MathSinh;
        if (member == "tanh") return RtFn::MathTanh;
        if (member == "exp2") return RtFn::MathExp2;
        if (member == "expm1") return RtFn::MathExpm1;
        if (member == "log1p") return RtFn::MathLog1p;
        if (member == "erf") return RtFn::MathErf;
        if (member == "erfc") return RtFn::MathErfc;
        if (member == "gamma") return RtFn::MathGamma;
        if (member == "lgamma") return RtFn::MathLgamma;
        if (member == "ulp") return RtFn::MathUlp;
        if (member == "nextafter") return RtFn::MathNextafter;
        if (member == "remainder") return RtFn::MathRemainder;
        if (member == "ldexp") return RtFn::MathLdexp;
        if (member == "fma") return RtFn::MathFma;
        if (member == "isclose") return RtFn::MathIsclose;
        if (member == "frexp") return RtFn::MathFrexp;
        if (member == "modf") return RtFn::MathModf;
        if (member == "isqrt") return RtFn::MathIsqrt;
        if (member == "comb") return RtFn::MathComb;
        if (member == "perm") return RtFn::MathPerm;
        if (member == "prod") return RtFn::MathProd;
        if (member == "sumprod") return RtFn::MathSumprod;
        if (member == "dist") return RtFn::MathDist;
    } else if (module == "sys" && member == "exit") {
        return RtFn::SysExit;
    }
    return std::nullopt;
}

bool Lowerer::builtin_value(const std::string& module, const std::string& member, VReg& out,
                            TypePtr& ty) {
    if (module == "math") {
        if (member == "pi") {
            out = const_float(3.14159265358979323846);
            ty = t_float();
            return true;
        }
        if (member == "e") {
            out = const_float(2.71828182845904523536);
            ty = t_float();
            return true;
        }
        if (member == "tau") {
            out = const_float(6.28318530717958647692);
            ty = t_float();
            return true;
        }
        if (member == "inf") {
            out = const_float(std::numeric_limits<double>::infinity());
            ty = t_float();
            return true;
        }
        if (member == "nan") {
            out = const_float(std::numeric_limits<double>::quiet_NaN());
            ty = t_float();
            return true;
        }
    } else if (module == "sys") {
        if (member == "maxsize") {
            out = const_int(std::numeric_limits<int64_t>::max());
            ty = t_int();
            return true;
        }
        if (member == "argv") {
            ty = mk_list(t_str());
            out = call_runtime(RtFn::SysArgv, ty, {});
            return true;
        }
        if (member == "stdin" || member == "stdout" || member == "stderr") {
            int64_t which = member == "stdin" ? 0 : member == "stdout" ? 1 : 2;
            ty = t_file();
            out = call_runtime(RtFn::SysStream, ty, {const_int(which)});
            return true;
        }
    }
    return false;
}

VReg Lowerer::call_native(const std::string& module, const std::string& member, Call& c,
                          const FuncInfoPtr& info) {
    auto fn = native_fn(module, member);
    if (!fn) {
        err(c.line, c.col, std::format("{}.{} is not a supported native function", module, member));
        return const_none();
    }
    std::vector<VReg> args = bind_args(c, info, nullptr);
    if (*fn == RtFn::SysExit) {
        call_void(*fn, args);
        return const_none();
    }
    TypePtr rt = info && info->ret ? info->ret : t_float();
    VReg v = call_runtime(*fn, rt, std::move(args));
    if (ir_is_heap(rt)) push_temp(v);
    return v;
}

void Lowerer::lower_import(Import& im) {
    // 模块 init 会执行任意用户代码，可能改动容器。
    ++side_effect_seq_;
    for (auto& al : im.names) {
        if (al.name.find('.') != std::string::npos && al.asname.empty()) {
            err(im.line, im.col, "dotted import needs 'as name' (or use from ... import)");
            continue;
        }
        std::string bind = al.asname.empty() ? al.name : al.asname;
        if (!opts_.module_lookup) {
            err(im.line, im.col, "import requires a multi-module build");
            continue;
        }
        auto mod = opts_.module_lookup(al.name);
        if (!mod) {
            err(im.line, im.col, std::format("cannot resolve module '{}'", al.name));
            continue;
        }
        if (!mod->builtin) ensure_module_init(mod);
        if (st_->is_module_top) module_aliases_[bind] = mod;
        else st_->modules[bind] = mod;
    }
}

void Lowerer::lower_import_from(ImportFrom& im) {
    ++side_effect_seq_;
    if (im.level != 0 || im.module.empty()) {
        err(im.line, im.col, "relative imports are not supported yet");
        return;
    }
    if (!opts_.module_lookup) {
        err(im.line, im.col, "import requires a multi-module build");
        return;
    }
    auto mod = opts_.module_lookup(im.module);
    if (!mod) {
        err(im.line, im.col, std::format("cannot resolve module '{}'", im.module));
        return;
    }
    if (!mod->builtin) ensure_module_init(mod);
    if (im.names.size() == 1 && im.names[0].name == "*") {
        for (auto& [name, ty] : mod->globals) {
            if (name.empty() || name[0] == '_') continue;
            if (mod->builtin && native_fn(mod->name, name)) {
                native_aliases_[name] = NativeRef{mod->name, name};
                continue;
            }
            VReg bv;
            TypePtr bty;
            if (mod->builtin && builtin_value(mod->name, name, bv, bty)) {
                bind_name_value(name, bv, bty);
                continue;
            }
            int gi = extern_global(module_global_symbol(mod->name, name),
                                   std::format("{}.{}", mod->name, name), ty);
            VReg v = load_global(gi, ty);
            if (!ty || ty->kind == TyKind::Unknown) rc_inc_unknown(v);
            bind_name_value(name, v, ty);
        }
        for (auto& [name, ic] : mod->classes)
            if (!name.empty() && name[0] != '_') imported_classes_[name] = ic;
        return;
    }
    for (auto& al : im.names) {
        std::string bind = al.asname.empty() ? al.name : al.asname;
        if (mod->builtin) {
            if (native_fn(mod->name, al.name)) {
                native_aliases_[bind] = NativeRef{mod->name, al.name};
                continue;
            }
            VReg bv;
            TypePtr bty;
            if (builtin_value(mod->name, al.name, bv, bty)) {
                bind_name_value(bind, bv, bty);
                continue;
            }
            err(im.line, im.col,
                std::format("module '{}' has no attribute '{}'", im.module, al.name));
            continue;
        }
        if (auto ci = mod->classes.find(al.name); ci != mod->classes.end()) {
            imported_classes_[bind] = ci->second;
            continue;
        }
        auto it = mod->globals.find(al.name);
        if (it == mod->globals.end()) {
            if (auto child = opts_.module_lookup(im.module + "." + al.name)) {
                if (st_->is_module_top) module_aliases_[bind] = child;
                else st_->modules[bind] = child;
                continue;
            }
            err(im.line, im.col,
                std::format("module '{}' has no attribute '{}'", im.module, al.name));
            continue;
        }
        int gi = extern_global(module_global_symbol(mod->name, al.name),
                               std::format("{}.{}", mod->name, al.name), it->second);
        VReg v = load_global(gi, it->second);
        // 快照绑定：本地/全局槽持有一份引用。
        if (!it->second || it->second->kind == TyKind::Unknown) rc_inc_unknown(v);
        bind_name_value(bind, v, it->second);
    }
}

}  // namespace py23
