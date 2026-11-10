// main.cpp —— 命令行入口
//
// 用法:
//   py23 tokens <file.py>   打印 token 流
//   py23 ast    <file.py>   打印语法树
//   py23 types  <file.py>   打印每个顶层名字的推断类型
//   py23 check  <file.py>   词法 + 语法 + 类型检查
//   py23 check --syntax <f> 只做词法/语法检查
//   py23 ir     <file.py>   打印降级后的 IR
//   py23 asm    <file.py>   打印生成的 x86-64 汇编
//   py23 build  <file.py> [-o out]   编译并链接成可执行文件
//   py23 run    <file.py>   编译、链接并运行
//   py23        <file.py>   等价于 ast
#include <algorithm>
#include <cctype>
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

namespace {

// 用 std::expected 表达"可能失败"的文件读取，避免异常穿透
std::expected<std::string, std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot open '{}'", path));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::unexpected(std::format("failed while reading '{}'", path));
    return ss.str();
}

// 打印出错行，并在指定列插入 ^。
// 超长行（生成代码常见几千字符一行）只显示插入符附近的片段，
// 否则一条错误会把终端刷爆。
void print_snippet(const std::string& source, int line, int col) {
    if (line <= 0) return;
    size_t cur = 1;
    size_t begin = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (cur == static_cast<size_t>(line)) break;
        if (source[i] == '\n') {
            ++cur;
            begin = i + 1;
        }
    }
    if (cur != static_cast<size_t>(line)) return;
    size_t end = source.find('\n', begin);
    if (end == std::string::npos) end = source.size();
    if (end <= begin) return;

    std::string_view text(source.data() + begin, end - begin);
    constexpr size_t kMaxCols = 120;
    size_t caret = (col > 0) ? static_cast<size_t>(col - 1) : 0;
    size_t start = 0;
    if (text.size() > kMaxCols) {
        if (caret > kMaxCols / 3) start = caret - kMaxCols / 3;
        if (start + kMaxCols > text.size()) start = text.size() - kMaxCols;
        if (caret < start) caret = start;
    }
    size_t len = std::min(kMaxCols, text.size() - start);

    // 展开 tab，否则插入符对不齐
    std::string shown;
    size_t caret_pad = 0;
    for (size_t i = start; i < start + len; ++i) {
        if (i == caret) caret_pad = shown.size();
        char c = text[i];
        if (c == '\t') {
            size_t n = 4 - shown.size() % 4;
            shown.append(n, ' ');
        } else {
            shown.push_back(c);
        }
    }
    if (caret >= start + len) caret_pad = shown.size();

    const char* ellipsis = start ? "..." : "";
    std::fprintf(stderr, "  %5d | %s%.*s\n", line, ellipsis, static_cast<int>(shown.size()),
                 shown.data());
    std::string pad(std::strlen(ellipsis) + caret_pad, ' ');
    std::fprintf(stderr, "        | %s^\n", pad.c_str());
}

void report(const std::string& path, const std::string& source, int line, int col,
            std::string_view msg) {
    std::fprintf(stderr, "%s:%d:%d: error: %.*s\n", path.c_str(), line, col,
                 static_cast<int>(msg.size()), msg.data());
    print_snippet(source, line, col);
}

void dump_tokens(const std::vector<py23::Token>& toks) {
    for (const auto& t : toks) {
        std::string detail;
        switch (t.kind) {
            case py23::Tok::INT: detail = std::format("  value={}", t.ival); break;
            case py23::Tok::FLOAT: detail = std::format("  value={}", t.fval); break;
            case py23::Tok::NAME:
            case py23::Tok::STRING:
            case py23::Tok::FSTRING: detail = "  \"" + t.text + "\""; break;
            default: break;
        }
        std::printf("%4d:%-3d %-12s%s\n", t.line, t.col, py23::tok_name(t.kind), detail.c_str());
    }
}

// ── 编译流水线 ──────────────────────────────────────────────────

