// driver/build.cpp —— 多模块解析、增量缓存、汇编与链接
#include "driver/build.h"
#include "driver/build_cache.h"
#include "driver/progcache.h"
#include "driver/util.h"
#include "common/target_abi.h"

#include <algorithm>
#include <atomic>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string_view>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include "common/ast.h"
#include "backend/codegen.h"
#include "syntax/desugar.h"
#include "driver/link.h"
#include "driver/modules.h"
#include "ir/ir.h"
#include "syntax/lexer.h"
#include "lower/lower.h"
#include "ir/module.h"
#include "syntax/parser.h"
#include "typecheck/typecheck.h"

#ifndef PYADAMAS_RT_OBJ
#define PYADAMAS_RT_OBJ "build/dev/rt.o"
#endif
#ifndef PYADAMAS_RT_DEBUG_OBJ
#define PYADAMAS_RT_DEBUG_OBJ "build/dev/rt_allocdbg.o"
#endif
#ifndef PYADAMAS_MODCACHE_DIR
#define PYADAMAS_MODCACHE_DIR "build/dev/modcache"
#endif

namespace pyadamas {

// 给 `cc` 传的额外编译/链接选项，由环境变量提供。
//
// 用途：
//   * `PYADAMAS_CC_FLAGS='-fsanitize=address -g'` —— 用 ASan 跑生成的程序，
//     定位 RC/GC 这类「堆被写坏」的问题（编译器的 IR 层面看不出错误，
//     只有内存检查器能抓到第一现场）；
//   * `-fsanitize=undefined`、`-fno-omit-frame-pointer` 等。
//
// 编译和链接都要带同一套 flag（ASan 需要运行时库参与链接），所以两处
// 共用这一个函数。
// --asan 时改用插桩版运行时（rt_asan.o）。
//
// 默认的 rt.o 不带 sanitizer 插桩，**运行时内部**（RC/GC/列表/字典的指针
// 操作）的内存错误 ASan 看不到 —— 只有生成代码那一侧被检查。
// 由 main.cpp 在解析 --asan 时置位。
static bool g_use_asan_runtime = false;
void set_asan_runtime(bool on) { g_use_asan_runtime = on; }

// PYADAMAS_CANARY / PYADAMAS_GUARD：链接分配器调试版运行时（canary /
// guard page），用来抓生成代码的堆越界（ASan 只插桩运行时不覆盖生成代码）。
// 默认 rt.o 里这些钩子展开成裸 malloc/free，**零开销**；这里显式选择时
// 才换 rt_allocdbg.o。走链接期选对象而不是运行期 if，是为了让开关关掉时
// 生成程序的 instructions:u 逐项与基线相同（见 docs/memory-debug.md）。
// 环境变量在进程内只读一次（同一进程内所有链接共用同一个选择）。


namespace {

// 模块缓存写入必须原子：多个进程可能同时编译同一个模块（并行测试/构建），
// 直接写最终文件会让另一个进程读到截断的 mod.s/mod.o/manifest。


bool parse_unit(ModuleUnit& u) {
    try {
        pyadamas::Lexer lexer(u.source);
        pyadamas::Parser parser(lexer.run());
        u.ast = parser.parse();
        u.mentions_big = parser.mentions_big();
        pyadamas::desugar_lambdas(*u.ast);
        return true;
    } catch (const pyadamas::LexError& e) {
        report(u.path, u.source, e.line, e.col, e.what());
    } catch (const pyadamas::ParseError& e) {
        report(u.path, u.source, e.line, e.col, e.what());
    }
    return false;
}

// AVX2 4-lane 的编译期开关（PYADAMAS_AVX2=0 只发 SSE2）。它直接改代码生成
// 结果但不在 Program 里，所以必须进清单 —— 否则「切了开关仍命中旧产物」，
// A/B 两条路径拿到同一个 mod.s（与已删除的 rc_split 同一类坑，实测踩到）。

// 文件内容摘要：识别「编译器/运行时换了」。不用 mtime —— 拷贝、解包、
// checkout 之后 mtime 都会变（不可信），跨机器也无法比较。
// 实现在 driver/progcache.cpp（按 8 字节字混合；逐字节 FNV 对 151MB 的
// /proc/self/exe 要 25–30ms，而热构建总共才 23ms）。

// 编译器/运行时的构建指纹。源码没变但换了编译器时不能复用旧对象：
// 代码生成和运行时的行为不进源码哈希，旧清单必须失效。
//
// 旧实现用 exe/rt.o 的 size+mtime：拷贝/解包后 mtime 不可信，且**跨机器
// 无法比较** → 共享缓存在不同机器上要么永远 miss，要么在「拷来一份旧
// mtime」时误命中（用别的编译器编出的 mod.o）。改成**内容摘要**：只依赖
// 二进制内容 + 接口格式/ABI 版本。
// 覆盖口：PYADAMAS_TOOLCHAIN_ID（CI/自制工具链标识；设了就整体替换）。

// 缓存根目录：默认仍是配置期写死的路径（不破坏既有行为），
// 可用 PYADAMAS_CACHE_DIR 覆盖（跨项目共享 store / CI 隔离 / 测试）。



struct Manifest {
    uint64_t source_hash = 0;
    int opt = -1;
    int fast_math = -1;
    // 零开销异常开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int zero_cost_exc = -1;
    // 旧清单没有这一行：保守按「需要索引」处理。
    bool unknown_index = true;
    // callee-saved 提升增强开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int ra_opt = -1;
    // 通用循环向量化开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int vect = -1;
    // 浮点归约 packed 累加开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int vect_fastfloat = -1;
    // AVX2 4-lane 开关（PYADAMAS_AVX2）。-1 = 旧清单没有这一行：缓存必然
    // 失效一次（否则切换开关会命中另一条路径的 mod.s）。
    int avx2 = -1;
    // 整数循环优化位掩码。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int loop_opt = -1;
    // 内联/devirt 深化开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int inline_opt = -1;
    // 只读借用槽细化开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int borrow_opt = -1;
    // 运行期类型标签开关（PYADAMAS_TYPE_TAGS）。-1 = 旧清单没有这一行：
    // 缓存必然失效一次（tag 编号/判定不同，混用会读错类型）。
    int type_tags = -1;
    int dynamic_big = -1;  // 旧清单不能提供动态 big 安全语义
    // P4 语言缺口开关（PYADAMAS_LANG_GAPS）。-1 = 旧清单没有这一行：
    // 缓存必然失效一次（异常类当值等改变 IR，混用会静默丢语义）。
    int lang_gaps = -1;
    // 模块是否用到 from c import（dlsym RTLD_DEFAULT）。旧清单没有这一行：
    // 保守按「用到」处理（链接期关 --gc-sections，绝不冒误剥风险）。
    bool c_sym_default = true;
    std::string toolchain;
    // 模块版本坐标（编码后）。版本进 mangling，缓存/清单必须跟着校验：
    // 同源码换版本 → 符号不同，旧产物不能复用。
    std::string module_version;
    // 目标 ABI（target_abi().name）。它决定调用约定/影子空间，直接改代码
    // 生成结果，必须进清单：否则「同一模块先按 SysV 编、再 --target=win64 编」
    // 会命中前者的 mod.o，链接出一个 ABI 错误的二进制（静默错代码）。
    std::string target;
    std::map<std::string, std::string> deps;
};

std::optional<Manifest> read_manifest(const ModuleUnit& u) {
    std::ifstream in(u.manifest_path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    // 2：加入 target（目标 ABI）字段；3：加入 modulever（模块版本坐标）。
    // 旧版清单一律判失效（字段缺失 → 保守重编，绝不误命中）。
    if (!std::getline(in, line) || line != "pyadamasmanifest 3") return std::nullopt;
    Manifest m;
    bool abi_ok = false;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "abi") {
            std::string v;
            ss >> v;
            abi_ok = v == std::to_string(pyadamas::kModuleAbiVersion);
        } else if (tag == "source") {
            std::string v;
            ss >> v;
            auto h = parse_u64(v);
            if (!h) return std::nullopt;
            m.source_hash = *h;
        } else if (tag == "opt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.opt = static_cast<int>(*o);
        } else if (tag == "fastmath") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.fast_math = static_cast<int>(*o);
        } else if (tag == "zerocostexc") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.zero_cost_exc = static_cast<int>(*o);
        } else if (tag == "unknownindex") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.unknown_index = *o != 0;
        } else if (tag == "raopt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.ra_opt = static_cast<int>(*o);
        } else if (tag == "vect") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.vect = static_cast<int>(*o);
        } else if (tag == "vectff") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.vect_fastfloat = static_cast<int>(*o);
        } else if (tag == "avx2") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.avx2 = static_cast<int>(*o);
        } else if (tag == "loopopt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.loop_opt = static_cast<int>(*o);
        } else if (tag == "inlineopt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.inline_opt = static_cast<int>(*o);
        } else if (tag == "borrowopt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.borrow_opt = static_cast<int>(*o);
        } else if (tag == "typetags") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.type_tags = static_cast<int>(*o);
        } else if (tag == "dynamicbig") {
            std::string v;
            ss >> v;
            if (v != "0" && v != "1") return std::nullopt;
            m.dynamic_big = v == "1";
        } else if (tag == "langgaps") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.lang_gaps = static_cast<int>(*o);
        } else if (tag == "csymdefault") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.c_sym_default = *o != 0;
        } else if (tag == "modulever") {
            ss >> m.module_version;
        } else if (tag == "target") {
            ss >> m.target;
        } else if (tag == "toolchain") {
            ss >> m.toolchain;
        } else if (tag == "dep") {
            std::string name, hash;
            ss >> name >> hash;
            if (name.empty() || hash.empty()) return std::nullopt;
            m.deps[name] = hash;
        }
    }
    if (!abi_ok) return std::nullopt;
    // 缺 target/modulever 的清单（旧格式/写了一半）保守判失效：宁可重编，
    // 不可误命中。
    if (m.target.empty() || m.module_version.empty()) return std::nullopt;
    return m;
}

