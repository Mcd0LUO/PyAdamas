// native_sigs.h —— 声明式原生模块签名（P0：签名从 C++ 挪进数据）
//
// 声明文件就是 **Python 语法**的 stub（stdlib/sigs/<模块路径>.pysig）：
//
//   # stdlib/sigs/time.pysig
//   def time() -> float: pass
//   def sleep(secs: float) -> None: pass
//   def localtime(secs: float = -1e300) -> tuple[int, ...]: pass
//   def strftime(*args: unknown) -> str: pass
//   CLOCK_MONOTONIC: int = 1
//
//   # 约定推导出的符号不适用时用装饰器覆盖
//   @native("pa_time_time")
//   def now() -> float: pass
//
// 三个关键设计（见 docs/native-stdlib-cookbook.md）：
//   1. **复用前端**：Lexer + Parser 是编译器自己的，注解走
//      TypeChecker::parse_annotation_for_decl（转发到 parse_annotation）。
//      这样「类型语法」全项目只有一份，新增类型不用同步第二张表。
//   2. **native 是兜底语义**：声明里的函数没有实现，天然就是 native；
//      运行期符号按 `pa_<module>_<name>` 约定推导（模块名的点变下划线），
//      例外用 `@native("pa_xxx")` 覆盖。
//   3. **路径即身份**：模块名 = 相对 stdlib/sigs/ 的路径，os/path.pysig →
//      模块 os.path。没有任何 module 声明语句，也就没有「声明名与文件名
//      不一致」的可能；同时 import 解析被禁止落进 sigs/ 目录
//      （driver_modules.cpp），杜绝「把 stub 当实现」。
//
// 失败路径一律硬失败并打印 `<文件>:<行号>: <msg>`：签名静默缺失/静默变
// unknown 会表现成「模块没有这个属性」或「按错的 ABI 传参」，极难查。
//
// 下游（RtFn 反查、ModuleInfo、builtin_sigs/lower_import 的接线）与上一版
// 完全一致，本文件只换「读文件 → 签名结构」这一段前端。
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ir_ops.h"     // RtFn + rt_name()
#include "module.h"     // ModuleInfo / TypePtr / FuncInfo / LiteralValue
#include "pydecl.h"     // 共用声明文件前端（Lexer/Parser/AST 白名单/装饰器/字面量）
#include "typecheck.h"  // TypeChecker::parse_annotation_for_decl

// 独立自测（单独编译本头文件）时没有 CMake 的宏。
#ifndef PYADAMAS_STDLIB_DIR
#define PYADAMAS_STDLIB_DIR "stdlib"
#endif

namespace pyadamas {
namespace nativesig {

// ── 数据模型（与自定义语法版本逐字相同：下游一行没动）──────────────

struct SigParam {
    std::string name;
    TypePtr ty;
    bool kwonly = false;
    bool has_default = false;
    LiteralValue def;
};

struct SigFunc {
    std::string name;         // 模块内属性名（Python 里看到的名字）
    std::string rt_symbol;    // 运行期 C 符号（如 "pa_time_sleep"）
    RtFn fn = RtFn::ListNew;  // 由 rt_symbol 反查 rt_name() 得到
    std::vector<SigParam> pos;
    std::vector<SigParam> kwonly;
    bool varargs = false;
    std::string vararg_name;
    TypePtr vararg_ty;
    bool kwargs = false;
    std::string kwarg_name;
    TypePtr kwarg_ty;
    TypePtr ret;
    int line = 0;  // 声明所在行（诊断用）
};

struct SigConst {
    std::string name;
    TypePtr ty;
    LiteralValue val;
    int line = 0;
};

struct ModuleSig {
    std::string module;
    std::string path;
    std::vector<SigFunc> funcs;
    std::vector<SigConst> consts;

