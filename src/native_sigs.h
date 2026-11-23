// native_sigs.h —— 声明式原生模块签名加载器（P0：签名从 C++ 挪进数据）
//
// 背景：原生模块（math/os/time/...）过去在 builtin_sigs.cpp 里手写签名表、
// 在 lower_import*.cpp 里手写「成员名 → RtFn」映射。于是「加一个原生 API」
// 要改 3~5 处 C++。这里把这两件事变成**数据**：stdlib/sigs/<module>.pysig
// 声明「Python 名 / 形参 / 返回类型 / 运行期符号」，编译期读取。
//
// 选型（运行期读取，而不是 CMake 代码生成）：签名本来就是「编译一次、启动
// 时加载」的数据，和 stdlib/*.py 的加载方式一致；header-only 实现，不需要
// 在 CMakeLists 里登记新 TU（那里有别人在改），也不引入构建期两步依赖。
// 代价：每次编译多一次文件 stat + 解析（只读被 import 的模块，实测 < 0.1ms，
// 见 docs/native-stdlib-cookbook.md 的「性能」一节）。
//
// 失败路径（刻意做成硬失败）：声明文件语法错 / 运行期符号对不上 rt_name()
// 表 → 打印 <file>:<line>: <msg> 并退出。绝不静默跳过——静默跳过会表现成
// 「模块没有这个属性」，极难查。
//
// 用法（接线只有两处，见 docs/native-stdlib-cookbook.md）：
//   // builtin_sigs.cpp：声明优先
//   if (auto m = nativesig::module_info(name)) { cache[name] = m; return m; }
//   // lower_import.cpp 的 native_fn()：反查 RtFn
//   if (auto f = nativesig::func_rtfn(module, member)) return *f;
//
// 格式：
//   module <名字>                       —— 必须与文件名一致（os.path → os.path.pysig）
//   func <py名> <运行期符号> (<形参>) -> <类型>
//   const <名字> <类型> = <字面量>
// 形参：name: type | name: type = <字面量> | *args: type | **kwargs: type
//       '*' 单独出现表示其后是 keyword-only 形参。
// 类型：int float bool str bytes none unknown file dylib regex match popen
//       list[T] set[T] deque[T] range[T] dict[K,V] tuple[A,B] tuple[T,...]
//       optional[T]
//
// 本文件只依赖 ir_ops.h（RtFn + rt_name）与 typecheck_types.h（类型工厂），
// 任何 TU 都能 include；所有函数 inline，无运行期初始化顺序问题。
#pragma once

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

#include "ir_ops.h"  // RtFn + rt_name()
#include "module.h"   // ModuleInfo / TypePtr / FuncInfo / LiteralValue / 类型工厂

// 独立自测（单独编译本头文件）时没有 CMake 的宏。
#ifndef PYADAMAS_STDLIB_DIR
#define PYADAMAS_STDLIB_DIR "stdlib"
#endif

