// driver/lock.cpp —— pyadamas.lock：树哈希、解析、渲染、diff、派生模块图
//
// 锁是构建的**唯一输入**：包集合（坐标/相对路径/树哈希）+ 模块映射 + 依赖边。
// 三条硬性质在这里实现：
//   1) 确定性：所有集合排序、无时间戳/主机名/绝对路径、LF、单尾换行；
//   2) 完整性：每个包都有源码树 SHA-256（不是 FNV：这是信任根）；
//   3) 可校验：--locked 用 diff_locks 回答「清单现在会产出什么、和锁差在哪」。
#include "driver/lock.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <string_view>
#include <utility>

#include "backend/target_abi.h"
#include "driver/modules.h"
#include "driver/sha256.h"
#include "driver/util.h"
#include "ir/module.h"

#ifndef PYADAMAS_VERSION
#define PYADAMAS_VERSION "dev"
#endif

namespace pyadamas {
namespace {

namespace fs = std::filesystem;

// ── 小工具 ──────────────────────────────────────────────────────

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string canonical_dir(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(fs::path(p), ec);
    if (ec) return fs::absolute(fs::path(p), ec).lexically_normal().string();
    return c.string();
}

std::string generic_rel(const std::string& from_dir, const std::string& target) {
    return fs::path(target).lexically_relative(fs::path(from_dir)).generic_string();
}

// 行式格式里所有字符串都加引号：路径可能带空格，审计时也能一眼看出边界。
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) out += std::format("\\x{:02x}", c);
                else out.push_back(ch);
        }
    }
    out += '"';
    return out;
}

bool parse_quoted(const std::string& line, std::size_t& i, std::string& out, std::string& err) {
    ++i;  // '"'
    while (i < line.size()) {
        const char c = line[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + 1 >= line.size()) {
            err = "unterminated escape sequence";
            return false;
        }
        const char e = line[i + 1];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'x': {
                if (i + 3 >= line.size()) {
                    err = "bad \\x escape";
                    return false;
                }
                auto hex = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                const int hi = hex(line[i + 2]);
                const int lo = hex(line[i + 3]);
                if (hi < 0 || lo < 0) {
                    err = "bad \\x escape";
                    return false;
                }
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                break;
            }
            default:
                err = std::format("bad escape '\\{}'", e);
                return false;
        }
        i += 2;
    }
    err = "unterminated quoted string";
    return false;
}

// 空白分词；引号内的空白不切分（path="a b" 是一个 token）。
bool tokenize(const std::string& line, std::vector<std::string>& out, std::string& err) {
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            if (line[i] == '"') {
                if (!parse_quoted(line, i, tok, err)) return false;
            } else {
                tok.push_back(line[i++]);
            }
        }
        out.push_back(std::move(tok));
    }
    return true;
}

bool field_value(const std::string& tok, std::string_view key, std::string& out) {
    const std::string prefix = std::string(key) + "=";
    if (!tok.starts_with(prefix)) return false;
    out = tok.substr(prefix.size());
    return true;
}

bool relative_path_ok(const std::string& p, bool allow_parent) {
    if (p.empty()) return false;
    const fs::path path(p);
    if (path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == "..") {
            if (!allow_parent) return false;
        } else if (part == "." || part.empty()) {
            return false;
        }
    }
    return true;
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out.push_back(ch);
        }
    }
    out += '"';
    return out;
}

// ── 源码树哈希 ──────────────────────────────────────────────────

bool collect_files(const fs::path& dir, const std::string& prefix, std::vector<std::string>& rels,
                   std::string& err) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        err = std::format("cannot read directory '{}': {}", dir.string(), ec.message());
        return false;
    }
    std::vector<fs::directory_entry> entries(it, fs::directory_iterator());
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a,
                                                  const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    });
    for (const auto& e : entries) {
        const std::string name = e.path().filename().string();
        // VCS 元数据不是源码：它随每次 commit/checkout 变化，进树哈希只会制造噪声。
        if (name == ".git") continue;
        const fs::file_status status = e.symlink_status(ec);
        if (ec) {
            err = std::format("cannot stat '{}': {}", e.path().string(), ec.message());
            return false;
        }
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (fs::is_symlink(status)) {
            err = std::format(
                "symlink '{}' is not supported by the P0 tree hash (use regular files; the P1 "
                "store will dereference and record link targets)",
                e.path().string());
            return false;
        }
        if (fs::is_directory(status)) {
            if (!collect_files(e.path(), rel, rels, err)) return false;
        } else if (fs::is_regular_file(status)) {
            rels.push_back(rel);
        } else {
            err = std::format("'{}' is not a regular file or directory; the P0 tree hash only "
                              "covers regular files",
                              e.path().string());
            return false;
        }
    }
    return true;
}

}  // namespace

