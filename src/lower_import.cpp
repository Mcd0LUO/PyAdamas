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
        if (member == "timeout") return RtFn::AsyncTimeoutNew;
        if (member == "TaskGroup") return RtFn::TaskGroupNew;
        if (member == "Semaphore") return RtFn::SemNew;
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
    } else if (module == "random") {
        if (member == "random") return RtFn::RandGNext;
        if (member == "seed") return RtFn::RandGSeed;
        if (member == "randint") return RtFn::RandGRandint;
        if (member == "uniform") return RtFn::RandGUniform;
        if (member == "choice") return RtFn::RandGChoice;
        if (member == "shuffle") return RtFn::RandGShuffle;
        if (member == "Random") return RtFn::RandNew;
        if (member == "randrange") return RtFn::RandRange;
        if (member == "getrandbits") return RtFn::RandGetrandbits;
        if (member == "gauss") return RtFn::RandGauss;
        if (member == "expovariate") return RtFn::RandExpovariate;
        if (member == "triangular") return RtFn::RandTriangular;
        if (member == "sample") return RtFn::RandSample;
        if (member == "choices") return RtFn::RandChoices;
    } else if (module == "os") {
        if (member == "mkdir") return RtFn::OsMkdir;
        if (member == "rmdir") return RtFn::OsRmdir;
        if (member == "remove") return RtFn::OsRemove;
        if (member == "rename") return RtFn::OsRename;
        if (member == "getcwd") return RtFn::OsGetcwd;
        if (member == "cpu_count") return RtFn::OsCpuCount;
        if (member == "listdir") return RtFn::OsListdir;
        if (member == "getenv") return RtFn::OsGetenv;
        if (member == "getpid") return RtFn::OsGetpid;
        if (member == "chdir") return RtFn::OsChdir;
        if (member == "chmod") return RtFn::OsChmod;
        if (member == "access") return RtFn::OsAccess;
        if (member == "makedirs") return RtFn::OsMakedirs;
    } else if (module == "os.path") {
        if (member == "join") return RtFn::OsPathJoin;
        if (member == "exists") return RtFn::OsPathExists;
        if (member == "isdir") return RtFn::OsPathIsdir;
        if (member == "isfile") return RtFn::OsPathIsfile;
        if (member == "basename") return RtFn::OsPathBasename;
        if (member == "dirname") return RtFn::OsPathDirname;
        if (member == "splitext") return RtFn::OsPathSplitext;
        if (member == "normpath") return RtFn::OsPathNormpath;
        if (member == "abspath") return RtFn::OsPathAbspath;
        if (member == "expanduser") return RtFn::OsPathExpanduser;
        if (member == "realpath") return RtFn::OsPathRealpath;
        if (member == "getsize") return RtFn::OsPathGetsize;
        if (member == "getmtime") return RtFn::OsPathGetmtime;
        if (member == "split") return RtFn::OsPathSplit;
        if (member == "samefile") return RtFn::OsPathSamefile;
    } else if (module == "subprocess") {
        if (member == "run") return RtFn::SubprocessRun;
    } else if (module == "time") {
        if (member == "time") return RtFn::TimeTime;
        if (member == "monotonic") return RtFn::TimeMonotonic;
        if (member == "perf_counter") return RtFn::TimePerfCounter;
        if (member == "sleep") return RtFn::TimeSleep;
    } else if (module == "itertools") {
        // 只为 from-import 登记别名；真正降级走 lower_itertools。
        if (member == "chain") return RtFn::IterChain;
        if (member == "islice") return RtFn::ListSlice;
        if (member == "repeat") return RtFn::IterRepeat;
    } else if (module == "collections") {
        if (member == "Counter") return RtFn::CollCounter;
    } else if (module == "functools") {
        // 只为 from-import 登记别名；真正降级走 lower_functools。
        if (member == "reduce" || member == "wraps" || member == "partial" ||
            member == "lru_cache")
            return RtFn::FuncReduce;
    } else if (module == "json") {
        if (member == "dumps") return RtFn::JsonDumps;
        if (member == "loads") return RtFn::JsonLoads;
    } else if (module == "re") {
        if (member == "compile") return RtFn::RegexCompile;
        if (member == "search" || member == "match" || member == "fullmatch")
            return RtFn::RegexSearchStr;
        if (member == "findall") return RtFn::RegexFindAllStr;
        if (member == "finditer") return RtFn::RegexFindIterStr;
        if (member == "sub") return RtFn::RegexSubStr;
        if (member == "split") return RtFn::RegexSplitStr;
        if (member == "escape") return RtFn::RegexEscape;
    }
    return std::nullopt;
}

