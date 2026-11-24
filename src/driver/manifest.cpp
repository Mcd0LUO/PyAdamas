// driver/manifest.cpp —— pyadamas.toml 最小 TOML 子集的解析与校验
//
// 设计取舍（见 docs/package-management-design.md §6.1 / 落地记录 P0-a）：
//   * 只做单行子集：多行字符串、数组、日期、嵌套表一律明确报错。清单是人写的
//     构建输入，宁可报错也不要猜。
//   * [dependencies] 只支持 path 依赖：registry/版本范围/git 都是 P2，
//     遇到就报错并说清「P0 只支持 path 依赖」——半实现比没实现更危险。
//   * [modules] 是「逻辑名 →（包, 模块）」的显式映射（Q1）：语言层 import 只认
//     合法标识符，包名可以带 '-'，所以映射必须是数据而不是语法。
//   * [aliases] 只做语法解析：非空即报「P2 才支持」（多版本共存是 P2 的别名表）。
#include "driver/manifest.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>

#include "driver/modules.h"
#include "driver/util.h"

namespace pyadamas {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

bool is_bare_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

void skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && is_space(s[i])) ++i;
}

struct Value {
    enum class Kind : unsigned char { Str, Table, Bool, Int };
    Kind kind = Kind::Str;
    std::string str;
    std::map<std::string, Value> table;
    std::map<std::string, SourceLoc> key_locs;
    SourceLoc loc;
};

class TomlParser {
  public:
    explicit TomlParser(Manifest& m) : m_(m) {}