std::string tree_hash(const std::string& dir, std::string& err) {
    err.clear();
    std::vector<std::string> rels;
    if (!collect_files(fs::path(dir), "", rels, err)) return {};
    std::sort(rels.begin(), rels.end());
    Sha256 h;
    for (const auto& rel : rels) {
        const fs::path full = fs::path(dir) / rel;
        std::error_code ec;
        const auto size = fs::file_size(full, ec);
        if (ec) {
            err = std::format("cannot stat '{}': {}", full.string(), ec.message());
            return {};
        }
        // 长度前缀消除歧义：路径/内容里出现 '\0' 也不会让两棵树哈希相同。
        h.update(rel);
        h.update(std::string_view("\0", 1));
        h.update(std::to_string(size));
        h.update(std::string_view("\0", 1));
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            err = std::format("cannot read '{}'", full.string());
            return {};
        }
        char buf[1 << 16];
        while (in) {
            in.read(buf, static_cast<std::streamsize>(sizeof buf));
            const std::streamsize n = in.gcount();
            if (n > 0) h.update(buf, static_cast<std::size_t>(n));
        }
        if (in.bad()) {
            err = std::format("failed while reading '{}'", full.string());
            return {};
        }
    }
    return h.final_hex();
}

// ── 解析 path 依赖闭包 ──────────────────────────────────────────

namespace {

class Resolver {
  public:
    explicit Resolver(const Manifest& root) : root_(root) {}

    std::optional<Lock> run() {
        lock_.compiler_version = PYADAMAS_VERSION;
        lock_.target = target_abi().name;
        lock_.module_format = pyadamas::kModuleFormatVersion;
        lock_.module_abi = pyadamas::kModuleAbiVersion;
        lock_.root_name = root_.name;
        lock_.root_version = root_.version;
        root_dir_ = canonical_dir(root_.dir);
        dir_owner_[root_dir_] = root_.name;
        if (!visit(root_)) return std::nullopt;
        if (!merge_modules()) return std::nullopt;
        std::sort(lock_.deps.begin(), lock_.deps.end(), [](const LockedDep& a, const LockedDep& b) {
            if (a.from != b.from) return a.from < b.from;
            if (a.to != b.to) return a.to < b.to;
            return a.path < b.path;
        });
        lock_.deps.erase(std::unique(lock_.deps.begin(), lock_.deps.end(),
                                     [](const LockedDep& a, const LockedDep& b) {
                                         return a.from == b.from && a.to == b.to &&
                                                a.path == b.path;
                                     }),
                         lock_.deps.end());
        return std::move(lock_);
    }

  private:
    struct Node {
        Manifest manifest;
        std::string dir;       // 规范化绝对目录
        std::string rel_path;  // 相对项目根（锁文件所在目录）
        std::string tree;
    };

    struct Origin {
        std::string manifest;
        SourceLoc loc;
    };

    bool fail(const Manifest& m, const SourceLoc& loc, const std::string& msg) {
        report(m.path, m.source, loc.line, loc.col, msg);
        return false;
    }

    std::string cycle_chain(const std::string& target_name) const {
        std::string chain;
        bool started = false;
        for (const auto& n : stack_) {
            if (!started && n != target_name) continue;
            started = true;
            chain += n + " -> ";
        }
        chain += target_name;
        return chain;
    }