namespace pyadamas {
namespace nativesig {

// ── 数据模型 ──────────────────────────────────────────────────────────

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

// ── 词法 ──────────────────────────────────────────────────────────────
struct Tok {
    enum class Kind : uint8_t { Ident, Int, Float, Str, Punct, End } kind = Kind::End;
    std::string s;   // Ident / Str 的内容，或 Punct 的拼写
    long long i = 0;
    double f = 0.0;
    int line = 0;
};

struct ParseError {
    int line = 0;
    std::string msg;
};

inline bool ident_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
inline bool digit_ch(char ch) { return ch >= '0' && ch <= '9'; }

// 词法：整个文件切成 token（保留行号），再按语句解析。
// 注释 '#' 到行尾；标识符允许点号（module 语句要写 os.path）。
inline std::vector<Tok> lex(std::string_view src) {
    std::vector<Tok> out;
    int line = 1;
    size_t i = 0;
    auto push = [&](Tok t) { out.push_back(std::move(t)); };
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '#') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (ident_start(c)) {
            size_t b = i;
            while (i < src.size() &&
                   (ident_start(src[i]) || digit_ch(src[i]) || src[i] == '.'))
                ++i;
            Tok t;
            t.kind = Tok::Kind::Ident;
            t.s = std::string(src.substr(b, i - b));
            t.line = line;
            push(std::move(t));
            continue;
        }
        // 数字：可带符号（本语法里 '-' 只出现在负数字面量与 '->' 中）
        if (digit_ch(c) || ((c == '-' || c == '+') && i + 1 < src.size() && digit_ch(src[i + 1]))) {
            size_t b = i;
            if (c == '-' || c == '+') ++i;
            bool is_float = false;
            while (i < src.size() && digit_ch(src[i])) ++i;
            if (i < src.size() && src[i] == '.') {
                is_float = true;
                ++i;
                while (i < src.size() && digit_ch(src[i])) ++i;
            }
            if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                size_t save = i;
                size_t j = i + 1;
                if (j < src.size() && (src[j] == '-' || src[j] == '+')) ++j;
                if (j < src.size() && digit_ch(src[j])) {
                    is_float = true;
                    i = j;
                    while (i < src.size() && digit_ch(src[i])) ++i;
                } else {
                    i = save;
                }
            }
            Tok t;
            t.line = line;
            std::string text(src.substr(b, i - b));
            if (is_float) {
                t.kind = Tok::Kind::Float;
                t.f = std::strtod(text.c_str(), nullptr);
            } else {
                t.kind = Tok::Kind::Int;
                t.i = std::strtoll(text.c_str(), nullptr, 10);
            }
            t.s = std::move(text);
            push(std::move(t));
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            ++i;
            std::string s;
            bool closed = false;
            while (i < src.size()) {
                char ch = src[i];
                if (ch == '\\' && i + 1 < src.size()) {
                    char e = src[i + 1];
                    if (e == 'n') s.push_back('\n');
                    else if (e == 't') s.push_back('\t');
                    else if (e == '\\') s.push_back('\\');
                    else if (e == '"' || e == '\'') s.push_back(e);
                    else { s.push_back('\\'); s.push_back(e); }
                    i += 2;
                    continue;
                }
                if (ch == q) { ++i; closed = true; break; }
                if (ch == '\n') break;  // 未闭合：交给调用点报错
                s.push_back(ch);
                ++i;
            }
            if (!closed) throw ParseError{line, "unterminated string literal"};
            Tok t;
            t.kind = Tok::Kind::Str;
            t.s = std::move(s);
            t.line = line;
            push(std::move(t));
            continue;
        }
        // '->' 与 '...' 是多字符记号，其余标点是单字符。
        if (c == '-' && i + 1 < src.size() && src[i + 1] == '>') {
            Tok t;
            t.kind = Tok::Kind::Punct;
            t.s = "->";
            t.line = line;
            push(std::move(t));
            i += 2;
            continue;
        }
        if (c == '.') {
            size_t b = i;
            while (i < src.size() && src[i] == '.') ++i;
            Tok t;
            t.kind = Tok::Kind::Punct;
            t.s = std::string(src.substr(b, i - b));
            t.line = line;
            push(std::move(t));
            continue;
        }
        static constexpr std::string_view kPunct = "()[],:=*";
        if (kPunct.find(c) != std::string_view::npos) {
            Tok t;
            t.kind = Tok::Kind::Punct;
            t.s = std::string(1, c);
            t.line = line;
            push(std::move(t));
            ++i;
            continue;
        }
        throw ParseError{line, std::string("unexpected character '") + c + "'"};
    }
    Tok end;
    end.kind = Tok::Kind::End;
    end.line = line;
    out.push_back(std::move(end));
    return out;
}

// ── 语法（递归下降；出错抛 ParseError，由 load() 统一转成文件:行 诊断）──
class Parser {
  public:
    Parser(std::string_view path, std::vector<Tok> toks) : path_(path), t_(std::move(toks)) {}

    ModuleSig parse_module(const std::string& expect_module) {
        ModuleSig ms;
        bool saw_module = false;
        while (cur().kind != Tok::Kind::End) {
            if (cur().kind != Tok::Kind::Ident)
                fail(cur(), std::string("expected a statement keyword, got '") + cur().s + "'");
            const std::string kw = cur().s;
            if (kw == "module") {
                if (saw_module) fail(cur(), "duplicate 'module' statement");
                next();
                if (cur().kind != Tok::Kind::Ident) fail(cur(), "expected module name");
                ms.module = cur().s;
                ms.path = std::string(path_);
                if (ms.module != expect_module)
                    fail(cur(), "'module " + ms.module + "' does not match file name (expects '" +
                                    expect_module + "')");
                next();
                saw_module = true;
            } else if (kw == "func") {
                ms.funcs.push_back(parse_func());
            } else if (kw == "const") {
                ms.consts.push_back(parse_const());
            } else {
                fail(cur(), "unknown statement '" + kw + "' (expected module/func/const)");
            }
        }
        if (!saw_module) fail(cur(), "missing 'module <name>' statement");
        // 重名检查：同一模块里 Python 名必须唯一（否则后一条静默覆盖前一条）。
        std::set<std::string> seen;
        for (auto& f : ms.funcs)
            if (!seen.insert(f.name).second) fail_line(f.line, "duplicate declaration '" + f.name + "'");
        for (auto& c : ms.consts)
            if (!seen.insert(c.name).second) fail_line(c.line, "duplicate declaration '" + c.name + "'");
        return ms;
    }