    const SigFunc* find_func(std::string_view n) const {
        for (auto& f : funcs)
            if (f.name == n) return &f;
        return nullptr;
    }
    const SigConst* find_const(std::string_view n) const {
        for (auto& c : consts)
            if (c.name == n) return &c;
        return nullptr;
    }
};

namespace detail {

// ── 诊断与文件 ────────────────────────────────────────────────────────

[[noreturn]] inline void fatal(const std::string& path, int line, const std::string& msg) {
    std::fprintf(stderr, "error: %s:%d: %s\n", path.c_str(), line, msg.c_str());
    std::exit(1);
}

// ── 模块名 ↔ 路径 ────────────────────────────────────────────────────
// 模块名 = 相对 stdlib/sigs/ 的路径：os.path → sigs/os/path.pysig。
// 每一段必须是合法标识符，禁止空段 / .. / 绝对路径（路径穿越防护）。
inline bool valid_module_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    bool start = true;
    for (char c : name) {
        if (c == '.') {
            if (start) return false;
            start = true;
            continue;
        }
        const unsigned char u = static_cast<unsigned char>(c);
        if (start) {
            if (!std::isalpha(u) && c != '_') return false;
            start = false;
        } else if (!std::isalnum(u) && c != '_') {
            return false;
        }
    }
    return !start;
}

inline std::filesystem::path sigs_dir() {
    return std::filesystem::path(PYADAMAS_STDLIB_DIR) / "sigs";
}

// 相对 sigs/ 的路径（os.path → os/path.pysig）。
inline std::string sig_rel_path(const std::string& module) {
    std::string rel;
    for (char c : module) rel.push_back(c == '.' ? '/' : c);
    rel += ".pysig";
    return rel;
}

inline std::string sig_path(const std::string& module) {
    return (sigs_dir() / std::filesystem::path(sig_rel_path(module))).string();
}

// ── AST 白名单解析（前端复用 src/pydecl.h，与 .cpya 加载器同一份）───────
// 顶层只认四种语句：模块 docstring、def（body 只有 pass / docstring+pass）、
// 带注解的常量（NAME: T = 字面量）。其它一律报错并给行号。

// 注解 → 类型。复用 typecheck 的 parse_annotation（经 public 薄封装），
// 所以类型语法与函数标注/变量标注完全同一份实现。
inline TypePtr resolve_type(TypeChecker& tc, Expr* e, int line, const std::string& what) {
    if (!e) pydecl::fail(line, what + " needs a type annotation");
    TypePtr t = tc.parse_annotation_for_decl(e, line);
    if (!t) pydecl::fail(line, what + ": cannot parse the type annotation");
    if (t->kind == TyKind::Unknown) {
        // 只有显式写 unknown/Any/object 才允许留在 unknown 域。其它名字
        // （拼错、Protocol、未登记的类）一律报错：native 签名静默退化成
        // unknown 会让调用点不再 coerce，运行期按错的 ABI 传参。
        const std::string base = pydecl::annotation_base_name(e);
        if (base != "unknown" && base != "Any" && base != "object")
            pydecl::fail(line, what + ": '" + base +
                                   "' is not a supported native signature type "
                                   "(write 'unknown' explicitly if that is intended)");
    }
    return t;
}

// 约定：pa_<模块名，点变下划线>_<函数名>。os.path.join → pa_os_path_join。
inline std::string default_symbol(const std::string& module, const std::string& name) {
    std::string s = "pa_";
    for (char c : module) s.push_back(c == '.' ? '_' : c);
    s.push_back('_');
    s += name;
    return s;
}

// pydecl::Literal → 接口里的 LiteralValue（.pysig 默认值/常量字面量）。
inline LiteralValue to_literal_value(const pydecl::Literal& l) {
    LiteralValue v;
    switch (l.kind) {
        case pydecl::Literal::Kind::Bool: v.kind = LiteralValue::Kind::Bool; v.b = l.b; break;
        case pydecl::Literal::Kind::Int:
            v.kind = LiteralValue::Kind::Int;
            v.i = l.i;
            v.u64 = l.u64;
            break;
        case pydecl::Literal::Kind::Float: v.kind = LiteralValue::Kind::Float; v.f = l.f; break;
        case pydecl::Literal::Kind::Str: v.kind = LiteralValue::Kind::Str; v.s = l.s; break;
        case pydecl::Literal::Kind::None: v.kind = LiteralValue::Kind::None; break;
    }
    return v;
}

// 声明文件 → ModuleSig。
class DeclParser {
  public:
    DeclParser(std::string path, TypeChecker& tc) : path_(std::move(path)), tc_(tc) {}

