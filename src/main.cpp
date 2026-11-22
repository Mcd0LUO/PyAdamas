// main.cpp —— 命令行入口
//
// 用法:
//   pyadamas tokens <file.py>   打印 token 流
//   pyadamas ast    <file.py>   打印语法树
//   pyadamas types  <file.py>   打印每个顶层名字的推断类型
//   pyadamas check  <file.py>   词法 + 语法 + 类型检查
//   pyadamas check --syntax <f> 只做词法/语法检查
//   pyadamas ir     <file.py>   打印降级后的 IR
//   pyadamas asm    <file.py>   打印生成的 x86-64 汇编
//   pyadamas build  <file.py> [-o out]   编译并链接成可执行文件
//   pyadamas run    <file.py>   编译、链接并运行
//   pyadamas cdecl  <h.h> -o <out.cpya>   从头文件子集生成 .cpya 声明
//   pyadamas        <file.py>   等价于 ast
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "ast.h"
#include "cdecl.h"
#include "codegen.h"
#include "desugar.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "module.h"
#include "parser.h"
#include "typecheck.h"

#ifndef PYADAMAS_RT_OBJ
#define PYADAMAS_RT_OBJ "build/dev/rt.o"
#endif
#ifndef PYADAMAS_MODCACHE_DIR
#define PYADAMAS_MODCACHE_DIR "build/dev/modcache"
#endif

#include "driver_build.h"
#include "target_abi.h"
#include "driver_util.h"

using pyadamas::read_file;
using pyadamas::report;
using pyadamas::shell_quote;
using pyadamas::make_temp;
using pyadamas::Program;
using pyadamas::load_program;
using pyadamas::prepare_program;
using pyadamas::link_program;

namespace {

void dump_tokens(const std::vector<pyadamas::Token>& toks) {
    for (const auto& t : toks) {
        std::string detail;
        switch (t.kind) {
            case pyadamas::Tok::INT: detail = std::format("  value={}", t.ival); break;
            case pyadamas::Tok::FLOAT: detail = std::format("  value={}", t.fval); break;
            case pyadamas::Tok::NAME:
            case pyadamas::Tok::STRING:
            case pyadamas::Tok::FSTRING: detail = "  \"" + t.text + "\""; break;
            default: break;
        }
        std::printf("%4d:%-3d %-12s%s\n", t.line, t.col, pyadamas::tok_name(t.kind), detail.c_str());
    }
}

// ── 编译流水线 ──────────────────────────────────────────────────

struct Frontend {
    std::unique_ptr<pyadamas::Module> mod;
    pyadamas::TypeChecker tc;