bool Lowerer::builtin_value(const std::string& module, const std::string& member, VReg& out,
                            TypePtr& ty) {
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
    if (module == "os") {
        if (member == "sep") {
            out = const_str("/");
            ty = t_str();
            return true;
        }
        if (member == "linesep") {
            out = const_str("\n");
            ty = t_str();
            return true;
        }
        // access() 的模式位（POSIX 取值）
        if (member == "F_OK") {
            out = const_int(0);
            ty = t_int();
            return true;
        }
        if (member == "X_OK") {
            out = const_int(1);
            ty = t_int();
            return true;
        }
        if (member == "W_OK") {
            out = const_int(2);
            ty = t_int();
            return true;
        }
        if (member == "R_OK") {
            out = const_int(4);
            ty = t_int();
            return true;
        }
    }
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

// random.randrange / getrandbits / gauss / expovariate / triangular /
// sample / choices：这几个运行时函数带一个 rng 首参（模块级传 0，表示用
// 全局 rng），签名里表达不了，所以参数在这里手工装配。
VReg Lowerer::lower_random_extra(const std::string& member, Call& c) {
    // choices 的 k 是关键字参数，其它几个不收关键字。
    if (!c.keywords.empty() && member != "choices") {
        err(c.line, c.col, std::format("random.{}() does not take keyword arguments", member));
    }
    auto iarg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    auto farg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_float());
    };
    if (member == "randrange") {
        if (c.args.size() < 1 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("random.randrange() takes 1 to 3 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg a = const_int(0);
        VReg b = const_int(0);
        VReg step = const_int(1);
        if (c.args.size() == 1) {
            b = iarg(0);
        } else {
            a = iarg(0);
            b = iarg(1);
            if (c.args.size() == 3) step = iarg(2);
        }
        return call_runtime(RtFn::RandRange, t_int(), {const_int(0), a, b, step});
    }
    if (member == "getrandbits") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "random.getrandbits() takes exactly one argument");
            return const_none();
        }
        VReg k = iarg(0);
        return call_runtime(RtFn::RandGetrandbits, t_int(), {const_int(0), k});
    }
    if (member == "gauss") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "random.gauss() takes exactly two arguments");
            return const_none();
        }
        VReg mu = farg(0);
        VReg sigma = farg(1);
        return call_runtime(RtFn::RandGauss, t_float(), {const_int(0), mu, sigma});
    }
    if (member == "expovariate") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "random.expovariate() takes exactly one argument");
            return const_none();
        }
        VReg lambd = farg(0);
        return call_runtime(RtFn::RandExpovariate, t_float(), {const_int(0), lambd});
    }
    if (member == "triangular") {
        if (c.args.size() < 2 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("random.triangular() takes 2 or 3 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg lo = farg(0);
        VReg hi = farg(1);
        VReg mode;
        if (c.args.size() == 3) {
            mode = farg(2);
        } else {
            // 没给 mode 时取中点 (low + high) / 2
            Inst add;
            add.op = IrOp::Bin;
            add.bop = BinKind::Add;
            add.a = lo;
            add.b = hi;
            VReg sum = def(t_float(), std::move(add));
            Inst div;
            div.op = IrOp::Bin;
            div.bop = BinKind::Div;
            div.a = sum;
            div.b = const_float(2.0);
            mode = def(t_float(), std::move(div));
        }
        return call_runtime(RtFn::RandTriangular, t_float(), {const_int(0), lo, hi, mode});
    }
    if (member == "sample" || member == "choices") {
        // sample 用位置参数 (seq, k)；choices 跟 CPython 一样用 k= 关键字。
        size_t want = member == "sample" ? 2 : 1;
        if (c.args.size() != want) {
            err(c.line, c.col,
                std::format("random.{}() takes {} positional argument{}, got {}", member, want,
                            want == 1 ? "" : "s", c.args.size()));
            return const_none();
        }
        VReg k;
        if (member == "sample") {
            k = iarg(1);
        } else if (!c.keywords.empty()) {
            if (c.keywords[0].arg != "k") {
                err(c.line, c.col, "random.choices() only takes the k= keyword");
                return const_none();
            }
            k = coerce(expr(*c.keywords[0].value),
                       tc_.type_of(c.keywords[0].value.get()), t_int());
        } else {
            k = const_int(1);
        }
        TypePtr st = tc_.type_of(c.args[0].get());
        TypePtr et = t_unknown();
        VReg seq;
        if (st && st->kind == TyKind::Str) {
            seq = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
            push_temp(seq);
            et = t_str();
        } else {
            seq = expr(*c.args[0]);
            if (st && st->kind == TyKind::List) {
                if (!st->sub.empty()) et = st->sub[0];
            } else if (st && st->kind == TyKind::Tuple) {
                if (!st->sub.empty()) {
                    et = st->sub[0];
                    for (size_t i = 1; i < st->sub.size(); ++i) et = join(et, st->sub[i]);
                }
            }
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::List) rt = mk_list(et);
        VReg v = call_runtime(member == "sample" ? RtFn::RandSample : RtFn::RandChoices, rt,
                              {const_int(0), seq, k});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("random.{} is not supported", member));
    return const_none();
}

VReg Lowerer::call_native(const std::string& module, const std::string& member, Call& c,
                          const FuncInfoPtr& info) {
    auto fn = native_fn(module, member);
    if (!fn) {
        err(c.line, c.col, std::format("{}.{} is not a supported native function", module, member));
        return const_none();
    }
    // random 里带 rng 首参的几个函数：参数在 lower_random_extra 里手工装配。
    if (module == "random") {
        if (member == "randrange" || member == "getrandbits" || member == "gauss" ||
            member == "expovariate" || member == "triangular" || member == "sample" ||
            member == "choices") {
            return lower_random_extra(member, c);
        }
    }
    // os.makedirs(name, exist_ok=False)：exist_ok 可选，参数在这里手工装配。
    if (module == "os" && member == "makedirs") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col,
                std::format("os.makedirs() takes 1 or 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg p = expr(*c.args[0]);
        p = coerce(p, tc_.type_of(c.args[0].get()), t_str());
        VReg ok = const_bool(false);
        if (c.args.size() == 2)
            ok = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_bool());
        call_void(RtFn::OsMakedirs, {p, ok});
        return const_none();
    }
    // random.choice 返回序列元素类型，FuncInfo 的静态签名表达不了；
    // 结果类型以类型检查阶段记录在 Call 上的类型为准。
    if (module == "random" && member == "choice") {        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "random.choice() expects 1 positional argument");
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
        // 运行时返回借用元素；当元素是堆对象时补一个引用进临时栈。
        VReg v = call_runtime(*fn, elem ? elem : t_unknown(), {seq});
        if (ir_is_heap(elem)) {
            rc_inc(v);
            push_temp(v);
        }
        return v;
    }
    std::vector<VReg> args = bind_args(c, info, nullptr);
    if (module == "re" && (member == "search" || member == "match" || member == "fullmatch")) {
        int64_t mode = member == "search" ? 0 : member == "match" ? 1 : 2;
        args.push_back(const_int(mode));
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind == TyKind::Unknown) rt = mk_optional(t_match());
        VReg v = call_runtime(RtFn::RegexSearchStr, rt, std::move(args));
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }
    if (*fn == RtFn::SysExit) {
        call_void(*fn, args);
        return const_none();
    }
    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind == TyKind::Unknown)
        rt = info && info->ret ? info->ret : t_float();
    VReg v = call_runtime(*fn, rt, std::move(args));
    if (ir_is_heap(rt)) push_temp(v);
    return v;
}

