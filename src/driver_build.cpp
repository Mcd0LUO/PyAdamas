// driver_build.cpp —— 多模块解析、增量缓存、汇编与链接
#include "driver_build.h"
#include "driver_util.h"

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

#ifndef PY23_RT_OBJ
#define PY23_RT_OBJ "build/dev/rt.o"
#endif
#ifndef PY23_MODCACHE_DIR
#define PY23_MODCACHE_DIR "build/dev/modcache"
#endif

namespace py23 {

namespace {

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

void scan_imports_body(std::vector<py23::StmtPtr>& body, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg);

void scan_imports_stmt(py23::Stmt& s, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg) {
    using namespace py23;
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

void scan_imports_body(std::vector<py23::StmtPtr>& body, std::vector<ImportReq>& reqs,
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
        py23::Lexer lexer(u.source);
        py23::Parser parser(lexer.run());
        u.ast = parser.parse();
        py23::desugar_lambdas(*u.ast);
        return true;
    } catch (const py23::LexError& e) {
        report(u.path, u.source, e.line, e.col, e.what());
    } catch (const py23::ParseError& e) {
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
    static const std::string id = one("/proc/self/exe") + "|" + one(PY23_RT_OBJ);
    return id;
}

void set_cache_paths(ModuleUnit& u) {
    std::string key = sanitize_symbol(u.name) + "-" + py23::hex64(py23::fnv1a64(u.path));
    u.cache_dir = std::string(PY23_MODCACHE_DIR) + "/" + key;
    u.asm_path = u.cache_dir + "/mod.s";
    u.obj_path = u.cache_dir + "/mod.o";
    u.iface_path = u.cache_dir + "/mod.py23i";
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
    if (!std::getline(in, line) || line != "py23manifest 1") return std::nullopt;
    Manifest m;
    bool abi_ok = false;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "abi") {
            std::string v;
            ss >> v;
            abi_ok = v == std::to_string(py23::kModuleAbiVersion);
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
    std::ofstream out(u.manifest_path, std::ios::binary);
    out << "py23manifest 1\n";
    out << "abi " << py23::kModuleAbiVersion << "\n";
    out << "source " << py23::fnv1a64(u.source) << "\n";
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
            req.name == "functools" || req.name == "datetime" || req.name == "pathlib") {
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
    std::ofstream out(u.asm_path, std::ios::binary);
    if (!out) {
        err = std::format("cannot write '{}'", u.asm_path);
        return false;
    }
    out << u.asm_text;
    out.close();
    std::string cmd = std::format("cc -c -o {} {}", shell_quote(u.obj_path), shell_quote(u.asm_path));
    if (std::system(cmd.c_str()) != 0) {
        err = "assembler failed";
        return false;
    }
    return true;
}

bool try_cache(Program& p, ModuleUnit& u) {
    if (p.rebuild) return false;
    auto m = read_manifest(u);
    if (!m) return false;
    if (m->source_hash != py23::fnv1a64(u.source)) return false;
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
        auto info = py23::decode_module_interface(*bytes, u.path);
        if (!info || (*info)->name != u.name) return false;
        (*info)->interface_hash = py23::hex64(py23::fnv1a64(*bytes));
        u.info = *info;
    }
    u.needs_unknown_index = m->unknown_index;
    p.needs_unknown_index |= u.needs_unknown_index;
    u.from_cache = true;
    return true;
}

py23::ModuleInfoPtr builtin_module(const std::string& name);

py23::ModuleInfoPtr program_module(Program& p, const std::string& name) {
    if (name == "math" || name == "sys" || name == "asyncio" || name == "random" ||
        name == "os" || name == "os.path" || name == "subprocess" || name == "time" ||
        name == "itertools" || name == "collections" || name == "json" || name == "re" ||
        name == "functools" || name == "datetime" || name == "pathlib")
        return builtin_module(name);
    auto it = p.units.find(name);
    return it == p.units.end() ? nullptr : it->second->info;
}

py23::FuncInfoPtr builtin_fn(const std::string& name, const std::vector<py23::TypePtr>& args,
                             py23::TypePtr ret) {
    auto f = std::make_shared<py23::FuncInfo>();
    f->name = name;
    for (size_t i = 0; i < args.size(); ++i) {
        py23::Param p;
        p.name = (name == "fsum" && i == 0) ? "xs" : std::format("x{}", i);
        p.ty = args[i];
        f->pos.push_back(std::move(p));
    }
    f->ret = ret;
    f->ret_known = true;
    return f;
}

py23::ModuleInfoPtr builtin_module(const std::string& name) {
    static std::map<std::string, py23::ModuleInfoPtr> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto info = std::make_shared<py23::ModuleInfo>();
    info->name = name;
    info->builtin = true;
    auto func = [&](const std::string& n, std::vector<py23::TypePtr> args, py23::TypePtr ret) {
        auto t = py23::new_ty(py23::TyKind::Func);
        t->fn = builtin_fn(n, args, ret);
        info->globals[n] = t;
    };
    auto lv_int = [](int64_t v) {
        py23::LiteralValue lv;
        lv.kind = py23::LiteralValue::Kind::Int;
        lv.i = v;
        return lv;
    };
    auto lv_bool = [](bool v) {
        py23::LiteralValue lv;
        lv.kind = py23::LiteralValue::Kind::Bool;
        lv.b = v;
        return lv;
    };
    auto lv_float = [](double v) {
        py23::LiteralValue lv;
        lv.kind = py23::LiteralValue::Kind::Float;
        lv.f = v;
        return lv;
    };
    auto lv_str = [](const std::string& v) {
        py23::LiteralValue lv;
        lv.kind = py23::LiteralValue::Kind::Str;
        lv.s = v;
        return lv;
    };
    auto func_with = [&](const std::string& n, const py23::FuncInfoPtr& f) {
        auto t = py23::new_ty(py23::TyKind::Func);
        t->fn = f;
        info->globals[n] = t;
    };
    if (name == "math") {
        info->globals["pi"] = py23::t_float();
        info->globals["e"] = py23::t_float();
        info->globals["tau"] = py23::t_float();
        info->globals["inf"] = py23::t_float();
        info->globals["nan"] = py23::t_float();
        for (auto& n : {"sqrt", "fabs", "sin", "cos", "tan", "log", "log2", "log10", "exp"})
            func(n, {py23::t_float()}, py23::t_float());
        func("floor", {py23::t_float()}, py23::t_int());
        func("ceil", {py23::t_float()}, py23::t_int());
        func("pow", {py23::t_float(), py23::t_float()}, py23::t_float());
        func("fmod", {py23::t_float(), py23::t_float()}, py23::t_float());
        for (auto& n : {"atan2", "hypot", "copysign"})
            func(n, {py23::t_float(), py23::t_float()}, py23::t_float());
        for (auto& n : {"degrees", "radians"})
            func(n, {py23::t_float()}, py23::t_float());
        func("trunc", {py23::t_float()}, py23::t_int());
        for (auto& n : {"isnan", "isinf", "isfinite"})
            func(n, {py23::t_float()}, py23::t_bool());
        func("factorial", {py23::t_int()}, py23::t_int());
        func("gcd", {py23::t_int(), py23::t_int()}, py23::t_int());
        func("lcm", {py23::t_int(), py23::t_int()}, py23::t_int());
        func("fsum", {py23::mk_list(py23::t_float())}, py23::t_float());
        for (auto& n : {"acos", "asin", "atan", "acosh", "asinh", "atanh", "cbrt",
                        "cosh", "sinh", "tanh", "exp2", "expm1", "log1p", "erf",
                        "erfc", "gamma", "lgamma", "ulp"})
            func(n, {py23::t_float()}, py23::t_float());
        for (auto& n : {"nextafter", "remainder"})
            func(n, {py23::t_float(), py23::t_float()}, py23::t_float());
        func("ldexp", {py23::t_float(), py23::t_int()}, py23::t_float());
        func("fma", {py23::t_float(), py23::t_float(), py23::t_float()},
             py23::t_float());
        func("isclose", {py23::t_float(), py23::t_float()}, py23::t_bool());
        func("frexp", {py23::t_float()}, py23::mk_tuple({py23::t_float(), py23::t_int()}));
        func("modf", {py23::t_float()}, py23::mk_tuple({py23::t_float(), py23::t_float()}));
        func("isqrt", {py23::t_int()}, py23::t_int());
        func("comb", {py23::t_int(), py23::t_int()}, py23::t_int());
        func("perm", {py23::t_int(), py23::t_int()}, py23::t_int());
        func("prod", {py23::mk_list(py23::t_int())}, py23::t_int());
        func("sumprod", {py23::mk_list(py23::t_int()), py23::mk_list(py23::t_int())},
             py23::t_int());
        func("dist", {py23::mk_list(py23::t_float()), py23::mk_list(py23::t_float())},
             py23::t_float());
    } else if (name == "sys") {
        info->globals["argv"] = py23::mk_list(py23::t_str());
        info->globals["maxsize"] = py23::t_int();
        info->globals["stdin"] = py23::t_file();
        info->globals["stdout"] = py23::t_file();
        info->globals["stderr"] = py23::t_file();
        func("exit", {py23::t_int()}, py23::t_none());
    } else if (name == "asyncio") {
        // run/create_task 的结果类型按实参在 typecheck 里特判；这里的签名
        // 只用于 from-import 的缺省检查和错误提示。
        func("run", {py23::mk_coroutine(py23::t_unknown())}, py23::t_unknown());
        func("sleep", {py23::t_float()}, py23::mk_coroutine(py23::t_none()));
        func("create_task", {py23::mk_coroutine(py23::t_unknown())},
             py23::mk_task(py23::t_unknown()));
        func("Future", {}, py23::mk_future(py23::t_unknown()));
        func("Event", {}, py23::mk_async_event());
        func("Lock", {}, py23::mk_async_lock());
        {
            auto qf = std::make_shared<py23::FuncInfo>();
            qf->name = "Queue";
            py23::Param p;
            p.name = "maxsize";
            p.ty = py23::t_int();
            p.has_default = true;
            py23::LiteralValue lv;
            lv.kind = py23::LiteralValue::Kind::Int;
            lv.i = 0;
            p.default_lit = lv;
            qf->pos.push_back(std::move(p));
            qf->ret = py23::mk_async_queue(py23::t_unknown());
            qf->ret_known = true;
            auto qt = py23::new_ty(py23::TyKind::Func);
            qt->fn = qf;
            info->globals["Queue"] = qt;
        }
        func("wait_for", {py23::t_unknown(), py23::t_float()},
             py23::mk_coroutine(py23::t_unknown()));
        func("timeout", {py23::t_float()}, py23::mk_async_timeout());
        func("TaskGroup", {}, py23::mk_async_taskgroup());
        {
            auto sf = std::make_shared<py23::FuncInfo>();
            sf->name = "Semaphore";
            py23::Param p;
            p.name = "value";
            p.ty = py23::t_int();
            p.has_default = true;
            py23::LiteralValue lv;
            lv.kind = py23::LiteralValue::Kind::Int;
            lv.i = 1;
            p.default_lit = lv;
            sf->pos.push_back(std::move(p));
            sf->ret = py23::mk_async_semaphore();
            sf->ret_known = true;
            auto st = py23::new_ty(py23::TyKind::Func);
            st->fn = sf;
            info->globals["Semaphore"] = st;
        }
        func("wait_readable", {py23::t_int()}, py23::mk_coroutine(py23::t_none()));
        func("wait_writable", {py23::t_int()}, py23::mk_coroutine(py23::t_none()));
        auto gather = std::make_shared<py23::FuncInfo>();
        gather->name = "gather";
        gather->varargs = true;
        gather->vararg_name = "aws";
        gather->vararg_ty = py23::t_unknown();
        gather->ret = py23::mk_coroutine(py23::mk_list(py23::t_unknown()));
        gather->ret_known = true;
        auto gt = py23::new_ty(py23::TyKind::Func);
        gt->fn = gather;
        info->globals["gather"] = gt;
    } else if (name == "time") {
        func("time", {}, py23::t_float());
        func("monotonic", {}, py23::t_float());
        func("perf_counter", {}, py23::t_float());
        func("sleep", {py23::t_float()}, py23::t_none());
    } else if (name == "itertools") {
        // 没有泛型：签名只用于 from-import 的可调用标记，返回类型在
        // 调用点由 itertools_call 按实参特化。
        auto two_lists = [&](const std::string& n, bool second_int, py23::TypePtr ret) {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "itertools." + n;
            py23::Param p;
            p.name = "a";
            p.ty = py23::mk_list(py23::t_unknown());
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = second_int ? "b" : "b";
            q.ty = second_int ? py23::t_int() : py23::mk_list(py23::t_unknown());
            f->pos.push_back(std::move(q));
            f->ret = std::move(ret);
            f->ret_known = true;
            func_with(n, f);
        };
        two_lists("chain", false, py23::mk_list(py23::t_unknown()));
        two_lists("repeat", true, py23::mk_list(py23::t_unknown()));
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "itertools.islice";
            py23::Param p;
            p.name = "x";
            p.ty = py23::mk_list(py23::t_unknown());
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "stop";
            q.ty = py23::t_int();
            f->pos.push_back(std::move(q));
            f->ret = py23::mk_list(py23::t_unknown());
            f->ret_known = true;
            func_with("islice", f);
        }
    } else if (name == "collections") {
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "collections.Counter";
            py23::Param p;
            p.name = "items";
            p.ty = py23::mk_list(py23::t_unknown());
            f->pos.push_back(std::move(p));
            f->ret = py23::mk_dict(py23::t_unknown(), py23::t_int());
            f->ret_known = true;
            func_with("Counter", f);
        }
        // OrderedDict：py23 的 dict 保序，直接当空 dict 用。
        func("OrderedDict", {}, py23::mk_dict(py23::t_str(), py23::t_unknown()));
        // deque(iterable=None, maxlen=None)：两个参数都可选。
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "collections.deque";
            py23::Param p;
            p.name = "iterable";
            p.ty = py23::mk_list(py23::t_unknown());
            p.has_default = true;
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "maxlen";
            q.ty = py23::t_int();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            f->ret = py23::new_ty(py23::TyKind::Deque);
            f->ret_known = true;
            func_with("deque", f);
        }
    } else if (name == "datetime") {
        auto dt = py23::new_ty(py23::TyKind::DateTime);
        // date(y, m, d) / today()
        func("date", {py23::t_int(), py23::t_int(), py23::t_int()}, dt);
        func("today", {}, dt);
        // now()：带时分秒的当前时间
        func("now", {}, dt);
        // datetime(...)：3 或 6 个位置参数，个数在降级阶段检查
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "datetime.datetime";
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = py23::t_int();
            f->ret = dt;
            f->ret_known = true;
            func_with("datetime", f);
        }
    } else if (name == "pathlib") {
        auto p = py23::new_ty(py23::TyKind::Path);
        func("Path", {py23::t_str()}, p);
    } else if (name == "functools") {
        // reduce 的返回类型按实参在调用点特化，签名只负责参数个数检查。
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "functools.reduce";
            py23::Param p;
            p.name = "function";
            p.ty = py23::t_unknown();
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "iterable";
            q.ty = py23::t_unknown();
            f->pos.push_back(std::move(q));
            py23::Param r;
            r.name = "initial";
            r.ty = py23::t_unknown();
            r.has_default = true;
            f->pos.push_back(std::move(r));
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with("reduce", f);
        }
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "functools.wraps";
            py23::Param p;
            p.name = "wrapped";
            p.ty = py23::t_unknown();
            f->pos.push_back(std::move(p));
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with("wraps", f);
        }
        for (auto& n : {"partial", "lru_cache"}) {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = std::string("functools.") + n;
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = py23::t_unknown();
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with(n, f);
        }
    } else if (name == "json") {
        // 类型由调用点的静态类型决定；签名只用于 from-import 的可调用标记。
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "json.dumps";
            py23::Param p;
            p.name = "obj";
            p.ty = py23::t_unknown();
            f->pos.push_back(std::move(p));
            // indent / sort_keys 可选；实际取值在降级阶段按关键字读。
            py23::Param q;
            q.name = "indent";
            q.ty = py23::t_int();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            py23::Param r;
            r.name = "sort_keys";
            r.ty = py23::t_bool();
            r.has_default = true;
            f->pos.push_back(std::move(r));
            f->ret = py23::t_str();
            f->ret_known = true;
            func_with("dumps", f);
        }
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "json.loads";
            py23::Param p;
            p.name = "s";
            p.ty = py23::t_str();
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "sample";
            q.ty = py23::t_unknown();
            f->pos.push_back(std::move(q));
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with("loads", f);
        }
    } else if (name == "re") {
        info->globals["I"] = py23::t_int();
        info->globals["IGNORECASE"] = py23::t_int();
        info->globals["M"] = py23::t_int();
        info->globals["MULTILINE"] = py23::t_int();
        info->globals["S"] = py23::t_int();
        info->globals["DOTALL"] = py23::t_int();
        info->globals["X"] = py23::t_int();
        info->globals["VERBOSE"] = py23::t_int();
        auto re_fn = [&](const std::string& n,
                         std::vector<std::pair<std::string, py23::TypePtr>> ps,
                         size_t ndef, py23::TypePtr ret) {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = n;
            for (size_t i = 0; i < ps.size(); ++i) {
                py23::Param p;
                p.name = ps[i].first;
                p.ty = ps[i].second;
                if (i + ndef >= ps.size()) {
                    p.has_default = true;
                    p.default_lit = lv_int(0);
                }
                f->pos.push_back(std::move(p));
            }
            f->ret = ret;
            f->ret_known = true;
            func_with(n, f);
        };
        re_fn("compile", {{"pattern", py23::t_str()}, {"flags", py23::t_int()}}, 1,
              py23::t_regex());
        re_fn("search",
              {{"pattern", py23::t_str()}, {"string", py23::t_str()}, {"flags", py23::t_int()}},
              1, py23::mk_optional(py23::t_match()));
        re_fn("match",
              {{"pattern", py23::t_str()}, {"string", py23::t_str()}, {"flags", py23::t_int()}},
              1, py23::mk_optional(py23::t_match()));
        re_fn("fullmatch",
              {{"pattern", py23::t_str()}, {"string", py23::t_str()}, {"flags", py23::t_int()}},
              1, py23::mk_optional(py23::t_match()));
        re_fn("findall",
              {{"pattern", py23::t_str()}, {"string", py23::t_str()}, {"flags", py23::t_int()}},
              1, py23::mk_list(py23::t_str()));
        re_fn("finditer",
              {{"pattern", py23::t_str()}, {"string", py23::t_str()}, {"flags", py23::t_int()}},
              1, py23::mk_list(py23::t_match()));
        re_fn("sub",
              {{"pattern", py23::t_str()},
               {"repl", py23::t_str()},
               {"string", py23::t_str()},
               {"count", py23::t_int()},
               {"flags", py23::t_int()}},
              2, py23::t_str());
        re_fn("split",
              {{"pattern", py23::t_str()},
               {"string", py23::t_str()},
               {"maxsplit", py23::t_int()},
               {"flags", py23::t_int()}},
              2, py23::mk_list(py23::t_str()));
        re_fn("escape", {{"pattern", py23::t_str()}}, 0, py23::t_str());
    } else if (name == "random") {
        // 不传种子时绑定哨兵 INT64_MIN，运行时用时间 + pid 播种。
        const int64_t kNoSeed = -9223372036854775807LL - 1;
        func("random", {}, py23::t_float());
        func("randint", {py23::t_int(), py23::t_int()}, py23::t_int());
        func("uniform", {py23::t_float(), py23::t_float()}, py23::t_float());
        func("getrandbits", {py23::t_int()}, py23::t_int());
        func("gauss", {py23::t_float(), py23::t_float()}, py23::t_float());
        func("expovariate", {py23::t_float()}, py23::t_float());
        // triangular：CPython 里三个参数都有默认值，这里只把后两个标成可选，
        // 实际取值由降级阶段按实参个数决定（mode 缺省取中点）。
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "random.triangular";
            const char* names[] = {"low", "high", "mode"};
            for (int i = 0; i < 3; ++i) {
                py23::Param p;
                p.name = names[i];
                p.ty = py23::t_float();
                if (i > 0) {
                    p.has_default = true;
                    p.default_lit = lv_float(0.0);
                }
                f->pos.push_back(std::move(p));
            }
            f->ret = py23::t_float();
            f->ret_known = true;
            func_with("triangular", f);
        }
        // randrange：1~3 个位置参数，个数和方向在降级阶段检查
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "random.randrange";
            f->varargs = true;
            f->vararg_name = "args";
            f->vararg_ty = py23::t_int();
            f->ret = py23::t_int();
            f->ret_known = true;
            func_with("randrange", f);
        }
        // sample / choices：返回 list[元素类型]，类型在 typecheck 里特化
        for (auto& n : {"sample", "choices"}) {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = std::string("random.") + n;
            py23::Param p;
            p.name = "seq";
            p.ty = py23::t_unknown();
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "k";
            q.ty = py23::t_int();
            f->pos.push_back(std::move(q));
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with(n, f);
        }
        // choice/shuffle 的返回类型按实参在 typecheck 里特判（random_call）；
        // 这两个 FuncInfo 用带模块前缀的名字，from-import 调用也能识别。
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "random.choice";
            py23::Param p;
            p.name = "seq";
            p.ty = py23::t_unknown();
            f->pos.push_back(std::move(p));
            f->ret = py23::t_unknown();
            f->ret_known = false;
            func_with("choice", f);
        }
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "random.shuffle";
            py23::Param p;
            p.name = "x";
            p.ty = py23::mk_list(py23::t_unknown());
            f->pos.push_back(std::move(p));
            f->ret = py23::t_none();
            f->ret_known = true;
            func_with("shuffle", f);
        }
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "seed";
            py23::Param p;
            p.name = "a";
            p.ty = py23::t_int();
            p.has_default = true;
            p.default_lit = lv_int(kNoSeed);
            f->pos.push_back(std::move(p));
            f->ret = py23::t_none();
            f->ret_known = true;
            func_with("seed", f);
        }
        {
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "Random";
            py23::Param p;
            p.name = "a";
            p.ty = py23::t_int();
            p.has_default = true;
            p.default_lit = lv_int(kNoSeed);
            f->pos.push_back(std::move(p));
            f->ret = py23::t_random();
            f->ret_known = true;
            func_with("Random", f);
        }
    } else if (name == "os") {
        info->globals["sep"] = py23::t_str();
        info->globals["linesep"] = py23::t_str();
        for (auto& n : {"F_OK", "R_OK", "W_OK", "X_OK"}) info->globals[n] = py23::t_int();
        func("mkdir", {py23::t_str()}, py23::t_none());
        func("rmdir", {py23::t_str()}, py23::t_none());
        func("remove", {py23::t_str()}, py23::t_none());
        func("rename", {py23::t_str(), py23::t_str()}, py23::t_none());
        func("getcwd", {}, py23::t_str());
        func("cpu_count", {}, py23::t_int());
        func("listdir", {py23::t_str()}, py23::mk_list(py23::t_str()));
        {
            // getenv(name, default="")：CPython 缺失返回 None，py23 用空串默认值。
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "getenv";
            py23::Param p;
            p.name = "name";
            p.ty = py23::t_str();
            f->pos.push_back(std::move(p));
            py23::Param d;
            d.name = "default";
            d.ty = py23::t_str();
            d.has_default = true;
            py23::LiteralValue lv;
            lv.kind = py23::LiteralValue::Kind::Str;
            lv.s = "";
            d.default_lit = lv;
            f->pos.push_back(std::move(d));
            f->ret = py23::t_str();
            f->ret_known = true;
            func_with("getenv", f);
        }
        func("getpid", {}, py23::t_int());
        func("chdir", {py23::t_str()}, py23::t_none());
        func("chmod", {py23::t_str(), py23::t_int()}, py23::t_none());
        func("access", {py23::t_str(), py23::t_int()}, py23::t_bool());
        {
            // makedirs(name, exist_ok=False)：exist_ok 的参数在降级阶段
            // 手工装配（默认值没有 AST 节点可用）。
            auto f = std::make_shared<py23::FuncInfo>();
            f->name = "os.makedirs";
            py23::Param p;
            p.name = "name";
            p.ty = py23::t_str();
            f->pos.push_back(std::move(p));
            py23::Param q;
            q.name = "exist_ok";
            q.ty = py23::t_bool();
            q.has_default = true;
            f->pos.push_back(std::move(q));
            f->ret = py23::t_none();
            f->ret_known = true;
            func_with("makedirs", f);
        }
    } else if (name == "os.path") {
        auto f = std::make_shared<py23::FuncInfo>();
        f->name = "join";
        f->varargs = true;
        f->vararg_name = "parts";
        f->vararg_ty = py23::t_str();
        f->ret = py23::t_str();
        f->ret_known = true;
        func_with("join", f);
        func("exists", {py23::t_str()}, py23::t_bool());
        func("isdir", {py23::t_str()}, py23::t_bool());
        func("isfile", {py23::t_str()}, py23::t_bool());
        for (auto& n : {"basename", "dirname", "normpath", "abspath", "expanduser"})
            func(n, {py23::t_str()}, py23::t_str());
        func("splitext", {py23::t_str()}, py23::mk_tuple({py23::t_str(), py23::t_str()}));
        func("realpath", {py23::t_str()}, py23::t_str());
        func("getsize", {py23::t_str()}, py23::t_int());
        func("getmtime", {py23::t_str()}, py23::t_float());
        func("split", {py23::t_str()}, py23::mk_tuple({py23::t_str(), py23::t_str()}));
        func("samefile", {py23::t_str(), py23::t_str()}, py23::t_bool());
    } else if (name == "subprocess") {
        auto f = std::make_shared<py23::FuncInfo>();
        f->name = "run";
        auto add = [&](const std::string& n, py23::TypePtr ty, bool has_def,
                       py23::LiteralValue lv) {
            py23::Param p;
            p.name = n;
            p.ty = std::move(ty);
            p.has_default = has_def;
            if (has_def) p.default_lit = lv;
            f->pos.push_back(std::move(p));
        };
        add("args", py23::t_unknown(), false, lv_int(0));
        add("capture_output", py23::t_bool(), true, lv_bool(false));
        add("text", py23::t_bool(), true, lv_bool(false));
        add("timeout", py23::t_float(), true, lv_float(-1.0));
        add("check", py23::t_bool(), true, lv_bool(false));
        add("shell", py23::t_bool(), true, lv_bool(false));
        add("cwd", py23::t_str(), true, lv_str(""));
        f->ret = py23::t_completed();
        f->ret_known = true;
        func_with("run", f);
        // Popen：真正降级在 lower_popen 里手工装配，这里只让类型检查通过。
        {
            auto g = std::make_shared<py23::FuncInfo>();
            g->name = "subprocess.Popen";
            auto padd = [&](const std::string& n, py23::TypePtr ty, bool has_def,
                            py23::LiteralValue lv) {
                py23::Param q;
                q.name = n;
                q.ty = std::move(ty);
                q.has_default = has_def;
                if (has_def) q.default_lit = lv;
                g->pos.push_back(std::move(q));
            };
            padd("args", py23::t_unknown(), false, lv_int(0));
            padd("shell", py23::t_bool(), true, lv_bool(false));
            padd("cwd", py23::t_unknown(), true, lv_str(""));
            padd("stdin", py23::t_unknown(), true, lv_int(0));
            padd("stdout", py23::t_unknown(), true, lv_int(0));
            padd("stderr", py23::t_unknown(), true, lv_int(0));
            padd("text", py23::t_bool(), true, lv_bool(false));
            g->ret = py23::t_popen();
            g->ret_known = true;
            func_with("Popen", g);
        }
        info->globals["PIPE"] = py23::t_int();
        info->globals["STDOUT"] = py23::t_int();
        info->globals["DEVNULL"] = py23::t_int();
    }
    cache[name] = info;
    return info;
}