void write_manifest(const Program& p, const ModuleUnit& u) {
    std::error_code ec;
    std::filesystem::create_directories(u.cache_dir, ec);
    std::ostringstream out;
    out << "pyadamasmanifest 3\n";
    out << "abi " << pyadamas::kModuleAbiVersion << "\n";
    out << "source " << pyadamas::fnv1a64(u.source) << "\n";
    out << "opt " << p.opt << "\n";
    out << "fastmath " << (p.fast_math ? 1 : 0) << "\n";
    out << "zerocostexc " << (p.zero_cost_exc ? 1 : 0) << "\n";
    out << "unknownindex " << (u.needs_unknown_index ? 1 : 0) << "\n";
    out << "raopt " << (p.ra_opt ? 1 : 0) << "\n";
    out << "vect " << (p.vect ? 1 : 0) << "\n";
    out << "vectff " << (p.vect_fastfloat ? 1 : 0) << "\n";
    out << "avx2 " << avx2_emit_tag() << "\n";
    out << "loopopt " << p.loop_opt << "\n";
    out << "inlineopt " << (p.inline_opt ? 1 : 0) << "\n";
    out << "borrowopt " << (p.borrow_opt ? 1 : 0) << "\n";
    out << "typetags " << (p.type_tags ? 1 : 0) << "\n";
    out << "dynamicbig " << (p.needs_dynamic_big ? 1 : 0) << "\n";
    out << "langgaps " << (p.lang_gaps ? 1 : 0) << "\n";
    out << "csymdefault " << (u.uses_c_sym_default ? 1 : 0) << "\n";
    out << "target " << target_abi().name << "\n";
    out << "modulever " << sanitize_symbol(pyadamas::module_version()) << "\n";
    out << "toolchain " << toolchain_id() << "\n";
    for (auto& d : u.deps) {
        auto it = p.units.find(d);
        std::string hash = (it != p.units.end() && it->second->info)
                               ? it->second->info->interface_hash
                               : "";
        out << "dep " << d << " " << hash << "\n";
    }
    write_atomic(u.manifest_path, out.str());
}