    ModuleSig parse(Module& mod, const std::string& module) {
        ModuleSig ms;
        ms.module = module;
        ms.path = path_;
        for (auto& st : mod.body) {
            if (pydecl::is_docstring(*st)) continue;  // 模块 docstring：允许
            switch (st->kind) {
                case StmtKind::FunctionDef: {
                    auto& fd = static_cast<FunctionDef&>(*st);
                    ms.funcs.push_back(parse_func(fd, module));
                    break;
                }
                case StmtKind::AnnAssign:
                    ms.consts.push_back(parse_const(static_cast<AnnAssign&>(*st)));
                    break;
                case StmtKind::Assign: {
                    auto& as = static_cast<Assign&>(*st);
                    std::string name;
                    if (!as.targets.empty())
                        if (const auto* n = dynamic_cast<const Name*>(as.targets[0].get()))
                            name = n->id;
                    pydecl::fail(as.line, "constant '" + name +
                                              "' needs a type annotation (write '" + name +
                                              ": T = <literal>')");
                }
                default:
                    pydecl::fail(st->line, std::string("only 'def f(...) -> T: pass' and "
                                                       "'NAME: T = <literal>' are allowed in a "
                                                       "signature file; found ") +
                                               pydecl::stmt_kind_name(st->kind));
            }
        }
        // 重名检查：函数与常量共用模块命名空间。
        std::set<std::string> seen;
        for (auto& f : ms.funcs)
            if (!seen.insert(f.name).second)
                pydecl::fail(f.line, "duplicate declaration '" + f.name + "'");
        for (auto& c : ms.consts)
            if (!seen.insert(c.name).second)
                pydecl::fail(c.line, "duplicate declaration '" + c.name + "'");
        return ms;
    }