struct Frontend {
    std::unique_ptr<py23::Module> mod;
    py23::TypeChecker tc;

    explicit Frontend(std::string file) : tc(std::move(file)) {}
};

// 解析 + 类型检查。有错误时打印并返回 nullopt。
std::optional<Frontend> run_frontend(const std::string& path, const std::string& source,
                                     bool syntax_only) {
    py23::Lexer lexer(source);
    auto tokens = lexer.run();

    py23::Parser parser(std::move(tokens));
    auto mod = parser.parse();
    py23::desugar_lambdas(*mod);

    Frontend fe(path);
    fe.mod = std::move(mod);

    if (syntax_only) return fe;

    fe.tc.check(*fe.mod);
    if (!fe.tc.diags().empty()) {
        for (const auto& d : fe.tc.diags()) report(path, source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s\n", fe.tc.diags().size(),
                     fe.tc.diags().size() == 1 ? "" : "s");
        return std::nullopt;
    }
    return fe;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// 建一个唯一临时文件，返回路径。调用方负责 unlink。
std::expected<std::string, std::string> make_temp(const char* suffix) {
    std::string tmpl = std::format("/tmp/py23-XXXXXX{}", suffix);
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), static_cast<int>(std::strlen(suffix)));
    if (fd < 0) return std::unexpected("cannot create temporary file");
    ::close(fd);
    return std::string(buf.data());
}

// ── 多模块构建 ──────────────────────────────────────────────────
//
// 每个模块独立解析、类型检查、降级、汇编成 .o；接口写到 .py23i，
// manifest 记录源码哈希和依赖接口哈希。链接时把入口和所有模块的 .o
// 加上运行时一起交给 cc。

struct ModuleUnit {
    std::string key;           // 内部映射键；入口是 "@entry"
    std::string name;          // 符号前缀；入口是 "__entry__"
    std::string runtime_name;  // __name__
    std::string path;
    std::string source;
    std::unique_ptr<py23::Module> ast;
    py23::ModuleInfoPtr info;
    std::shared_ptr<py23::TypeChecker> tc;
    std::optional<py23::IRModule> ir;
    std::vector<std::string> deps;
    std::string cache_dir;
    std::string asm_path;
    std::string obj_path;
    std::string iface_path;
    std::string manifest_path;
    std::string asm_text;
    bool is_entry = false;
    bool is_package = false;
    std::string parent_name;
    bool from_cache = false;
    int state = 0;  // 0 未访问 / 1 正在访问 / 2 完成
};

struct Program {
    std::map<std::string, std::unique_ptr<ModuleUnit>> units;
    std::vector<ModuleUnit*> order;
    ModuleUnit* entry = nullptr;
    std::vector<std::string> include_dirs;
    bool rebuild = false;
    bool verbose = false;
    int opt = 2;
    bool fast_math = false;
};

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
        if (req.name == "math" || req.name == "sys" || req.name == "asyncio") {
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

bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p) {
    p.include_dirs = include_dirs;
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
    u.from_cache = true;
    return true;
}

py23::ModuleInfoPtr builtin_module(const std::string& name);

py23::ModuleInfoPtr program_module(Program& p, const std::string& name) {
    if (name == "math" || name == "sys" || name == "asyncio") return builtin_module(name);
    auto it = p.units.find(name);
    return it == p.units.end() ? nullptr : it->second->info;
}