// 解析一个 import：显式模块映射优先，未命中回落现有路径搜索。
//
// 原生（builtin）模块在调用点就被短路了，映射不能覆盖它们 —— 它们没有源
// 文件，语义由 builtin_sigs 决定。
static std::optional<ResolvedModule> resolve_import(Program& p, const std::string& importer,
                                                    const std::string& full_name) {
    if (p.module_map) {
        auto it = p.module_map->paths.find(full_name);
        if (it != p.module_map->paths.end()) {
            const std::string& path = it->second;
            const bool is_pkg = path.ends_with("/__init__.py") || path.ends_with("\\__init__.py");
            return ResolvedModule{path, is_pkg};
        }
    }
    return resolve_import_source(importer, full_name, p.include_dirs);
}

bool visit_unit(Program& p, ModuleUnit& u, std::vector<ModuleUnit*>& stack) {
    if (u.state == 2) return true;
    if (u.state == 1) {
        std::string chain;
        for (auto* x : stack) chain += x->name + " -> ";
        chain += u.name;
        std::fprintf(stderr, "%s: error: circular import: %s\n", u.path.c_str(), chain.c_str());
        return false;
    }
    u.state = 1;
    stack.push_back(&u);

    std::vector<ImportReq> reqs;
    std::vector<ImportIssue> issues;
    scan_imports_body(u.ast->body, reqs, issues, unit_package(u));
    if (!issues.empty()) {
        for (auto& i : issues) report(u.path, u.source, i.line, i.col, i.msg);
        return false;
    }

    // cimport 的 .cpya 声明文件不属于模块依赖图，只收集链接参数。
    {
        std::string dir = std::filesystem::path(u.path).parent_path().string();
        for (auto& s : u.ast->body) {
            if (s->kind != StmtKind::CImport) continue;
            auto& ci = static_cast<CImport&>(*s);
            for (auto& a : ci.names) {
                std::string err;
                auto decl = load_cpya(a.name, dir, p.include_dirs, err);
                if (!decl) {
                    report(u.path, u.source, s->line, s->col, err);
                    return false;
                }
                for (auto& l : decl->links) {
                    if (std::find(p.link_args.begin(), p.link_args.end(), l) == p.link_args.end())
                        p.link_args.push_back(l);
                }
            }
        }
    }

    // 合并同名依赖，保留 ImportFrom 的子模块候选名。
    std::map<std::string, ImportReq> merged;
    for (auto& r : reqs) {
        auto it = merged.find(r.name);
        if (it == merged.end()) merged[r.name] = r;
        else
            // A4（0.649）：同一个子模块被写两遍（`from pkg import a` ×2，或
            // `import pkg.a` + `from pkg import a`）时，from_names 会重复，
            // 进而让 pkg.a 入队两次、u.deps 出现两条同名项——manifest 读回
            // 却是 std::map（只剩一条），deps.size() 永远对不上 → 入口永久重编。
            for (auto& n : r.from_names)
                if (std::find(it->second.from_names.begin(), it->second.from_names.end(),
                              n) == it->second.from_names.end())
                    it->second.from_names.push_back(n);
    }
    std::vector<ImportReq> queue;
    for (auto& [n, r] : merged) queue.push_back(r);

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        ImportReq req = queue[qi];
        // `from . import mod`（包 __init__.py 里）：相对解析后 req.name 等于
        // 当前单元自身。只要所有 from_names 都能解析成子模块，这是「导入子模块」
        // 而不是循环导入 —— 跳过自身依赖、只把子模块排进队列（否则标准包写法
        // `from . import mod` 在 __init__.py 里必然报 circular import）。
        if (req.name == u.name && !req.from_names.empty()) {
            bool all_sub = true;
            for (auto& n : req.from_names)
                if (!resolve_import(p, u.path, req.name + "." + n)) {
                    all_sub = false;
                    break;
                }
            if (all_sub) {
                for (auto& n : req.from_names) {
                    std::string child = req.name + "." + n;
                    if (!p.units.count(child)) queue.push_back({child, req.line, req.col, {}});
                }
                continue;
            }
        }
        bool dotted = req.name.find('.') != std::string::npos;
        bool name_ok = dotted ? [&] {
            size_t s = 0;
            while (s <= req.name.size()) {
                size_t d = req.name.find('.', s);
                std::string part = req.name.substr(s, d == std::string::npos ? std::string::npos
                                                                             : d - s);
                if (!valid_module_name(part)) return false;
                if (d == std::string::npos) break;
                s = d + 1;
            }
            return true;
        }()
                              : valid_module_name(req.name);
        if (!name_ok) {
            report(u.path, u.source, req.line, req.col, "invalid module name");
            return false;
        }
        if (req.name == "__entry__") {
            report(u.path, u.source, req.line, req.col,
                   "'__entry__' is reserved for the entry module");
            return false;
        }
        // 纯原生模块没有源文件/对象，只提供编译期接口；原生 + stdlib
        // 扩展源的模块（functools）仍要按普通模块编译扩展源。
        const ModuleEntry* entry = find_module_entry(req.name);
        if (is_native_module(entry)) continue;
        auto found = resolve_import(p, u.path, req.name);
        if (!found) {
            // 0.559：`import x` 但旁边只有 x.cpya —— 报清楚要改用 cimport。
            std::error_code cec;
            auto has_cpya = [&](const std::filesystem::path& dir) {
                return std::filesystem::is_regular_file(dir / (req.name + ".cpya"), cec);
            };
            bool cpya_nearby = has_cpya(std::filesystem::path(u.path).parent_path());
            for (auto& d : p.include_dirs)
                if (!cpya_nearby) cpya_nearby = has_cpya(d);
            if (cpya_nearby) {
                report(u.path, u.source, req.line, req.col,
                       std::format("'{}' is a C declaration file (.cpya): use "
                                   "'cimport {}' instead of 'import {}'",
                                   req.name, req.name, req.name));
                return false;
            }
            report(u.path, u.source, req.line, req.col,
                   std::format("cannot find module '{}'", req.name));
            return false;
        }
        std::string canonical = canonical_path(found->path);
        // 单一数据源：随编译器分发的 stdlib 源必须在模块表里登记。
        if (!entry && path_under_stdlib(canonical)) {
            report(u.path, u.source, req.line, req.col,
                   std::format("stdlib module '{}' is not registered in the builtin module table",
                               req.name));
            return false;
        }
        auto it = p.units.find(req.name);
        ModuleUnit* dep = nullptr;
        if (it != p.units.end()) {
            if (it->second->path != canonical) {
                report(u.path, u.source, req.line, req.col,
                       std::format("module '{}' resolves to two different files", req.name));
                return false;
            }
            dep = it->second.get();
        } else {
            auto unit = std::make_unique<ModuleUnit>();
            unit->key = req.name;
            unit->name = req.name;
            unit->runtime_name = req.name;
            unit->path = canonical;
            unit->is_package = found->is_package;
            auto dot = req.name.rfind('.');
            if (dot != std::string::npos) unit->parent_name = req.name.substr(0, dot);
            auto src = read_file(canonical);
            if (!src) {
                report(u.path, u.source, req.line, req.col, src.error());
                return false;
            }
            unit->source = std::move(*src);
            set_cache_paths(*unit);
            if (!parse_unit(*unit)) return false;
            dep = unit.get();
            p.units[req.name] = std::move(unit);
        }
        if (!visit_unit(p, *dep, stack)) return false;
        // 同名依赖只记一条：manifest 是 map 语义，重复项会让缓存键恒不相等。
        if (std::find(u.deps.begin(), u.deps.end(), req.name) == u.deps.end())
            u.deps.push_back(req.name);
        // from pkg import mod：mod 可能是子模块，按完整名加载一份接口。
        for (auto& n : req.from_names) {
            std::string child = req.name + "." + n;
            if (p.units.count(child)) continue;
            if (resolve_import(p, u.path, child))
                queue.push_back({child, req.line, req.col, {}});
        }
    }

    stack.pop_back();
    u.state = 2;
    p.order.push_back(&u);
    return true;
}

