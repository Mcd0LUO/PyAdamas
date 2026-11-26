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
//   pyadamas resolve <file.py|dir>  读 pyadamas.toml，解析 path 依赖闭包并写锁
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

#include "common/ast.h"
#include "cimport/cdecl.h"
#include "backend/codegen.h"
#include "syntax/desugar.h"
#include "ir/ir.h"
#include "syntax/lexer.h"
#include "lower/lower.h"
#include "ir/module.h"
#include "syntax/parser.h"
#include "typecheck/typecheck.h"

#ifndef PYADAMAS_RT_OBJ
#define PYADAMAS_RT_OBJ "build/dev/rt.o"
#endif
#ifndef PYADAMAS_MODCACHE_DIR
#define PYADAMAS_MODCACHE_DIR "build/dev/modcache"
#endif

#include "ir/deps_json.h"
#include "driver/build.h"
#include "driver/lock.h"
#include "driver/manifest.h"
#include "driver/modules.h"
#include "backend/target_abi.h"
#include "driver/util.h"

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
                                     bool syntax_only, pyadamas::ModuleLookup module_lookup = nullptr,
                                     bool strict_names = false) {
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
    // 0.484（pya-lsp 缺口 4）：check 也要报模块级未定义名字。build/run 一直
    // 是严格的（初值 0 的全局会静默算错），check 之前宽松是因为 0.171 的标准库
    // 基线里「条件导入/C 扩展常量」会误报；现在按「名字在模块内任何作用域出现过
    // /被 import 过/是内建」判定，条件定义与后续定义都不误杀（见 expr.cpp 的
    // star_import_/dynamic_globals_ 放行），标准库基线复测通过后才默认打开。
    // --lenient-names 保留为逃生口（诊断口径回归时用）。
    fe.tc.set_strict_module_names(strict_names);
    {
        // 0.555：与 build 同口径 —— 源码出现 dataclass 时按需打开 lang_gaps。
        const char* lg_env = std::getenv("PYADAMAS_LANG_GAPS");
        const bool lg_off = lg_env && std::string_view(lg_env) == "0";
        const bool lg_on = lg_env && std::string_view(lg_env) != "0";
        // 只对**项目文件**自动开：标准库/CPython Lib 提到 dataclass 很常见，
        // 给它们开 lang_gaps 会改变诊断口径（626 基线会掉）。
        const std::string sd = pyadamas::stdlib_dir();
        const bool project_file =
            path.find("/python3.") == std::string::npos &&
            (sd.empty() || path.rfind(sd, 0) != 0);
        fe.tc.set_lang_gaps(lg_on ||
                            (!lg_off && project_file &&
                             source.find("dataclass") != std::string::npos));
    }
    fe.tc.check(*fe.mod);
    if (!fe.tc.diags().empty()) {
        for (const auto& d : fe.tc.diags()) report(path, source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s\n", fe.tc.diags().size(),
                     fe.tc.diags().size() == 1 ? "" : "s");
        return std::nullopt;
    }
    return fe;
}

