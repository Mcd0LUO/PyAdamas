// lower/import.cpp —— import / 模块别名、外部符号与 import 语句
#include "lower/impl.h"

#include "cimport/native_sigs.h"  // 声明式原生模块签名（P0）

namespace pyadamas {


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

const ImportedClass* Lowerer::find_imported_class(const ClassInfo* cls) {
    if (!cls) return nullptr;
    for (auto& [name, ic] : imported_classes_)
        if (ic.cls.get() == cls) return &ic;
    auto scan = [&](const ModuleInfoPtr& mod) -> const ImportedClass* {
        if (!mod) return nullptr;
        for (auto& [name, ic] : mod->classes)
            if (ic.cls.get() == cls) return &ic;
        return nullptr;
    };
    if (st_) {
        for (auto& [name, mod] : st_->modules)
            if (const ImportedClass* ic = scan(mod)) return ic;
    }
    for (auto& [name, mod] : module_aliases_)
        if (const ImportedClass* ic = scan(mod)) return ic;
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

/* 模块里的「构造器」：由 lower_<module> 分派处理，运行时没有对应函数，
   所以 native_fn 里查不到。from X import Y 需要单独认这些名字。 */
static bool module_ctor(const std::string& module, const std::string& name) {
    if (module == "pathlib") return name == "Path";
    if (module == "collections")
        return name == "deque" || name == "OrderedDict" || name == "defaultdict";
    if (module == "datetime")
        return name == "date" || name == "datetime" || name == "timedelta";
    if (module == "operator") {
        static const char* kOps[] = {
            "add",   "sub",   "mul",      "truediv", "floordiv", "mod",
            "pow",   "and_",  "or_",      "xor",     "lshift",   "rshift",
            "concat", "getitem", "contains", "eq",    "ne",       "lt",
            "le",    "gt",    "ge",       "is_",     "is_not",   "neg",
            "pos",   "abs",   "invert",   "truth",   "not_",     "itemgetter",
            "attrgetter"};
        for (const char* op : kOps)
            if (name == op) return true;
        return false;
    }
    return false;
}

std::optional<RtFn> Lowerer::native_fn(const std::string& module, const std::string& member) {
    // 声明式签名优先（P0）：成员 → RtFn 由 stdlib/sigs/<module>.pysig 提供；
    // 没有声明的模块继续走下面的硬编码映射。
    if (auto declared = pyadamas::nativesig::func_rtfn(module, member)) return *declared;
    if (module == "dylib") {
        if (member == "load") return RtFn::DYLIBLoad;
        if (member == "close") return RtFn::DYLIBClose;
        return std::nullopt;
    }
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
        if (member == "timeout") return RtFn::AsyncTimeoutNew;
        if (member == "TaskGroup") return RtFn::TaskGroupNew;
        if (member == "Semaphore") return RtFn::SemNew;
        if (member == "wait_readable" || member == "wait_writable")
            return RtFn::AsyncioWaitFd;
        return std::nullopt;
    }
    // sys.exit 已迁到 stdlib/sigs/sys.pysig（pa_sys_exit 由约定推导）。
    // sys.exc_info 的返回类型含 ClassObj，签名保留在 typecheck/builtin_sigs.cpp 的
    // sys merge 分支，member→RtFn 映射也留在这里。
    if (module == "sys" && member == "exit") return RtFn::SysExit;
    if (module == "sys" && member == "exc_info") {
        return RtFn::ExcInfo;
    } else if (module == "os") {
        // 其余 os 成员已迁到 stdlib/sigs/os.pysig；walk 仍在硬编码。
        if (member == "walk") return RtFn::OsWalk;
    } else if (module == "subprocess") {
        if (member == "run") return RtFn::SubprocessRun;
        // 只为 from-import 登记别名；真正降级走 lower_popen。
        if (member == "Popen") return RtFn::PopenNew;
    } else if (module == "itertools") {
        // 只为 from-import 登记别名；真正降级走 lower_itertools。
        if (member == "chain") return RtFn::IterChain;
        if (member == "islice") return RtFn::ListSlice;
        if (member == "repeat") return RtFn::IterRepeat;
        if (member == "product") return RtFn::IterProduct;
        if (member == "permutations") return RtFn::IterPermutations;
        if (member == "combinations") return RtFn::IterCombinations;
        if (member == "combinations_with_replacement") return RtFn::IterCombinationsWR;
        if (member == "zip_longest") return RtFn::IterZipLongest;
        if (member == "accumulate") return RtFn::IterAccumulate;
    } else if (module == "collections") {
        if (member == "Counter") return RtFn::CollCounter;
    } else if (module == "functools") {
        // 只为 from-import 登记别名；真正降级走 lower_functools。
        if (member == "reduce" || member == "wraps" || member == "partial" ||
            member == "lru_cache")
            return RtFn::FuncReduce;
    } else if (module == "json") {
        if (member == "dumps") return RtFn::JsonDumps;
        if (member == "dump") return RtFn::JsonDump;
        if (member == "load") return RtFn::JsonLoad;
        if (member == "loads") return RtFn::JsonLoads;
        // 动态 JSON（0.447）：只为 from-import 登记别名；真正降级走
        // lower_json（parse/stringify 的运行时入口在那边按类型选）。
        if (member == "parse") return RtFn::JsonParse;
        if (member == "stringify") return RtFn::JsonDumps;
        if (member == "get" || member == "get_or" || member == "has" ||
            member == "len" || member == "keys" || member == "type" ||
            member == "is_null" || member == "as_str" || member == "as_int" ||
            member == "as_float" || member == "as_bool" || member == "as_dict" ||
            member == "as_list" || member == "null" || member == "bool" ||
            member == "path")
            return RtFn::JsonOp;
    } else if (module == "re") {
        if (member == "compile") return RtFn::RegexCompile;
        if (member == "search" || member == "match" || member == "fullmatch")
            return RtFn::RegexSearchStr;
        if (member == "findall") return RtFn::RegexFindAllStr;
        if (member == "finditer") return RtFn::RegexFindIterStr;
        if (member == "sub") return RtFn::RegexSubStr;
        if (member == "subn") return RtFn::RegexSubnStr;
        if (member == "split") return RtFn::RegexSplitStr;
        if (member == "escape") return RtFn::RegexEscape;
    }
    return std::nullopt;
}

bool Lowerer::builtin_value(const std::string& module, const std::string& member, VReg& out,
                            TypePtr& ty) {
    // sys.float_info / sys.flags（0.520）：内建记录对象，属性访问在 expr_assign.cpp 里
    // 直接发射原生 getter，**不使用接收者值**，所以这里给一个占位 0 即可。
    if (module == "sys" && (member == "float_info" || member == "flags")) {
        out = const_int(0);
        ty = member == "float_info" ? t_float_info() : t_sys_flags();
        return true;
    }
    // 声明式常量（P0）：stdlib/sigs/<module>.pysig 的 const，按字面量直接产出。
    if (const auto* dc = pyadamas::nativesig::find_const(module, member)) {
        switch (dc->val.kind) {
            case LiteralValue::Kind::Int: out = const_int(dc->val.i); break;
            case LiteralValue::Kind::Float: out = const_float(dc->val.f); break;
            case LiteralValue::Kind::Bool: out = const_bool(dc->val.b); break;
            case LiteralValue::Kind::Str: out = const_str(dc->val.s); break;
            default: out = const_none(); break;
        }
        ty = dc->ty;
        return true;
    }
    if (module == "re") {
        if (member == "I" || member == "IGNORECASE") {
            out = const_int(2);
            ty = t_int();
            return true;
        }
        if (member == "M" || member == "MULTILINE") {
            out = const_int(8);
            ty = t_int();
            return true;
        }
        if (member == "S" || member == "DOTALL") {
            out = const_int(16);
            ty = t_int();
            return true;
        }
        if (member == "X" || member == "VERBOSE") {
            out = const_int(64);
            ty = t_int();
            return true;
        }
    }
    if (module == "subprocess") {
        // 与 CPython 的取值一致：PIPE=-1、STDOUT=-2、DEVNULL=-3。
        if (member == "PIPE") {
            out = const_int(-1);
            ty = t_int();
            return true;
        }
        if (member == "STDOUT") {
            out = const_int(-2);
            ty = t_int();
            return true;
        }
        if (member == "DEVNULL") {
            out = const_int(-3);
            ty = t_int();
            return true;
        }
    }
    // os 的标量常量已迁到 stdlib/sigs/os.pysig；environ 是运行期 dict，
    // 无法声明，保留在此。
    if (module == "os") {
        if (member == "environ") {
            ty = mk_dict(t_str(), t_str());
            out = call_runtime(RtFn::OsEnviron, ty, {});
            push_temp(out);
            return true;
        }
    }
    // math 的全部常量已迁到 stdlib/sigs/math.pysig（nan 的特判在
    // builtin_value 开头，见上文）。
    // sys.maxsize 已迁到 stdlib/sigs/sys.pysig；下面是运行期调用生成的
    // 成员（argv/executable/标准流），pysig 无法表达，保留在 C++。
    if (module == "sys") {
        if (member == "version_info") {
            // (3, 14, 4, "final", 0)：与 stdlib/sigs/sys.pysig 的 hexversion /
            // version 同一口径（项目对标的 CPython 3.14.4）。元组是新分配的
            // 堆对象，进临时栈。
            ty = mk_tuple({t_int(), t_int(), t_int(), t_str(), t_int()});
            Inst mt;
            mt.op = IrOp::MakeTuple;
            mt.imm = list_kind_filled(ty);
            mt.args = {const_int(3), const_int(14), const_int(4), const_str("final"),
                       const_int(0)};
            out = def_heap(ty, std::move(mt));  // def_heap 已把堆结果登进临时栈
            return true;
        }
        // 解释器身份（0.518）：pyadamas 没有运行期「安装前缀」概念，这三个
        // 按**目标平台**给编译期快照，与 sys.pysig 里 version/platform 同口径；
        // sys.path 是 list[str] 快照（可被用户 insert，但不参与我们的导入解析）。
        if (member == "prefix" || member == "exec_prefix") {
            ty = t_str();
            out = const_str("/usr");
            return true;
        }
        if (member == "path") {
            ty = mk_list(t_str());
            Inst ml;
            ml.op = IrOp::MakeList;
            ml.imm = list_kind_filled(ty);
            ml.args = {const_str(""), const_str("/usr/lib/python3.14"),
                       const_str("/usr/lib/python3.14/lib-dynload")};
            out = def_heap(ty, std::move(ml));
            return true;
        }
        if (member == "argv") {
            ty = mk_list(t_str());
            out = call_runtime(RtFn::SysArgv, ty, {});
            return true;
        }
        if (member == "executable") {
            ty = t_str();
            out = call_runtime(RtFn::SysExecutable, ty, {});
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

// random.randrange / getrandbits / gauss / expovariate / triangular /
// sample / choices：这几个运行时函数带一个 rng 首参（模块级传 0，表示用
// 全局 rng），签名里表达不了，所以参数在这里手工装配。
void Lowerer::lower_import(Import& im) {
    // 模块 init 会执行任意用户代码，可能改动容器。
    ++side_effect_seq_;
    for (auto& al : im.names) {
        if (al.name == "python" || al.name.starts_with("python.")) {
            err(im.line, im.col,
                "use 'from python import <module>' instead of 'import python'");
            continue;
        }
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
        // 原生 + stdlib 扩展源的模块也要跑扩展模块 init（原生模块自身没
        // 有运行期对象，但扩展全局需要初始化）。
        if (!mod->builtin || mod->source_init) ensure_module_init(mod);
        if (st_->is_module_top) module_aliases_[bind] = mod;
        else st_->modules[bind] = mod;
    }
}

void Lowerer::lower_import_from(ImportFrom& im) {
    ++side_effect_seq_;
    // from c import X：免写直调不产生运行期绑定——调用点在 lower_call
    // 按 FuncInfo::c_lazy 拦截，符号地址在首次调用时经 dlsym 解析。
    if (im.level == 0 && im.module == "c") return;
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
    if (!mod->builtin || mod->source_init) ensure_module_init(mod);
    if (mod->cpy_proxy) {
        // from python import <module>：登记 CPython 模块代理别名。没有
        // 运行期模块对象，不发 ImportModule；调用走 lower_cpy_call。
        if (im.names.size() == 1 && im.names[0].name == "*") {
            err(im.line, im.col, "from python import * is not supported");
            return;
        }
        for (auto& al : im.names) {
            std::string bind = al.asname.empty() ? al.name : al.asname;
            auto child = opts_.module_lookup(mod->name + "." + al.name);
            if (!child) {
                err(im.line, im.col,
                    std::format("cannot resolve CPython module '{}'", al.name));
                continue;
            }
            if (st_->is_module_top) module_aliases_[bind] = child;
            else st_->modules[bind] = child;
        }
        return;
    }
    if (im.names.size() == 1 && im.names[0].name == "*") {
        for (auto& [name, ty] : mod->globals) {
            if (name.empty() || name[0] == '_') continue;
            if (mod->builtin && (native_fn(mod->name, name) || module_ctor(mod->name, name))) {
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
            // stdlib 扩展补进来的名字（原生没有）：没有原生降级路径，按
            // 普通外部全局处理。
            if (mod->source_globals.count(al.name)) {
                auto it = mod->globals.find(al.name);
                if (it != mod->globals.end()) {
                    int gi = extern_global(module_global_symbol(mod->name, al.name),
                                           std::format("{}.{}", mod->name, al.name), it->second);
                    VReg v = load_global(gi, it->second);
                    if (!it->second || it->second->kind == TyKind::Unknown) rc_inc_unknown(v);
                    bind_name_value(bind, v, it->second);
                    continue;
                }
            }
            if (native_fn(mod->name, al.name) || module_ctor(mod->name, al.name)) {
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
                // 子模块也是独立编译单元：绑定别名前必须确保它自己的 init 跑过，
                // 否则它的函数全局还是 0，后面 sub.f() 就是 call 0（SIGSEGV）。
                // 父模块的 init 不保证会 import 子模块（__init__.py 可能什么都不写）。
                if (!child->builtin || child->source_init) ensure_module_init(child);
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


}  // namespace pyadamas