VReg Lowerer::lower_itertools(const std::string& member, Call& c) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("itertools.{}() does not take keyword arguments", member));
    }
    if (member == "chain") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.chain() expects 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg a = expr(*c.args[0]);
        VReg b = expr(*c.args[1]);
        VReg v = call_runtime(RtFn::IterChain, rt, {a, b, const_int(list_kind(rt))});
        push_temp(v);
        return v;
    }
    if (member == "islice") {
        if (c.args.size() != 2 && c.args.size() != 3) {
            err(c.line, c.col,
                std::format("itertools.islice() expects 2 or 3 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        VReg x = expr(*c.args[0]);
        VReg start = const_int(0);
        VReg stop = const_int(0);
        if (c.args.size() == 2) {
            stop = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        } else {
            start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            stop = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_int());
        }
        TypePtr rt = tc_.type_of(&c);
        VReg v = call_runtime(RtFn::ListSlice, rt, {x, start, stop, const_int(1)});
        push_temp(v);
        return v;
    }
    if (member == "repeat") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.repeat() expects 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg v = expr(*c.args[0]);
        VReg n = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        VReg out = call_runtime(RtFn::IterRepeat, rt, {v, n, const_int(list_kind(rt))});
        push_temp(out);
        return out;
    }
    err(c.line, c.col, std::format("itertools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_collections(const std::string& member, Call& c) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("collections.{}() does not take keyword arguments", member));
    }
    if (member == "Counter") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("collections.Counter() expects 1 argument, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr at = tc_.type_of(c.args[0].get());
        TypePtr et = (at && at->kind == TyKind::List && !at->sub.empty()) ? at->sub[0]
                                                                          : t_unknown();
        VReg items = expr(*c.args[0]);
        TypePtr rt = tc_.type_of(&c);
        VReg v = call_runtime(RtFn::CollCounter, rt,
                              {items, const_int(key_kind(et)), const_int(own_kind(et))});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("collections.{} is not supported", member));
    return const_none();
}

bool Lowerer::json_desc(const TypePtr& t, std::string& out, int line, int col) {
    if (!t || t->kind == TyKind::Unknown) {
        err(line, col, "json needs a statically known type");
        return false;
    }
    switch (t->kind) {
        case TyKind::None: out = "n"; return true;
        case TyKind::Bool: out = "b"; return true;
        case TyKind::Int: out = "i"; return true;
        case TyKind::Float: out = "f"; return true;
        case TyKind::Str: out = "s"; return true;
        case TyKind::List: {
            if (t->sub.empty()) {
                err(line, col, "json: list element type is unknown");
                return false;
            }
            std::string inner;
            if (!json_desc(t->sub[0], inner, line, col)) return false;
            out = "(l" + inner + ")";
            return true;
        }
        case TyKind::Dict: {
            if (t->sub.size() < 2) {
                err(line, col, "json: dict type is unknown");
                return false;
            }
            if (t->sub[0] && t->sub[0]->kind != TyKind::Str) {
                err(line, col,
                    std::format("json dict keys must be str, got '{}'", type_name(t->sub[0])));
                return false;
            }
            std::string inner;
            if (!json_desc(t->sub[1], inner, line, col)) return false;
            out = "(d" + inner + ")";
            return true;
        }
        case TyKind::Tuple: {
            if (t->variadic) {
                err(line, col, "json does not support variable-length tuples");
                return false;
            }
            std::string s = "(t";
            for (auto& sub : t->sub) {
                std::string inner;
                if (!json_desc(sub, inner, line, col)) return false;
                s += inner;
            }
            s += ")";
            out = std::move(s);
            return true;
        }
        default:
            err(line, col,
                std::format("json does not support type '{}'", type_name(t)));
            return false;
    }
}

VReg Lowerer::lower_functools(const std::string& member, Call& c) {
    if (member == "reduce") {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "functools.reduce() does not take keyword arguments");
        }
        if (c.args.size() < 2 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("functools.reduce() expects 2 or 3 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr ft = tc_.type_of(c.args[0].get());
        if (ft && ft->kind != TyKind::Func && ft->kind != TyKind::BoundMethod &&
            ft->kind != TyKind::Unknown) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("functools.reduce() expects a function, got '{}'", type_name(ft)));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt) rt = t_unknown();
        VReg f = expr(*c.args[0]);
        TypePtr elem = t_unknown();
        VReg xs = seq_as_list(c.args[1].get(), elem);
        bool use_fp = rt->kind == TyKind::Float;
        // 运行时按结果域读元素：float 版把列表当 double 数组，元素域必须
        // 一致，否则会读出错误的位模式。
        if (use_fp && elem && elem->kind != TyKind::Float && elem->kind != TyKind::Unknown) {
            err(c.args[1]->line, c.args[1]->col,
                "reduce() over mixed int/float is not supported yet");
            return const_none();
        }
        bool has_init = c.args.size() == 3;
        VReg init = use_fp ? const_float(0.0) : const_int(0);
        if (has_init)
            init = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), rt);
        VReg v = call_runtime(use_fp ? RtFn::FuncReduceF : RtFn::FuncReduce, rt,
                              {f, xs, init, const_int(has_init ? 1 : 0)});
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }
    if (member == "wraps") {
        // py23 不建模 __name__/__doc__，@functools.wraps(f) 这一层在装饰器
        // 处理里直接跳过；当普通函数调用说明用户想拿返回值，做不了。
        err(c.line, c.col, "functools.wraps() is only supported as a decorator");
        for (auto& a : c.args) expr(*a);
        return const_none();
    }
    if (member == "partial" || member == "lru_cache") {
        err(c.line, c.col, std::format("functools.{}() is not supported yet", member));
        for (auto& a : c.args) expr(*a);
        for (auto& kw : c.keywords) expr(*kw.value);
        return const_none();
    }
    err(c.line, c.col, std::format("functools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_datetime(const std::string& member, Call& c) {
    if (!c.keywords.empty()) {
        err(c.line, c.col, std::format("datetime.{}() does not take keyword arguments", member));
    }
    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind != TyKind::DateTime) rt = new_ty(TyKind::DateTime);
    if (member == "now" || member == "today") {
        if (!c.args.empty())
            err(c.line, c.col, std::format("datetime.{}() takes no arguments", member));
        VReg v = call_runtime(RtFn::DateTimeNow, rt, {const_int(member == "today" ? 1 : 0)});
        push_temp(v);
        return v;
    }
    // 实参按从左到右求值（C++ 不保证花括号里的求值顺序，必须分开写）。
    auto iarg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    if (member == "date") {
        if (c.args.size() != 3) {
            err(c.line, c.col, "datetime.date() takes 3 arguments (year, month, day)");
            return const_none();
        }
        VReg y = iarg(0);
        VReg mo = iarg(1);
        VReg d = iarg(2);
        VReg v = call_runtime(RtFn::DateNew, rt, {y, mo, d});
        push_temp(v);
        return v;
    }
    if (member == "datetime") {
        if (c.args.size() != 3 && c.args.size() != 6) {
            err(c.line, c.col, "datetime.datetime() takes 3 or 6 arguments");
            return const_none();
        }
        VReg y = iarg(0);
        VReg mo = iarg(1);
        VReg d = iarg(2);
        VReg h = const_int(0);
        VReg mi = const_int(0);
        VReg s = const_int(0);
        if (c.args.size() == 6) {
            h = iarg(3);
            mi = iarg(4);
            s = iarg(5);
        }
        VReg v = call_runtime(RtFn::DateTimeNew, rt, {y, mo, d, h, mi, s});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("datetime.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_pathlib(const std::string& member, Call& c) {
    if (member == "Path") {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "pathlib.Path() does not take keyword arguments");
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, "pathlib.Path() takes exactly one argument");
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Path) rt = new_ty(TyKind::Path);
        VReg s = expr(*c.args[0]);
        s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
        VReg v = call_runtime(RtFn::PathNew, rt, {s});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("pathlib.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_json(const std::string& member, Call& c) {
    // dumps 收 indent= / sort_keys=，其它几个不收关键字。
    if (!c.keywords.empty() && member != "dumps") {
        err(c.line, c.col, std::format("json.{}() does not take keyword arguments", member));
    }
    if (member == "dumps") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("json.dumps() expects 1 positional argument, got {}", c.args.size()));
            return const_none();
        }
        // indent= / sort_keys= 是关键字参数；indent 用 0 表示紧凑（对应 None）。
        VReg indent = const_int(0);
        VReg sort_keys = const_int(0);
        for (auto& kw : c.keywords) {
            if (kw.arg == "indent") {
                indent = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
            } else if (kw.arg == "sort_keys") {
                sort_keys = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
            } else {
                err(kw.value->line, kw.value->col,
                    std::format("json.dumps() got an unexpected keyword argument '{}'", kw.arg));
            }
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        std::string desc;
        if (!json_desc(vt, desc, c.args[0]->line, c.args[0]->col)) return const_none();
        if (vt && vt->kind == TyKind::None) return const_str("null");
        VReg v = expr(*c.args[0]);
        VReg s = call_runtime(RtFn::JsonDumps, t_str(),
                              {v, const_str(desc), indent, sort_keys});
        push_temp(s);
        return s;
    }
    if (member == "loads") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("json.loads() expects 2 arguments (text, sample), got {}",
                            c.args.size()));
            return const_none();
        }
        // 样例值只提供类型，运行时不求值（类型检查阶段已经检查过）。
        TypePtr sample = tc_.type_of(c.args[1].get());
        std::string desc;
        if (!json_desc(sample, desc, c.args[1]->line, c.args[1]->col)) return const_none();
        VReg text = expr(*c.args[0]);
        VReg v = call_runtime(RtFn::JsonLoads, sample, {text, const_str(desc)});
        if (ir_is_heap(sample)) push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("json.{} is not supported", member));
    return const_none();
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