  private:
    const Tok& cur() const { return t_[i_]; }
    const Tok& next() { return t_[i_ < t_.size() - 1 ? ++i_ : i_]; }
    [[noreturn]] void fail(const Tok& t, const std::string& msg) const {
        throw ParseError{t.line, msg};
    }
    [[noreturn]] void fail_line(int line, const std::string& msg) const { throw ParseError{line, msg}; }

    bool is_punct(std::string_view p) const {
        return cur().kind == Tok::Kind::Punct && cur().s == p;
    }
    bool punct_at(size_t k, std::string_view p) const {
        return k < t_.size() && t_[k].kind == Tok::Kind::Punct && t_[k].s == p;
    }
    void expect_punct(std::string_view p) {
        if (!is_punct(p))
            fail(cur(), "expected '" + std::string(p) + "', got '" + cur().s + "'");
        next();
    }
    std::string expect_ident(const char* what) {
        if (cur().kind != Tok::Kind::Ident) fail(cur(), std::string("expected ") + what);
        std::string s = cur().s;
        next();
        return s;
    }

    SigFunc parse_func() {
        SigFunc f;
        f.line = cur().line;
        next();  // func
        f.name = expect_ident("function name");
        f.rt_symbol = expect_ident("runtime symbol name (e.g. pa_time_sleep)");
        expect_punct("(");
        bool kwonly_mode = false;
        bool first = true;
        while (!is_punct(")")) {
            if (!first) expect_punct(",");
            first = false;
            if (is_punct(")")) break;  // 允许尾逗号
            if (is_punct("*")) {
                const int star_line = cur().line;
                next();
                if (cur().kind == Tok::Kind::Ident && punct_at(i_ + 1, ":")) {
                    // *args: 类型
                    f.varargs = true;
                    f.vararg_name = cur().s;
                    next();
                    expect_punct(":");
                    f.vararg_ty = parse_type();
                } else {
                    // 单独的 '*'：其后是 keyword-only
                    if (kwonly_mode) fail_line(star_line, "duplicate '*' in parameter list");
                    kwonly_mode = true;
                }
                continue;
            }
            SigParam p;
            p.kwonly = kwonly_mode;
            p.name = expect_ident("parameter name");
            expect_punct(":");
            p.ty = parse_type();
            if (is_punct("=")) {
                next();
                p.has_default = true;
                p.def = parse_literal();
            }
            if (p.kwonly) f.kwonly.push_back(std::move(p));
            else f.pos.push_back(std::move(p));
        }
        expect_punct(")");
        expect_punct("->");
        f.ret = parse_type();
        return f;  // 运行期符号在整文件解析完后统一反查（见 resolve_rt_symbols）
    }

    SigConst parse_const() {
        SigConst c;
        c.line = cur().line;
        next();  // const
        c.name = expect_ident("constant name");
        c.ty = parse_type();
        expect_punct("=");
        c.val = parse_literal();
        // 字面量种类与声明类型必须一致（否则运行期会按错宽度取值）。
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
        if (!ok) fail_line(c.line, "constant '" + c.name + "' initializer does not match its type");
        return c;
    }

    // ── 类型 ──
    TypePtr parse_type() {
        std::string base = expect_ident("type name");
        std::vector<TypePtr> args;
        bool variadic = false;
        if (is_punct("[")) {
            next();
            bool first = true;
            while (!is_punct("]")) {
                if (!first) expect_punct(",");
                first = false;
                if (is_punct("]")) break;  // 尾逗号
                if (is_punct("...")) { variadic = true; next(); continue; }
                args.push_back(parse_type());
            }
            expect_punct("]");
        }
        return make_type(base, args, variadic);
    }

    [[noreturn]] void bad_type(const std::string& base, size_t nargs,
                               const std::string& why) const {
        fail_line(cur().line, "type '" + base + "'" + (nargs ? " with type arguments" : "") +
                                  ": " + why);
    }

    TypePtr make_type(const std::string& base, const std::vector<TypePtr>& args, bool variadic) {
        const size_t n = args.size();
        if (variadic && n != 1) bad_type(base, n, "'...' is only valid as tuple[T, ...]");
        auto scalar = [&](const TypePtr& t) {
            if (n) bad_type(base, n, "takes no type arguments");
            return t;
        };
        if (base == "int") return scalar(t_int());
        if (base == "float") return scalar(t_float());
        if (base == "bool") return scalar(t_bool());
        if (base == "str") return scalar(t_str());
        if (base == "bytes") return scalar(t_bytes());
        if (base == "none" || base == "None") return scalar(t_none());
        if (base == "unknown" || base == "any" || base == "Any" || base == "object")
            return scalar(t_unknown());
        if (base == "file" || base == "File") return scalar(t_file());
        if (base == "dylib") return scalar(t_dylib());
        if (base == "regex" || base == "Regex") return scalar(t_regex());
        if (base == "match" || base == "Match") return scalar(t_match());
        if (base == "popen" || base == "Popen") return scalar(t_popen());
        if (base == "list") { if (n != 1) bad_type(base, n, "expects list[T]"); return mk_list(args[0]); }
        if (base == "set") { if (n != 1) bad_type(base, n, "expects set[T]"); return mk_set(args[0]); }
        if (base == "deque") { if (n != 1) bad_type(base, n, "expects deque[T]"); return mk_deque(args[0]); }
        if (base == "range") { if (n != 1) bad_type(base, n, "expects range[T]"); return mk_range(args[0]); }
        if (base == "optional" || base == "Optional") {
            if (n != 1) bad_type(base, n, "expects optional[T]");
            return mk_optional(args[0]);
        }
        if (base == "dict") {
            if (n != 2) bad_type(base, n, "expects dict[K, V]");
            return mk_dict(args[0], args[1]);
        }
        if (base == "tuple") {
            if (variadic) return mk_tuple_rest(args[0]);
            if (n == 0) return mk_tuple({});
            return mk_tuple(args);
        }
        bad_type(base, n, "unknown type name");
    }

