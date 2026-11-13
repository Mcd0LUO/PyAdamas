// lower_import.cpp —— import / 模块别名与外部符号绑定
#include "lower_impl.h"

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
    if (module == "random") return name == "Random";
    return false;
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
        // 只为 from-import 登记别名；真正降级走 lower_popen。
        if (member == "Popen") return RtFn::PopenNew;
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

/* stdin/stdout/stderr 的取值：None → 0（继承），PIPE/STDOUT/DEVNULL 是模块
   常量，已经降级成 -1/-2/-3 的整数常量，这里直接取整数值。 */
VReg Lowerer::stream_mode(Expr& e) {
    VReg v = expr(e);
    TypePtr t = tc_.type_of(&e);
    if (t && t->kind == TyKind::None) return const_int(0);
    return coerce(v, t, t_int());
}

VReg Lowerer::lower_popen(Call& c) {
    if (c.args.size() != 1) {
        err(c.line, c.col,
            std::format("subprocess.Popen() takes exactly one positional argument, got {}",
                        c.args.size()));
        return const_none();
    }
    TypePtr at = tc_.type_of(c.args[0].get());
    VReg argv;
    if (at && at->kind == TyKind::Str) {
        // Popen("cmd", shell=True)：字符串当单条命令，包成单元素列表。
        VReg s = expr(*c.args[0]);
        argv = call_runtime(RtFn::StrWrapList, mk_list(t_str()), {s});
        push_temp(argv);
    } else {
        if (at && at->kind != TyKind::List && at->kind != TyKind::Unknown)
            err(c.args[0]->line, c.args[0]->col,
                std::format("subprocess.Popen() expects a list of str or a str, got '{}'",
                            type_name(at)));
        argv = expr(*c.args[0]);
    }

    VReg shell = const_bool(false);
    VReg cwd = const_str("");
    VReg in_mode = const_int(0);
    VReg out_mode = const_int(0);
    VReg err_mode = const_int(0);
    for (auto& kw : c.keywords) {
        if (kw.arg == "shell") {
            shell = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "cwd") {
            TypePtr vt = tc_.type_of(kw.value.get());
            VReg v = expr(*kw.value);
            cwd = (vt && vt->kind == TyKind::None) ? const_str("") : coerce(v, vt, t_str());
        } else if (kw.arg == "stdin") {
            in_mode = stream_mode(*kw.value);
        } else if (kw.arg == "stdout") {
            out_mode = stream_mode(*kw.value);
        } else if (kw.arg == "stderr") {
            err_mode = stream_mode(*kw.value);
        } else if (kw.arg == "text") {
            // PyAdamas 的 subprocess 始终返回文本：text 只做求值，不影响行为。
            expr(*kw.value);
        } else {
            err(kw.value->line, kw.value->col,
                std::format("subprocess.Popen() got an unexpected keyword argument '{}'",
                            kw.arg));
        }
    }

    TypePtr rt = new_ty(TyKind::Popen);
    VReg v = call_runtime(RtFn::PopenNew, rt,
                          {argv, shell, cwd, in_mode, out_mode, err_mode});
    push_temp(v);
    return v;
}

/* subprocess.run("cmd", shell=True)：第一个位置参数是字符串时，包成单元素
   列表再走和列表一样的签名。列表参数走通用路径（bind_args）。 */
VReg Lowerer::lower_subprocess_run(Call& c) {
    VReg s = expr(*c.args[0]);
    VReg argv = call_runtime(RtFn::StrWrapList, mk_list(t_str()), {s});
    push_temp(argv);

    VReg capture = const_bool(false);
    VReg text = const_bool(false);
    VReg timeout = const_float(-1.0);
    VReg check = const_bool(false);
    VReg shell = const_bool(false);
    VReg cwd = const_str("");
    if (c.args.size() > 1)
        err(c.args[1]->line, c.args[1]->col,
            "subprocess.run() 的其余参数要用关键字传（capture_output= / shell= 等）");
    for (auto& kw : c.keywords) {
        if (kw.arg == "capture_output") {
            capture = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "text") {
            text = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "timeout") {
            timeout = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_float());
        } else if (kw.arg == "check") {
            check = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "shell") {
            shell = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "cwd") {
            TypePtr vt = tc_.type_of(kw.value.get());
            VReg v = expr(*kw.value);
            cwd = (vt && vt->kind == TyKind::None) ? const_str("") : coerce(v, vt, t_str());
        } else {
            err(kw.value->line, kw.value->col,
                std::format("subprocess.run() got an unexpected keyword argument '{}'",
                            kw.arg));
        }
    }

    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind != TyKind::CompletedProcess) rt = t_completed();
    VReg v = call_runtime(RtFn::SubprocessRun, rt,
                          {argv, capture, text, timeout, check, shell, cwd});
    push_temp(v);
    return v;
}