bool assemble_object(const ModuleUnit& u, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(u.cache_dir, ec);
    std::string tmp_asm = cache_temp_path(u.asm_path, ".s");
    std::string tmp_obj = cache_temp_path(u.obj_path);
    {
        std::ofstream out(tmp_asm, std::ios::binary);
        if (!out) {
            err = std::format("cannot write '{}'", u.asm_path);
            return false;
        }
        out << u.asm_text;
        if (!out) {
            out.close();
            std::filesystem::remove(tmp_asm, ec);
            err = std::format("cannot write '{}'", u.asm_path);
            return false;
        }
    }
    // 汇编走 as_prefix()：配置期拿到 as 路径时直接 exec as（省 gcc 驱动的
    // 一次进程链），否则仍是 "cc -c"。目标 ABI 为 Win64 时生成的是 COFF
    // 汇编，必须交给 gcc 交叉汇编器，不能直调 as。
    const bool elf_target = std::string_view(target_abi().name) == "x86_64-sysv";
    // 带额外 flag（--asan 的 -fsanitize=... 等）时必须继续走 cc：那些不是
    // 汇编器选项，直调 as 会直接失败。
    const char* extra = std::getenv("PYADAMAS_CC_FLAGS");
    const bool plain = (extra == nullptr || *extra == '\0');
    std::string cmd = std::format("{} -o {} {}{}", (elf_target && plain) ? as_prefix() : "cc -c",
                                  shell_quote(tmp_obj), shell_quote(tmp_asm),
                                  cc_extra_flags());
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(tmp_obj, ec);
        std::filesystem::remove(tmp_asm, ec);
        err = "assembler failed";
        return false;
    }
    std::filesystem::rename(tmp_obj, u.obj_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_obj, ec);
        std::filesystem::remove(tmp_asm, ec);
        err = std::format("cannot write '{}'", u.obj_path);
        return false;
    }
    std::filesystem::rename(tmp_asm, u.asm_path, ec);
    if (ec) std::filesystem::remove(tmp_asm, ec);
    return true;
}

bool try_cache(Program& p, ModuleUnit& u) {
    if (p.rebuild) return false;
    auto m = read_manifest(u);
    if (!m) return false;
    if (m->source_hash != pyadamas::fnv1a64(u.source)) return false;
    if (m->target != std::string(target_abi().name)) return false;  // 换了目标 ABI
    if (m->module_version != sanitize_symbol(pyadamas::module_version()))
        return false;  // 换了模块版本坐标（符号会变）
    if (m->toolchain != toolchain_id()) return false;  // 换了编译器/运行时
    if (m->opt != p.opt) return false;
    if (m->fast_math != (p.fast_math ? 1 : 0)) return false;
    if (m->zero_cost_exc != (p.zero_cost_exc ? 1 : 0)) return false;
    if (m->ra_opt != (p.ra_opt ? 1 : 0)) return false;
    if (m->vect != (p.vect ? 1 : 0)) return false;
    if (m->vect_fastfloat != (p.vect_fastfloat ? 1 : 0)) return false;
    if (m->avx2 != avx2_emit_tag()) return false;
    if (m->loop_opt != p.loop_opt) return false;
    if (m->inline_opt != (p.inline_opt ? 1 : 0)) return false;
    if (m->borrow_opt != (p.borrow_opt ? 1 : 0)) return false;
    if (m->type_tags != (p.type_tags ? 1 : 0)) return false;
    if (m->dynamic_big != (p.needs_dynamic_big ? 1 : 0)) return false;
    if (m->lang_gaps != (p.lang_gaps ? 1 : 0)) return false;
    if (m->deps.size() != u.deps.size()) return false;
    for (auto& d : u.deps) {
        auto it = m->deps.find(d);
        if (it == m->deps.end()) return false;
        auto unit_it = p.units.find(d);
        if (unit_it == p.units.end() || !unit_it->second->info) return false;
        if (it->second != unit_it->second->info->interface_hash) return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(u.obj_path, ec)) return false;
    if (!u.is_entry) {
        auto bytes = read_file(u.iface_path);
        if (!bytes) return false;
        // 0.540：跨模块类引用（tag 29）需要按模块名取别的模块的接口。
        auto iface_loader = [&p](const std::string& name) -> pyadamas::ModuleInfoPtr {
            auto it = p.units.find(name);
            if (it != p.units.end() && it->second && it->second->info) return it->second->info;
            if (auto b = pyadamas::builtin_module(name)) return b;
            return nullptr;
        };
        auto info = pyadamas::decode_module_interface(*bytes, u.path, iface_loader);
        if (!info || (*info)->name != u.name) return false;
        (*info)->interface_hash = pyadamas::hex64(pyadamas::fnv1a64(*bytes));
        u.info = *info;
    }
    u.needs_unknown_index = m->unknown_index;
    p.needs_unknown_index |= u.needs_unknown_index;
    u.uses_c_sym_default = m->c_sym_default;
    p.uses_c_sym_default |= u.uses_c_sym_default;
    u.from_cache = true;
    return true;
}