    bool visit(const Manifest& m) {
        const std::string cur_dir = canonical_dir(m.dir);
        state_[cur_dir] = 1;
        stack_.push_back(m.name);
        for (const auto& [dep_name, dep] : m.deps) {
            std::error_code ec;
            const fs::path raw = fs::path(m.dir) / dep.path;
            if (!fs::is_directory(raw, ec)) {
                fail(m, dep.loc,
                     std::format("dependency '{}': path '{}' does not exist (resolved to '{}'); "
                                 "P0 dependencies must be directories that contain pyadamas.toml",
                                 dep_name, dep.path, raw.lexically_normal().string()));
                return false;
            }
            const std::string target = canonical_dir(raw.string());
            const std::string edge_path = fs::path(dep.path).lexically_normal().generic_string();
            const auto owner = dir_owner_.find(target);
            if (owner != dir_owner_.end()) {
                if (owner->second != dep_name) {
                    fail(m, dep.loc,
                         std::format("dependency '{}' at '{}' is already used as package '{}' "
                                     "(one directory cannot be two packages)",
                                     dep_name, dep.path, owner->second));
                    return false;
                }
                if (state_[target] == 1) {
                    fail(m, dep.loc,
                         std::format("path dependency cycle: {}", cycle_chain(target)));
                    return false;
                }
                lock_.deps.push_back({m.name, dep_name, edge_path});
                continue;
            }
            const fs::path manifest_file = fs::path(target) / "pyadamas.toml";
            if (!fs::is_regular_file(manifest_file, ec)) {
                fail(m, dep.loc,
                     std::format("dependency '{}' at '{}' has no pyadamas.toml; P0 requires "
                                 "every path dependency to be a package with its own manifest",
                                 dep_name, dep.path));
                return false;
            }
            auto dep_manifest = load_manifest(manifest_file.string());
            if (!dep_manifest) return false;
            if (dep_manifest->name != dep_name) {
                fail(m, dep.loc,
                     std::format("manifest at '{}' declares package '{}', but [dependencies] "
                                 "calls it '{}'; make the two names match",
                                 dep.path, dep_manifest->name, dep_name));
                return false;
            }
            if (dep_name == root_.name) {
                fail(m, dep.loc,
                     std::format("package name '{}' is used both by the root project and a "
                                 "dependency; rename one",
                                 dep_name));
                return false;
            }
            if (const auto other = by_name_.find(dep_name); other != by_name_.end()) {
                fail(m, dep.loc,
                     std::format("package '{}' resolves to two different directories: '{}' "
                                 "and '{}'",
                                 dep_name, other->second.dir, target));
                return false;
            }
            std::string hash_err;
            std::string tree = tree_hash(target, hash_err);
            if (tree.empty()) {
                fail(m, dep.loc,
                     std::format("cannot hash source tree of dependency '{}' at '{}': {}",
                                 dep_name, dep.path, hash_err));
                return false;
            }
            Node node;
            node.manifest = std::move(*dep_manifest);
            node.dir = target;
            node.rel_path = generic_rel(root_dir_, target);
            node.tree = std::move(tree);
            LockedPackage pkg;
            pkg.name = dep_name;
            pkg.version = node.manifest.version;
            pkg.path = node.rel_path;
            pkg.tree = node.tree;
            lock_.packages.emplace(dep_name, std::move(pkg));
            dir_owner_[target] = dep_name;
            auto [it, inserted] = by_name_.emplace(dep_name, std::move(node));
            (void)inserted;
            state_[target] = 1;
            lock_.deps.push_back({m.name, dep_name, edge_path});
            if (!visit(it->second.manifest)) return false;
        }
        state_[cur_dir] = 2;
        stack_.pop_back();
        return true;
    }