VReg Lowerer::call_native(const std::string& module, const std::string& member, Call& c,
                          const FuncInfoPtr& info) {
    if (module == "subprocess" && member == "Popen") return lower_popen(c);
    if (module == "subprocess" && member == "run" && !c.args.empty()) {
        TypePtr at = tc_.type_of(c.args[0].get());
        if (at && at->kind == TyKind::Str) return lower_subprocess_run(c);
    }
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
    // 把若干个列表实参装进一个临时 kind 0 列表，交给运行时的变参辅助函数。
    // kind 0 表示不持有元素引用：临时列表销毁时不会去 dec 源列表。
    auto pack_seqs = [&](size_t from, size_t to) {
        VReg seqs = call_runtime(RtFn::ListNew, mk_list(t_unknown()),
                                 {const_int(0), const_int(0)});
        push_temp(seqs);
        for (size_t i = from; i < to; ++i) {
            VReg v = expr(*c.args[i]);
            call_void(RtFn::ListAppend, {seqs, v});
        }
        return seqs;
    };
    // 结果列表的元素是元组：外层列表类别固定为 1，这里算的是元组内部的
    // 元素类别（list_kind 对 Tuple 类型会按各字段合并）。
    auto tuple_elem_kind = [&](const TypePtr& rt) {
        if (rt && rt->kind == TyKind::List && !rt->sub.empty()) return list_kind(rt->sub[0]);
        return 2;
    };
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
    if (member == "product") {
        TypePtr rt = tc_.type_of(&c);
        VReg seqs = pack_seqs(0, c.args.size());
        VReg out = call_runtime(RtFn::IterProduct, rt,
                                {seqs, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "permutations" || member == "combinations" ||
        member == "combinations_with_replacement") {
        bool ok = member == "permutations" ? (c.args.size() == 1 || c.args.size() == 2)
                                           : c.args.size() == 2;
        if (!ok) {
            err(c.line, c.col,
                std::format("itertools.{}() expects {} arguments, got {}", member,
                            member == "permutations" ? "1 or 2" : "2", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg pool = expr(*c.args[0]);
        VReg r = const_int(-1);
        if (c.args.size() > 1)
            r = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        RtFn fn = member == "permutations"          ? RtFn::IterPermutations
                  : member == "combinations"        ? RtFn::IterCombinations
                                                    : RtFn::IterCombinationsWR;
        VReg out = call_runtime(fn, rt, {pool, r, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "zip_longest") {
        if (c.args.empty()) {
            err(c.line, c.col, "itertools.zip_longest() expects at least 1 argument");
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg seqs = pack_seqs(0, c.args.size());
        VReg out = call_runtime(RtFn::IterZipLongest, rt,
                                {seqs, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "accumulate") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("itertools.accumulate() expects 1 argument, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        TypePtr et = rt && rt->kind == TyKind::List && !rt->sub.empty() ? rt->sub[0]
                                                                        : t_unknown();
        VReg xs = expr(*c.args[0]);
        VReg out = call_runtime(RtFn::IterAccumulate, rt,
                                {xs, const_int(et && et->kind == TyKind::Float ? 1 : 0)});
        push_temp(out);
        return out;
    }
    err(c.line, c.col, std::format("itertools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::build_itemgetter(Call& c, TypePtr fty) {
    std::vector<VReg> keys;
    std::vector<TypePtr> keytys;
    keys.reserve(c.args.size());
    keytys.reserve(c.args.size());
    for (auto& a : c.args) {
        TypePtr kt = tc_.type_of(a.get());
        keys.push_back(expr(*a));
        keytys.push_back(kt ? kt : t_unknown());
    }
    int nkey = static_cast<int>(keys.size());
    if (nkey == 0) {
        err(c.line, c.col, "operator.itemgetter() expects at least 1 argument");
        return const_none();
    }

    // 合成 getter 函数：obj 是 unknown 参数，key 从闭包 env 的 cell 里读。
    std::string label = std::format("__pa_itemgetter_{}", mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = t_unknown();
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);
    int oslot = wf->new_slot(t_unknown());
    wf->params.push_back(IRParam{"obj", t_unknown(), oslot});
    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    VReg env = load_slot(es, t_unknown());
    VReg obj = load_slot(oslot, t_unknown());
    std::vector<VReg> vals;
    vals.reserve(static_cast<size_t>(nkey));
    for (int i = 0; i < nkey; ++i) {
        VReg cell = field_load(env, 16 + 8 * i, mk_list(t_unknown()));
        Inst ik;
        ik.op = IrOp::IndexLoad;
        ik.a = cell;
        ik.b = const_int(0);
        VReg key = def(t_unknown(), std::move(ik));
        Inst ld;
        ld.op = IrOp::IndexLoad;
        ld.a = obj;
        ld.b = key;
        vals.push_back(def(t_unknown(), std::move(ld)));
    }
    VReg out;
    if (nkey == 1) {
        out = vals[0];
    } else {
        // 多键返回元组；元素是容器内部的借用引用，入元组前 retain。
        for (auto& v : vals) retain_elem(v, 2);
        Inst mk;
        mk.op = IrOp::MakeTuple;
        mk.imm = 2;
        mk.args = std::move(vals);
        out = def_heap(t_unknown(), std::move(mk));
    }
    emit_return(out, t_unknown());
    pop_frame();

    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, fty, {code, const_int(nkey)});
    push_temp(closure);
    for (int i = 0; i < nkey; ++i) {
        VReg cell = make_cell_value(keys[static_cast<size_t>(i)],
                                    keytys[static_cast<size_t>(i)]);
        rc_inc(cell);
        field_store(closure, 16 + 8 * i, cell);
    }
    return closure;
}

VReg Lowerer::lower_operator(const std::string& member, Call& c) {
    if (!c.keywords.empty())
        err(c.line, c.col,
            std::format("operator.{}() does not take keyword arguments", member));
    TypePtr rt = tc_.type_of(&c);
    auto argty = [&](size_t i) { return tc_.type_of(c.args[i].get()); };
    auto eval_all = [&]() {
        for (auto& a : c.args) (void)expr(*a);
    };
    static const std::unordered_map<std::string, Tok> kBin = {
        {"add", Tok::PLUS},           {"sub", Tok::MINUS},
        {"mul", Tok::STAR},           {"truediv", Tok::SLASH},
        {"floordiv", Tok::DOUBLESLASH}, {"mod", Tok::PERCENT},
        {"pow", Tok::DOUBLESTAR},     {"and_", Tok::AMPER},
        {"or_", Tok::VBAR},           {"xor", Tok::CIRCUMFLEX},
        {"lshift", Tok::LSHIFT},      {"rshift", Tok::RSHIFT},
        {"concat", Tok::PLUS},
    };
    static const std::unordered_map<std::string, CmpOp> kCmp = {
        {"eq", CmpOp::Eq}, {"ne", CmpOp::Ne}, {"lt", CmpOp::Lt},
        {"le", CmpOp::Le}, {"gt", CmpOp::Gt}, {"ge", CmpOp::Ge},
    };

    if (member == "itemgetter") {
        TypePtr fty = rt && rt->kind == TyKind::Func ? rt : nullptr;
        if (!fty) {
            eval_all();
            err(c.line, c.col, "operator.itemgetter() is not callable here");
            return const_none();
        }
        return build_itemgetter(c, fty);
    }
    if (member == "attrgetter") {
        eval_all();
        err(c.line, c.col, "operator.attrgetter() is not supported yet");
        return const_none();
    }
    if (auto it = kBin.find(member); it != kBin.end()) {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_none();
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        TypePtr lt = argty(0);
        TypePtr rty = argty(1);
        if ((lt && (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta)) ||
            (rty && (rty->kind == TyKind::DateTime || rty->kind == TyKind::TimeDelta))) {
            err(c.line, c.col,
                std::format("operator.{}() does not support datetime/timedelta yet",
                            member));
            return const_none();
        }
        if ((lt && lt->kind == TyKind::Class) || (rty && rty->kind == TyKind::Class)) {
            VReg dr = lower_dunder_binop(l, lt, r, rty, it->second, false);
            if (dr != kNoReg) return dr;
        }
        // operator.mul 也支持序列重复。
        if (member == "mul") {
            auto seq = [](const TypePtr& t) {
                return t && (t->kind == TyKind::Str || t->kind == TyKind::List ||
                             t->kind == TyKind::Tuple);
            };
            if (seq(lt) || seq(rty)) {
                bool left = seq(lt);
                VReg base = left ? l : r;
                VReg n = coerce(left ? r : l, left ? rty : lt, t_int());
                RtFn fn = rt && rt->kind == TyKind::Str ? RtFn::StrRepeat : RtFn::ListRepeat;
                VReg v = call_runtime(fn, rt, {base, n});
                push_temp(v);
                return v;
            }
        }
        // 和普通二元运算保持一致：容器拼接后端还没有对应指令。
        if (rt && (rt->kind == TyKind::List || rt->kind == TyKind::Tuple ||
                   rt->kind == TyKind::Set || rt->kind == TyKind::Dict)) {
            err(c.line, c.col,
                std::format("operator.{}() on '{}' is not supported by the code generator "
                            "yet",
                            member, type_name(rt)));
            return const_none();
        }
        if (rt && rt->kind == TyKind::Str && member != "add" && member != "concat") {
            err(c.line, c.col,
                std::format("operator.{}() on 'str' is not supported", member));
            return const_none();
        }
        if (rt && rt->kind == TyKind::Float) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rty)) r = convert_to(r, rty, t_float());
        }
        Inst in;
        in.op = IrOp::Bin;
        in.bop = map_bin(it->second);
        in.a = l;
        in.b = r;
        return def_heap(rt, std::move(in));
    }
    if (auto it = kCmp.find(member); it != kCmp.end()) {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        TypePtr lt = argty(0);
        TypePtr rty = argty(1);
        CmpOp op = it->second;
        if (lt && rty && lt->kind == rty->kind &&
            (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta)) {
            RtFn fn = lt->kind == TyKind::TimeDelta ? RtFn::TimeDeltaCmp : RtFn::DateTimeCmp;
            VReg cv = call_runtime(fn, t_int(), {l, r});
            Inst in;
            in.op = IrOp::Cmp;
            in.cop = map_cmp(op);
            in.a = cv;
            in.b = const_int(0);
            return def(t_bool(), std::move(in));
        }
        if (op == CmpOp::Eq || op == CmpOp::Ne) {
            VReg eq = lower_dunder_eq(l, lt, r, rty, op == CmpOp::Ne);
            if (eq != kNoReg) return eq;
            VReg ceq = lower_container_eq(l, lt, r, rty);
            if (ceq != kNoReg) {
                if (op == CmpOp::Ne) {
                    Inst un;
                    un.op = IrOp::Un;
                    un.uop = UnKind::Not;
                    un.a = ceq;
                    return def(t_bool(), std::move(un));
                }
                return ceq;
            }
        } else {
            VReg dc = lower_dunder_cmp(l, lt, r, rty, op);
            if (dc != kNoReg) return dc;
        }
        if ((ir_is_float(lt) || ir_is_float(rty)) && is_num(lt) && is_num(rty)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rty)) r = convert_to(r, rty, t_float());
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
    }
    if (member == "is_" || member == "is_not") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        auto as_bits = [&](VReg v, TypePtr t) -> VReg {
            if (!t || !ir_is_heap(t)) return v;
            Inst mv;
            mv.op = IrOp::Mov;
            mv.a = v;
            return def(t_int(), std::move(mv));
        };
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = member == "is_" ? CmpKind::Eq : CmpKind::Ne;
        in.a = as_bits(l, argty(0));
        in.b = as_bits(r, argty(1));
        return def(t_bool(), std::move(in));
    }
    if (member == "truth" || member == "not_") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("operator.{}() expects 1 argument, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg v = expr(*c.args[0]);
        VReg t = truth_bool(v, argty(0));
        if (member == "truth") return t;
        Inst un;
        un.op = IrOp::Un;
        un.uop = UnKind::Not;
        un.a = t;
        return def(t_bool(), std::move(un));
    }
    if (member == "neg" || member == "pos" || member == "abs" || member == "invert") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("operator.{}() expects 1 argument, got {}", member,
                            c.args.size()));
            eval_all();
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr t = argty(0);
        if (member == "abs") {
            if (t && t->kind == TyKind::Float)
                return call_runtime(RtFn::MathFabs, t_float(), {v});
            if (t && t->kind == TyKind::Complex)
                return call_runtime(RtFn::ComplexAbs, t_float(), {v});
            return call_runtime(RtFn::AbsI64, t_int(), {v});
        }
        if (member == "pos") return v;
        if (member == "neg" && t && t->kind == TyKind::Complex) {
            VReg out = call_runtime(RtFn::ComplexNeg, rt, {v});
            push_temp(out);
            return out;
        }
        if (member == "neg" && t && t->kind == TyKind::TimeDelta) {
            VReg out = call_runtime(RtFn::TimeDeltaNeg, rt, {v});
            push_temp(out);
            return out;
        }
        Inst in;
        in.op = IrOp::Un;
        in.uop = member == "neg" ? UnKind::Neg : UnKind::BitNot;
        in.a = v;
        return def(rt, std::move(in));
    }
    if (member == "contains") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.contains() expects 2 arguments, got {}",
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg cont = expr(*c.args[0]);
        VReg item = expr(*c.args[1]);
        TypePtr ct = argty(0);
        TypePtr ity = argty(1);
        if (ct && (ct->kind == TyKind::List || ct->kind == TyKind::Tuple)) {
            TypePtr et = ct->sub.empty() ? t_unknown() : ct->sub[0];
            item = coerce(item, ity, et);
            int ck = key_kind(et);
            if (ct->kind == TyKind::Tuple)
                for (auto& sub : ct->sub)
                    if (key_kind(sub) != ck) {
                        ck = 3;
                        break;
                    }
            return call_runtime(RtFn::ListContains, t_bool(),
                                {cont, item, const_int(ck)});
        }
        if (ct && (ct->kind == TyKind::Dict || ct->kind == TyKind::Set)) {
            TypePtr kt = ct->sub.empty() ? t_unknown() : ct->sub[0];
            item = coerce(item, ity, kt);
            return call_runtime(RtFn::DictContains, t_bool(), {cont, item});
        }
        if (ct && (ct->kind == TyKind::Str || ct->kind == TyKind::Bytes))
            return call_runtime(RtFn::StrContains, t_bool(), {cont, item});
        if (ct && ct->kind == TyKind::Range) {
            item = coerce(item, ity, t_int());
            return call_runtime(RtFn::RangeContains, t_bool(), {cont, item});
        }
        err(c.line, c.col, std::format("operator.contains() on '{}' is not supported",
                                       type_name(ct)));
        return const_bool(false);
    }
    // getitem
    if (c.args.size() != 2) {
        err(c.line, c.col,
            std::format("operator.getitem() expects 2 arguments, got {}", c.args.size()));
        eval_all();
        return const_none();
    }
    VReg obj = expr(*c.args[0]);
    VReg idx = expr(*c.args[1]);
    TypePtr ot = argty(0);
    TypePtr ity = argty(1);
    if (ot && ot->kind == TyKind::Dict) {
        TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
        idx = coerce(idx, ity, kt);
        return call_runtime(RtFn::DictGet, rt, {obj, idx});
    }
    if (ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple ||
               ot->kind == TyKind::Str)) {
        idx = coerce(idx, ity, t_int());
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = obj;
        in.b = idx;
        if (ot->kind == TyKind::Str && rt && rt->kind == TyKind::Str)
            return def_heap(rt, std::move(in));
        return def(rt, std::move(in));
    }
    err(c.line, c.col,
        std::format("operator.getitem() on '{}' is not supported", type_name(ot)));
    return const_none();
}

VReg Lowerer::lower_collections(const std::string& member, Call& c) {
    // deque 收 maxlen= 关键字，其它几个不收。
    if (!c.keywords.empty() && member != "deque") {
        err(c.line, c.col,
            std::format("collections.{}() does not take keyword arguments", member));
    }
    if (member == "defaultdict") {
        if (c.args.size() > 2) {
            err(c.line, c.col,
                std::format("collections.defaultdict() expects at most 2 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        int64_t factory = 0;
        if (!c.args.empty()) {
            auto* n = dynamic_cast<Name*>(c.args[0].get());
            auto* none = dynamic_cast<Constant*>(c.args[0].get());
            if (n && is_builtin(n->id)) {
                if (n->id == "int") factory = 1;
                else if (n->id == "float") factory = 2;
                else if (n->id == "bool") factory = 3;
                else if (n->id == "str") factory = 4;
                else if (n->id == "list") factory = 5;
                else if (n->id == "dict") factory = 6;
                else if (n->id == "set") factory = 7;
                else if (n->id == "tuple") factory = 8;
                else {
                    expr(*c.args[0]);
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("collections.defaultdict() factory '{}' is not supported",
                                    n->id));
                }
            } else if (none && none->ck == Constant::CK::None) {
                expr(*c.args[0]);
            } else {
                expr(*c.args[0]);
                err(c.args[0]->line, c.args[0]->col,
                    "collections.defaultdict() currently supports built-in factories only");
            }
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Dict) rt = mk_dict(t_unknown(), t_unknown());
        DictExpr empty;
        empty.line = c.line;
        empty.col = c.col;
        VReg dict = lower_dict(empty, rt);
        if (factory) call_void(RtFn::DictSetDefaultFactory, {dict, const_int(factory)});
        if (c.args.size() == 2) {
            VReg init = expr(*c.args[1]);
            call_void(RtFn::DictUpdate, {dict, init});
        }
        return dict;
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
    if (member == "OrderedDict") {
        // pyadamas 的 dict 本来就按插入序保存，OrderedDict 直接当空 dict 用。
        if (!c.args.empty()) {
            err(c.line, c.col, "collections.OrderedDict() takes no arguments in pyadamas");
            return const_none();
        }
        DictExpr empty;
        empty.line = c.line;
        empty.col = c.col;
        return lower_dict(empty, mk_dict(t_str(), t_unknown()));
    }
    if (member == "deque") {
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Deque) rt = new_ty(TyKind::Deque);
        VReg maxlen = const_int(0);
        for (auto& kw : c.keywords) {
            if (kw.arg == "maxlen") {
                maxlen = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
            } else {
                err(kw.value->line, kw.value->col,
                    std::format("collections.deque() got an unexpected keyword argument '{}'",
                                kw.arg));
            }
        }
        if (c.args.size() > 2) {
            err(c.line, c.col,
                "collections.deque() takes at most 2 positional arguments");
            return const_none();
        }
        if (c.args.size() == 2) {
            maxlen = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        }
        if (!c.args.empty()) {
            TypePtr at = tc_.type_of(c.args[0].get());
            VReg items;
            if (at && at->kind == TyKind::Str) {
                items = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
                push_temp(items);
            } else {
                items = expr(*c.args[0]);
            }
            VReg v = call_runtime(RtFn::DequeFromList, rt, {items, maxlen});
            push_temp(v);
            return v;
        }
        VReg v = call_runtime(RtFn::DequeNew, rt, {maxlen});
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

// 原始形参下标（pos 在前，kwonly 从 pos.size() 起）→ 类型。
static TypePtr fn_param_ty(const FuncInfoPtr& fn, int idx) {
    int n = static_cast<int>(fn->pos.size());
    if (idx < n) return fn->pos[static_cast<size_t>(idx)].ty;
    return fn->kwonly[static_cast<size_t>(idx - n)].ty;
}

static const std::string& fn_param_name(const FuncInfoPtr& fn, int idx) {
    int n = static_cast<int>(fn->pos.size());
    if (idx < n) return fn->pos[static_cast<size_t>(idx)].name;
    return fn->kwonly[static_cast<size_t>(idx - n)].name;
}

// 闭包捕获的值必须是 cell（单元素 list[T]），析构时由闭包统一 dec。
VReg Lowerer::make_cell_value(VReg v, TypePtr T) {
    TypePtr cell_ty = mk_list(T ? T : t_unknown());
    int kind = list_kind(cell_ty);
    retain_elem(v, kind);
    Inst in;
    in.op = IrOp::MakeList;
    in.imm = kind;
    in.args.push_back(v);
    return def_heap(cell_ty, std::move(in));
}

// functools.partial：合成一个转发函数，把 f 和绑定实参放进闭包 cell，
// 其余形参原样转发。返回的闭包签名就是 PartialPlan::sig。
VReg Lowerer::build_partial_wrapper(Call& c, VReg fnv, TypePtr fty,
                                    const PartialPlan& plan, int line, int col) {
    FuncInfoPtr fn = (fty && fty->kind == TyKind::Func) ? fty->fn : nullptr;
    if (!fn || !plan.sig) {
        err(line, col, "functools.partial() requires a function with a known signature");
        return const_none();
    }
    FuncInfoPtr sig = plan.sig;
    int npos = static_cast<int>(fn->pos.size());
    int ntotal = npos + static_cast<int>(fn->kwonly.size());
    std::vector<int> cap_of_param(static_cast<size_t>(ntotal), -1);
    for (size_t i = 0; i < plan.bound_params.size(); ++i)
        cap_of_param[static_cast<size_t>(plan.bound_params[i])] = static_cast<int>(i) + 1;

    // 实参求值顺序与 CPython 一致：f 之后先位置参数，再关键字参数。
    std::vector<Expr*> arg_exprs;
    for (size_t i = 1; i < c.args.size(); ++i) arg_exprs.push_back(c.args[i].get());
    for (auto& kw : c.keywords) arg_exprs.push_back(kw.value.get());
    std::vector<VReg> bound_vals;
    std::vector<TypePtr> bound_tys;
    for (size_t i = 0; i < arg_exprs.size(); ++i) {
        VReg v = expr(*arg_exprs[i]);
        if (i >= plan.bound_params.size()) continue;
        TypePtr want = fn_param_ty(fn, plan.bound_params[i]);
        v = coerce(v, tc_.type_of(arg_exprs[i]), want);
        bound_vals.push_back(v);
        bound_tys.push_back(want ? want : t_unknown());
    }
    if (plan.bound_params.empty()) return fnv;  // partial(f) 就是 f

    std::string label = std::format("__pa_partial_{}_{}", fn->name, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    TypePtr rty = fn->ret ? fn->ret : t_unknown();
    wf->ret = rty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    // 暴露给调用方的形参：按 sig 的顺序分配槽，再映射回原始形参下标。
    std::vector<int> slot_of_param(static_cast<size_t>(ntotal), -1);
    size_t rem_i = 0;
    auto add_sig_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        if (rem_i < plan.remaining.size())
            slot_of_param[static_cast<size_t>(plan.remaining[rem_i++])] = s;
    };
    for (auto& p : sig->pos) add_sig_param(p);
    for (auto& p : sig->kwonly) add_sig_param(p);

    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    auto read_cap = [&](int cap, TypePtr T) {
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16 + 8 * cap, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    };

    VReg callee = read_cap(0, fty);
    std::vector<VReg> call_args;
    call_args.reserve(static_cast<size_t>(ntotal));
    for (int i = 0; i < ntotal; ++i) {
        TypePtr T = fn_param_ty(fn, i);
        if (cap_of_param[static_cast<size_t>(i)] > 0)
            call_args.push_back(read_cap(cap_of_param[static_cast<size_t>(i)], T));
        else if (slot_of_param[static_cast<size_t>(i)] >= 0)
            call_args.push_back(load_slot(slot_of_param[static_cast<size_t>(i)], T));
        else
            call_args.push_back(const_none());
    }
    Inst call;
    call.op = IrOp::Call;
    call.a = callee;
    call.args = std::move(call_args);
    VReg r = def_heap(rty, std::move(call));
    emit_return(r, rty);
    pop_frame();

    TypePtr out_ty = tc_.type_of(&c);
    if (!out_ty || out_ty->kind != TyKind::Func) {
        out_ty = new_ty(TyKind::Func);
        out_ty->fn = sig;
    }
    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(
        RtFn::ClosureNew, out_ty,
        {code, const_int(static_cast<int64_t>(1 + bound_vals.size()))});
    push_temp(closure);
    VReg fcell = make_cell_value(fnv, fty);
    rc_inc(fcell);
    field_store(closure, 16, fcell);
    for (size_t i = 0; i < bound_vals.size(); ++i) {
        VReg cell = make_cell_value(bound_vals[i], bound_tys[i]);
        rc_inc(cell);
        field_store(closure, 16 + 8 * (static_cast<int>(i) + 1), cell);
    }
    return closure;
}

// @functools.lru_cache：缓存的键按形参逐个套一层 dict，最后一级存返回值。
// maxsize 只求值不参与淘汰（缓存无上限）；键类型必须是 int/bool/float/str，
// unknown 按机器字（和 dict 的既有约定一致）。
VReg Lowerer::build_lru_wrapper(const std::string& base, VReg fnv, TypePtr fty,
                                int line, int col) {
    FuncInfoPtr fn = (fty && fty->kind == TyKind::Func) ? fty->fn : nullptr;
    if (!fn) {
        err(line, col, "@lru_cache requires a function with a known signature");
        return fnv;
    }
    if (fn->varargs || fn->kwargs) {
        err(line, col, "@lru_cache does not support functions with *args/**kwargs");
        return fnv;
    }
    int npos = static_cast<int>(fn->pos.size());
    int ntotal = npos + static_cast<int>(fn->kwonly.size());
    if (ntotal == 0) {
        err(line, col, "@lru_cache requires at least one parameter");
        return fnv;
    }
    for (int i = 0; i < ntotal; ++i) {
        TypePtr t = fn_param_ty(fn, i);
        bool ok = !t || t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                  t->kind == TyKind::Float || t->kind == TyKind::Str ||
                  t->kind == TyKind::Unknown;
        if (!ok) {
            err(line, col,
                std::format("@lru_cache parameter '{}' has unsupported key type '{}'",
                            fn_param_name(fn, i), type_name(t)));
            return fnv;
        }
    }
    TypePtr rty = fn->ret ? fn->ret : t_unknown();
    TypePtr root_ty = mk_dict(fn_param_ty(fn, 0), t_unknown());

    std::string label = std::format("__pa_lru_{}_{}", base, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = rty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<int> param_slots(static_cast<size_t>(ntotal), -1);
    int pi = 0;
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        param_slots[static_cast<size_t>(pi++)] = s;
    };
    for (auto& p : fn->pos) add_param(p);
    for (auto& p : fn->kwonly) add_param(p);

    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    auto read_cap = [&](int cap, TypePtr T) {
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16 + 8 * cap, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    };

    VReg callee = read_cap(0, fty);
    VReg cur = read_cap(1, root_ty);
    VReg ret = const_none();
    for (int i = 0; i < ntotal; ++i) {
        TypePtr kt = fn_param_ty(fn, i);
        int ko = own_kind(kt);
        VReg key = load_slot(param_slots[static_cast<size_t>(i)], kt);
        VReg cond = call_runtime(RtFn::DictContains, t_bool(), {cur, key});
        int hit = new_block("lru.hit");
        int miss = new_block("lru.miss");
        int join = new_block("lru.join");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cond;
        br.target = hit;
        br.target2 = miss;
        emit(std::move(br));

        if (i + 1 < ntotal) {
            TypePtr next_kt = fn_param_ty(fn, i + 1);
            TypePtr next_ty = (i + 2 < ntotal) ? mk_dict(next_kt, t_unknown())
                                               : mk_dict(next_kt, rty);
            int cs = new_temp_slot(next_ty);
            switch_to(miss);
            VReg nd = call_runtime(RtFn::DictNew, next_ty,
                                   {const_int(key_kind(next_kt)),
                                    const_int(own_kind(next_kt)), const_int(2),
                                    const_int(-1), const_int(-1)});
            push_temp(nd);
            retain_elem(key, ko);
            rc_inc(nd);  // 字典持有一份，临时栈那份随后清掉
            call_void(RtFn::DictSet, {cur, key, nd});
            store_slot(cs, nd);
            Inst jm;
            jm.op = IrOp::Jump;
            jm.target = join;
            emit(std::move(jm));

            switch_to(hit);
            VReg got = call_runtime(RtFn::DictGet, next_ty, {cur, key});
            store_slot(cs, got);
            Inst jh;
            jh.op = IrOp::Jump;
            jh.target = join;
            emit(std::move(jh));

            switch_to(join);
            cur = load_slot(cs, next_ty);
            continue;
        }

        // 最后一级：命中直接取；未命中调 f，把返回值存进缓存。
        int rs = new_temp_slot(rty);
        switch_to(miss);
        std::vector<VReg> call_args;
        call_args.reserve(static_cast<size_t>(ntotal));
        for (int k = 0; k < ntotal; ++k)
            call_args.push_back(
                load_slot(param_slots[static_cast<size_t>(k)], fn_param_ty(fn, k)));
        Inst call;
        call.op = IrOp::Call;
        call.a = callee;
        call.args = std::move(call_args);
        VReg rv = def_heap(rty, std::move(call));
        retain_elem(key, ko);
        if (ir_is_heap(rty)) rc_inc(rv);
        call_void(RtFn::DictSet, {cur, key, rv});
        store_slot(rs, rv);
        Inst jm;
        jm.op = IrOp::Jump;
        jm.target = join;
        emit(std::move(jm));

        switch_to(hit);
        VReg got = call_runtime(RtFn::DictGet, rty, {cur, key});
        store_slot(rs, got);
        Inst jh;
        jh.op = IrOp::Jump;
        jh.target = join;
        emit(std::move(jh));

        switch_to(join);
        ret = load_slot(rs, rty);
    }
    emit_return(ret, rty);
    pop_frame();

    TypePtr t0 = fn_param_ty(fn, 0);
    VReg root = call_runtime(RtFn::DictNew, root_ty,
                             {const_int(key_kind(t0)), const_int(own_kind(t0)),
                              const_int(2), const_int(-1), const_int(-1)});
    push_temp(root);
    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, fty, {code, const_int(2)});
    push_temp(closure);
    VReg fcell = make_cell_value(fnv, fty);
    rc_inc(fcell);
    field_store(closure, 16, fcell);
    VReg ccell = make_cell_value(root, root_ty);
    rc_inc(ccell);
    field_store(closure, 24, ccell);
    return closure;
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
        // 一致，否则会读出错误的位模式。int 列表先逐元素提升成 float；
        // 但回调必须能按 double 调用（形参是 float/unknown），否则算错。
        if (use_fp && elem && elem->kind != TyKind::Float && elem->kind != TyKind::Unknown) {
            bool cb_ok = false;
            if (ft && ft->kind == TyKind::Func && ft->fn && ft->fn->pos.size() >= 2) {
                auto compatible = [](const TypePtr& t) {
                    return !t || t->kind == TyKind::Unknown || t->kind == TyKind::Float;
                };
                cb_ok = compatible(ft->fn->pos[0].ty) && compatible(ft->fn->pos[1].ty);
            }
            if (!cb_ok) {
                err(c.args[1]->line, c.args[1]->col,
                    "reduce() over mixed int/float requires a float callback");
                return const_none();
            }
            VReg conv = promote_container(xs, mk_list(elem), mk_list(t_float()));
            if (conv == kNoReg) {
                err(c.args[1]->line, c.args[1]->col,
                    "reduce() over this element type is not supported yet");
                return const_none();
            }
            xs = conv;
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
        // pyadamas 不建模 __name__/__doc__，@functools.wraps(f) 这一层在装饰器
        // 处理里直接跳过；当普通函数调用说明用户想拿返回值，做不了。
        err(c.line, c.col, "functools.wraps() is only supported as a decorator");
        for (auto& a : c.args) expr(*a);
        return const_none();
    }
    if (member == "partial") {
        if (c.args.empty()) {
            err(c.line, c.col, "functools.partial() expects at least 1 argument");
            return const_none();
        }
        TypePtr ft = tc_.type_of(c.args[0].get());
        auto plan = tc_.partial_plan(&c);
        if (!plan || !ft || ft->kind != TyKind::Func || !ft->fn) {
            err(c.line, c.col,
                "functools.partial() requires a function with a known signature");
            for (auto& a : c.args) expr(*a);
            for (auto& kw : c.keywords) expr(*kw.value);
            return const_none();
        }
        VReg fnv = expr(*c.args[0]);
        return build_partial_wrapper(c, fnv, ft, *plan, c.line, c.col);
    }
    if (member == "lru_cache") {
        // @lru_cache 在装饰器处理里合成包装闭包；当普通调用说明想拿返回值。
        err(c.line, c.col, "functools.lru_cache() is only supported as a decorator");
        for (auto& a : c.args) expr(*a);
        for (auto& kw : c.keywords) expr(*kw.value);
        return const_none();
    }
    err(c.line, c.col, std::format("functools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::lower_datetime(const std::string& member, Call& c) {
    // timedelta 收同名关键字参数，其它构造函数不收。
    if (!c.keywords.empty() && member != "timedelta") {
        err(c.line, c.col, std::format("datetime.{}() does not take keyword arguments", member));
    }
    if (member == "timedelta") {
        TypePtr tdty = tc_.type_of(&c);
        if (!tdty || tdty->kind != TyKind::TimeDelta) tdty = new_ty(TyKind::TimeDelta);
        static const char* kParams[7] = {"days",         "seconds", "microseconds",
                                         "milliseconds", "minutes", "hours",
                                         "weeks"};
        VReg vals[7];
        bool set[7] = {false, false, false, false, false, false, false};
        for (int i = 0; i < 7; ++i) vals[i] = const_int(0);
        if (c.args.size() > 7) {
            err(c.line, c.col,
                std::format("datetime.timedelta() takes at most 7 arguments, got {}",
                            c.args.size()));
        }
        for (size_t i = 0; i < c.args.size() && i < 7; ++i) {
            vals[i] = coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
            set[i] = true;
        }
        for (auto& kw : c.keywords) {
            int idx = -1;
            for (int i = 0; i < 7; ++i)
                if (kw.arg == kParams[i]) idx = i;
            if (idx < 0) {
                err(kw.value->line, kw.value->col,
                    std::format("datetime.timedelta() got an unexpected keyword argument '{}'",
                                kw.arg));
                expr(*kw.value);
                continue;
            }
            if (set[idx]) {
                err(kw.value->line, kw.value->col,
                    std::format("datetime.timedelta() got multiple values for argument '{}'",
                                kw.arg));
                expr(*kw.value);
                continue;
            }
            vals[idx] = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
            set[idx] = true;
        }
        // 单位换算放进整数运算，运行时只接收 days / seconds / microseconds。
        auto mul_add = [&](VReg a, int64_t k, VReg b) {
            Inst m;
            m.op = IrOp::Bin;
            m.bop = BinKind::Mul;
            m.a = a;
            m.b = const_int(k);
            VReg mv = def(t_int(), std::move(m));
            Inst ad;
            ad.op = IrOp::Bin;
            ad.bop = BinKind::Add;
            ad.a = mv;
            ad.b = b;
            return def(t_int(), std::move(ad));
        };
        VReg days = mul_add(vals[6], 7, vals[0]);                 // weeks * 7 + days
        VReg micros = mul_add(vals[3], 1000, vals[2]);            // millis * 1000 + micros
        VReg secs = mul_add(vals[5], 3600, mul_add(vals[4], 60, vals[1]));
        VReg v = call_runtime(RtFn::TimeDeltaNew, tdty, {days, secs, micros});
        push_temp(v);
        return v;
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