pyadamas::ModuleInfoPtr program_module(Program& p, const std::string& name) {
    auto unit_info = [&p, &name]() -> pyadamas::ModuleInfoPtr {
        auto it = p.units.find(name);
        return it == p.units.end() ? nullptr : it->second->info;
    };
    // CPython 桥子代理（python.<module>）是开集，不进模块表，按需生成。
    if (name.starts_with("python.")) return builtin_module(name);
    const ModuleEntry* e = find_module_entry(name);
    if (!e) return unit_info();  // 未登记：用户模块 / 包内子模块
    switch (e->kind) {
        case ModuleKind::Native:
            return builtin_module(name);
        case ModuleKind::Python:
            return unit_info();
        case ModuleKind::NativePython: {
            auto base = builtin_module(name);
            if (auto ext = unit_info()) return merge_extension_module(base, ext);
            // 扩展源还没建出接口（理论上不会出现在正常构建序里）：退回原生，
            // 让类型检查按原生接口跑，别静默丢接口。
            return base;
        }
    }
    return unit_info();
}

bool build_unit(Program& p, ModuleUnit& u, bool emit_object, bool entry_ir) {
    auto now = [] { return std::chrono::steady_clock::now(); };
    const bool timing = std::getenv("PYADAMAS_TIMING") != nullptr;
    auto t0 = now();
    auto tc = std::make_shared<pyadamas::TypeChecker>(u.path);
    tc->set_module_lookup([&p](const std::string& name) -> pyadamas::ModuleInfoPtr {
        return program_module(p, name);
    });
    tc->set_cinclude_dirs(p.include_dirs);
    // build/run 会把模块编成机器码：模块级未定义名字必须当错误报，
    // 否则降级成初值 0 的全局，运行期静默算错。
    tc->set_strict_module_names(true);
    tc->set_lang_gaps(p.lang_gaps);
    tc->check(*u.ast);
    auto t1 = now();
    if (!tc->diags().empty()) {
        for (auto& d : tc->diags()) report(u.path, u.source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s in %s\n", tc->diags().size(),
                     tc->diags().size() == 1 ? "" : "s", u.path.c_str());
        return false;
    }
    u.tc = tc;

    auto info = std::make_shared<pyadamas::ModuleInfo>();
    info->name = u.name;
    info->version = pyadamas::module_version();
    info->source_path = u.path;
    // 导出类的完整祖先链：本地类按 C3 MRO 拍平，导入类直接沿用接口里
    // 已展开的链。mirror 元数据靠 module/name/qualname 生成。
    auto ancestors_of = [&](const pyadamas::ClassInfo* cls) {
        std::vector<pyadamas::ClassAncestor> out;
        auto add = [&](const pyadamas::ClassAncestor& a) {
            for (auto& e : out)
                if (e.module == a.module && e.name == a.name) return;
            out.push_back(a);
        };
        if (cls->imported()) return cls->ext_ancestors;
        for (const pyadamas::ClassInfo* k : cls->mro) {
            if (!k || k == cls) continue;
            if (k->imported()) {
                add({k->origin_module, k->external_name(), k->origin_qualname});
                for (auto& a : k->ext_ancestors) add(a);
            } else {
                add({u.name, k->name, u.runtime_name + "." + k->name});
            }
        }
        return out;
    };
    if (auto* sc = tc->module_scope()) {
        for (auto& [name, sym] : sc->vars) {
            if (!u.is_entry && sym.ty && sym.ty->kind == pyadamas::TyKind::Class && sym.ty->cls) {
                // 扁平类接口：字段按根→派生顺序拍平，方法按最终槽位覆盖。
                const pyadamas::ClassInfo* cls = sym.ty->cls;
                std::vector<const pyadamas::ClassInfo*> chain;
                for (auto* cp = cls; cp; cp = cp->base) chain.push_back(cp);
                auto flat = std::make_shared<pyadamas::ClassInfo>();
                flat->name = name;
                flat->complete = true;
                flat->has_init = cls->has_init;
                flat->has_field_defaults = cls->has_field_defaults;
                flat->open = cls->is_open();
                // 动态属性槽标记跟着接口走：导入方才能放行 args.x 直读/
                // 直写/getattr（运行期 dict_off 在定义模块的 vtable 上）。
                for (const pyadamas::ClassInfo* k = cls; k; k = k->base)
                    if (k->needs_dict) { flat->needs_dict = true; break; }
                // 异常类标记要跟着接口走，导入方才能放行 except/raise 名字。
                flat->is_exception_class = cls->is_exception_class;
                flat->is_rich_exception = cls->is_rich_exception;
                flat->exc_parent = cls->exc_parent;
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    for (auto& f : (*it)->fields) flat->fields.push_back(f);
                // __match_args__ 决定位置类模式的字段顺序，要跟着接口走。
                if (const auto* ma = cls->match_args_of())
                    flat->match_args = *ma;
                else
                    flat->match_args = cls->match_args;
                // 完整槽表：按 C3 MRO 收集所有 (name, kind)，同名不同槽
                // 作为 aliases 一起导出；不同名同槽是真正的冲突。
                std::map<std::string, pyadamas::MethodInfo> by_key;
                std::map<std::string, std::vector<int>> key_slots;
                std::map<int, std::string> slot_owner;
                bool conflict = false;
                std::vector<const pyadamas::ClassInfo*> mro;
                for (auto* k : cls->mro) mro.push_back(k);
                if (mro.empty()) mro.push_back(cls);
                for (const pyadamas::ClassInfo* k : mro) {
                    if (!k) continue;
                    for (auto& m : k->methods) {
                        if (!m.call_sig) continue;
                        std::string key = pyadamas::method_slot_key(m.name, m.kind);
                        auto add_slot = [&](int slot) -> bool {
                            auto sit = slot_owner.find(slot);
                            if (sit != slot_owner.end() && sit->second != key) {
                                report(u.path, u.source, 0, 0,
                                       std::format("method slot {} of '{}' is used by both "
                                                   "'{}' and '{}' (cross-module slot conflict)",
                                                   slot, name, sit->second, key));
                                conflict = true;
                                return false;
                            }
                            slot_owner[slot] = key;
                            auto& v = key_slots[key];
                            if (std::find(v.begin(), v.end(), slot) == v.end()) v.push_back(slot);
                            return true;
                        };
                        if (!add_slot(m.slot)) break;
                        for (int a : m.aliases)
                            if (!add_slot(a)) break;
                        if (conflict) break;
                        if (!by_key.count(key)) by_key[key] = m;
                    }
                    if (conflict) break;
                }
                if (conflict) return false;
                for (auto& [key, m] : by_key) {
                    pyadamas::MethodInfo nm;
                    nm.name = m.name;
                    nm.kind = m.kind;
                    nm.slot = m.slot;
                    for (int slot : key_slots[key])
                        if (slot != m.slot) nm.aliases.push_back(slot);
                    nm.call_sig = m.call_sig;
                    nm.sig = m.call_sig;
                    // 0.536：装饰过的方法必须把标记带过接口（temp-proj 的 J2：
                    // 跨模块调用被装饰方法会按 vtable 走空槽 → 段错误）。
                    nm.decorated = m.decorated;
                    flat->methods.push_back(std::move(nm));
                }
                // 属性描述符按方法索引重建，导入方才能识别 @property。
                for (size_t mi = 0; mi < flat->methods.size(); ++mi) {
                    pyadamas::MethodInfo& m = flat->methods[mi];
                    if (!m.is_accessor()) continue;
                    auto& pi = flat->properties[m.name];
                    pi.owner = flat.get();
                    switch (m.kind) {
                        case pyadamas::MethodKind::PropertyGetter:
                            pi.getter = static_cast<int>(mi);
                            break;
                        case pyadamas::MethodKind::PropertySetter:
                            pi.setter = static_cast<int>(mi);
                            break;
                        case pyadamas::MethodKind::PropertyDeleter:
                            pi.deleter = static_cast<int>(mi);
                            break;
                        default: break;
                    }
                }
                flat->origin_module = cls->imported() ? cls->origin_module : u.name;
                flat->origin_qualname =
                    cls->imported() ? cls->origin_qualname : u.runtime_name + "." + name;
                flat->ext_ancestors = ancestors_of(cls);
                pyadamas::ImportedClass ic;
                ic.cls = flat;
                ic.ctor_symbol = pyadamas::module_class_new_symbol(
                    flat->origin_module, cls->imported() ? cls->external_name() : name);
                ic.module = flat->origin_module;
                ic.qualname = flat->origin_qualname;
                if (auto* im = flat->method("__init__"); im && im->call_sig)
                    ic.ctor_sig = im->call_sig;
                if (!ic.ctor_sig) ic.ctor_sig = std::make_shared<pyadamas::FuncInfo>();
                info->classes[name] = std::move(ic);
                continue;
            }
            info->globals[name] = sym.ty;
        }
    }
    // 0.543：导出用户 struct（.cpya 的 c_struct 不导出，布局由 C 头决定）。
    // **拷贝**一份：导入方的 Type::st 指向 ModuleInfo 持有的对象，不借用定义模块 TypeChecker 的所有权。
    for (auto& [sname, sp] : tc->local_structs())
        if (sp && !sp->c_struct) info->structs[sname] = std::make_shared<pyadamas::StructInfo>(*sp);
    info->globals["__name__"] = pyadamas::t_str();
    info->globals["__file__"] = pyadamas::t_str();
    // 新构建的模块也走一遍接口编解码：导出签名里的类类型必须指向扁平
    // ClassInfo（带 origin_module），不能泄漏本模块内部的 ClassInfo 指针。
    if (!u.is_entry) {
        auto bytes = pyadamas::encode_module_interface(*info);
        if (!bytes) {
            report(u.path, u.source, 0, 0,
                   std::format("cannot encode module interface: {}", bytes.error()));
            return false;
        }
        auto iface_loader = [&p](const std::string& name) -> pyadamas::ModuleInfoPtr {
            auto it = p.units.find(name);
            if (it != p.units.end() && it->second && it->second->info) return it->second->info;
            if (auto b = pyadamas::builtin_module(name)) return b;
            return nullptr;
        };
        auto norm = pyadamas::decode_module_interface(*bytes, u.path, iface_loader);
        if (!norm) {
            report(u.path, u.source, 0, 0,
                   std::format("cannot decode module interface: {}", norm.error()));
            return false;
        }
        (*norm)->interface_hash = pyadamas::hex64(pyadamas::fnv1a64(*bytes));
        info = *norm;
    }
    u.info = info;

    if (!emit_object && !entry_ir) return true;
    if (entry_ir && !u.is_entry) return true;

    pyadamas::LowerOptions opt;
    opt.module_symbol = u.name;
    opt.runtime_name = u.runtime_name;
    opt.source_path = u.path;
    opt.is_entry = u.is_entry;
    opt.opt_level = p.opt;
    opt.loop_opt = p.loop_opt;
    opt.inline_opt = p.inline_opt;
    opt.borrow_opt = p.borrow_opt;
    opt.type_tags = p.type_tags;
    opt.needs_dynamic_big = p.needs_dynamic_big;
    opt.lang_gaps = p.lang_gaps;
    opt.fast_math = p.fast_math;
    opt.zero_cost_exc = p.zero_cost_exc;
    opt.module_lookup = [&p](const std::string& name) -> pyadamas::ModuleInfoPtr {
        return program_module(p, name);
    };
    auto lr = pyadamas::lower_module(*u.ast, *tc, u.path, opt);
    auto t2 = now();
    if (!lr.errors.empty()) {
        for (auto& e : lr.errors) report(u.path, u.source, e.line, e.col, e.msg);
        std::fprintf(stderr, "%zu error%s in code generation (%s)\n", lr.errors.size(),
                     lr.errors.size() == 1 ? "" : "s", u.path.c_str());
        return false;
    }
    u.needs_unknown_index = pyadamas::ir_module_needs_unknown_index(lr.mod);
    // A/B 诊断：强制保留存活堆索引（对照「关闭」带来的收益）。
    if (std::getenv("PYADAMAS_FORCE_UNKNOWN_INDEX")) u.needs_unknown_index = true;
    p.needs_unknown_index |= u.needs_unknown_index;
    u.uses_c_sym_default = pyadamas::ir_module_uses_c_sym_default(lr.mod);
    p.uses_c_sym_default |= u.uses_c_sym_default;
    if (entry_ir) {
        u.ir = std::move(lr.mod);
        if (timing) {
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr, "[timing] %s check=%.1f lower=%.1f ms\n", u.name.c_str(),
                         ms(t0, t1), ms(t1, t2));
        }
        return true;
    }
    if (!emit_object) return true;

    // 方案 C：编译器能证明本模块不做「反查这个字是不是堆对象」时，索引
    // 维护是纯开销，关掉它（实测 bench_rc_gen 省 22ms）。
    // 方案 C：独立于 needs_unknown_index 判断。
    //
    // needs_unknown_index 回答「索引要不要建」，而这里回答「建了有没有人查」。
    // 一个模块可能既需要索引（有裸 unknown 槽）又从不查询（那些 unknown 只在
    // 容器里流转）—— 此时维护索引纯属浪费。实测 bench_rc_gen 属于这种：
    // needs=1，但关闭索引维护后输出完全正确、快 22ms。
    bool skip_idx = u.is_entry && pyadamas::ir_module_can_skip_index_maint(lr.mod);
    u.asm_text = pyadamas::generate_asm(lr.mod, u.is_entry && !p.needs_unknown_index,
                                        p.zero_cost_exc, skip_idx,
                                        p.ra_opt, p.inline_opt,
                                        p.type_tags, p.vect, p.needs_dynamic_big,
                                        p.vect_fastfloat);
    auto t3 = now();
    std::string err;
    if (!assemble_object(u, err)) {
        std::fprintf(stderr, "%s: error: %s\n", u.path.c_str(), err.c_str());
        return false;
    }
    auto t4 = now();
    if (timing) {
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr, "[timing] %s check=%.1f lower=%.1f asm=%.1f cc=%.1f ms\n",
                     u.name.c_str(), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
    }
    if (!u.is_entry) {
        auto bytes = pyadamas::encode_module_interface(*info);
        if (!bytes) {
            report(u.path, u.source, 1, 1,
                   std::format("cannot export module interface: {}", bytes.error()));
            return false;
        }
        info->interface_hash = pyadamas::hex64(pyadamas::fnv1a64(*bytes));
        std::error_code ec;
        std::filesystem::create_directories(u.cache_dir, ec);
        if (!write_atomic(u.iface_path, *bytes)) {
            report(u.path, u.source, 1, 1, "cannot write module interface");
            return false;
        }
    }
    write_manifest(p, u);
    return true;
}

}  // namespace