    LiteralValue parse_literal() {
        LiteralValue v;
        const Tok& t = cur();
        if (t.kind == Tok::Kind::Int) { v.kind = LiteralValue::Kind::Int; v.i = t.i; next(); return v; }
        if (t.kind == Tok::Kind::Float) { v.kind = LiteralValue::Kind::Float; v.f = t.f; next(); return v; }
        if (t.kind == Tok::Kind::Str) { v.kind = LiteralValue::Kind::Str; v.s = t.s; next(); return v; }
        if (t.kind == Tok::Kind::Ident) {
            if (t.s == "true" || t.s == "True") { v.kind = LiteralValue::Kind::Bool; v.b = true; next(); return v; }
            if (t.s == "false" || t.s == "False") { v.kind = LiteralValue::Kind::Bool; v.b = false; next(); return v; }
            if (t.s == "none" || t.s == "None") { v.kind = LiteralValue::Kind::None; next(); return v; }
        }
        fail(t, "expected a literal (int / float / string / true / false / None)");
    }

    std::string_view path_;
    std::vector<Tok> t_;
    size_t i_ = 0;
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

[[noreturn]] inline void fatal(const std::string& path, int line, const std::string& msg);

// 运行期符号 → RtFn：只允许反查到**已经存在**的运行期入口，所以声明文件
// 不可能凭空造出一个后端不会生成调用的编号。
//
// 一次扫描只为「这个文件需要的符号」找编号（time 的 8 个符号都在 RtFn 表
// 前半段，扫到即停，实测 ~2us）；扫完仍有没找到的就硬失败并报声明行号。
// 走全表扫描（637 个入口）只在「写错符号名」这条错误路径上发生。
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
                  "' is not in the RtFn table (add the runtime entry + src/ir_rtfn*.h first)");
    }
}

inline std::string sig_path(const std::string& module) {
    return (std::filesystem::path(PYADAMAS_STDLIB_DIR) / "sigs" / (module + ".pysig")).string();
}

[[noreturn]] inline void fatal(const std::string& path, int line, const std::string& msg) {
    std::fprintf(stderr, "error: %s:%d: %s\n", path.c_str(), line, msg.c_str());
    std::exit(1);
}

inline std::string read_file_or_fatal(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fatal(path, 0, "cannot open declaration file");
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace detail

// ── 对外接口 ──────────────────────────────────────────────────────────

// 读并解析 <stdlib>/sigs/<module>.pysig。没有声明文件返回 nullptr。
// 有文件但语法错/符号错 → 打印 <file>:<line>: <msg> 后退出（不静默）。
inline const ModuleSig* load(const std::string& module) {
    struct Cache {
        std::map<std::string, std::unique_ptr<ModuleSig>> hit;
        std::set<std::string> miss;
    };
    static Cache cache;
    if (auto it = cache.hit.find(module); it != cache.hit.end()) return it->second.get();
    if (cache.miss.count(module)) return nullptr;
    const std::string path = detail::sig_path(module);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        cache.miss.insert(module);
        return nullptr;
    }
    const std::string src = detail::read_file_or_fatal(path);
    auto ms = std::make_unique<ModuleSig>();
    try {
        detail::Parser p(path, detail::lex(src));
        *ms = p.parse_module(module);
        detail::resolve_rt_symbols(*ms);
    } catch (const detail::ParseError& e) {
        detail::fatal(path, e.line, e.msg);
    }
    auto* raw = ms.get();
    cache.hit[module] = std::move(ms);
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
        out += "  func " + f.name + " " + f.rt_symbol + " (" + std::to_string(f.pos.size()) +
               " pos) -> ret\n";
    for (auto& c : ms.consts) out += "  const " + c.name + "\n";
    return out;
}

}  // namespace nativesig
}  // namespace pyadamas