// 包管理（P0）：清单/锁 → ModuleMap（Q1 + §5.6.1 的落地）。
//
// 三条不变量：
//   1) 没有 pyadamas.toml 的项目完全走老路（--module-map / 路径搜索），行为不变；
//   2) 有清单时包模块**只能**从锁派生的映射进来（幽灵依赖不可能）；
//   3) --locked 时锁必须与「现在重新解析会得到的结果」语义一致，否则失败并打印 diff。
// 返回 false 表示应当终止（诊断已打印）。
bool attach_module_input(pyadamas::Program& prog, const std::string& entry_path,
                         const std::string& module_map_path, const std::string& manifest_flag,
                         bool locked) {
    if (!module_map_path.empty()) {
        if (locked) {
            std::fprintf(stderr,
                         "error: --locked cannot be combined with --module-map: with a manifest "
                         "the lock is the map\n");
            return false;
        }
        // 显式 --module-map 是 legacy 逃生口：它优先于清单。但清单项目里
        // 静默绕过锁会让 --locked 形同虚设，所以至少要把「被忽略」说出来。
        const std::string seen_manifest =
            !manifest_flag.empty() ? manifest_flag
                                   : pyadamas::find_manifest(entry_path).value_or("");
        if (!seen_manifest.empty())
            std::fprintf(stderr,
                         "note: --module-map was given; ignoring '%s' and its lock for this "
                         "build\n",
                         seen_manifest.c_str());
        std::string err;
        auto map = pyadamas::load_module_map(module_map_path, err);
        if (!map) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return false;
        }
        prog.module_map = std::make_shared<pyadamas::ModuleMap>(std::move(*map));
        return true;
    }

    std::string manifest_path = manifest_flag;
    if (manifest_path.empty()) manifest_path = pyadamas::find_manifest(entry_path).value_or("");
    if (manifest_path.empty()) {
        if (locked) {
            std::fprintf(stderr,
                         "error: --locked needs a pyadamas.toml (none found from '%s' upward); "
                         "drop --locked for a manifest-less project\n",
                         entry_path.c_str());
            return false;
        }
        return true;  // 无清单：保持现有行为
    }

    auto manifest = pyadamas::load_manifest(manifest_path);
    if (!manifest) return false;
    auto want = pyadamas::resolve_lock(*manifest);
    if (!want) return false;

    const std::filesystem::path manifest_dir =
        std::filesystem::path(manifest_path).parent_path();
    const std::string lock_file = (manifest_dir / "pyadamas.lock").string();
    std::error_code ec;
    const bool lock_exists = std::filesystem::is_regular_file(lock_file, ec);

    pyadamas::Lock chosen;
    bool have_lock = false;
    if (locked) {
        if (!lock_exists) {
            // 「锁缺失」也是一种不一致：把「将会写出的锁」全部按新增打出来。
            const std::string diff = pyadamas::diff_locks(*want, pyadamas::Lock{});
            std::fprintf(stderr,
                         "error: pyadamas.lock is missing (--locked); run 'pyadamas resolve' to "
                         "create it\n%s",
                         diff.c_str());
            return false;
        }
        auto got = pyadamas::read_lock(lock_file);
        if (!got) return false;
        const std::string diff = pyadamas::diff_locks(*want, *got);
        if (!diff.empty()) {
            std::fprintf(stderr,
                         "error: pyadamas.lock is out of date: the manifest or a dependency "
                         "source changed\n%s"
                         "  run 'pyadamas resolve' to update the lock, or revert the change\n",
                         diff.c_str());
            return false;
        }
        chosen = std::move(*got);
        have_lock = true;
    } else if (lock_exists) {
        // 非 --locked 是「宽松模式」（Cargo 式）：锁过期/损坏就按清单重解析，
        // 但必须把差异打出来，绝不让漂移静默发生。
        auto got = pyadamas::read_lock(lock_file);
        if (got) {
            const std::string diff = pyadamas::diff_locks(*want, *got);
            if (diff.empty()) {
                chosen = std::move(*got);
                have_lock = true;
            } else {
                std::fprintf(stderr,
                             "note: pyadamas.lock is out of date; building from pyadamas.toml "
                             "(run 'pyadamas resolve' to update the lock)\n%s",
                             diff.c_str());
            }
        } else {
            std::fprintf(stderr,
                         "note: pyadamas.lock cannot be read; building from pyadamas.toml "
                         "(run 'pyadamas resolve' to rewrite it)\n");
        }
    } else {
        std::fprintf(stderr,
                     "note: no pyadamas.lock yet; building from pyadamas.toml "
                     "(run 'pyadamas resolve' to pin it)\n");
    }
    if (!have_lock) chosen = std::move(*want);

    auto map = pyadamas::module_map_from_lock(chosen, manifest_dir.string());
    if (!map) return false;
    prog.module_map = std::make_shared<pyadamas::ModuleMap>(std::move(*map));
    return true;
}

}  // namespace