// 给 progcache.cpp 的导出壳：cache_root() 在匿名命名空间里，外部链接不到。
std::string prog_cache_root() { return cache_root(); }

bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p) {
    if (!validate_module_table()) return false;
    p.include_dirs = include_dirs;
    // 随编译器分发的标准库源码目录：导入方同目录与用户 -I 优先，最后落这里。
    p.include_dirs.emplace_back(pyadamas::stdlib_dir());
    auto unit = std::make_unique<ModuleUnit>();
    unit->key = "@entry";
    unit->name = "__entry__";
    unit->runtime_name = "__main__";
    unit->path = canonical_path(entry_path);
    unit->is_entry = true;
    auto src = read_file(unit->path);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", src.error().c_str());
        return false;
    }
    unit->source = std::move(*src);
    set_cache_paths(*unit);
    if (!parse_unit(*unit)) return false;
    p.entry = unit.get();
    p.units["@entry"] = std::move(unit);
    std::vector<ModuleUnit*> stack;
    if (!visit_unit(p, *p.entry, stack)) return false;
    // 所有依赖都已解析（包括可能命中缓存的模块），先定全图模式再开始 lowering。
    p.needs_dynamic_big = std::ranges::any_of(p.order, [](const ModuleUnit* u) {
        return u->mentions_big;
    });
    return true;
}