    bool module_relpath(const Node& pkg, const Manifest& owner, const ManifestModule& bind,
                        std::string& rel) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true) {
            const std::size_t dot = bind.module.find('.', start);
            parts.push_back(bind.module.substr(
                start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        fs::path dir(pkg.dir);
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            dir /= parts[i];
            std::error_code ec;
            if (!fs::is_regular_file(dir / "__init__.py", ec)) {
                return fail(owner, bind.loc,
                            std::format("module '{}' in package '{}': '{}' must be a directory "
                                        "package (missing {})",
                                        bind.module, pkg.manifest.name,
                                        (fs::path(pkg.dir) / parts[i]).string(),
                                        (fs::path(pkg.dir) / parts[i] / "__init__.py").string()));
            }
        }
        const fs::path as_file = dir / (parts.back() + ".py");
        const fs::path as_package = dir / parts.back() / "__init__.py";
        std::error_code ec;
        fs::path chosen;
        if (fs::is_regular_file(as_file, ec)) {
            chosen = as_file;
        } else if (fs::is_regular_file(as_package, ec)) {
            chosen = as_package;
        } else {
            return fail(owner, bind.loc,
                        std::format("module '{}' was not found in package '{}' ('{}'): expected "
                                    "'{}' or '{}'",
                                    bind.module, pkg.manifest.name, pkg.dir,
                                    (fs::path(pkg.dir) / (bind.module + ".py")).string(),
                                    (fs::path(pkg.dir) / bind.module / "__init__.py").string()));
        }
        rel = chosen.lexically_relative(pkg.dir).generic_string();
        return true;
    }

    bool merge_modules() {
        std::vector<std::pair<std::string, const Manifest*>> owners;
        owners.emplace_back(root_.name, &root_);
        for (const auto& [name, node] : by_name_) owners.emplace_back(name, &node.manifest);
        std::sort(owners.begin(), owners.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::map<std::string, Origin> origin;
        for (const auto& [owner_name, m] : owners) {
            for (const auto& [logical, bind] : m->modules) {
                if (const auto declared = m->deps.find(bind.package); declared == m->deps.end()) {
                    return fail(*m, bind.loc,
                                std::format("module '{}' binds package '{}', which is not in "
                                            "[dependencies] of '{}'; add an explicit path "
                                            "dependency first",
                                            logical, bind.package, owner_name));
                }
                const auto pkg = by_name_.find(bind.package);
                if (pkg == by_name_.end()) {
                    return fail(*m, bind.loc,
                                std::format("module '{}' binds package '{}', but that package is "
                                            "not in the resolved dependency closure",
                                            logical, bind.package));
                }
                if (logical == "__entry__" || logical == "__main__") {
                    return fail(*m, bind.loc,
                                std::format("logical module name '{}' is reserved by the compiler",
                                            logical));
                }
                if (find_module_entry(logical) != nullptr) {
                    return fail(*m, bind.loc,
                                std::format("logical module name '{}' is reserved by the standard "
                                            "library/builtin module table; choose a different name "
                                            "(aliasing a stdlib name is not allowed, and aliases "
                                            "are P2)",
                                            logical));
                }
                std::string rel;
                if (!module_relpath(pkg->second, *m, bind, rel)) return false;
                LockedModule lm;
                lm.logical = logical;
                lm.package = bind.package;
                lm.module = bind.module;
                lm.path = rel;
                const auto it = lock_.modules.find(logical);
                if (it == lock_.modules.end()) {
                    lock_.modules.emplace(logical, std::move(lm));
                    origin.emplace(logical, Origin{m->path, bind.loc});
                    continue;
                }
                if (it->second.package == bind.package && it->second.module == bind.module &&
                    it->second.path == rel)
                    continue;
                const Origin& first = origin.at(logical);
                return fail(*m, bind.loc,
                            std::format("module '{}' is bound twice: {}/{} ({}:{}) and {}/{}; a "
                                        "single import name can bind one module (aliases are P2)",
                                        logical, it->second.package, it->second.module,
                                        first.manifest, first.loc.line, bind.package, bind.module));
            }
        }
        return true;
    }

    const Manifest& root_;
    std::string root_dir_;
    Lock lock_;
    std::map<std::string, Node> by_name_;
    std::map<std::string, std::string> dir_owner_;
    std::map<std::string, int> state_;
    std::vector<std::string> stack_;
};

}  // namespace

std::optional<Lock> resolve_lock(const Manifest& root) {
    Resolver resolver(root);
    return resolver.run();
}

// ── 渲染 ────────────────────────────────────────────────────────

std::string Lock::render() const {
    std::string out;
    out += "# pyadamas.lock —— machine generated by 'pyadamas resolve'; do not edit.\n";
    out += std::format("lock {}\n", format);
    out += std::format(
        "toolchain pyadamas {} target {} module-format {} module-abi {}\n", compiler_version,
        target, module_format, module_abi);
    out += std::format("root {} {}\n", root_name, root_version);

    std::string pkgs;
    for (const auto& [name, p] : packages) {
        pkgs += std::format("pkg {}@{} path={} tree=sha256:{}\n", p.name, p.version, quote(p.path),
                            p.tree);
    }
    std::string maps;
    for (const auto& [logical, m] : modules) {
        maps += std::format("map {} {} {} {}\n", m.logical, m.package, m.module, quote(m.path));
    }
    std::string edges;
    for (const auto& d : deps) {
        edges += std::format("dep {} {} path={}\n", d.from, d.to, quote(d.path));
    }
    if (!pkgs.empty()) out += "\n" + pkgs;
    if (!maps.empty()) out += "\n" + maps;
    if (!edges.empty()) out += "\n" + edges;
    return out;
}

// ── 解析锁文件 ─────────────────────────────────────────────────

namespace {

class LockParser {
  public:
    LockParser(std::string file, std::string text)
        : file_(std::move(file)), text_(std::move(text)) {}