    bool run() {
        std::size_t pos = 0;
        int line_no = 0;
        while (pos <= m_.source.size()) {
            const std::size_t nl = m_.source.find('\n', pos);
            std::string line = m_.source.substr(pos, nl == std::string::npos ? std::string::npos
                                                                            : nl - pos);
            pos = (nl == std::string::npos) ? m_.source.size() + 1 : nl + 1;
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!parse_line(line, line_no)) return false;
        }
        return finish();
    }

  private:
    Manifest& m_;
    std::string table_;
    SourceLoc table_loc_;
    bool saw_package_ = false;
    bool has_name_ = false;
    bool has_version_ = false;
    std::map<std::string, SourceLoc> tables_;
    std::map<std::string, SourceLoc> keys_;

    bool error(int line, int col, const std::string& msg) {
        report(m_.path, m_.source, line, col, msg);
        return false;
    }

    bool parse_line(const std::string& line, int line_no) {
        std::size_t i = 0;
        skip_ws(line, i);
        if (i == line.size() || line[i] == '#') return true;
        if (line[i] == '[') return header(line, i, line_no);

        std::string key;
        SourceLoc kloc;
        if (!parse_key(line, i, line_no, key, kloc)) return false;
        skip_ws(line, i);
        if (i >= line.size() || line[i] != '=')
            return error(line_no, static_cast<int>(i) + 1, "expected '=' after key '" + key + "'");
        ++i;
        skip_ws(line, i);
        Value v;
        if (!parse_value(line, i, line_no, v)) return false;
        skip_ws(line, i);
        if (i < line.size() && line[i] != '#')
            return error(line_no, static_cast<int>(i) + 1,
                         "unexpected characters after value (P0 supports one assignment per line)");

        const std::string dedup = table_ + '\0' + key;
        if (const auto it = keys_.find(dedup); it != keys_.end())
            return error(kloc.line, kloc.col, "duplicate key '" + key + "' in [" + table_ + "]");
        keys_.emplace(dedup, kloc);

        if (table_ == "package") return package_key(key, kloc, v);
        if (table_ == "dependencies") return dependency_key(key, kloc, v);
        if (table_ == "modules") return module_key(key, kloc, v);
        if (table_ == "aliases")
            return error(kloc.line, kloc.col,
                         "[aliases] is P2: P0 resolves one version per logical name; remove the "
                         "[aliases] table (multi-version aliases land with the registry)");
        return error(kloc.line, kloc.col,
                     "key '" + key + "' must be inside a table ([package], [dependencies], "
                     "[modules])");
    }

    bool header(const std::string& line, std::size_t& i, int line_no) {
        if (line.compare(i, 2, "[[") == 0)
            return error(line_no, static_cast<int>(i) + 1,
                         "array-of-tables ('[[...]]') is not supported in P0");
        const std::size_t close = line.find(']', i);
        if (close == std::string::npos)
            return error(line_no, static_cast<int>(i) + 1, "unterminated table header");
        std::string name = line.substr(i + 1, close - i - 1);
        const std::size_t first = name.find_first_not_of(" \t");
        const std::size_t last = name.find_last_not_of(" \t");
        const SourceLoc loc{line_no, static_cast<int>(i) + 2};
        name = (first == std::string::npos) ? std::string() : name.substr(first, last - first + 1);
        std::size_t after = close + 1;
        skip_ws(line, after);
        if (after < line.size() && line[after] != '#')
            return error(line_no, static_cast<int>(after) + 1,
                         "unexpected characters after table header");
        if (name.empty()) return error(line_no, static_cast<int>(i) + 1, "empty table name");
        if (name.find('.') != std::string::npos)
            return error(loc.line, loc.col,
                         "nested tables ('[" + name + "]') are not supported in P0: use "
                         "[dependencies] with inline tables");
        if (name != "package" && name != "dependencies" && name != "modules" && name != "aliases")
            return error(loc.line, loc.col,
                         "unknown table '[" + name + "]' (P0 recognizes [package], "
                         "[dependencies], [modules], [aliases])");
        if (tables_.count(name) != 0)
            return error(loc.line, loc.col, "duplicate table '[" + name + "]'");
        tables_.emplace(name, loc);
        table_ = name;
        table_loc_ = loc;
        if (name == "package") saw_package_ = true;
        return true;
    }

    bool parse_key(const std::string& line, std::size_t& i, int line_no, std::string& out,
                   SourceLoc& loc) {
        loc = {line_no, static_cast<int>(i) + 1};
        out.clear();
        if (i < line.size() && line[i] == '"') return parse_string(line, i, line_no, out);
        while (i < line.size() && is_bare_key_char(line[i])) out.push_back(line[i++]);
        if (out.empty())
            return error(line_no, static_cast<int>(i) + 1,
                         "expected a key (bare key or quoted string)");
        return true;
    }

    bool parse_string(const std::string& line, std::size_t& i, int line_no, std::string& out) {
        const int col = static_cast<int>(i) + 1;
        if (i >= line.size() || line[i] != '"')
            return error(line_no, col, "expected a quoted string");
        ++i;
        out.clear();
        while (i < line.size()) {
            const char c = line[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                if (i + 1 >= line.size())
                    return error(line_no, static_cast<int>(i) + 1, "unterminated escape sequence");
                const char e = line[i + 1];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default:
                        return error(line_no, static_cast<int>(i) + 1,
                                     std::format("unsupported escape '\\{}' (P0 supports "
                                                 "\\\" \\\\ \\n \\t \\r)",
                                                 e));
                }
                i += 2;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return error(line_no, static_cast<int>(i) + 1,
                             "control character in string (P0 has single-line strings only)");
            out.push_back(c);
            ++i;
        }
        return error(line_no, col, "unterminated string (P0 has single-line strings only)");
    }

    bool parse_value(const std::string& line, std::size_t& i, int line_no, Value& out) {
        out.loc = {line_no, static_cast<int>(i) + 1};
        if (i >= line.size()) return error(line_no, static_cast<int>(i) + 1, "expected a value");
        const char c = line[i];
        if (c == '"') {
            out.kind = Value::Kind::Str;
            return parse_string(line, i, line_no, out.str);
        }
        if (c == '{') return parse_inline_table(line, i, line_no, out);
        if (c == '[')
            return error(line_no, static_cast<int>(i) + 1,
                         "arrays are not supported in P0 (use one key per line)");
        if (line.compare(i, 4, "true") == 0) {
            i += 4;
            out.kind = Value::Kind::Bool;
            return true;
        }
        if (line.compare(i, 5, "false") == 0) {
            i += 5;
            out.kind = Value::Kind::Bool;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+') {
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                                       line[i] == '.' || line[i] == '+' || line[i] == '-'))
                ++i;
            out.kind = Value::Kind::Int;
            return true;
        }
        return error(line_no, static_cast<int>(i) + 1,
                     "expected a string, inline table, or boolean (P0 does not support this value)");
    }

    bool parse_inline_table(const std::string& line, std::size_t& i, int line_no, Value& out) {
        out.kind = Value::Kind::Table;
        ++i;  // '{'
        skip_ws(line, i);
        if (i < line.size() && line[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skip_ws(line, i);
            std::string key;
            SourceLoc loc;
            if (!parse_key(line, i, line_no, key, loc)) return false;
            skip_ws(line, i);
            if (i >= line.size() || line[i] != '=')
                return error(line_no, static_cast<int>(i) + 1,
                             "expected '=' after inline-table key '" + key + "'");
            ++i;
            skip_ws(line, i);
            Value v;
            if (!parse_value(line, i, line_no, v)) return false;
            if (out.table.count(key) != 0)
                return error(loc.line, loc.col, "duplicate key '" + key + "' in inline table");
            out.table.emplace(key, std::move(v));
            out.key_locs.emplace(key, loc);
            skip_ws(line, i);
            if (i < line.size() && line[i] == ',') {
                ++i;
                continue;
            }
            if (i < line.size() && line[i] == '}') {
                ++i;
                return true;
            }
            return error(line_no, static_cast<int>(i) + 1,
                         "expected ',' or '}' in inline table (P0 keeps inline tables on one line)");
        }
    }

    bool require_string(const Value& v, const std::string& what, std::string& out) {
        if (v.kind != Value::Kind::Str)
            return error(v.loc.line, v.loc.col, what + " must be a quoted string");
        out = v.str;
        return true;
    }

    bool package_key(const std::string& key, const SourceLoc& loc, const Value& v) {
        if (key == "name") {
            if (!require_string(v, "'name'", m_.name)) return false;
            if (m_.name.empty())
                return error(loc.line, loc.col, "'name' must be non-empty");
            if (!valid_package_name(m_.name))
                return error(loc.line, loc.col,
                             "invalid package name '" + m_.name +
                                 "' (lowercase letters/digits, '-' '_' '.', must start with a "
                                 "letter or digit)");
            has_name_ = true;
            return true;
        }
        if (key == "version") {
            if (!require_string(v, "'version'", m_.version)) return false;
            if (m_.version.empty())
                return error(loc.line, loc.col, "'version' must be non-empty");
            if (!valid_version(m_.version))
                return error(loc.line, loc.col,
                             "invalid version '" + m_.version +
                                 "' (letters/digits and '.', '-', '+', '_'; path dependencies are "
                                 "pinned by their tree hash, not by version ranges)");
            has_version_ = true;
            return true;
        }
        return error(loc.line, loc.col,
                     "unknown key '" + key + "' in [package] (expected 'name' and 'version')");
    }

    bool dependency_key(const std::string& key, const SourceLoc& loc, const Value& v) {
        if (!valid_package_name(key))
            return error(loc.line, loc.col,
                         "invalid dependency name '" + key +
                             "' (lowercase letters/digits, '-' '_' '.', must start with a letter "
                             "or digit)");
        if (v.kind != Value::Kind::Table)
            return error(loc.line, loc.col,
                         "dependency '" + key + "' must be an inline table: " + key +
                             " = { path = \"../libfoo\" } (P0 only supports path dependencies; "
                             "registry/version requirements are P2)");
        ManifestDep dep;
        dep.loc = loc;
        for (const auto& [k, val] : v.table) {
            if (k == "path") {
                if (!require_string(val, "'path'", dep.path)) return false;
            } else if (k == "version" || k == "registry" || k == "git" || k == "url" || k == "rev" ||
                       k == "tag" || k == "branch") {
                return error(val.loc.line, val.loc.col,
                             "P0 only supports path dependencies: dependency '" + key + "' uses '" +
                                 k +
                                 "'; version ranges/registry/git dependencies are P2 (path = "
                                 "\"../libfoo\")");
            } else {
                return error(val.loc.line, val.loc.col,
                             "unknown key '" + k + "' in dependency '" + key +
                                 "' (P0 supports only 'path')");
            }
        }
        if (dep.path.empty())
            return error(loc.line, loc.col,
                         "dependency '" + key + "' needs a non-empty 'path' (P0 only supports path "
                         "dependencies)");
        m_.deps.emplace(key, std::move(dep));
        return true;
    }

    bool module_key(const std::string& key, const SourceLoc& loc, const Value& v) {
        if (!valid_dotted_module(key))
            return error(loc.line, loc.col,
                         "invalid logical module name '" + key +
                             "' (dotted identifiers; quote names containing '.')");
        if (v.kind != Value::Kind::Table)
            return error(loc.line, loc.col,
                         "module binding '" + key +
                             "' must be an inline table: " + key +
                             " = { package = \"libfoo\", module = \"foo\" }");
        ManifestModule bind;
        bind.loc = loc;
        for (const auto& [k, val] : v.table) {
            if (k == "package") {
                if (!require_string(val, "'package'", bind.package)) return false;
            } else if (k == "module") {
                if (!require_string(val, "'module'", bind.module)) return false;
            } else {
                return error(val.loc.line, val.loc.col,
                             "unknown key '" + k + "' in module binding '" + key +
                                 "' (expected 'package' and 'module')");
            }
        }
        if (bind.package.empty())
            return error(loc.line, loc.col,
                         "module binding '" + key + "' needs 'package' (the [dependencies] key)");
        if (!valid_package_name(bind.package))
            return error(loc.line, loc.col,
                         "invalid package name '" + bind.package + "' in module binding '" + key +
                             "'");
        if (bind.module.empty())
            return error(loc.line, loc.col,
                         "module binding '" + key + "' needs 'module' (the module name inside the "
                         "package)");
        if (!valid_dotted_module(bind.module))
            return error(loc.line, loc.col,
                         "invalid module name '" + bind.module + "' in module binding '" + key +
                             "' (dotted identifiers)");
        m_.modules.emplace(key, std::move(bind));
        return true;
    }

    bool finish() {
        if (!saw_package_)
            return error(1, 1,
                         "missing [package] table: a manifest must declare its own name and "
                         "version");
        if (!has_name_)
            return error(table_loc_.line, table_loc_.col, "missing 'name' in [package]");
        if (!has_version_)
            return error(table_loc_.line, table_loc_.col, "missing 'version' in [package]");
        return true;
    }
};

}  // namespace