  private:
    SigFunc parse_func(FunctionDef& fd, const std::string& module) {
        SigFunc f;
        f.line = fd.line;
        f.name = fd.name;
        if (fd.is_async) pydecl::fail(fd.line, "async def is not allowed in a signature file");
        // 装饰器：@native("pa_xxx") 覆盖约定推导的运行期符号。
        for (auto& d : fd.decorators) {
            if (!d) continue;
            const auto dec = pydecl::decorator_of(*d, fd.name);
            if (dec.name != "native")
                pydecl::fail(dec.line, "unknown decorator on '" + fd.name +
                                           "' (only @native(\"pa_symbol\") is allowed in a "
                                           "signature file)");
            if (!dec.has_string_arg)
                pydecl::fail(dec.line, "@native needs a runtime symbol: @native(\"pa_xxx\")");
            if (!f.rt_symbol.empty())
                pydecl::fail(dec.line, "duplicate decorator on '" + fd.name + "'");
            f.rt_symbol = dec.arg;
        }
        if (f.rt_symbol.empty()) f.rt_symbol = default_symbol(module, fd.name);
        pydecl::check_pass_body(fd);

        // 位置参数 + 默认值（Python 的 defaults 对齐到最后 ndef 个位置参数）。
        std::vector<Arg*> ordered;
        for (auto& a : fd.args.posonly) ordered.push_back(&a);
        for (auto& a : fd.args.args) ordered.push_back(&a);
        const size_t ndef = fd.args.defaults.size();
        if (ndef > ordered.size())
            pydecl::fail(fd.line, "function '" + fd.name + "': default value without a parameter");
        const size_t first_def = ordered.size() - ndef;
        for (size_t i = 0; i < ordered.size(); ++i) {
            Arg& a = *ordered[i];
            SigParam p;
            p.name = a.name;
            const std::string what = "parameter '" + a.name + "' of '" + fd.name + "'";
            p.ty = resolve_type(tc_, a.annotation.get(), fd.line, what);
            if (i >= first_def) {
                const Expr* dv = fd.args.defaults[i - first_def].get();
                auto lit = pydecl::literal_of(dv);
                if (!lit)
                    pydecl::fail(dv ? dv->line : fd.line,
                                 "default value of parameter '" + a.name +
                                     "' must be a literal (int / float / str / bool / None)");
                p.has_default = true;
                p.def = to_literal_value(*lit);
            }
            f.pos.push_back(std::move(p));
        }
        if (fd.args.vararg) {
            f.varargs = true;
            f.vararg_name = fd.args.vararg->name;
            f.vararg_ty = resolve_type(tc_, fd.args.vararg->annotation.get(), fd.line,
                                       "*" + fd.args.vararg->name + " of '" + fd.name + "'");
        }
        // keyword-only 参数（'*' 之后）。
        for (size_t i = 0; i < fd.args.kwonly.size(); ++i) {
            Arg& a = fd.args.kwonly[i];
            SigParam p;
            p.kwonly = true;
            p.name = a.name;
            p.ty = resolve_type(tc_, a.annotation.get(), fd.line,
                                "parameter '" + a.name + "' of '" + fd.name + "'");
            if (i < fd.args.kw_defaults.size() && fd.args.kw_defaults[i]) {
                const Expr* dv = fd.args.kw_defaults[i].get();
                auto lit = pydecl::literal_of(dv);
                if (!lit)
                    pydecl::fail(dv ? dv->line : fd.line,
                                 "default value of parameter '" + a.name + "' must be a literal");
                p.has_default = true;
                p.def = to_literal_value(*lit);
            }
            f.kwonly.push_back(std::move(p));
        }
        if (fd.args.kwarg) {
            f.kwargs = true;
            f.kwarg_name = fd.args.kwarg->name;
            f.kwarg_ty = resolve_type(tc_, fd.args.kwarg->annotation.get(), fd.line,
                                      "**" + fd.args.kwarg->name + " of '" + fd.name + "'");
        }
        // 返回类型：必须写（native 签名不写返回值等于让调用点猜）。
        f.ret = resolve_type(tc_, fd.returns.get(), fd.line, "return type of '" + fd.name + "'");
        return f;
    }

    SigConst parse_const(AnnAssign& aa) {
        SigConst c;
        c.line = aa.line;
        const auto* n = dynamic_cast<const Name*>(aa.target.get());
        if (!n) pydecl::fail(aa.line, "constant target must be a simple name");
        c.name = n->id;
        if (!aa.annotation)
            pydecl::fail(aa.line, "constant '" + c.name + "' needs a type annotation");
        c.ty = resolve_type(tc_, aa.annotation.get(), aa.line, "constant '" + c.name + "'");
        if (!aa.value) pydecl::fail(aa.line, "constant '" + c.name + "' needs a value");
        auto lit = pydecl::literal_of(aa.value.get());
        if (!lit)
            pydecl::fail(aa.value->line, "constant '" + c.name +
                                             "' initializer must be a literal "
                                             "(int / float / str / bool / None)");
        c.val = to_literal_value(*lit);
        // 字面量种类与声明类型必须一致（否则运行期按错宽度取值）。
        const TyKind want = c.ty ? c.ty->kind : TyKind::Unknown;
        bool ok = true;
        switch (c.val.kind) {
            case LiteralValue::Kind::Int:
                ok = want == TyKind::Int || want == TyKind::Bool ||
                     (want >= TyKind::I8 && want <= TyKind::U64);
                break;
            case LiteralValue::Kind::Float: ok = want == TyKind::Float; break;
            case LiteralValue::Kind::Bool: ok = want == TyKind::Bool || want == TyKind::Int; break;
            case LiteralValue::Kind::Str: ok = want == TyKind::Str; break;
            case LiteralValue::Kind::None: ok = want == TyKind::Unknown || want == TyKind::None; break;
            default: ok = false; break;
        }
        if (!ok)
            pydecl::fail(c.line,
                         "constant '" + c.name + "': initializer does not match the declared type");
        return c;
    }