    explicit Frontend(std::string file) : tc(std::move(file)) {}
};

// 解析 + 类型检查。有错误时打印并返回 nullopt。
std::optional<Frontend> run_frontend(const std::string& path, const std::string& source,
                                     bool syntax_only,
                                     pyadamas::ModuleLookup module_lookup = nullptr) {
    pyadamas::Lexer lexer(source);
    auto tokens = lexer.run();

    pyadamas::Parser parser(std::move(tokens));
    auto mod = parser.parse();
    pyadamas::desugar_lambdas(*mod);

    Frontend fe(path);
    fe.mod = std::move(mod);

    if (syntax_only) return fe;

    if (module_lookup) fe.tc.set_lenient_imports(true);
    if (module_lookup) fe.tc.set_module_lookup(std::move(module_lookup));
    fe.tc.set_lang_gaps(std::getenv("PYADAMAS_LANG_GAPS") != nullptr &&
                        std::string_view(std::getenv("PYADAMAS_LANG_GAPS")) != "0");
    fe.tc.check(*fe.mod);
    if (!fe.tc.diags().empty()) {
        for (const auto& d : fe.tc.diags()) report(path, source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s\n", fe.tc.diags().size(),
                     fe.tc.diags().size() == 1 ? "" : "s");
        return std::nullopt;
    }
    return fe;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "pyadamas —— Python 语法外壳的编译器\n"
                     "\n"
                     "usage: %s [tokens|ast|types|check|ir|asm|build|run|cdecl] [-O0|-O1|-O2] "
                     "[--fast-math] [--no-zero-cost-exc] [--syntax] [--asan|--ubsan] "
                     "[-o out] <file.py>\n",
                     argv[0]);
        return 2;
    }

    std::string mode;
    std::string path;
    std::string output;
    std::vector<std::string> include_dirs;
    std::vector<std::string> prog_args;
    std::vector<std::string> cdecl_links;
    bool syntax_only = false;
    bool verbose = false;
    bool rebuild = false;
    int opt_level = 2;
    bool fast_math = false;
    // Z5（0.202）：表驱动展开是默认路径。可挂起函数（生成器/协程）仍然走
    // setjmp —— 它们有 5 处 C 级 setjmp 兜底，抓不到 _Unwind，这是设计上的
    // 永久降级。`--no-zero-cost-exc` 是回退开关（出问题时能一键退回旧路径）。
    bool zero_cost_exc = true;
    bool resolve_imports = false;
    // 生成程序的内存检查器。空 = 不启用。
    //
    // RC/GC 这类「堆被写坏」的 bug 在 IR 层面完全看不出问题 —— 只有内存
    // 检查器能抓到第一现场（谁第一个读到已释放内存）。0.216 的 unknown 槽
    // 提升 bug 就是靠 ASan 定位的：IR 配平、-O0 也崩、关 GC 就好，
    // 单看代码无从下手。
    std::string sanitize;

    static const std::vector<std::string> kModes = {"tokens", "ast",     "check", "types",
                                                    "ir",     "asm",     "build", "run",
                                                    "cdecl"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--syntax") syntax_only = true;
        else if (a == "--imports") resolve_imports = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--rebuild") rebuild = true;
        else if (a == "--fast-math") fast_math = true;
        else if (a == "--asan") sanitize = "address";
        else if (a == "--ubsan") sanitize = "undefined";
        else if (a.starts_with("--sanitize=")) sanitize = a.substr(11);
        else if (a == "--zero-cost-exc") zero_cost_exc = true;
        else if (a == "--no-zero-cost-exc") zero_cost_exc = false;
        else if (a == "-O0") opt_level = 0;
        else if (a == "-O1") opt_level = 1;
        else if (a == "-O2") opt_level = 2;
        else if (a.starts_with("--opt=")) {
            std::string v = a.substr(6);
            if (v == "0") opt_level = 0;
            else if (v == "1") opt_level = 1;
            else if (v == "2") opt_level = 2;
            else {
                std::fprintf(stderr, "error: invalid optimization level '%s' (expected 0,1,2)\n",
                             v.c_str());
                return 2;
            }
        }
        else if (a.starts_with("--target=")) {
            // 目标 ABI。只影响调用约定/寄存器角色与影子空间，不改指令集：
            // 目前两个目标都是 x86-64，区别在于 SysV 与 Win64 的 ABI。
            const char* want = argv[i] + 9;
            const pyadamas::TargetABI* abi = pyadamas::find_target_abi(want);
            if (!abi) {
                std::fprintf(stderr,
                             "error: unknown target '%s' (expected x86_64-sysv, "
                             "x86_64-win64)\n",
                             want);
                return 2;
            }
            pyadamas::set_target_abi(abi);
        }
        else if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "--link" && i + 1 < argc) cdecl_links.push_back(argv[++i]);
        else if (a == "-I" && i + 1 < argc) include_dirs.push_back(argv[++i]);
        else if (a.starts_with("-I") && a.size() > 2) include_dirs.push_back(a.substr(2));
        else if (a == "--") {
            // `pyadamas run prog.py -- a b`：余下参数原样传给被运行的程序。
            for (++i; i < argc; ++i) prog_args.push_back(argv[i]);
            break;
        }
        else if (mode.empty() && std::ranges::find(kModes, a) != kModes.end()) mode = a;
        else path = a;
    }
    if (mode.empty()) mode = "ast";
    if (path.empty()) {
        std::fprintf(stderr, "error: no input file\n");
        return 2;
    }

    if (mode == "cdecl") {
        if (output.empty()) {
            std::fprintf(stderr, "error: cdecl needs '-o <out.cpya>'\n");
            return 2;
        }
        std::string module = std::filesystem::path(output).stem().string();
        std::string err;
        std::string text = pyadamas::cdecl_generate(path, module, cdecl_links, err);
        if (text.empty()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::ofstream out(output, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "error: cannot write '%s'\n", output.c_str());
            return 1;
        }
        out << text;
        return 0;
    }

    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", src.error().c_str());
        return 1;
    }

    try {
        if (mode == "tokens") {
            pyadamas::Lexer lexer(*src);
            dump_tokens(lexer.run());
            return 0;
        }

        if (mode == "ast" || (mode == "check" && syntax_only)) {
            auto fe = run_frontend(path, *src, mode == "check");
            if (!fe) return 1;
            if (mode == "ast") {
                fe->mod->dump(std::cout);
                return 0;
            }
            std::printf("ok: %s (%zu statements)\n", path.c_str(), fe->mod->body.size());
            return 0;
        }

        if (mode == "check" || mode == "types") {
            // check/types 默认走通用前端（不解析 import 图，宽松）；
            // --imports 时加载依赖模块接口，让跨模块类型/成员可见（更严格）。
            pyadamas::ModuleLookup lookup = nullptr;
            if (resolve_imports) {
                std::vector<std::string> missing;
                lookup = pyadamas::load_check_lookup(path, include_dirs, missing);
            }
            auto fe = run_frontend(path, *src, false, lookup);
            if (!fe) return 1;
            if (mode == "types") {
                for (auto& [name, ty] : fe->tc.top_symbols())
                    std::printf("%-24s %s\n", name.c_str(), ty.c_str());
                return 0;
            }
            if (verbose && !fe->tc.unsupported.empty()) {
                auto& uns = fe->tc.unsupported;
                std::ranges::sort(uns);
                auto uniq = std::ranges::unique(uns);
                uns.erase(uniq.begin(), uniq.end());
                std::fprintf(stderr, "note: %zu construct(s) parsed but not yet compiled:",
                             uns.size());
                for (auto& u : uns) std::fprintf(stderr, " %s", u.c_str());
                std::fprintf(stderr, "\n");
            }
            std::printf("ok: %s (%zu statements, %zu functions, %zu expressions)\n", path.c_str(),
                        fe->mod->body.size(), fe->tc.n_func, fe->tc.n_expr);
            return 0;
        }

        Program prog;
        // 把 sanitizer 折成 cc 的编译/链接选项，通过 PYADAMAS_CC_FLAGS 传给
        // driver_build（那里编译和链接共用同一套 flag）。
        if (!sanitize.empty()) {
            std::string flags = "-fsanitize=" + sanitize + " -g -fno-omit-frame-pointer";
            if (sanitize == "address")
                // 默认关掉 leak 检测：GC/RC 程序里「故意留到进程退出」的对象
                // 很多，leak 报告会淹没真正的 use-after-free。要看泄漏时用
                // ASAN_OPTIONS=detect_leaks=1 显式打开。
                flags += " -fsanitize-address-use-after-scope";
            setenv("PYADAMAS_CC_FLAGS", flags.c_str(), 1);
            // 链接插桩版运行时（见 driver_build 的 set_asan_runtime）。
            pyadamas::set_asan_runtime(sanitize == "address");
        }
        prog.rebuild = rebuild;
        // live range splitting（PYADAMAS_RA_SPLIT）已废弃：分配器在 -O0 下
        // 会让循环计数错乱（死循环）、-O2 下算错值（冒泡排序输出未排序）。
        // docs/regalloc.md §已否决的方案 已判「默认关、不要重试」，ra-opt-design
        // 里也记了它在生成器+except 上 SIGSEGV。它和 codegen_block 的槽位置
        // 发射代码（块入口 reload、融合存储双写、fp 别名掩码）缠在一起，删
        // 干净要动默认路径的发射逻辑，风险大于收益 —— 所以这里直接拒绝入口，
        // 让这条路径再也产不出错误代码；rc_split 恒 false，dispatch 不可达。
        prog.rc_split = false;
        if (const char* sp = std::getenv("PYADAMAS_RA_SPLIT");
            sp != nullptr && std::string_view(sp) != "0" && sp[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_RA_SPLIT 已废弃（split 分配器会死循环/算错"
                         "值，见 docs/regalloc.md）\n");
            return 2;
        }
        if (const char* sc = std::getenv("PYADAMAS_SPLIT_MAXREG");
            sc != nullptr && sc[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_SPLIT_MAXREG 已废弃（只对已废弃的 split "
                         "分配器有效）\n");
            return 2;
        }
        // 图着色分配器（PYADAMAS_RA_GRAPH）已整段删除：它在 -O2 下会把
        // 浮点累加器跨调用的活跃性算错（0.314 修过的 N19a 形态），静默算
        // 错值；而它本来就是默认关的实验路径（docs/regalloc.md 已判放弃）。
        // 留一个显式报错而不是静默忽略，免得 A/B 脚本以为自己在跑另一条
        // 分配路径拿到假数据。
        if (const char* rg = std::getenv("PYADAMAS_RA_GRAPH");
            rg != nullptr && std::string_view(rg) != "0" && rg[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_RA_GRAPH 已移除（图着色分配器会在 -O2 下"
                         "生成错误代码，见 docs/regalloc.md）\n");
            return 2;
        }
        // callee-saved 提升增强（默认关）：PYADAMAS_RA_OPT=1 开启，参与缓存失效。
        prog.ra_opt = std::getenv("PYADAMAS_RA_OPT") != nullptr &&
                      std::string_view(std::getenv("PYADAMAS_RA_OPT")) != "0";
        // 特化 SIMD（**默认关**，0.384）：codegen_simd*.cpp 是照着
        // bench/compare 里那几个程序的循环体写出来的固定 IR 模板，属于对基准
        // 的特化优化 —— 只有形状完全一致的代码才吃得到，等于把「测出来的性能」
        // 当成「通用性能」。它继续留着只作对照，PYADAMAS_SIMD=1 显式开。
        // 默认路径改走下面的通用向量化。
        const char* simd_env = std::getenv("PYADAMAS_SIMD");
        prog.simd = simd_env && std::string_view(simd_env) != "0";
        // 通用循环自动向量化（**默认开**，0.384）：认任意可数循环的
        // map/reduction（codegen_vect.cpp），不依赖具体程序的形状。
        // PYADAMAS_VECT=0 显式关。参与缓存失效。
        const char* vect_env = std::getenv("PYADAMAS_VECT");
        prog.vect = !vect_env || std::string_view(vect_env) != "0";
        // 整数循环优化（默认关）：PYADAMAS_LOOP_OPT=1 开推荐子集（掩码 3 =
        // 跳转线程化 + 仿射 BCE）。bit2 同块 load 去重实测负收益（matmul
        // BCE 收益被吃掉一半，见 docs/intloop-design.md §3），不进默认，
        // 仅显式掩码（4/5/6/7）开发期 A/B 用。参与缓存失效。
        if (const char* lo = std::getenv("PYADAMAS_LOOP_OPT")) {
            std::string_view sv(lo);
            if (sv == "1") prog.loop_opt = 3;
            else if (sv != "0" && !sv.empty()) {
                int v = 0;
                bool ok = true;
                for (char ch : sv) {
                    if (ch < '0' || ch > '9') { ok = false; break; }
                    v = v * 10 + (ch - '0');
                    if (v > 7) { ok = false; break; }
                }
                prog.loop_opt = ok ? v : 0;
            }
        }
        // 内联/devirt 深化（默认关）：PYADAMAS_INLINE_OPT=1 开启，参与缓存失效。
        prog.inline_opt = std::getenv("PYADAMAS_INLINE_OPT") != nullptr &&
                          std::string_view(std::getenv("PYADAMAS_INLINE_OPT")) != "0";
        // 只读借用槽细化（默认关）：PYADAMAS_BORROW_OPT=1 开启，参与缓存失效。
        prog.borrow_opt = std::getenv("PYADAMAS_BORROW_OPT") != nullptr &&
                          std::string_view(std::getenv("PYADAMAS_BORROW_OPT")) != "0";
        // 运行期类型标签（默认关）：PYADAMAS_TYPE_TAGS=1 开启，参与缓存失效。
        // 开启后 unknown 值的 tag 追加 NONE/BOOL/精确堆子类，解锁
        // isinstance/repr/pprint（docs/numeric-types-design.md §2）。
        prog.type_tags = std::getenv("PYADAMAS_TYPE_TAGS") != nullptr &&
                         std::string_view(std::getenv("PYADAMAS_TYPE_TAGS")) != "0";
        // P4 语言缺口（默认关）：PYADAMAS_LANG_GAPS=1 开启，参与缓存失效。
        // 异常类当值 / file 协议 / traceback / dataclass（设计 §3/§4）。
        // CLASS tag=11 等精确标签是 P4 的表示基础，所以强制带上 type_tags。
        prog.lang_gaps = std::getenv("PYADAMAS_LANG_GAPS") != nullptr &&
                         std::string_view(std::getenv("PYADAMAS_LANG_GAPS")) != "0";
        if (prog.lang_gaps) prog.type_tags = true;
        prog.verbose = verbose;
        prog.opt = opt_level;
        prog.fast_math = fast_math;
        prog.zero_cost_exc = zero_cost_exc;
        if (!load_program(path, include_dirs, prog)) return 1;

        if (mode == "ir" || mode == "asm") {
            if (!prepare_program(prog, false, true)) return 1;
            if (!prog.entry->ir) return 1;
            if (mode == "ir") {
                pyadamas::dump_ir(*prog.entry->ir, std::cout);
            } else {
                auto a = std::chrono::steady_clock::now();
                std::string asm_text = pyadamas::generate_asm(
                    *prog.entry->ir, !prog.needs_unknown_index, prog.zero_cost_exc,
                    false, prog.rc_split, prog.simd, prog.ra_opt,
                    prog.inline_opt, prog.type_tags, prog.vect, prog.needs_dynamic_big);
                auto b = std::chrono::steady_clock::now();
                if (std::getenv("PYADAMAS_TIMING"))
                    std::fprintf(stderr, "[timing] codegen=%.1f ms\n",
                                 std::chrono::duration<double, std::milli>(b - a).count());
                std::cout << asm_text;
            }
            return 0;
        }

        // build / run：模块各自汇编成 .o，再链接。
        if (!prepare_program(prog, true, false)) return 1;

        std::string out = output;
        bool ephemeral = false;
        if (out.empty()) {
            if (mode == "run") {
                auto tmp = make_temp("");
                if (!tmp) {
                    std::fprintf(stderr, "error: %s\n", tmp.error().c_str());
                    return 1;
                }
                out = *tmp;
                ::unlink(out.c_str());
                ephemeral = true;
            } else {
                out = path;
                if (out.ends_with(".py")) out.resize(out.size() - 3);
            }
        }
        if (!link_program(prog, out)) return 1;

        if (mode == "run") {
            std::string run_path = out;
            if (run_path.find('/') == std::string::npos) run_path = "./" + run_path;
            std::string cmd = shell_quote(run_path);
            for (auto& pa : prog_args) cmd += " " + shell_quote(pa);
            int rc = std::system(cmd.c_str());
            if (ephemeral) ::unlink(out.c_str());
            if (rc < 0) return 1;
            return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
        }
        return 0;
    } catch (const pyadamas::LexError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const pyadamas::ParseError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: internal error: %s\n", path.c_str(), e.what());
    }
    return 1;
}