bool build_unit(Program& p, ModuleUnit& u, bool emit_object, bool entry_ir) {
    auto now = [] { return std::chrono::steady_clock::now(); };
    const bool timing = std::getenv("PY23_TIMING") != nullptr;
    auto t0 = now();
    auto tc = std::make_shared<py23::TypeChecker>(u.path);
    tc->set_module_lookup([&p](const std::string& name) -> py23::ModuleInfoPtr {
        return program_module(p, name);
    });
    tc->check(*u.ast);
    auto t1 = now();
    if (!tc->diags().empty()) {
        for (auto& d : tc->diags()) report(u.path, u.source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s in %s\n", tc->diags().size(),
                     tc->diags().size() == 1 ? "" : "s", u.path.c_str());
        return false;
    }
    u.tc = tc;

    auto info = std::make_shared<py23::ModuleInfo>();
    info->name = u.name;
    info->source_path = u.path;
    if (auto* sc = tc->module_scope()) {
        for (auto& [name, sym] : sc->vars) {
            if (!u.is_entry && sym.ty && sym.ty->kind == py23::TyKind::Class && sym.ty->cls) {
                // 扁平类接口：字段按根→派生顺序拍平，方法按最终槽位覆盖。
                const py23::ClassInfo* cls = sym.ty->cls;
                std::vector<const py23::ClassInfo*> chain;
                for (auto* cp = cls; cp; cp = cp->base) chain.push_back(cp);
                auto flat = std::make_shared<py23::ClassInfo>();
                flat->name = name;
                flat->complete = true;
                flat->has_init = cls->has_init;
                flat->open = cls->is_open();
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    for (auto& f : (*it)->fields) flat->fields.push_back(f);
                std::map<int, py23::MethodInfo> by_slot;
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    for (auto& m : (*it)->methods)
                        if (m.call_sig) by_slot[m.slot] = m;
                for (auto& [slot, m] : by_slot) {
                    py23::MethodInfo nm;
                    nm.name = m.name;
                    nm.slot = slot;
                    nm.call_sig = m.call_sig;
                    nm.sig = m.call_sig;
                    flat->methods.push_back(std::move(nm));
                }
                py23::ImportedClass ic;
                ic.cls = flat;
                ic.ctor_symbol = py23::module_class_new_symbol(u.name, name);
                if (cls->has_init) {
                    if (auto* im = cls->method("__init__"); im && im->call_sig) {
                        if (im->call_sig->varargs || im->call_sig->kwargs) {
                            report(u.path, u.source, 1, 1,
                                   "exported class __init__ with *args/**kwargs is not supported yet");
                            return false;
                        }
                        ic.ctor_sig = im->call_sig;
                    }
                }
                if (!ic.ctor_sig) ic.ctor_sig = std::make_shared<py23::FuncInfo>();
                info->classes[name] = std::move(ic);
                continue;
            }
            info->globals[name] = sym.ty;
        }
    }
    info->globals["__name__"] = py23::t_str();
    info->globals["__file__"] = py23::t_str();
    u.info = info;

    if (!emit_object && !entry_ir) return true;
    if (entry_ir && !u.is_entry) return true;

    py23::LowerOptions opt;
    opt.module_symbol = u.name;
    opt.runtime_name = u.runtime_name;
    opt.source_path = u.path;
    opt.is_entry = u.is_entry;
    opt.opt_level = p.opt;
    opt.fast_math = p.fast_math;
    opt.module_lookup = [&p](const std::string& name) -> py23::ModuleInfoPtr {
        return program_module(p, name);
    };
    auto lr = py23::lower_module(*u.ast, *tc, u.path, opt);
    auto t2 = now();
    if (!lr.errors.empty()) {
        for (auto& e : lr.errors) report(u.path, u.source, e.line, e.col, e.msg);
        std::fprintf(stderr, "%zu error%s in code generation (%s)\n", lr.errors.size(),
                     lr.errors.size() == 1 ? "" : "s", u.path.c_str());
        return false;
    }
    u.needs_unknown_index = py23::ir_module_needs_unknown_index(lr.mod);
    // A/B 诊断：强制保留存活堆索引（对照「关闭」带来的收益）。
    if (std::getenv("PY23_FORCE_UNKNOWN_INDEX")) u.needs_unknown_index = true;
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

    u.asm_text = py23::generate_asm(lr.mod, u.is_entry && !p.needs_unknown_index);
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
        auto bytes = py23::encode_module_interface(*info);
        if (!bytes) {
            report(u.path, u.source, 1, 1,
                   std::format("cannot export module interface: {}", bytes.error()));
            return false;
        }
        info->interface_hash = py23::hex64(py23::fnv1a64(*bytes));
        std::error_code ec;
        std::filesystem::create_directories(u.cache_dir, ec);
        std::ofstream out(u.iface_path, std::ios::binary);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!out) {
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
    p.include_dirs.emplace_back(PY23_STDLIB_DIR);
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
    std::string cmd = "cc -o " + shell_quote(output);
    for (auto* u : p.order) cmd += " " + shell_quote(u->obj_path);
    cmd += " " + shell_quote(PY23_RT_OBJ) + " -lm";
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "error: link failed\n       command: %s\n", cmd.c_str());
        return false;
    }
    return true;
}

}  // namespace py23