    std::string path_;
    TypeChecker& tc_;
};

// ── 运行期符号 → RtFn 反查表 ─────────────────────────────────────────
// RtFn 没有 Count 哨兵，rt_name() 对未定义编号返回 "?"。RtFn 底层类型是
// uint16_t（固定底层类型 → 整个值域合法），所以扫 0..0xFFFF 能拿到全表。
// 只在第一次用到声明式模块时构建一次（637 个入口，实测 ~0.1ms）。
inline const std::map<std::string, RtFn>& rtfn_by_symbol() {
    static const std::map<std::string, RtFn> table = [] {
        std::map<std::string, RtFn> out;
        for (uint32_t i = 0; i <= 0xFFFFu; ++i) {
            const char* n = rt_name(static_cast<RtFn>(i));
            if (n && std::strcmp(n, "?") != 0) out.emplace(n, static_cast<RtFn>(i));
        }
        return out;
    }();
    return table;
}

// 运行期符号 → RtFn：只允许反查到**已经存在**的运行期入口，所以声明文件
// 不可能凭空造出一个后端不会生成调用的编号。
//
// 一次扫描只为「这个文件需要的符号」找编号（time 的 8 个符号都在 RtFn 表
// 前半段，扫到即停，实测 ~2us）；扫完仍有没找到的就硬失败并报声明行号。
// 全表扫描（637 个入口）只在「写错符号名」这条错误路径上发生。
inline void resolve_rt_symbols(ModuleSig& ms) {
    std::unordered_map<std::string, std::vector<size_t>> want;
    for (size_t i = 0; i < ms.funcs.size(); ++i) want[ms.funcs[i].rt_symbol].push_back(i);
    if (want.empty()) return;
    size_t remaining = want.size();
    for (uint32_t i = 0; i <= 0xFFFFu && remaining; ++i) {
        const char* n = rt_name(static_cast<RtFn>(i));
        if (!n || n[0] == '?' || n[0] == 0) continue;
        auto it = want.find(n);
        if (it == want.end()) continue;
        for (size_t k : it->second) ms.funcs[k].fn = static_cast<RtFn>(i);
        want.erase(it);
        --remaining;
    }
    if (remaining == 0) return;
    // 报第一个没解析成功的声明（把「文件:行 + 符号名」都带上）。
    for (auto& [sym, idxs] : want) {
        if (idxs.empty()) continue;
        fatal(ms.path, ms.funcs[idxs[0]].line,
              "runtime symbol '" + sym +
                  "' is not in the RtFn table (add the runtime entry + src/ir_rtfn*.h first, "
                  "or fix @native(\"...\"))");
    }
}

}  // namespace detail

// ── 对外接口 ──────────────────────────────────────────────────────────

// 读并解析 <stdlib>/sigs/<模块路径>.pysig（Python 语法）。没有声明文件返回
// nullptr（调用方退回硬编码）。有文件但内容不合法 → 打印 <文件>:<行>: <msg>
// 后退出（绝不静默跳过）。
inline const ModuleSig* load(const std::string& module) {
    struct Cache {
        std::map<std::string, std::unique_ptr<ModuleSig>> hit;
        std::set<std::string> miss;
    };
    static Cache cache;
    if (auto it = cache.hit.find(module); it != cache.hit.end()) return it->second.get();
    if (cache.miss.count(module)) return nullptr;
    // 模块名即路径：非法名字（空段 / .. / 绝对路径 / 非法字符）直接报错。
    if (!detail::valid_module_name(module))
        detail::fatal(detail::sigs_dir().string(), 0,
                      "invalid module name '" + module +
                          "' (a signature module's name is its path under stdlib/sigs/, "
                          "e.g. os.path → os/path.pysig)");
    const std::string path = detail::sig_path(module);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        cache.miss.insert(module);
        return nullptr;
    }
    // 路径防护（纵深防御）：解析出的真实路径必须仍在 stdlib/sigs/ 下，
    // 防止符号链接把声明目录指到别处（名字校验已经挡了 .. 穿越）。
    {
        auto root = std::filesystem::weakly_canonical(detail::sigs_dir(), ec);
        auto real = std::filesystem::weakly_canonical(path, ec);
        if (ec || root.empty() || real.string().rfind(root.string() + "/", 0) != 0)
            detail::fatal(path, 0, "declaration file resolves outside stdlib/sigs/ (module '" +
                                       module + "')");
    }
    std::unique_ptr<Module> ast;
    ModuleSig ms;
    try {
        ast = pydecl::parse_file(path);
        TypeChecker tc(path);  // 只用来解析注解，不跑 check
        detail::DeclParser dp(path, tc);
        ms = dp.parse(*ast, module);
    } catch (const pydecl::Error& e) {
        detail::fatal(path, e.line, e.msg);
    }
    detail::resolve_rt_symbols(ms);
    auto out = std::make_unique<ModuleSig>(std::move(ms));
    auto* raw = out.get();
    cache.hit[module] = std::move(out);
    return raw;
}