    std::optional<Lock> run() {
        std::size_t pos = 0;
        int line_no = 0;
        bool any = false;
        while (pos <= text_.size()) {
            const std::size_t nl = text_.find('\n', pos);
            std::string line = text_.substr(pos, nl == std::string::npos ? std::string::npos
                                                                        : nl - pos);
            pos = (nl == std::string::npos) ? text_.size() + 1 : nl + 1;
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            std::string err;
            std::vector<std::string> toks;
            if (!tokenize(line, toks, err)) {
                report(file_, text_, line_no, 1, "invalid lock syntax: " + err);
                return std::nullopt;
            }
            if (toks.empty()) continue;
            if (toks[0] == "lock") {
                if (!parse_lock_line(toks, line_no)) return std::nullopt;
            } else if (!any) {
                report(file_, text_, line_no, 1, "the first line of a lock must be 'lock 1'");
                return std::nullopt;
            } else if (toks[0] == "toolchain") {
                if (!parse_toolchain(toks, line_no)) return std::nullopt;
            } else if (toks[0] == "root") {
                if (!parse_root(toks, line_no)) return std::nullopt;
            } else if (toks[0] == "pkg") {
                if (!parse_pkg(toks, line_no)) return std::nullopt;
            } else if (toks[0] == "map") {
                if (!parse_map(toks, line_no)) return std::nullopt;
            } else if (toks[0] == "dep") {
                if (!parse_dep(toks, line_no)) return std::nullopt;
            } else {
                report(file_, text_, line_no, 1,
                       std::format("unknown lock line '{}' (this compiler understands lock "
                                   "format 1)",
                                   toks[0]));
                return std::nullopt;
            }
            any = true;
        }
        if (!saw_lock_) {
            report(file_, text_, 1, 1, "missing 'lock 1' line");
            return std::nullopt;
        }
        if (!saw_root_) {
            report(file_, text_, 1, 1, "missing 'root <name> <version>' line");
            return std::nullopt;
        }
        for (const auto& ref : map_refs_) {
            if (lock_.packages.count(ref.package) == 0) {
                report(file_, text_, ref.line, 1,
                       std::format("module '{}' references package '{}' which has no pkg line",
                                   ref.logical, ref.package));
                return std::nullopt;
            }
        }
        for (const auto& ref : dep_refs_) {
            if (ref.from != lock_.root_name && lock_.packages.count(ref.from) == 0) {
                report(file_, text_, ref.line, 1,
                       std::format("dep edge starts at unknown package '{}'", ref.from));
                return std::nullopt;
            }
            if (lock_.packages.count(ref.to) == 0) {
                report(file_, text_, ref.line, 1,
                       std::format("dep edge points at unknown package '{}'", ref.to));
                return std::nullopt;
            }
        }
        return std::move(lock_);
    }

  private:
    struct Ref {
        int line = 0;
        std::string logical;
        std::string package;
        std::string from;
        std::string to;
    };

    bool parse_lock_line(const std::vector<std::string>& t, int line) {
        if (saw_lock_) {
            report(file_, text_, line, 1, "duplicate 'lock' line");
            return false;
        }
        if (t.size() != 2) {
            report(file_, text_, line, 1, "expected 'lock 1'");
            return false;
        }
        if (t[1] != "1") {
            report(file_, text_, line, 1,
                   std::format("unsupported lock format '{}' (this compiler understands 1); run "
                               "'pyadamas resolve' with a matching toolchain",
                               t[1]));
            return false;
        }
        saw_lock_ = true;
        return true;
    }

    bool parse_toolchain(const std::vector<std::string>& t, int line) {
        if (t.size() != 9 || t[1] != "pyadamas" || t[3] != "target" || t[5] != "module-format" ||
            t[7] != "module-abi") {
            report(file_, text_, line, 1,
                   "expected 'toolchain pyadamas <version> target <abi> module-format <n> "
                   "module-abi <n>'");
            return false;
        }
        lock_.compiler_version = t[2];
        lock_.target = t[4];
        lock_.module_format = parse_int(t[6]);
        lock_.module_abi = parse_int(t[8]);
        if (lock_.module_format < 0 || lock_.module_abi < 0) {
            report(file_, text_, line, 1, "module-format/module-abi must be non-negative integers");
            return false;
        }
        return true;
    }