static int cmd_deps(const std::string& path,
                    const std::vector<std::string>& include_dirs,
                    const std::string& module_map_path,
                    const std::string& manifest_flag,
                    const std::string& output,
                    bool verbose,
                    bool json_out,
                    bool locked) {
    (void)json_out;  // deps 只有 JSON 一种输出；--json 是显式别名
            Program prog;
            prog.verbose = verbose;
            prog.rebuild = true;
            if (!attach_module_input(prog, path, module_map_path, manifest_flag, locked))
                return 1;
            if (!load_program(path, include_dirs, prog)) return 1;
            if (!prepare_program(prog, false, false)) return 1;
            std::error_code root_ec;
            const std::string root_abs = std::filesystem::weakly_canonical(
                std::filesystem::path(path).parent_path(), root_ec).string();
            const std::string cwd = std::filesystem::current_path().string();
            const std::string json = pyadamas::module_graph_json(prog, root_abs, cwd);
            if (output.empty()) {
                std::fwrite(json.data(), 1, json.size(), stdout);
            } else {
                std::ofstream out(output, std::ios::binary);
                if (!out) {
                    std::fprintf(stderr, "error: cannot write '%s'\n", output.c_str());
                    return 1;
                }
                out << json;
                if (!out) {
                    std::fprintf(stderr, "error: cannot write '%s'\n", output.c_str());
                    return 1;
                }
            }
            return 0;
}

static int cmd_resolve(const std::string& path, const std::string& manifest_flag,
                         bool locked) {
    (void)locked;
        std::string manifest_path = manifest_flag;
        if (manifest_path.empty()) manifest_path = pyadamas::find_manifest(path).value_or("");
        if (manifest_path.empty()) {
            std::fprintf(stderr,
                         "error: no pyadamas.toml found from '%s' upward; run resolve inside a "
                         "package project or pass --manifest <file>\n",
                         path.empty() ? "." : path.c_str());
            return 1;
        }
        auto manifest = pyadamas::load_manifest(manifest_path);
        if (!manifest) return 1;
        auto lock = pyadamas::resolve_lock(*manifest);
        if (!lock) return 1;
        const std::string lock_file =
            (std::filesystem::path(manifest_path).parent_path() / "pyadamas.lock").string();
        if (!pyadamas::write_lock_file(lock_file, lock->render())) return 1;
        std::printf("wrote %s (%zu package%s, %zu module%s)\n", lock_file.c_str(),
                    lock->packages.size(), lock->packages.size() == 1 ? "" : "s",
                    lock->modules.size(), lock->modules.size() == 1 ? "" : "s");
        return 0;
}

