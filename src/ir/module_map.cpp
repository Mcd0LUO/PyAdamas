// ir/module_map.cpp —— --module-map 的读取与校验
//
// 只解析 `pyadamas deps --json` 的 schema（JSON 子集：对象/数组/字符串/
// 数字/布尔/null），不为通用 JSON 引入依赖。刻意做成「严格 + 明确报错」：
// 映射是构建输入，任何一条指向错文件都必须在编译前炸掉，而不是静默回落。
#include "ir/module_map.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace pyadamas {
namespace {

struct JVal {
    enum class Kind : std::uint8_t { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const std::string& key) const {
        if (kind != Kind::Obj) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class Parser {
  public:
    Parser(const std::string& text) : s_(text) {}

    bool parse(JVal& out, std::string& err) {
        skip_ws();
        if (!value(out, err)) return false;
        skip_ws();
        if (i_ != s_.size()) {
            err = std::string("trailing characters at byte ") + std::to_string(i_);
            return false;
        }
        return true;
    }

  private:
    const std::string& s_;
    std::size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
                continue;
            }
            break;
        }
    }

    bool fail(std::string& err, const char* what) {
        err = std::string(what) + " at byte " + std::to_string(i_);
        return false;
    }

    bool value(JVal& out, std::string& err) {
        if (i_ >= s_.size()) return fail(err, "unexpected end of JSON");
        const char c = s_[i_];
        if (c == '{') return object(out, err);
        if (c == '[') return array(out, err);
        if (c == '"') {
            out.kind = JVal::Kind::Str;
            return string(out.str, err);
        }
        if (c == 't' || c == 'f') {
            const bool v = (c == 't');
            const std::size_t n = v ? 4 : 5;
            if (s_.compare(i_, n, v ? "true" : "false") != 0) return fail(err, "bad literal");
            i_ += n;
            out.kind = JVal::Kind::Bool;
            out.b = v;
            return true;
        }
        if (c == 'n') {
            if (s_.compare(i_, 4, "null") != 0) return fail(err, "bad literal");
            i_ += 4;
            out.kind = JVal::Kind::Null;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return number(out, err);
        return fail(err, "unexpected character");
    }

    bool object(JVal& out, std::string& err) {
        out.kind = JVal::Kind::Obj;
        ++i_;  // '{'
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail(err, "expected object key");
            std::string key;
            if (!string(key, err)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail(err, "expected ':'");
            ++i_;
            skip_ws();
            JVal v;
            if (!value(v, err)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail(err, "expected ',' or '}'");
        }
    }

    bool array(JVal& out, std::string& err) {
        out.kind = JVal::Kind::Arr;
        ++i_;  // '['
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            skip_ws();
            JVal v;
            if (!value(v, err)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail(err, "expected ',' or ']'");
        }
    }

    bool string(std::string& out, std::string& err) {
        ++i_;  // '"'
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return fail(err, "bad escape");
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // 只做「保留原字符」的降级：映射里的模块名/路径都是 ASCII。
                    if (i_ + 4 > s_.size()) return fail(err, "bad \\u escape");
                    i_ += 4;
                    out.push_back('?');
                    break;
                }
                default: return fail(err, "bad escape");
            }
        }
        return fail(err, "unterminated string");
    }

    bool number(JVal& out, std::string& err) {
        const std::size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ == start) return fail(err, "bad number");
        out.kind = JVal::Kind::Num;
        try {
            out.num = std::stod(s_.substr(start, i_ - start));
        } catch (...) {
            return fail(err, "bad number");
        }
        return true;
    }
};

std::string read_all(const std::string& file, bool& ok) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

}  // namespace

std::optional<ModuleMap> load_module_map(const std::string& file, std::string& err) {
    bool ok = false;
    const std::string text = read_all(file, ok);
    if (!ok) {
        err = "cannot read module map '" + file + "'";
        return std::nullopt;
    }
    JVal root;
    std::string perr;
    Parser parser(text);
    if (!parser.parse(root, perr)) {
        err = std::format("module map '{}': invalid JSON ({})", file, perr);
        return std::nullopt;
    }
    const JVal* mods = root.get("modules");
    if (mods == nullptr || mods->kind != JVal::Kind::Arr) {
        err = std::format("module map '{}': missing 'modules' array", file);
        return std::nullopt;
    }
    const std::filesystem::path map_dir =
        std::filesystem::absolute(std::filesystem::path(file)).parent_path();
    // deps 输出里的路径相对**项目根**（不是 map 文件目录）。为了让「map 放在
    // 项目根」与「map 放在 cwd 而项目根是子目录」两种用法都能直接喂回来，
    // 相对路径先按 map 目录解析，找不到再按 map 目录 + root 字段解析。
    std::string root_field;
    if (const JVal* r = root.get("root"); r != nullptr && r->kind == JVal::Kind::Str)
        root_field = r->str;
    ModuleMap out;
    for (const JVal& m : mods->arr) {
        if (m.kind != JVal::Kind::Obj) {
            err = std::format("module map '{}': 'modules' entries must be objects", file);
            return std::nullopt;
        }
        const JVal* name = m.get("name");
        if (name == nullptr || name->kind != JVal::Kind::Str || name->str.empty()) {
            err = std::format("module map '{}': entry without 'name'", file);
            return std::nullopt;
        }
        // 入口模块不是被 import 的对象，跳过。
        if (const JVal* e = m.get("entry"); e != nullptr && e->kind == JVal::Kind::Bool && e->b)
            continue;
        if (name->str == "__main__") continue;
        // stdlib 模块由编译器提供，不进映射（有映射反而不稳定：路径是编译期布局）。
        const JVal* source = m.get("source");
        if (source != nullptr && source->kind == JVal::Kind::Str && source->str == "stdlib")
            continue;
        const JVal* path = m.get("path");
        if (path == nullptr || path->kind != JVal::Kind::Str || path->str.empty()) {
            err = std::format("module map '{}': module '{}' has no 'path'", file, name->str);
            return std::nullopt;
        }
        std::filesystem::path p(path->str);
        std::error_code ec;
        std::vector<std::filesystem::path> candidates;
        if (p.is_absolute()) {
            candidates.push_back(p);
        } else {
            candidates.push_back(map_dir / p);
            if (!root_field.empty()) candidates.push_back(map_dir / root_field / p);
        }
        std::filesystem::path abs;
        bool found = false;
        for (const auto& cand : candidates) {
            std::filesystem::path c = std::filesystem::weakly_canonical(cand, ec);
            if (ec) c = cand.lexically_normal();
            if (std::filesystem::is_regular_file(c, ec)) {
                abs = c;
                found = true;
                break;
            }
        }
        if (!found) {
            err = std::format("module map '{}': module '{}' points to missing file '{}'",
                              file, name->str, (map_dir / p).lexically_normal().string());
            return std::nullopt;
        }
        if (auto it = out.paths.find(name->str); it != out.paths.end()) {
            err = std::format("module map '{}': duplicate module '{}'", file, name->str);
            return std::nullopt;
        }
        out.paths.emplace(name->str, abs.string());
    }
    return out;
}

}  // namespace pyadamas