// 声明 → ModuleInfo（签名表）。没有声明文件返回 nullptr，调用方退回硬编码。
// 返回的对象被缓存，与 builtin_module 的旧缓存语义一致（同一 ModuleInfoPtr）。
inline ModuleInfoPtr module_info(const std::string& module) {
    static std::map<std::string, ModuleInfoPtr> cache;
    auto it = cache.find(module);
    if (it != cache.end()) return it->second;
    const ModuleSig* ms = load(module);
    if (!ms) return nullptr;
    auto info = std::make_shared<ModuleInfo>();
    info->name = ms->module;
    info->builtin = true;
    for (auto& f : ms->funcs) {
        auto fi = std::make_shared<FuncInfo>();
        fi->name = f.name;
        auto add = [&](const SigParam& p, bool kwonly) {
            Param pp;
            pp.name = p.name;
            pp.ty = p.ty;
            pp.has_default = p.has_default;
            if (p.has_default) pp.default_lit = p.def;
            pp.kwonly = kwonly;
            return pp;
        };
        for (auto& p : f.pos) fi->pos.push_back(add(p, false));
        for (auto& p : f.kwonly) fi->kwonly.push_back(add(p, true));
        fi->varargs = f.varargs;
        fi->vararg_name = f.vararg_name;
        fi->vararg_ty = f.vararg_ty;
        fi->kwargs = f.kwargs;
        fi->kwarg_name = f.kwarg_name;
        fi->kwarg_ty = f.kwarg_ty;
        fi->ret = f.ret;
        fi->ret_known = true;
        auto t = new_ty(TyKind::Func);
        t->fn = std::move(fi);
        info->globals[f.name] = std::move(t);
    }
    for (auto& c : ms->consts) info->globals[c.name] = c.ty;
    cache[module] = info;
    return info;
}

// 模块成员 → RtFn。没有声明 / 没有这个成员返回 nullopt（调用方继续走硬编码）。
inline std::optional<RtFn> func_rtfn(const std::string& module, const std::string& member) {
    const ModuleSig* ms = load(module);
    if (!ms) return std::nullopt;
    const SigFunc* f = ms->find_func(member);
    if (!f) return std::nullopt;
    return f->fn;
}

// 声明的模块常量（值 + 类型）。没有则 nullptr。
inline const SigConst* find_const(const std::string& module, const std::string& member) {
    const ModuleSig* ms = load(module);
    if (!ms) return nullptr;
    return ms->find_const(member);
}

// 这个模块是否由声明文件驱动（已迁移 / 新模块）。诊断与文档用。
inline bool is_declared(const std::string& module) { return load(module) != nullptr; }

// 人类可读的转储（自测与调试用；不进编译主路径）。
inline std::string describe(const ModuleSig& ms) {
    std::string out = "module " + ms.module + " (" + ms.path + ")\n";
    for (auto& f : ms.funcs)
        out += "  def " + f.name + " -> " + f.rt_symbol + " (" + std::to_string(f.pos.size()) +
               " pos) -> ret\n";
    for (auto& c : ms.consts) out += "  const " + c.name + "\n";
    return out;
}

}  // namespace nativesig
}  // namespace pyadamas