static int cmd_cdecl(const std::string& path, const std::string& output,
                       const std::vector<std::string>& cdecl_links,
                       const std::vector<std::string>& include_dirs) {
        if (output.empty()) {
            std::fprintf(stderr, "error: cdecl needs '-o <out.cpya>'\n");
            return 2;
        }
        std::string module = std::filesystem::path(output).stem().string();
        std::string err;
        std::string text = pyadamas::cdecl_generate(path, module, cdecl_links, include_dirs, err);
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

static bool setup_program(Program& prog, const std::string& path,
                           const std::vector<std::string>& include_dirs,
                           const std::string& sanitize, bool rebuild,
                           const std::string& module_map_path,
                           const std::string& manifest_flag, bool locked,
                           bool verbose, int opt_level, bool fast_math,
                           bool zero_cost_exc) {
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
        // live range splitting（PYADAMAS_RA_SPLIT）的代码**已整段删除**：
        // 分配器在 -O0 下会让循环计数错乱（死循环）、-O2 下算错值（冒泡排序
        // 输出未排序），三轮实现均 +27~28%，docs/regalloc.md §已否决的方案
        // 已判「不要重试」。删掉的是 assign_slot_regs_split、slot_loc_ 系列
        // 按块位置表、spill_placement.{cpp,h} 与 manifest 的 rcsplit 行。
        //
        // 保留显式报错（rc=2）而不是静默忽略：A/B 脚本设了这个变量就以为
        // 自己在跑另一条分配路径，静默跑默认路径会拿到假数据。
        if (const char* sp = std::getenv("PYADAMAS_RA_SPLIT");
            sp != nullptr && std::string_view(sp) != "0" && sp[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_RA_SPLIT 已删除（split 分配器会死循环/"
                         "算错值），见 docs/regalloc.md\n");
            return 2;
        }
        if (const char* sc = std::getenv("PYADAMAS_SPLIT_MAXREG");
            sc != nullptr && sc[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_SPLIT_MAXREG 已删除（只对已删除的 split "
                         "分配器有效），见 docs/regalloc.md\n");
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
        // 特化 SIMD 模板（backend/simd*.cpp，PYADAMAS_SIMD）**0.480 已整段删除**：
        // 它们是照着 bench/compare 里几个程序的循环体写死的 IR 形状模板，默认关；
        // 通用向量化（0.384）落地后它们一次都没再匹配上（12 基准 asm 零标记、
        // 指令数与「关掉向量化」逐位相同）。留显式报错而不是静默忽略，免得
        // A/B 脚本以为自己在跑特化路径却拿到同一份产物。
        if (const char* se = std::getenv("PYADAMAS_SIMD");
            se != nullptr && std::string_view(se) != "0" && se[0] != '\0') {
            std::fprintf(stderr,
                         "error: PYADAMAS_SIMD 已删除（特化 SIMD 模板对当前 IR 全部"
                         "失配，见 docs/simd-vectorization-design.md 与 CHANGELOG "
                         "0.480）\n");
            return 2;
        }
        // 通用循环自动向量化（**默认开**，0.384）：认任意可数循环的
        // map/reduction（backend/vect.cpp），不依赖具体程序的形状。
        // PYADAMAS_VECT=0 显式关。参与缓存失效。
        const char* vect_env = std::getenv("PYADAMAS_VECT");
        prog.vect = !vect_env || std::string_view(vect_env) != "0";
        // 浮点归约 packed 累加（默认关）：快但不保序，与标量逐位结果可能不同。
        const char* ff_env = std::getenv("PYADAMAS_VECT_FASTFLOAT");
        prog.vect_fastfloat = ff_env && std::string_view(ff_env) != "0";
        // 整数循环优化（默认关）：PYADAMAS_LOOP_OPT=1 开推荐子集（掩码 3 =
        // 跳转线程化 + 仿射 BCE）。bit2 同块 load 去重实测负收益（matmul
        // BCE 收益被吃掉一半，见 docs/intloop-design.md §3），不进默认，
        // 仅显式掩码（4/5/6/7）开发期 A/B 用。参与缓存失效。
        // 默认开 bit0（跳转线程化）：纯 CFG 等价变换，实测 nqueens -2.9%、
        // 其它标量基准中性；含「单块直线循环体」的函数由 pass 内部跳过，
        // 避免破坏通用向量化（matmul 会从 10.1M 劣化到 54.8M）。
        prog.loop_opt = 3;
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
        // 运行期类型标签（0.528 起**默认开**）：PYADAMAS_TYPE_TAGS=0 可关。
        // 开启后 unknown 值的 tag 追加 NONE/BOOL/精确堆子类，解锁
        // isinstance/repr/pprint（docs/numeric-types-design.md §2）。
        //
        // 为什么改成默认开：关着时 isinstance(x, int) 对动态值是**静默错值**——
        // 只有 str/list/dict/float 有探针，int/bool/bytes 一律返回 False
        // （实测 h(1) -> 0、[1,"a",2.0,3] 计数 -> 0）。这比多一个字 tag 的代价
        // 严重得多。参与缓存失效，旧 modcache 自动失效。
        // 三态：=1 强制开（调试用）、=0 强制关（逃生口）、未设=按需自动开
        //（prepare_program 里扫到 isinstance(..., 内置类型) 才开，见 0.528）。
        const char* tags_env = std::getenv("PYADAMAS_TYPE_TAGS");
        prog.type_tags = tags_env && std::string_view(tags_env) == "1";
        prog.type_tags_off = tags_env && std::string_view(tags_env) == "0";
        // P4 语言缺口（默认关）：PYADAMAS_LANG_GAPS=1 开启，参与缓存失效。
        // 异常类当值 / file 协议 / traceback / dataclass（设计 §3/§4）。
        // CLASS tag=11 等精确标签是 P4 的表示基础，所以强制带上 type_tags。
        // 三态：=1 强制开、=0 强制关、未设=按需自动开（prepare_program 扫到 dataclass 才开）。
        const char* lg_env = std::getenv("PYADAMAS_LANG_GAPS");
        prog.lang_gaps = lg_env && std::string_view(lg_env) != "0";
        prog.lang_gaps_off = lg_env && std::string_view(lg_env) == "0";
        if (prog.lang_gaps) prog.type_tags = true;
        prog.verbose = verbose;
        prog.opt = opt_level;
        prog.fast_math = fast_math;
        prog.zero_cost_exc = zero_cost_exc;
        if (!attach_module_input(prog, path, module_map_path, manifest_flag, locked)) return false;
        if (!load_program(path, include_dirs, prog)) return false;
    return true;
}

static int cmd_ir_asm(pyadamas::Program& prog, const std::string& mode) {
            if (!prepare_program(prog, false, true)) return 1;
            if (!prog.entry->ir) return 1;
            if (mode == "ir") {
                pyadamas::dump_ir(*prog.entry->ir, std::cout);
            } else {
                auto a = std::chrono::steady_clock::now();
                std::string asm_text = pyadamas::generate_asm(
                    *prog.entry->ir, !prog.needs_unknown_index, prog.zero_cost_exc,
                    false, prog.ra_opt,
                    prog.inline_opt, prog.type_tags, prog.vect, prog.needs_dynamic_big,
                    prog.vect_fastfloat);
                auto b = std::chrono::steady_clock::now();
                if (std::getenv("PYADAMAS_TIMING"))
                    std::fprintf(stderr, "[timing] codegen=%.1f ms\n",
                                 std::chrono::duration<double, std::milli>(b - a).count());
                std::cout << asm_text;
            }
            return 0;
    return 0;
}

static int cmd_build_or_run(pyadamas::Program& prog, const std::string& mode,
                            const std::vector<std::string>& prog_args,
                            const std::string& output, const std::string& path) {
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
}

static int cmd_frontend(const std::string& path, const std::string& src,
                        const std::vector<std::string>& include_dirs,
                        const std::string& mode, bool syntax_only, bool lenient_names,
                        bool no_imports, bool resolve_imports, bool no_unsupported_note) {
        if (mode == "tokens") {
            pyadamas::Lexer lexer(src);
            dump_tokens(lexer.run());
            return 0;
        }

        if (mode == "ast" || (mode == "check" && syntax_only)) {
            auto fe = run_frontend(path, src, mode == "check");
            if (!fe) return 1;
            if (mode == "ast") {
                fe->mod->dump(std::cout);
                return 0;
            }
            std::printf("ok: %s (%zu statements)\n", path.c_str(), fe->mod->body.size());
            return 0;
        }

        if (mode == "check" || mode == "types") {
            // check/types 默认**不**加载 import 图（0.486 回退：0.484 曾默认加载，
            // 在真实代码上把通过率打到 5/20 —— 我们的 stdlib 签名表不全，
            // subprocess.check_output / sys.version_info 这类未建模成员会被误杀，
            // 违背「前端宽、后端严」）。--imports 显式开启模块属性检查。
            // 模块级未定义名字默认就报（真正的拼写错误，误报面小；--lenient-names 逃生口）。
            // 默认加载 import 图，但**只解析本项目的模块**：原生/标准库模块在
            // 非 --imports 时返回 nullptr（按 unknown 处理），避免 0.486 的误杀；
            // 本地模块接口完整，跨模块调用不再退化成 unknown。--no-imports 关整张图。
            pyadamas::ModuleLookup lookup = nullptr;
            if (!no_imports) {
                std::vector<std::string> missing;
                lookup = pyadamas::load_check_lookup(path, include_dirs, missing, resolve_imports);
            }
            auto fe = run_frontend(path, src, false, lookup, !lenient_names);
            if (!fe) return 1;
            if (mode == "types") {
                for (auto& [name, ty] : fe->tc.top_symbols())
                    std::printf("%-24s %s\n", name.c_str(), ty.c_str());
                return 0;
            }
            // 0.571：未建模构造默认**可见**。以前只在 -v 时打印，等于默认静默放过；
            // 「不支持就是不支持」的前提是用户看得见。--no-unsupported-note 可关。
            if (!fe->tc.unsupported.empty() && !no_unsupported_note) {
                auto& uns = fe->tc.unsupported;
                std::ranges::sort(uns);
                auto uniq = std::ranges::unique(uns);
                uns.erase(uniq.begin(), uniq.end());
                // 按类别（冒号/引号前的前缀）汇总，最多列 6 类，避免刷屏。
                std::map<std::string, size_t> by_kind;
                for (auto& u : uns) {
                    auto pos = u.find_first_of("'\"");
                    by_kind[u.substr(0, pos == std::string::npos ? u.size() : pos)]++;
                }
                if (uns.size() <= 4) {
                    // 少量时逐条列出（测试与用户都更需要具体名字）
                    std::fprintf(stderr, "note: %zu construct(s) parsed but not yet compiled:",
                                 uns.size());
                    for (auto& u : uns) std::fprintf(stderr, " %s", u.c_str());
                    std::fprintf(stderr, " [--no-unsupported-note to silence]\n");
                } else {
                    std::fprintf(stderr, "note: %zu construct(s) parsed but not yet compiled",
                                 uns.size());
                    size_t shown = 0;
                    for (auto& [k, n] : by_kind) {
                        if (shown++ == 6) { std::fprintf(stderr, ", ..."); break; }
                        std::fprintf(stderr, "%s%s(%zu)", shown == 1 ? ": " : ", ", k.c_str(),
                                     n);
                    }
                    std::fprintf(stderr, " [--no-unsupported-note to silence]\n");
                }
            }
            std::printf("ok: %s (%zu statements, %zu functions, %zu expressions)\n", path.c_str(),
                        fe->mod->body.size(), fe->tc.n_func, fe->tc.n_expr);
            return 0;
        }
    return 1;
}

// 0.599：main() 的命令行选项。以前是 21 个 main 里的局部变量，
// 每个 cmd_* 都要带一长串参数；收进结构体后签名稳定、加选项不再改调用点。
struct Options {
    std::string mode;
    std::string path;
    std::string output;
    std::vector<std::string> include_dirs;
    std::vector<std::string> prog_args;
    std::vector<std::string> cdecl_links;
    bool syntax_only = false;
    bool verbose = false;
    bool no_unsupported_note = false;
    bool rebuild = false;
    int opt_level = 2;
    bool fast_math = false;
    bool zero_cost_exc = true;
    bool resolve_imports = false;
    bool no_imports = false;
    bool lenient_names = false;
    bool json_out = false;
    std::string module_map_path;
    std::string manifest_flag;
    bool locked = false;
    std::string sanitize;
};

// 0.600：命令行解析从 main() 提出。返回 0 表示继续，非 0 是进程退出码。
static int parse_options(int argc, char** argv, Options& o) {
    static const std::vector<std::string> kModes = {"tokens", "ast",  "check", "types", "ir",
                                                    "asm",    "build", "run",  "deps",  "cdecl",
                                                    "resolve"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--syntax") o.syntax_only = true;
        else if (a == "--imports") o.resolve_imports = true;
        else if (a == "--no-imports") o.no_imports = true;
        else if (a == "--lenient-names") o.lenient_names = true;
        else if (a == "--json") o.json_out = true;
        else if (a.starts_with("--out=")) o.output = a.substr(6);
        else if (a == "--out" && i + 1 < argc) o.output = argv[++i];
        else if (a.starts_with("--module-map=")) o.module_map_path = a.substr(13);
        else if (a == "--module-map" && i + 1 < argc) o.module_map_path = argv[++i];
        else if (a == "--locked") o.locked = true;
        else if (a.starts_with("--manifest=")) o.manifest_flag = a.substr(11);
        else if (a == "--manifest" && i + 1 < argc) o.manifest_flag = argv[++i];
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "--no-unsupported-note" || a == "-Wno-unsupported")
            o.no_unsupported_note = true;
        else if (a == "--rebuild") o.rebuild = true;
        else if (a == "--fast-math") o.fast_math = true;
        else if (a == "--asan") o.sanitize = "address";
        else if (a == "--ubsan") o.sanitize = "undefined";
        else if (a.starts_with("--sanitize=")) o.sanitize = a.substr(11);
        else if (a == "--zero-cost-exc") o.zero_cost_exc = true;
        else if (a == "--no-zero-cost-exc") o.zero_cost_exc = false;
        else if (a == "-O0") o.opt_level = 0;
        else if (a == "-O1") o.opt_level = 1;
        else if (a == "-O2") o.opt_level = 2;
        else if (a.starts_with("--opt=")) {
            std::string v = a.substr(6);
            if (v == "0") o.opt_level = 0;
            else if (v == "1") o.opt_level = 1;
            else if (v == "2") o.opt_level = 2;
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
        else if (a == "-o" && i + 1 < argc) o.output = argv[++i];
        else if (a == "--link" && i + 1 < argc) o.cdecl_links.push_back(argv[++i]);
        else if (a == "-I" && i + 1 < argc) o.include_dirs.push_back(argv[++i]);
        else if (a.starts_with("-I") && a.size() > 2) o.include_dirs.push_back(a.substr(2));
        else if (a == "--") {
            // `pyadamas run prog.py -- a b`：余下参数原样传给被运行的程序。
            for (++i; i < argc; ++i) o.prog_args.push_back(argv[i]);
            break;
        }
        else if (o.mode.empty() && std::ranges::find(kModes, a) != kModes.end()) o.mode = a;
        else o.path = a;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "pyadamas —— Python 语法外壳的编译器\n"
                     "\n"
                     "usage: %s [tokens|ast|types|check|ir|asm|build|run|deps|cdecl|resolve] "
                     "[-O0|-O1|-O2] [--fast-math] [--no-zero-cost-exc] [--syntax] "
                     "[--asan|--ubsan] [-o out|--out out] [--json] [--o.locked] "
                     "[--manifest file] [--module-map file] <file.py>\n",
                     argv[0]);
        return 2;
    }

    Options o;

    static const std::vector<std::string> kModes = {"tokens", "ast",  "check", "types", "ir",
                                                    "asm",    "build", "run",  "deps",  "cdecl",
                                                    "resolve"};
    if (int rc = parse_options(argc, argv, o)) return rc;
    if (o.mode.empty()) o.mode = "ast";
    // resolve 可以只给项目目录（甚至什么都不给：用 cwd），不需要入口文件。
    if (o.path.empty() && o.mode != "resolve") {
        std::fprintf(stderr, "error: no input file\n");
        return 2;
    }
    const bool package_mode = o.mode == "resolve" || o.mode == "deps" || o.mode == "asm" ||
                              o.mode == "ir" || o.mode == "build" || o.mode == "run";
    if (!package_mode && (o.locked || !o.manifest_flag.empty())) {
        std::fprintf(stderr,
                     "error: --o.locked/--manifest are only supported by "
                     "resolve/build/run/asm/ir/deps\n");
        return 2;
    }

    if (o.mode == "cdecl")
        return cmd_cdecl(o.path, o.output, o.cdecl_links, o.include_dirs);

    // resolve：清单 → path 依赖闭包 → 写锁。不读入口文件（可以只给项目目录）。
    if (o.mode == "resolve")
        return cmd_resolve(o.path, o.manifest_flag, o.locked);

    auto src = read_file(o.path);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", src.error().c_str());
        return 1;
    }

    try {
        if (!package_mode) {
            return cmd_frontend(o.path, *src, o.include_dirs, o.mode, o.syntax_only, o.lenient_names,
                                o.no_imports, o.resolve_imports, o.no_unsupported_note);
        }

        // deps：只建模块图与接口（不降级、不汇编），输出确定性 JSON。
        if (o.mode == "deps") {
            return cmd_deps(o.path, o.include_dirs, o.module_map_path, o.manifest_flag,
                            o.output, o.verbose,
                            o.json_out, o.locked);
        }

        Program prog;
        if (!setup_program(prog, o.path, o.include_dirs, o.sanitize, o.rebuild, o.module_map_path,
                           o.manifest_flag, o.locked, o.verbose, o.opt_level, o.fast_math,
                           o.zero_cost_exc))
            return 1;

        if (o.mode == "ir" || o.mode == "asm")
            return cmd_ir_asm(prog, o.mode);

        // build / run：模块各自汇编成 .o，再链接。
        return cmd_build_or_run(prog, o.mode, o.prog_args, o.output, o.path);
    } catch (const pyadamas::LexError& e) {
        report(o.path, *src, e.line, e.col, e.what());
    } catch (const pyadamas::ParseError& e) {
        report(o.path, *src, e.line, e.col, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: internal error: %s\n", o.path.c_str(), e.what());
    }
    return 1;
}
