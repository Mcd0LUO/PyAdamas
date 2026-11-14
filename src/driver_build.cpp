// driver_build.cpp —— 多模块解析、增量缓存、汇编与链接
#include "driver_build.h"
#include "driver_util.h"

#include <algorithm>
#include <atomic>
#include <unistd.h>
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
    // 旧清单没有这一行：保守按「需要索引」处理。
    bool unknown_index = true;
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
        } else if (tag == "unknownindex") {
            std::string v;
            ss >> v;
            auto o = parse_u64(v);
            if (!o) return std::nullopt;
            m.unknown_index = *o != 0;
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
    out << "unknownindex " << (u.needs_unknown_index ? 1 : 0) << "\n";
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
        if (req.name == "math" || req.name == "sys" || req.name == "asyncio" ||
            req.name == "random" || req.name == "os" || req.name == "os.path" ||
            req.name == "subprocess" || req.name == "time" || req.name == "itertools" ||
            req.name == "collections" || req.name == "json" || req.name == "re" ||
            req.name == "functools" || req.name == "datetime" || req.name == "pathlib" ||
            req.name == "operator") {
            // 内置模块没有源文件/对象，只提供编译期接口。
            continue;
        }
        auto found = resolve_module_path(u.path, req.name, p.include_dirs);
        if (!found) {
            report(u.path, u.source, req.line, req.col,
                   std::format("cannot find module '{}'", req.name));
            return false;
        }
        std::string canonical = canonical_path(found->path);
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
    std::string cmd = std::format("cc -c -o {} {}", shell_quote(tmp_obj), shell_quote(tmp_asm));
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
    u.from_cache = true;
    return true;
}

pyadamas::ModuleInfoPtr program_module(Program& p, const std::string& name) {
    if (name == "math" || name == "sys" || name == "asyncio" || name == "random" ||
        name == "os" || name == "os.path" || name == "subprocess" || name == "time" ||
        name == "itertools" || name == "collections" || name == "json" || name == "re" ||
        name == "functools" || name == "datetime" || name == "pathlib" ||
        name == "operator")
        return builtin_module(name);
    auto it = p.units.find(name);
    return it == p.units.end() ? nullptr : it->second->info;
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
    opt.fast_math = p.fast_math;
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

    u.asm_text = pyadamas::generate_asm(lr.mod, u.is_entry && !p.needs_unknown_index);
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

bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p) {
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
    std::string cmd = "cc -o " + shell_quote(output) + " -Wl,--eh-frame-hdr";
    for (auto* u : p.order) cmd += " " + shell_quote(u->obj_path);
    cmd += " " + shell_quote(PYADAMAS_RT_OBJ) + " -lm";
    for (auto& l : p.link_args) cmd += " " + shell_quote(l);
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "error: link failed\n       command: %s\n", cmd.c_str());
        return false;
    }
    return true;
}

}  // namespace pyadamas