py23::FuncInfoPtr builtin_fn(const std::string& name, const std::vector<py23::TypePtr>& args,
                             py23::TypePtr ret) {
    auto f = std::make_shared<py23::FuncInfo>();
    f->name = name;
    for (size_t i = 0; i < args.size(); ++i) {
        py23::Param p;
        p.name = std::format("x{}", i);
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
    if (name == "math") {
        info->globals["pi"] = py23::t_float();
        info->globals["e"] = py23::t_float();
        info->globals["tau"] = py23::t_float();
        for (auto& n : {"sqrt", "fabs", "sin", "cos", "tan", "log", "log2", "log10", "exp"})
            func(n, {py23::t_float()}, py23::t_float());
        func("floor", {py23::t_float()}, py23::t_int());
        func("ceil", {py23::t_float()}, py23::t_int());
        func("pow", {py23::t_float(), py23::t_float()}, py23::t_float());
        func("fmod", {py23::t_float(), py23::t_float()}, py23::t_float());
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
    }
    cache[name] = info;
    return info;
}

bool build_unit(Program& p, ModuleUnit& u, bool emit_object, bool entry_ir) {
    auto tc = std::make_shared<py23::TypeChecker>(u.path);
    tc->set_module_lookup([&p](const std::string& name) -> py23::ModuleInfoPtr {
        return program_module(p, name);
    });
    tc->check(*u.ast);
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
    if (!lr.errors.empty()) {
        for (auto& e : lr.errors) report(u.path, u.source, e.line, e.col, e.msg);
        std::fprintf(stderr, "%zu error%s in code generation (%s)\n", lr.errors.size(),
                     lr.errors.size() == 1 ? "" : "s", u.path.c_str());
        return false;
    }
    if (entry_ir) {
        u.ir = std::move(lr.mod);
        return true;
    }
    if (!emit_object) return true;

    u.asm_text = py23::generate_asm(lr.mod);
    std::string err;
    if (!assemble_object(u, err)) {
        std::fprintf(stderr, "%s: error: %s\n", u.path.c_str(), err.c_str());
        return false;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "py23 —— Python 语法外壳的编译器\n"
                     "\n"
                     "usage: %s [tokens|ast|types|check|ir|asm|build|run] [-O0|-O1|-O2] "
                     "[--fast-math] [--syntax] [-o out] <file.py>\n",
                     argv[0]);
        return 2;
    }

    std::string mode;
    std::string path;
    std::string output;
    std::vector<std::string> include_dirs;
    bool syntax_only = false;
    bool verbose = false;
    bool rebuild = false;
    int opt_level = 2;
    bool fast_math = false;

    static const std::vector<std::string> kModes = {"tokens", "ast",     "check", "types",
                                                    "ir",     "asm",     "build", "run"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--syntax") syntax_only = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--rebuild") rebuild = true;
        else if (a == "--fast-math") fast_math = true;
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
        else if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "-I" && i + 1 < argc) include_dirs.push_back(argv[++i]);
        else if (a.starts_with("-I") && a.size() > 2) include_dirs.push_back(a.substr(2));
        else if (mode.empty() && std::ranges::find(kModes, a) != kModes.end()) mode = a;
        else path = a;
    }
    if (mode.empty()) mode = "ast";
    if (path.empty()) {
        std::fprintf(stderr, "error: no input file\n");
        return 2;
    }

    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", src.error().c_str());
        return 1;
    }

    try {
        if (mode == "tokens") {
            py23::Lexer lexer(*src);
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
            // check/types 定位是通用前端：不解析 import 图，缺失/相对/
            // 星号导入只记 unsupported，交给 build 阶段判死。
            auto fe = run_frontend(path, *src, false);
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
        prog.rebuild = rebuild;
        prog.verbose = verbose;
        prog.opt = opt_level;
        prog.fast_math = fast_math;
        if (!load_program(path, include_dirs, prog)) return 1;

        if (mode == "ir" || mode == "asm") {
            if (!prepare_program(prog, false, true)) return 1;
            if (!prog.entry->ir) return 1;
            if (mode == "ir") {
                py23::dump_ir(*prog.entry->ir, std::cout);
            } else {
                std::cout << py23::generate_asm(*prog.entry->ir);
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
            int rc = std::system(cmd.c_str());
            if (ephemeral) ::unlink(out.c_str());
            if (rc < 0) return 1;
            return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
        }
        return 0;
    } catch (const py23::LexError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const py23::ParseError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: internal error: %s\n", path.c_str(), e.what());
    }
    return 1;
}