bool valid_package_name(const std::string& s) {
    if (s.empty()) return false;
    if (!std::isalnum(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (std::islower(static_cast<unsigned char>(c)) ||
            std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
            continue;
        return false;
    }
    return true;
}

bool valid_version(const std::string& s) {
    if (s.empty()) return false;
    if (!std::isalnum(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
            c == '_')
            continue;
        return false;
    }
    return true;
}

bool valid_dotted_module(const std::string& s) {
    if (s.empty()) return false;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = s.find('.', start);
        const std::string part =
            s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!valid_module_name(part)) return false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

std::optional<Manifest> load_manifest(const std::string& file) {
    auto text = read_file(file);
    if (!text) {
        std::fprintf(stderr, "error: cannot read manifest '%s' (%s)\n", file.c_str(),
                     text.error().c_str());
        return std::nullopt;
    }
    std::error_code ec;
    Manifest m;
    m.path = std::filesystem::absolute(file, ec);
    if (ec) m.path = file;
    m.dir = std::filesystem::path(m.path).parent_path().string();
    m.source = std::move(*text);
    TomlParser parser(m);
    if (!parser.run()) return std::nullopt;
    return m;
}

std::optional<std::string> find_manifest(const std::string& path) {
    std::error_code ec;
    std::filesystem::path start;
    if (path.empty()) {
        start = std::filesystem::current_path(ec);
    } else {
        const std::filesystem::path p(path);
        start = std::filesystem::is_directory(p, ec) ? p : p.parent_path();
    }
    if (start.empty()) start = std::filesystem::current_path(ec);
    start = std::filesystem::weakly_canonical(start, ec);
    if (ec) start = std::filesystem::absolute(path.empty() ? "." : path, ec);
    for (auto dir = start;; dir = dir.parent_path()) {
        const auto candidate = dir / "pyadamas.toml";
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
        if (dir == dir.parent_path()) break;
    }
    return std::nullopt;
}

}  // namespace pyadamas