    bool parse_root(const std::vector<std::string>& t, int line) {
        if (saw_root_) {
            report(file_, text_, line, 1, "duplicate 'root' line");
            return false;
        }
        if (t.size() != 3) {
            report(file_, text_, line, 1, "expected 'root <name> <version>'");
            return false;
        }
        if (!valid_package_name(t[1]) || !valid_version(t[2])) {
            report(file_, text_, line, 1,
                   std::format("invalid root package '{}@{}'", t[1], t[2]));
            return false;
        }
        lock_.root_name = t[1];
        lock_.root_version = t[2];
        saw_root_ = true;
        return true;
    }

    bool parse_pkg(const std::vector<std::string>& t, int line) {
        if (t.size() != 4) {
            report(file_, text_, line, 1,
                   "expected 'pkg <name>@<version> path=\"...\" tree=sha256:<hex>'");
            return false;
        }
        std::string name, version;
        if (!split_name_version(t[1], name, version)) {
            report(file_, text_, line, 1,
                   std::format("invalid package coordinate '{}' (expected <name>@<version>)", t[1]));
            return false;
        }
        std::string path;
        std::string tree_field;
        if (!field_value(t[2], "path", path) || !field_value(t[3], "tree", tree_field)) {
            report(file_, text_, line, 1,
                   "expected 'path=\"...\"' and 'tree=sha256:<hex>' after the coordinate");
            return false;
        }
        if (!relative_path_ok(path, true)) {
            report(file_, text_, line, 1,
                   std::format("package path '{}' must be a relative path (locks must be portable; "
                               "run 'pyadamas resolve' to regenerate)",
                               path));
            return false;
        }
        if (!tree_field.starts_with("sha256:") || !is_hex64(tree_field.substr(7))) {
            report(file_, text_, line, 1,
                   "tree must be 'sha256:' followed by 64 lowercase hex digits");
            return false;
        }
        if (lock_.packages.count(name) != 0) {
            report(file_, text_, line, 1, std::format("duplicate package '{}'", name));
            return false;
        }
        LockedPackage pkg;
        pkg.name = name;
        pkg.version = version;
        pkg.path = path;
        pkg.tree = tree_field.substr(7);
        lock_.packages.emplace(name, std::move(pkg));
        return true;
    }

    bool parse_map(const std::vector<std::string>& t, int line) {
        if (t.size() != 5) {
            report(file_, text_, line, 1,
                   "expected 'map <logical> <package> <module> \"<path-inside-package>\"'");
            return false;
        }
        if (!valid_dotted_module(t[1]) || !valid_package_name(t[2]) ||
            !valid_dotted_module(t[3])) {
            report(file_, text_, line, 1,
                   "map needs a dotted logical module name, a package name, and a dotted module "
                   "name");
            return false;
        }
        if (!relative_path_ok(t[4], false)) {
            report(file_, text_, line, 1,
                   std::format("module path '{}' must be relative to the package root", t[4]));
            return false;
        }
        if (lock_.modules.count(t[1]) != 0) {
            report(file_, text_, line, 1, std::format("duplicate map for module '{}'", t[1]));
            return false;
        }
        if (find_module_entry(t[1]) != nullptr) {
            report(file_, text_, line, 1,
                   std::format("module '{}' is reserved by the standard library/builtin module "
                               "table",
                               t[1]));
            return false;
        }
        LockedModule m;
        m.logical = t[1];
        m.package = t[2];
        m.module = t[3];
        m.path = t[4];
        lock_.modules.emplace(m.logical, std::move(m));
        Ref ref;
        ref.line = line;
        ref.logical = t[1];
        ref.package = t[2];
        map_refs_.push_back(std::move(ref));
        return true;
    }

