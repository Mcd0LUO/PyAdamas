// driver_build.cpp —— 多模块解析、增量缓存、汇编与链接
#include "driver_build.h"
#include "driver_util.h"
#include "target_abi.h"

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

#include "ast.h"
#include "codegen.h"
#include "desugar.h"
#include "driver_link.h"
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

static std::string cc_extra_flags() {
    const char* f = std::getenv("PYADAMAS_CC_FLAGS");
    if (!f || !*f) return std::string();
    return std::string(" ") + f;
}

namespace {

// 模块缓存写入必须原子：多个进程可能同时编译同一个模块（并行测试/构建），
// 直接写最终文件会让另一个进程读到截断的 mod.s/mod.o/manifest。
static std::string cache_temp_path(const std::string& path, const char* ext = "") {
    static std::atomic<uint64_t> seq{0};
    return std::format("{}.tmp.{}.{}{}", path, static_cast<long long>(::getpid()),
                       seq.fetch_add(1, std::memory_order_relaxed), ext);
}

static bool write_atomic(const std::string& path, const std::string& data) {
    std::string tmp = cache_temp_path(path);
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string sanitize_symbol(const std::string& s) {
    std::string out;
    for (char c : s)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_');
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

bool valid_module_name(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

std::string canonical_path(const std::string& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    return ec ? std::filesystem::absolute(p).lexically_normal().string() : c.string();
}

struct ResolvedModule {
    std::string path;
    bool is_package = false;
};

std::optional<ResolvedModule> resolve_module_path(const std::string& importer,
                                                  const std::string& full_name,
                                                  const std::vector<std::string>& include_dirs) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dot = full_name.find('.', start);
        parts.push_back(full_name.substr(start, dot == std::string::npos ? std::string::npos
                                                                        : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    std::vector<std::filesystem::path> roots;
    roots.push_back(std::filesystem::path(importer).parent_path());
    for (auto& d : include_dirs) roots.emplace_back(d);
    // 相对导入被改写成完整包名后，模块所在包可能比入口目录深；
    // 从 importer 目录逐级向上找包根。
    for (auto p = std::filesystem::path(importer).parent_path().parent_path();
         !p.empty(); p = p.parent_path()) {
        roots.push_back(p);
        if (p == p.root_path()) break;
    }
    std::error_code ec;
    for (auto& root : roots) {
        std::filesystem::path dir = root;
        bool ok = true;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            dir /= parts[i];
            if (!std::filesystem::is_regular_file(dir / "__init__.py", ec)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        auto file = dir / (parts.back() + ".py");
        if (std::filesystem::is_regular_file(file, ec))
            return ResolvedModule{canonical_path(file.string()), false};
        auto pkg = dir / parts.back() / "__init__.py";
        if (std::filesystem::is_regular_file(pkg, ec))
            return ResolvedModule{canonical_path(pkg.string()), true};
    }
    return std::nullopt;
}

// ── 模块表：内建模块与 stdlib Python 源的单一数据源 ───────────────
// 每个可导入模块在这里占一行；import 文件解析（visit_unit）、类型检查
// 用的模块接口（program_module）、check 模式的 import 图（check_load）
// 都只查这张表，不再各自维护一份硬编码名单。
//   Native       原生模块：接口由 builtin_sigs.cpp 提供，没有源文件。
//   Python       纯 Python 源模块：由随编译器分发的 stdlib/<source> 提供。
//   NativePython 原生模块 + stdlib 扩展源：导入时编译扩展源并执行其 init；
//                原生名字优先，源文件里原生没有的名字合并进模块接口。
// source 相对 stdlib/ 目录；deps 是登记用的依赖清单（真实依赖仍由 AST
// 扫描得到），表自检会核对它们都已登记，防止改表时留下死引用。
enum class ModuleKind : std::uint8_t { Native, Python, NativePython };

struct ModuleEntry {
    std::string_view name;
    ModuleKind kind;
    std::string_view source;
    std::string_view deps;
};

inline constexpr ModuleEntry kModuleTable[] = {
    // ── 原生模块 ──────────────────────────────────────────────
    {"math", ModuleKind::Native, "", ""},
    {"sys", ModuleKind::Native, "", ""},
    {"asyncio", ModuleKind::Native, "", ""},
    {"random", ModuleKind::Native, "", ""},
    {"os", ModuleKind::Native, "", ""},
    {"os.path", ModuleKind::Native, "", ""},
    {"subprocess", ModuleKind::Native, "", ""},
    {"time", ModuleKind::Native, "", ""},
    {"itertools", ModuleKind::NativePython, "itertools.py", ""},
    {"collections", ModuleKind::Native, "", ""},
    {"json", ModuleKind::Native, "", ""},
    {"re", ModuleKind::Native, "", ""},
    {"datetime", ModuleKind::Native, "", ""},
    {"pathlib", ModuleKind::Native, "", ""},
    {"operator", ModuleKind::Native, "", ""},
    // CPython 桥：from python import <module>。纯编译期代理，无源文件、
    // 无运行期模块对象；调用降级成 rt_cpy 子进程 IPC（见 docs/cpybridge-design.md）。
    {"python", ModuleKind::Native, "", ""},
    // 免写直调的伪模块：c 提供 from c import <symbol>（任意符号名，
    // 签名由调用点推断）；dylib 提供 dlopen/dlsym 动态库句柄。
    {"c", ModuleKind::Native, "", ""},
    {"dylib", ModuleKind::Native, "", ""},
    // ── 原生 + stdlib 扩展源（补 API 只改 stdlib/<source>，不用碰 src/）──
    {"functools", ModuleKind::NativePython, "functools.py", ""},
    // ── stdlib Python 源模块 ──────────────────────────────────
    {"abc", ModuleKind::Python, "abc.py", ""},
    {"argparse", ModuleKind::Python, "argparse.py", "sys,os.path"},
    {"base64", ModuleKind::Python, "base64.py", ""},
    {"binascii", ModuleKind::Python, "binascii.py", ""},
    {"bisect", ModuleKind::Python, "bisect.py", ""},
    {"collections.abc", ModuleKind::Python, "collections/abc.py", ""},
    {"calendar", ModuleKind::Python, "calendar.py", ""},
    {"configparser", ModuleKind::Python, "configparser.py", "os.path"},
    {"csv", ModuleKind::Python, "csv.py", ""},
    {"copy", ModuleKind::Python, "copy.py", ""},
    {"fnmatch", ModuleKind::Python, "fnmatch.py", "re"},
    {"enum", ModuleKind::Python, "enum.py", ""},
    {"fractions", ModuleKind::Python, "fractions.py", "math"},
    {"glob", ModuleKind::Python, "glob.py", "os,os.path"},
    {"gzip", ModuleKind::Python, "gzip.py", "zlib,_gzipio"},
    {"_gzipio", ModuleKind::Python, "_gzipio.py", ""},
    {"hashlib", ModuleKind::Python, "hashlib.py", ""},
    {"heapq", ModuleKind::Python, "heapq.py", ""},
    {"hmac", ModuleKind::Python, "hmac.py", "hashlib"},
    {"linalg", ModuleKind::Python, "linalg.py", ""},
    {"logging", ModuleKind::Python, "logging.py", "sys"},
    {"shutil", ModuleKind::Python, "shutil.py", "os,os.path,fnmatch"},
    {"secrets", ModuleKind::Python, "secrets.py", "random,base64"},
    {"statistics", ModuleKind::Python, "statistics.py", "math"},
    {"string", ModuleKind::Python, "string.py", ""},
    {"tempfile", ModuleKind::Python, "tempfile.py", "os,os.path,random,shutil"},
    {"textwrap", ModuleKind::Python, "textwrap.py", ""},
    {"typing", ModuleKind::Python, "typing.py", ""},
    {"urllib", ModuleKind::Python, "urllib/__init__.py", ""},
    {"urllib.error", ModuleKind::Python, "urllib/error.py", ""},
    {"urllib.parse", ModuleKind::Python, "urllib/parse.py", ""},
    {"urllib.request", ModuleKind::Python, "urllib/request.py", "urllib.error"},
    {"uuid", ModuleKind::Python, "uuid.py", "random"},
    {"zipfile", ModuleKind::Python, "zipfile.py", "zlib,_gzipio,os,os.path"},
    {"zlib", ModuleKind::Python, "zlib.py", ""},
};

const ModuleEntry* find_module_entry(std::string_view name) {
    for (const auto& e : kModuleTable)
        if (e.name == name) return &e;
    return nullptr;
}

bool is_native_module(const ModuleEntry* e) { return e && e->kind == ModuleKind::Native; }

// 表里登记的 stdlib 源路径（source 为相对 stdlib/ 的路径）。
std::string module_source_path(const ModuleEntry& e) {
    return canonical_path(
        (std::filesystem::path(PYADAMAS_STDLIB_DIR) / std::string(e.source)).string());
}

bool path_under_stdlib(const std::string& path) {
    static const std::string root = canonical_path(PYADAMAS_STDLIB_DIR);
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
           path[root.size()] == '/';
}

// 解析一个 import 的源文件：
// - 原生 + 扩展源：固定取随编译器分发的源，不允许同名用户文件劫持内建模块；
// - 其余（纯 Python 源模块 / 用户模块）：走正常搜索路径（导入方目录、
//   用户 -I 目录、最后 stdlib），保持既有的「用户可覆盖」语义。
std::optional<ResolvedModule> resolve_import_source(const std::string& importer,
                                                    const std::string& name,
                                                    const std::vector<std::string>& include_dirs) {
    const ModuleEntry* e = find_module_entry(name);
    if (e && e->kind == ModuleKind::NativePython) {
        std::string path = module_source_path(*e);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
        bool is_pkg = std::string(e->source).ends_with("__init__.py");
        return ResolvedModule{path, is_pkg};
    }
    return resolve_module_path(importer, name, include_dirs);
}

// 表格自检：登记了源的模块，源文件必须存在；deps 里的模块必须已登记。
// 每次进程只跑一次；Release 构建跳过（避免给正式二进制增加启动 IO）。
bool validate_module_table() {
#ifdef NDEBUG
    return true;
#else
    static const bool ok = [] {
        bool good = true;
        std::error_code ec;
        for (const auto& e : kModuleTable) {
            if (!e.source.empty()) {
                auto p = std::filesystem::path(PYADAMAS_STDLIB_DIR) / std::string(e.source);
                if (!std::filesystem::is_regular_file(p, ec)) {
                    std::fprintf(stderr,
                                 "error: module table: source for '%s' not found: %s\n",
                                 std::string(e.name).c_str(), p.string().c_str());
                    good = false;
                }
            } else if (e.kind != ModuleKind::Native) {
                std::fprintf(stderr,
                             "error: module table: '%s' is not native but registers no source\n",
                             std::string(e.name).c_str());
                good = false;
            }
            size_t start = 0;
            while (!e.deps.empty()) {
                size_t comma = e.deps.find(',', start);
                std::string_view dep =
                    e.deps.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                        : comma - start);
                if (!dep.empty() && !find_module_entry(dep)) {
                    std::fprintf(stderr,
                                 "error: module table: '%s' depends on unregistered '%s'\n",
                                 std::string(e.name).c_str(), std::string(dep).c_str());
                    good = false;
                }
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }
        }
        return good;
    }();
    return ok;
#endif
}

struct ImportReq {
    std::string name;
    int line = 1;
    int col = 1;
    std::vector<std::string> from_names;  // ImportFrom 的名字，可能是子模块
};

struct ImportIssue {
    int line = 1;
    int col = 1;
    std::string msg;
};

void scan_imports_body(std::vector<pyadamas::StmtPtr>& body, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg);

void scan_imports_stmt(pyadamas::Stmt& s, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg) {
    using namespace pyadamas;
    switch (s.kind) {
        case StmtKind::Import: {
            auto& im = static_cast<Import&>(s);
            for (auto& a : im.names) {
                if (a.name.find('.') != std::string::npos && a.asname.empty()) {
                    issues.push_back({s.line, s.col,
                                      "dotted import needs 'as name' (or use from ... import)"});
                    continue;
                }
                reqs.push_back({a.name, s.line, s.col, {}});
            }
            break;
        }
        case StmtKind::ImportFrom: {
            auto& im = static_cast<ImportFrom&>(s);
            std::string module = im.module;
            if (im.level > 0) {
                std::string base = pkg;
                for (int level = 1; level < im.level; ++level) {
                    auto dot = base.rfind('.');
                    if (dot == std::string::npos) {
                        base.clear();
                        break;
                    }
                    base.resize(dot);
                }
                if (base.empty() && im.level > 1) {
                    issues.push_back({s.line, s.col, "relative import goes beyond top-level package"});
                    break;
                }
                module = module.empty() ? base : (base.empty() ? module : base + "." + module);
                im.module = module;
                im.level = 0;
            }
            if (module.empty()) {
                issues.push_back({s.line, s.col, "relative import outside a package"});
                break;
            }
            ImportReq req{module, s.line, s.col, {}};
            for (auto& a : im.names)
                if (a.name != "*") req.from_names.push_back(a.name);
            reqs.push_back(std::move(req));
            break;
        }
        case StmtKind::If: {
            auto& f = static_cast<If&>(s);
            for (auto& b : f.branches) scan_imports_body(b.body, reqs, issues, pkg);
            scan_imports_body(f.orelse, reqs, issues, pkg);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<While&>(s);
            scan_imports_body(w.body, reqs, issues, pkg);
            scan_imports_body(w.orelse, reqs, issues, pkg);
            break;
        }
        case StmtKind::For: {
            auto& f = static_cast<For&>(s);
            scan_imports_body(f.body, reqs, issues, pkg);
            scan_imports_body(f.orelse, reqs, issues, pkg);
            break;
        }
        case StmtKind::With: {
            auto& w = static_cast<With&>(s);
            scan_imports_body(w.body, reqs, issues, pkg);
            break;
        }
        case StmtKind::Try: {
            auto& t = static_cast<Try&>(s);
            scan_imports_body(t.body, reqs, issues, pkg);
            for (auto& h : t.handlers) scan_imports_body(h.body, reqs, issues, pkg);
            scan_imports_body(t.orelse, reqs, issues, pkg);
            scan_imports_body(t.finalbody, reqs, issues, pkg);
            break;
        }
        case StmtKind::FunctionDef: {
            auto& f = static_cast<FunctionDef&>(s);
            scan_imports_body(f.body, reqs, issues, pkg);
            break;
        }
        case StmtKind::ClassDef: {
            auto& c = static_cast<ClassDef&>(s);
            scan_imports_body(c.body, reqs, issues, pkg);
            break;
        }
        default: break;
    }
}

void scan_imports_body(std::vector<pyadamas::StmtPtr>& body, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg) {
    for (auto& s : body) scan_imports_stmt(*s, reqs, issues, pkg);
}

std::string unit_package(const ModuleUnit& u) {
    if (u.is_package) return u.name;
    auto dot = u.name.rfind('.');
    return dot == std::string::npos ? std::string() : u.name.substr(0, dot);
}

bool parse_unit(ModuleUnit& u) {
    try {
        pyadamas::Lexer lexer(u.source);
        pyadamas::Parser parser(lexer.run());
        u.ast = parser.parse();
        pyadamas::desugar_lambdas(*u.ast);
        return true;
    } catch (const pyadamas::LexError& e) {
        report(u.path, u.source, e.line, e.col, e.what());
    } catch (const pyadamas::ParseError& e) {
        report(u.path, u.source, e.line, e.col, e.what());
    }
    return false;
}

// 编译器/运行时的构建指纹。源码没变但换了编译器时不能复用旧对象：
// 代码生成和运行时的行为不进源码哈希，旧清单必须失效。
std::string toolchain_id() {
    auto one = [](const char* path) {
        struct stat st {};
        if (::stat(path, &st) != 0) return std::string("na");
        return std::format("{}:{}:{}", static_cast<long long>(st.st_size),
                           static_cast<long long>(st.st_mtim.tv_sec),
                           static_cast<long long>(st.st_mtim.tv_nsec));
    };
    static const std::string id = one("/proc/self/exe") + "|" + one(PYADAMAS_RT_OBJ);
    return id;
}

void set_cache_paths(ModuleUnit& u) {
    std::string key = sanitize_symbol(u.name) + "-" + pyadamas::hex64(pyadamas::fnv1a64(u.path));
    u.cache_dir = std::string(PYADAMAS_MODCACHE_DIR) + "/" + key;
    u.asm_path = u.cache_dir + "/mod.s";
    u.obj_path = u.cache_dir + "/mod.o";
    u.iface_path = u.cache_dir + "/mod.pya";
    u.manifest_path = u.cache_dir + "/manifest";
}

std::optional<uint64_t> parse_u64(const std::string& s) {
    try {
        size_t pos = 0;
        uint64_t v = std::stoull(s, &pos, 10);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

struct Manifest {
    uint64_t source_hash = 0;
    int opt = -1;
    int fast_math = -1;
    // 零开销异常开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int zero_cost_exc = -1;
    // 旧清单没有这一行：保守按「需要索引」处理。
    bool unknown_index = true;
    // live range splitting 开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    //
    // **这是缓存正确性的必要部分**：环境变量会改变代码生成结果，但
    // 缓存键只有「模块名 + 源文件路径」，不含任何编译选项。不把它写进
    // 清单就会出现「切了开关仍命中旧产物」—— 实测导致连续几轮 A/B
    // 测试拿到完全相同的二进制，性能结论全错。
    int rc_split = -1;
    // 图着色分配器开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int ra_graph = -1;
    // callee-saved 提升增强开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int ra_opt = -1;
    // SIMD 向量化开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int simd = -1;
    // 整数循环优化位掩码。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int loop_opt = -1;
    // 内联/devirt 深化开关。-1 = 旧清单没有这一行（缓存必然失效一次）。
    int inline_opt = -1;
    // 模块是否用到 from c import（dlsym RTLD_DEFAULT）。旧清单没有这一行：
    // 保守按「用到」处理（链接期关 --gc-sections，绝不冒误剥风险）。
    bool c_sym_default = true;
    std::string toolchain;
    std::map<std::string, std::string> deps;
};

std::optional<Manifest> read_manifest(const ModuleUnit& u) {
    std::ifstream in(u.manifest_path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line != "pyadamasmanifest 1") return std::nullopt;
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
        } else if (tag == "rcsplit") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.rc_split = static_cast<int>(*o);
        } else if (tag == "ragraph") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.ra_graph = static_cast<int>(*o);
        } else if (tag == "raopt") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.ra_opt = static_cast<int>(*o);
        } else if (tag == "simd") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.simd = static_cast<int>(*o);
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
        } else if (tag == "csymdefault") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.c_sym_default = *o != 0;
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
    return m;
}

void write_manifest(const Program& p, const ModuleUnit& u) {
    std::error_code ec;
    std::filesystem::create_directories(u.cache_dir, ec);
    std::ostringstream out;
    out << "pyadamasmanifest 1\n";
    out << "abi " << pyadamas::kModuleAbiVersion << "\n";
    out << "source " << pyadamas::fnv1a64(u.source) << "\n";
    out << "opt " << p.opt << "\n";
    out << "fastmath " << (p.fast_math ? 1 : 0) << "\n";
    out << "zerocostexc " << (p.zero_cost_exc ? 1 : 0) << "\n";
    out << "unknownindex " << (u.needs_unknown_index ? 1 : 0) << "\n";
    out << "rcsplit " << (p.rc_split ? 1 : 0) << "\n";
    out << "ragraph " << (p.ra_graph ? 1 : 0) << "\n";
    out << "raopt " << (p.ra_opt ? 1 : 0) << "\n";
    out << "simd " << (p.simd ? 1 : 0) << "\n";
    out << "loopopt " << p.loop_opt << "\n";
    out << "inlineopt " << (p.inline_opt ? 1 : 0) << "\n";
    out << "csymdefault " << (u.uses_c_sym_default ? 1 : 0) << "\n";
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
            for (auto& n : r.from_names) it->second.from_names.push_back(n);
    }
    std::vector<ImportReq> queue;
    for (auto& [n, r] : merged) queue.push_back(r);

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        ImportReq req = queue[qi];
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
        auto found = resolve_import_source(u.path, req.name, p.include_dirs);
        if (!found) {
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
        u.deps.push_back(req.name);
        // from pkg import mod：mod 可能是子模块，按完整名加载一份接口。
        for (auto& n : req.from_names) {
            std::string child = req.name + "." + n;
            if (p.units.count(child)) continue;
            if (resolve_module_path(u.path, child, p.include_dirs))
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
    if (m->toolchain != toolchain_id()) return false;  // 换了编译器/运行时
    if (m->opt != p.opt) return false;
    if (m->fast_math != (p.fast_math ? 1 : 0)) return false;
    if (m->zero_cost_exc != (p.zero_cost_exc ? 1 : 0)) return false;
    if (m->rc_split != (p.rc_split ? 1 : 0)) return false;
    if (m->ra_graph != (p.ra_graph ? 1 : 0)) return false;
    if (m->ra_opt != (p.ra_opt ? 1 : 0)) return false;
    if (m->simd != (p.simd ? 1 : 0)) return false;
    if (m->loop_opt != p.loop_opt) return false;
    if (m->inline_opt != (p.inline_opt ? 1 : 0)) return false;
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
        auto info = pyadamas::decode_module_interface(*bytes, u.path);
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

// 原生模块 + stdlib 扩展源：接口取并集，原生名字优先（扩展只能补新名字，
// 不能悄悄改掉原生语义）。source_init/source_globals 记录给降级用。
pyadamas::ModuleInfoPtr merge_extension_module(const pyadamas::ModuleInfoPtr& base,
                                               const pyadamas::ModuleInfoPtr& ext) {
    auto merged = std::make_shared<pyadamas::ModuleInfo>(*base);
    merged->source_init = true;
    for (auto& [name, ty] : ext->globals) {
        // __name__/__file__ 之类由编译器统一给，不参与合并。
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') continue;
        if (merged->globals.count(name)) continue;
        merged->globals[name] = ty;
        merged->source_globals.insert(name);
    }
    for (auto& [name, ic] : ext->classes) {
        if (merged->classes.count(name)) continue;
        merged->classes[name] = ic;
    }
    return merged;
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
        auto norm = pyadamas::decode_module_interface(*bytes, u.path);
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
                                        p.zero_cost_exc, skip_idx, p.rc_split,
                                        p.ra_graph, p.simd, p.ra_opt, p.inline_opt);
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


// ── check 模式的 import 图解析 ─────────────────────────────────────
// 和 build 共用 resolve_module_path / scan_imports_body / builtin_module，
// 但只跑到「建出接口」为止，不降级、不生成代码。依赖解析失败时返回
// nullptr（调用方按 unknown 处理），并把失败的模块名记进 missing。
namespace {

struct CheckCtx {
    std::vector<std::string> include_dirs;
    std::map<std::string, pyadamas::ModuleInfoPtr> loaded;
    std::set<std::string> loading;
    std::vector<std::string> missing;
    // 依赖模块的 TypeChecker 拥有 ClassInfo；必须活到 check 结束，
    // 否则接口里的 ClassInfo* 会悬空。
    std::vector<std::shared_ptr<pyadamas::TypeChecker>> keep_alive;
};

// 把某个模块的类型检查结果压成扁平 ModuleInfo（类方法/字段按 MRO 拍平），
// 和 build_unit 的接口导出逻辑保持一致，但不做编解码归一。
pyadamas::ModuleInfoPtr check_build_info(pyadamas::TypeChecker& tc,
                                          const std::string& runtime_name) {
    auto info = std::make_shared<pyadamas::ModuleInfo>();
    auto* sc = tc.module_scope();
    if (!sc) return info;
    for (auto& [name, sym] : sc->vars) {
        if (sym.ty && sym.ty->kind == pyadamas::TyKind::Class && sym.ty->cls) {
            const pyadamas::ClassInfo* cls = sym.ty->cls;
            if (cls->imported()) {
                pyadamas::ImportedClass ic;
                ic.cls = std::shared_ptr<pyadamas::ClassInfo>(const_cast<pyadamas::ClassInfo*>(cls), [](pyadamas::ClassInfo*){});
                ic.module = cls->origin_module;
                ic.qualname = cls->origin_qualname;
                info->classes[name] = std::move(ic);
                continue;
            }
            // 扁平类：字段按根→派生拍平，方法按最终槽位。
            std::vector<const pyadamas::ClassInfo*> chain;
            for (auto* cp = cls; cp; cp = cp->base) chain.push_back(cp);
            auto flat = std::make_shared<pyadamas::ClassInfo>();
            flat->name = name;
            flat->complete = true;
            flat->has_init = cls->has_init;
            flat->open = cls->is_open();
            flat->is_exception_class = cls->is_exception_class;
            flat->is_rich_exception = cls->is_rich_exception;
            flat->exc_parent = cls->exc_parent;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                for (auto& f : (*it)->fields) flat->fields.push_back(f);
            if (const auto* ma = cls->match_args_of()) flat->match_args = *ma;
            else flat->match_args = cls->match_args;
            std::map<std::string, pyadamas::MethodInfo> by_key;
            std::map<std::string, std::vector<int>> key_slots;
            std::vector<const pyadamas::ClassInfo*> mro(cls->mro.begin(), cls->mro.end());
            if (mro.empty()) mro.push_back(cls);
            for (const pyadamas::ClassInfo* k : mro) {
                if (!k) continue;
                for (auto& m : k->methods) {
                    if (!m.call_sig) continue;
                    std::string key = pyadamas::method_slot_key(m.name, m.kind);
                    auto& v = key_slots[key];
                    if (std::find(v.begin(), v.end(), m.slot) == v.end()) v.push_back(m.slot);
                    for (int a : m.aliases)
                        if (std::find(v.begin(), v.end(), a) == v.end()) v.push_back(a);
                    if (!by_key.count(key)) by_key[key] = m;
                }
            }
            for (auto& [key, m] : by_key) {
                pyadamas::MethodInfo nm;
                nm.name = m.name;
                nm.kind = m.kind;
                nm.slot = m.slot;
                for (int slot : key_slots[key])
                    if (slot != m.slot) nm.aliases.push_back(slot);
                nm.call_sig = m.call_sig;
                nm.sig = m.call_sig;
                flat->methods.push_back(std::move(nm));
            }
            for (size_t mi = 0; mi < flat->methods.size(); ++mi) {
                pyadamas::MethodInfo& m = flat->methods[mi];
                if (!m.is_accessor()) continue;
                auto& pi = flat->properties[m.name];
                pi.owner = flat.get();
                switch (m.kind) {
                    case pyadamas::MethodKind::PropertyGetter: pi.getter = static_cast<int>(mi); break;
                    case pyadamas::MethodKind::PropertySetter: pi.setter = static_cast<int>(mi); break;
                    case pyadamas::MethodKind::PropertyDeleter: pi.deleter = static_cast<int>(mi); break;
                    default: break;
                }
            }
            flat->origin_module = runtime_name;
            flat->origin_qualname = runtime_name + "." + name;
            // 祖先链：本地类按 C3 MRO 拍平，导入类沿用它的 origin。
            for (const pyadamas::ClassInfo* k : cls->mro) {
                if (!k || k == cls) continue;
                pyadamas::ClassAncestor a;
                if (k->imported()) { a.module = k->origin_module; a.name = k->external_name(); a.qualname = k->origin_qualname; }
                else { a.module = runtime_name; a.name = k->name; a.qualname = runtime_name + "." + k->name; }
                bool dup = false;
                for (auto& e : flat->ext_ancestors)
                    if (e.module == a.module && e.name == a.name) { dup = true; break; }
                if (!dup) flat->ext_ancestors.push_back(std::move(a));
            }
            pyadamas::ImportedClass ic;
            if (auto* im = flat->method("__init__"); im && im->call_sig) ic.ctor_sig = im->call_sig;
            if (!ic.ctor_sig) ic.ctor_sig = std::make_shared<pyadamas::FuncInfo>();
            ic.cls = flat;
            ic.module = runtime_name;
            ic.qualname = flat->origin_qualname;
            info->classes[name] = std::move(ic);
            continue;
        }
        info->globals[name] = sym.ty;
    }
    info->globals["__name__"] = pyadamas::t_str();
    info->globals["__file__"] = pyadamas::t_str();
    return info;
}

pyadamas::ModuleInfoPtr check_load(CheckCtx& ctx, const std::string& importer,
                                    const std::string& name);

// 按文件解析一个模块、建出接口；ctx.loading 负责环检测。失败时记
// missing 并返回 nullptr（调用方按 unknown 处理）。
pyadamas::ModuleInfoPtr check_load_file(CheckCtx& ctx, const std::string& name,
                                        const std::string& path) {
    auto src = read_file(path);
    if (!src) {
        ctx.loading.erase(name);
        ctx.missing.push_back(name);
        return nullptr;
    }
    try {
        pyadamas::Lexer lexer(*src);
        pyadamas::Parser parser(lexer.run());
        auto mod = parser.parse();
        pyadamas::desugar_lambdas(*mod);
        auto tc = std::make_shared<pyadamas::TypeChecker>(path);
        std::string cap_name = name;
        tc->set_module_lookup([&ctx, path, cap_name](const std::string& dep) {
            return check_load(ctx, path, dep);
        });
        tc->set_lenient_imports(true);
        tc->check(*mod);
        ctx.keep_alive.push_back(tc);
        auto info = check_build_info(*tc, name);
        info->name = name;
        info->source_path = path;
        ctx.loading.erase(name);
        return info;
    } catch (...) {
        ctx.loading.erase(name);
        ctx.missing.push_back(name);
        return nullptr;
    }
}

pyadamas::ModuleInfoPtr check_load(CheckCtx& ctx, const std::string& importer,
                                    const std::string& name) {
    if (auto it = ctx.loaded.find(name); it != ctx.loaded.end()) return it->second;
    // CPython 桥子代理：与 build 端 program_module 同口径。
    if (name.starts_with("python.")) {
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    const ModuleEntry* entry = find_module_entry(name);
    if (is_native_module(entry)) {
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    if (entry && entry->kind == ModuleKind::NativePython) {
        // 原生 + stdlib 扩展源：接口 = 原生接口 + 扩展新增名字。
        std::string path = module_source_path(*entry);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ctx.loading.count(name)) {
            ctx.missing.push_back(name);
            ctx.loaded[name] = nullptr;
            return nullptr;
        }
        ctx.loading.insert(name);
        auto ext = check_load_file(ctx, name, path);
        if (!ext) {
            ctx.loaded[name] = nullptr;
            return nullptr;
        }
        auto info = merge_extension_module(pyadamas::builtin_module(name), ext);
        info->name = name;
        info->source_path = path;
        ctx.loaded[name] = info;
        return info;
    }
    if (ctx.loading.count(name)) return nullptr;  // 环：按 unknown
    auto found = resolve_import_source(importer, name, ctx.include_dirs);
    if (!found) {
        ctx.missing.push_back(name);
        ctx.loaded[name] = nullptr;
        return nullptr;
    }
    std::string path = canonical_path(found->path);
    // 单一数据源：stdlib 目录里的 Python 源必须在模块表里登记。
    if (!entry && path_under_stdlib(path)) {
        ctx.missing.push_back(name);
        ctx.loaded[name] = nullptr;
        return nullptr;
    }
    ctx.loading.insert(name);
    auto info = check_load_file(ctx, name, path);
    ctx.loaded[name] = info;
    return info;
}

}  // namespace

CheckLookup load_check_lookup(const std::string& entry_path,
                               const std::vector<std::string>& include_dirs,
                               std::vector<std::string>& missing) {
    auto ctx = std::make_shared<CheckCtx>();
    ctx->include_dirs = include_dirs;
    ctx->include_dirs.emplace_back(PYADAMAS_STDLIB_DIR);
    std::string entry = canonical_path(entry_path);
    auto lookup = [ctx, entry](const std::string& name) -> ModuleInfoPtr {
        return check_load(*ctx, entry, name);
    };
    // 预加载一次，收集 missing。
    auto src = read_file(entry);
    if (src) {
        pyadamas::Lexer lexer(*src);
        pyadamas::Parser parser(lexer.run());
        auto mod = parser.parse();
        if (mod) {
            std::vector<ImportReq> reqs;
            std::vector<ImportIssue> issues;
            scan_imports_body(mod->body, reqs, issues, std::string());
            for (auto& r : reqs) lookup(r.name);
        }
    }
    missing = ctx->missing;
    return lookup;
}

bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p) {
    if (!validate_module_table()) return false;
    p.include_dirs = include_dirs;
    // 随编译器分发的标准库源码目录：导入方同目录与用户 -I 优先，最后落这里。
    p.include_dirs.emplace_back(PYADAMAS_STDLIB_DIR);
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
    return visit_unit(p, *p.entry, stack);
}

bool prepare_program(Program& p, bool emit_objects, bool entry_ir) {
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
    // 链接有两条路（见 driver_link.cpp）：配置期成功复刻出 gcc 的 ld 命令行
    // 时直接 exec ld，省掉 gcc 驱动 + collect2 的 11 ms 固定开销；否则回退
    // 到 cc。两条路生成的二进制逐字节相同（--build-id 由输入决定）。
    std::string objs;
    for (auto* u : p.order) objs += " " + shell_quote(u->obj_path);
    // --asan：链接插桩版运行时，让 RC/GC 内部的越界/UAF 也能被检查到。
    // 两个 rt 都带同样的符号，链接期只取一个。
    objs += " " + shell_quote(g_use_asan_runtime ? PYADAMAS_RT_ASAN_OBJ : PYADAMAS_RT_OBJ);
    // -ldl：rt_cffi 的 dlopen/dlsym（glibc 2.34+ 已并入 libc，-ldl 是空壳，
    // 旧 glibc 仍需要）。Win64 目前只做交叉汇编验证，不走这条链接路径。
    objs += " -lm -ldl";
    for (auto& l : p.link_args) objs += " " + shell_quote(l);
    const bool elf_target = std::string_view(target_abi().name) == "x86_64-sysv";
    // --gc-sections（slim 轮）：剥掉用户程序未引用的运行时函数/数据段。
    // PYADAMAS_GC_SECTIONS=1 开启（默认关，灰度）；direct-ld 与 cc 回退两条
    // 路等价附加，保持「direct-ld == cc 逐字节」不变量。
    // from c import（dlsym RTLD_DEFAULT）的程序可能运行期查找任何运行时符号，
    // 静态可达性覆盖不到 —— 整程序保守关 gc（uses_c_sym_default，见 ir.cpp）。
    const bool gc = elf_target && gc_sections_enabled() && !p.uses_c_sym_default;
    const std::string gc_ld = gc ? " --gc-sections" : "";
    const std::string gc_cc = gc ? " -Wl,--gc-sections" : "";
    if (elf_target && direct_ld_available() && direct_ld_args_ok(p.link_args)) {
        std::string ld_cmd = direct_ld_prefix() + gc_ld + " -o " + shell_quote(output) + objs + " " +
                             direct_ld_tail();
        if (std::system(ld_cmd.c_str()) == 0) return true;
        // ld 直接链接失败（环境差异）：不报错，回退 cc 再试一次。
    }
    std::string cmd = "cc -o " + shell_quote(output) + " -Wl,--eh-frame-hdr" + gc_cc + cc_extra_flags() + objs;
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "error: link failed\n       command: %s\n", cmd.c_str());
        return false;
    }
    return true;
}

}  // namespace pyadamas
