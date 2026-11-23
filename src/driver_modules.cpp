// driver_modules.cpp —— 模块表与 import 源解析
//
// 从 driver_build.cpp 拆出（纯代码移动，函数体不变）：符号/路径工具、
// 模块表查询、import 源解析、AST import 扫描、原生+扩展接口合并。
// 接口声明见 driver_modules.h。
#include "driver_modules.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace pyadamas {

// 模块名 → 文件名/目录名片段。与符号 mangling 共用同一份**单射**编码
// （module.cpp 的 module_symbol_prefix）：旧的「非字母数字→下划线」是有损的，
// "a.b" 与 "a_b" 会映射到同一个缓存目录（缓存互相覆盖）和同一个符号前缀。
std::string sanitize_symbol(const std::string& s) { return module_symbol_prefix(s); }

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
namespace {
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
    // P4 块2（lang_gaps）：内存流 StringIO/BytesIO（file 协议泛化）。
    {"io", ModuleKind::Python, "io.py", ""},
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
    {"difflib", ModuleKind::Python, "difflib.py", ""},
    {"contextlib", ModuleKind::Python, "contextlib.py", ""},
    {"csv", ModuleKind::Python, "csv.py", ""},
    {"copy", ModuleKind::Python, "copy.py", ""},
    // P4 块4（lang_gaps）：@dataclass 编译期展开的运行期恒等装饰器。
    {"dataclasses", ModuleKind::Python, "dataclasses.py", ""},
    // 定点十进制：big 精确整数内核，边界见 stdlib/decimal.py 模块头。
    {"decimal", ModuleKind::Python, "decimal.py", ""},
    {"fnmatch", ModuleKind::Python, "fnmatch.py", "re"},
    {"enum", ModuleKind::Python, "enum.py", ""},
    {"fractions", ModuleKind::Python, "fractions.py", ""},
    {"glob", ModuleKind::Python, "glob.py", "os,os.path"},
    {"gzip", ModuleKind::Python, "gzip.py", "zlib,_gzipio"},
    {"_gzipio", ModuleKind::Python, "_gzipio.py", ""},
    {"hashlib", ModuleKind::Python, "hashlib.py", ""},
    {"heapq", ModuleKind::Python, "heapq.py", ""},
    {"hmac", ModuleKind::Python, "hmac.py", "hashlib"},
    {"linalg", ModuleKind::Python, "linalg.py", ""},
    {"logging", ModuleKind::Python, "logging.py", "sys"},
    {"queue", ModuleKind::Python, "queue.py", "heapq,time"},
    {"shutil", ModuleKind::Python, "shutil.py", "os,os.path,fnmatch"},
    {"pprint", ModuleKind::Python, "pprint.py", ""},
    {"secrets", ModuleKind::Python, "secrets.py", "random,base64"},
    {"statistics", ModuleKind::Python, "statistics.py", "math"},
    {"struct", ModuleKind::Python, "struct.py", "math"},
    {"string", ModuleKind::Python, "string.py", ""},
    {"tarfile", ModuleKind::Python, "tarfile.py", "gzip,_gzipio,os,os.path"},
    {"tempfile", ModuleKind::Python, "tempfile.py", "os,os.path,random,shutil"},
    {"textwrap", ModuleKind::Python, "textwrap.py", ""},
    // P4 块3（lang_gaps）：traceback 格式化（sys.exc_info + __pa_tb_frames）。
    {"traceback", ModuleKind::Python, "traceback.py", "sys"},
    {"typing", ModuleKind::Python, "typing.py", ""},
    {"urllib", ModuleKind::Python, "urllib/__init__.py", ""},
    {"urllib.error", ModuleKind::Python, "urllib/error.py", ""},
    {"urllib.parse", ModuleKind::Python, "urllib/parse.py", ""},
    {"urllib.request", ModuleKind::Python, "urllib/request.py", "urllib.error"},
    {"uuid", ModuleKind::Python, "uuid.py", "random"},
    {"zipfile", ModuleKind::Python, "zipfile.py", "zlib,_gzipio,os,os.path"},
    {"zlib", ModuleKind::Python, "zlib.py", ""},
};
}  // namespace

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

namespace {
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
}  // namespace

void scan_imports_body(std::vector<pyadamas::StmtPtr>& body, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg) {
    for (auto& s : body) scan_imports_stmt(*s, reqs, issues, pkg);
}

std::string unit_package(const ModuleUnit& u) {
    if (u.is_package) return u.name;
    auto dot = u.name.rfind('.');
    return dot == std::string::npos ? std::string() : u.name.substr(0, dot);
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

}  // namespace pyadamas