    bool parse_dep(const std::vector<std::string>& t, int line) {
        if (t.size() != 4) {
            report(file_, text_, line, 1,
                   "expected 'dep <from> <to> path=\"...\"' (P0 has path dependencies only)");
            return false;
        }
        std::string path;
        if (!field_value(t[3], "path", path)) {
            report(file_, text_, line, 1, "dep line needs 'path=\"...\"'");
            return false;
        }
        if (!relative_path_ok(path, true)) {
            report(file_, text_, line, 1,
                   std::format("dep path '{}' must be relative (it is the requirement as written "
                               "in the declaring manifest)",
                               path));
            return false;
        }
        if (!valid_package_name(t[1]) || !valid_package_name(t[2])) {
            report(file_, text_, line, 1, "dep line needs valid package names");
            return false;
        }
        lock_.deps.push_back({t[1], t[2], path});
        Ref ref;
        ref.line = line;
        ref.from = t[1];
        ref.to = t[2];
        dep_refs_.push_back(std::move(ref));
        return true;
    }

    static int parse_int(const std::string& s) {
        if (s.empty()) return -1;
        int v = 0;
        for (char c : s) {
            if (c < '0' || c > '9' || v > 100000) return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }

    static bool split_name_version(const std::string& s, std::string& name, std::string& version) {
        const std::size_t at = s.rfind('@');
        if (at == std::string::npos || at == 0 || at + 1 == s.size()) return false;
        name = s.substr(0, at);
        version = s.substr(at + 1);
        return valid_package_name(name) && valid_version(version);
    }

    std::string file_;
    std::string text_;
    Lock lock_;
    bool saw_lock_ = false;
    bool saw_root_ = false;
    std::vector<Ref> map_refs_;
    std::vector<Ref> dep_refs_;
};

}  // namespace

std::optional<Lock> read_lock(const std::string& file) {
    auto text = read_file(file);
    if (!text) {
        std::fprintf(stderr, "error: cannot read lock '%s' (%s)\n", file.c_str(),
                     text.error().c_str());
        return std::nullopt;
    }
    LockParser parser(file, std::move(*text));
    return parser.run();
}

bool write_lock_file(const std::string& file, const std::string& text) {
    const fs::path target(file);
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "error: cannot write '%s'\n", tmp.string().c_str());
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            std::fprintf(stderr, "error: cannot write '%s'\n", tmp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        std::fprintf(stderr, "error: cannot replace '%s' (%s)\n", file.c_str(),
                     ec.message().c_str());
        return false;
    }
    return true;
}

// ── 语义 diff ──────────────────────────────────────────────────

std::string diff_locks(const Lock& want, const Lock& got) {
    std::string out;
    auto add = [&out](const std::string& line) { out += line + "\n"; };

    if (!want.root_name.empty() || !got.root_name.empty()) {
        const std::string a = std::format("{}@{}", want.root_name, want.root_version);
        const std::string b = std::format("{}@{}", got.root_name, got.root_version);
        // 统一方向：当前锁 -> 重新解析会写出的锁（与 package/map/dep 行一致）。
        if (a != b) add(std::format("  ~ root {} -> {}", b, a));
    }

    std::set<std::string> names;
    for (const auto& [n, p] : want.packages) names.insert(n);
    for (const auto& [n, p] : got.packages) names.insert(n);
    for (const auto& name : names) {
        const auto w = want.packages.find(name);
        const auto g = got.packages.find(name);
        if (w == want.packages.end()) {
            add(std::format("  - package {}@{} path={} tree=sha256:{}", g->second.name,
                            g->second.version, quote(g->second.path), g->second.tree));
            continue;
        }
        if (g == got.packages.end()) {
            add(std::format("  + package {}@{} path={} tree=sha256:{}", w->second.name,
                            w->second.version, quote(w->second.path), w->second.tree));
            continue;
        }
        if (w->second.version != g->second.version)
            add(std::format("  ~ package {}: version {} -> {}", name, g->second.version,
                            w->second.version));
        if (w->second.path != g->second.path)
            add(std::format("  ~ package {}: path {} -> {}", name, quote(g->second.path),
                            quote(w->second.path)));
        if (w->second.tree != g->second.tree)
            add(std::format("  ~ package {}: tree sha256:{} -> sha256:{}", name, g->second.tree,
                            w->second.tree));
    }

    std::set<std::string> logicals;
    for (const auto& [n, m] : want.modules) logicals.insert(n);
    for (const auto& [n, m] : got.modules) logicals.insert(n);
    for (const auto& logical : logicals) {
        const auto w = want.modules.find(logical);
        const auto g = got.modules.find(logical);
        if (w == want.modules.end()) {
            add(std::format("  - map {} -> {}/{} ({})", logical, g->second.package,
                            g->second.module, quote(g->second.path)));
            continue;
        }
        if (g == got.modules.end()) {
            add(std::format("  + map {} -> {}/{} ({})", logical, w->second.package,
                            w->second.module, quote(w->second.path)));
            continue;
        }
        const std::string a =
            std::format("{}/{} ({})", g->second.package, g->second.module, quote(g->second.path));
        const std::string b =
            std::format("{}/{} ({})", w->second.package, w->second.module, quote(w->second.path));
        if (a != b) add(std::format("  ~ map {}: {} -> {}", logical, a, b));
    }

    auto dep_key = [](const LockedDep& d) { return d.from + "\x1f" + d.to; };
    std::map<std::string, const LockedDep*> wdeps;
    std::map<std::string, const LockedDep*> gdeps;
    std::set<std::string> edge_keys;
    for (const auto& d : want.deps) {
        wdeps[dep_key(d)] = &d;
        edge_keys.insert(dep_key(d));
    }
    for (const auto& d : got.deps) {
        gdeps[dep_key(d)] = &d;
        edge_keys.insert(dep_key(d));
    }
    for (const auto& key : edge_keys) {
        const auto w = wdeps.find(key);
        const auto g = gdeps.find(key);
        if (w == wdeps.end()) {
            add(std::format("  - dep {} -> {} (path={})", g->second->from, g->second->to,
                            quote(g->second->path)));
            continue;
        }
        if (g == gdeps.end()) {
            add(std::format("  + dep {} -> {} (path={})", w->second->from, w->second->to,
                            quote(w->second->path)));
            continue;
        }
        if (w->second->path != g->second->path)
            add(std::format("  ~ dep {} -> {}: path {} -> {}", w->second->from, w->second->to,
                            quote(g->second->path), quote(w->second->path)));
    }
    return out;
}

// ── 锁 → 模块映射（复用 --module-map 的 schema/加载器）──────────

std::string lock_module_map_json(const Lock& lock, const std::string& lock_dir) {
    std::string out;
    out += "{\n";
    out += "  \"kind\": \"pyadamas-lock-map\",\n";
    out += "  \"format\": 1,\n";
    // root 只是给审计者的提示：真实路径一律绝对，所以 map 文件放哪里都成立
    // （现有加载器的相对路径规则是先按 map 目录、再按 map 目录+root）。
    out += "  \"root\": ";
    out += json_quote(lock_dir);
    out += ",\n  \"modules\": [";
    bool first = true;
    for (const auto& [logical, m] : lock.modules) {
        const auto pkg = lock.packages.find(m.package);
        if (pkg == lock.packages.end()) return {};
        const std::string abs =
            (fs::path(lock_dir) / pkg->second.path / m.path).lexically_normal().string();
        if (!first) out += ",";
        first = false;
        out += "\n    {\"name\": " + json_quote(logical) + ", \"path\": " + json_quote(abs) +
               ", \"source\": \"external\"}";
    }
    if (!first) out += "\n  ";
    out += "]\n}\n";
    return out;
}

std::optional<ModuleMap> module_map_from_lock(const Lock& lock, const std::string& lock_dir) {
    const std::string json = lock_module_map_json(lock, lock_dir);
    if (json.empty()) {
        std::fprintf(stderr,
                     "error: lock references a package that is not pinned (internal inconsistency); "
                     "run 'pyadamas resolve'\n");
        return std::nullopt;
    }
    auto tmp = make_temp(".map");
    if (!tmp) {
        std::fprintf(stderr, "error: %s\n", tmp.error().c_str());
        return std::nullopt;
    }
    struct Cleanup {
        std::string path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } cleanup{*tmp};
    {
        std::ofstream out(*tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "error: cannot write the lock-derived module map '%s'\n",
                         tmp->c_str());
            return std::nullopt;
        }
        out << json;
        out.close();
        if (!out) {
            std::fprintf(stderr, "error: cannot write the lock-derived module map '%s'\n",
                         tmp->c_str());
            return std::nullopt;
        }
    }
    std::string err;
    auto map = load_module_map(*tmp, err);
    if (!map) {
        std::fprintf(stderr, "error: the lock-derived module map is invalid: %s\n", err.c_str());
        return std::nullopt;
    }
    return map;
}

}  // namespace pyadamas