bool prepare_program(Program& p, bool emit_objects, bool entry_ir) {
    if (program_wants_lang_gaps(p)) {
        p.lang_gaps = true;
        p.type_tags = true;  // P4 的 CLASS tag 是 dataclass 展开的表示基础
    }
    if (program_wants_type_tags(p)) p.type_tags = true;
    // 动态 big 模式下的 unknown 探针（AnyBigBinary/AnyInvert 与 key_kind 9）
    // 都靠堆注册表确认「这个机器字确实是堆对象」。任一模块需要就整程序建表：
    // 索引一旦关闭，探针恒假，等值 BigInt 会被当成两个不同指针。
    if (p.needs_dynamic_big) p.needs_unknown_index = true;
    for (auto* u : p.order) {
        if (emit_objects && try_cache(p, *u)) {
            if (emit_objects && p.verbose)
                std::fprintf(stderr, "note: cached module %s\n", u->name.c_str());
            continue;
        }
        if (emit_objects && p.verbose)
            std::fprintf(stderr, "note: building module %s\n", u->name.c_str());
        if (!build_unit(p, *u, emit_objects, entry_ir)) return false;
    }
    return true;
}


bool link_program(Program& p, const std::string& output) {
    // --eh-frame-hdr：运行时 traceback 用 _Unwind_Backtrace 找 FDE，
    // 可执行文件必须带 .eh_frame_hdr 索引。多数发行版默认就开，这里显式
    // 写死，免得换个链接器就退化成「只能打异常、打不出回溯」。
    //
    // 链接有两条路（见 driver/link.cpp）：配置期成功复刻出 gcc 的 ld 命令行
    // 时直接 exec ld，省掉 gcc 驱动 + collect2 的 11 ms 固定开销；否则回退
    // 到 cc。两条路生成的二进制逐字节相同（--build-id 由输入决定）。
    std::vector<std::string> obj_paths;
    obj_paths.reserve(p.order.size() + 1);
    for (auto* u : p.order) obj_paths.push_back(u->obj_path);
    // 运行时变体三选一（符号集合相同，链接期只取一个）：
    //   --asan            → rt_asan.o（运行时代码插桩）
    //   PYADAMAS_CANARY/GUARD → rt_allocdbg.o（分配器 canary / guard page）
    //   默认              → rt.o（零开销；钩子宏是裸 malloc/free）
    // 注意 --asan 与 canary/guard 同时给时 asan 优先（两者分配器改造
    // 互斥，文档里写清）。
    bool dbg_rt = false;
    if (!g_use_asan_runtime && debug_alloc_runtime_enabled()) {
        if (file_digest(PYADAMAS_RT_DEBUG_OBJ) == 0) {
            std::fprintf(stderr,
                         "error: PYADAMAS_CANARY/PYADAMAS_GUARD 需要分配器调试版运行时\n"
                         "       （%s 不存在；先重新构建，或取消该环境变量）\n",
                         PYADAMAS_RT_DEBUG_OBJ);
            return false;
        }
        dbg_rt = true;
    }
    const std::string rt_obj = g_use_asan_runtime
                                  ? PYADAMAS_RT_ASAN_OBJ
                                  : (dbg_rt ? PYADAMAS_RT_DEBUG_OBJ : PYADAMAS_RT_OBJ);
    obj_paths.push_back(rt_obj);
    std::string objs;
    for (const auto& o : obj_paths) objs += " " + shell_quote(o);
    // -ldl：rt_cffi 的 dlopen/dlsym（glibc 2.34+ 已并入 libc，-ldl 是空壳，
    // 旧 glibc 仍需要）。Win64 目前只做交叉汇编验证，不走这条链接路径。
    objs += " -lm -ldl";
    for (auto& l : p.link_args) objs += " " + shell_quote(l);
    const bool elf_target = std::string_view(target_abi().name) == "x86_64-sysv";
    // --gc-sections（slim 轮）：剥掉用户程序未引用的运行时函数/数据段。
    // PYADAMAS_GC_SECTIONS=1 开启（默认关，灰度）；direct-ld 与 cc 回退两条
    // 路等价附加，保持「direct-ld == cc 逐字节」不变量。
    // from c import（dlsym RTLD_DEFAULT）的程序可能运行期查找任何运行时符号，
    // 静态可达性覆盖不到 —— 整程序保守关 gc（uses_c_sym_default，见 ir/ir.cpp）。
    const bool gc = elf_target && gc_sections_enabled() && !p.uses_c_sym_default;
    const std::string gc_ld = gc ? " --gc-sections" : "";
    const std::string gc_cc = gc ? " -Wl,--gc-sections" : "";
    // ── 整程序产物缓存（0.478）──
    // 键由**真实链接输入**推导：每个 .o（含运行时）的内容摘要 + target +
    // toolchain + gc/asan/cc 额外 flags + cimport 链接参数。任一变化键就变。
    // 不从命令行选项名单猜，避免「漏一项 = 静默用错二进制」。
    std::string key;
    const bool cache_on = progcache_enabled() && !p.rebuild && elf_target;
    if (cache_on) {
        std::string extra = std::format("target={}|toolchain={}|gc={}|asan={}|cc={}|args=",
                                        target_abi().name, toolchain_id(), gc ? 1 : 0,
                                        g_use_asan_runtime ? 1 : 0, cc_extra_flags());
        for (auto& l : p.link_args) extra += l + "\x1f";
        key = progcache_key(obj_paths, extra);
        if (!key.empty() && progcache_fetch(key, output)) {
            if (p.verbose)
                std::fprintf(stderr, "note: program cache hit %s\n", key.c_str());
            return true;
        }
    }
    auto store = [&]() {
        if (cache_on && !key.empty()) progcache_store(key, output);
    };
    if (elf_target && direct_ld_available() && direct_ld_args_ok(p.link_args)) {
        std::string ld_cmd = direct_ld_prefix() + gc_ld + " -o " + shell_quote(output) + objs + " " +
                             direct_ld_tail();
        if (std::system(ld_cmd.c_str()) == 0) {
            store();
            return true;
        }
        // ld 直接链接失败（环境差异）：不报错，回退 cc 再试一次。
    }
    std::string cmd = "cc -o " + shell_quote(output) + " -Wl,--eh-frame-hdr" + gc_cc + cc_extra_flags() + objs;
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "error: link failed\n       command: %s\n", cmd.c_str());
        return false;
    }
    store();
    return true;
}

}  // namespace pyadamas
