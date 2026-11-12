// cimport.cpp —— .cpya 声明文件解析
#include "cimport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pyadamas {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去掉 # 注释，但引号内的 # 保留。
std::string strip_comment(const std::string& s) {
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '#') return s.substr(0, i);
    }
    return s;
}

bool is_ident(const std::string& s) {
    if (s.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c0) || c0 == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

std::string unquote(const std::string& s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 2 < s.size()) {
            char n = s[++i];
            switch (n) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(n); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 按顶层逗号切分（类型里的 [] 不产生逗号，这里仍做括号保护）。
std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    bool in_str = false;
    std::string cur;
    for (char c : s) {
        if (in_str) {
            cur.push_back(c);
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; cur.push_back(c); continue; }
        if (c == '[' || c == '(' || c == '{') ++depth;
        else if (c == ']' || c == ')' || c == '}') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

std::string normalize_link(const std::string& raw) {
    std::string v = trim(raw);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return unquote(v);
    return "-l" + v;
}

}  // namespace

std::optional<CKind> parse_ckind(const std::string& text) {
    std::string t = trim(text);
    if (t == "None") return CKind::None;
    if (t == "int") return CKind::Int;
    if (t == "int32") return CKind::Int32;
    if (t == "uint32") return CKind::UInt32;
    if (t == "float") return CKind::Float;
    if (t == "bool") return CKind::Bool;
    if (t == "str") return CKind::Str;
    if (t == "ptr") return CKind::Ptr;
    if (t == "Optional[str]") return CKind::OptStr;
    if (t.starts_with("ptr[") && t.back() == ']') {
        std::string inner = trim(t.substr(4, t.size() - 5));
        if (!inner.empty() && parse_ckind(inner)) return CKind::Ptr;
    }
    return std::nullopt;
}

std::optional<CImportDecl> load_cpya(const std::string& name, const std::string& source_dir,
                                    const std::vector<std::string>& include_dirs,
                                    std::string& err) {
    namespace fs = std::filesystem;
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    rel += ".cpya";

    fs::path found;
    std::vector<fs::path> roots;
    roots.push_back(source_dir.empty() ? fs::path(".") : fs::path(source_dir));
    for (auto& d : include_dirs) roots.push_back(d);
    roots.push_back(fs::current_path());
    for (auto& r : roots) {
        fs::path p = r / rel;
        if (fs::exists(p) && fs::is_regular_file(p)) {
            found = p;
            break;
        }
    }
    if (found.empty()) {
        err = std::format("cannot find C declaration file '{}' for cimport '{}'", rel, name);
        return std::nullopt;
    }

    std::ifstream in(found);
    if (!in) {
        err = std::format("cannot open C declaration file '{}'", found.string());
        return std::nullopt;
    }

    CImportDecl decl;
    decl.name = name;
    bool saw_cmodule = false;
    bool saw_any = false;
    std::unordered_set<std::string> seen_py;
    std::unordered_set<std::string> seen_links;

    auto fail = [&](int line, const std::string& msg) -> std::optional<CImportDecl> {
        err = std::format("{}:{}: {}", found.string(), line, msg);
        return std::nullopt;
    };

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string text = trim(strip_comment(line));
        if (text.empty()) continue;

        if (!saw_any) {
            saw_any = true;
            if (!text.starts_with("cmodule")) {
                return fail(lineno, "the first declaration must be 'cmodule <name>'");
            }
        }

        if (text.starts_with("cmodule")) {
            std::string rest = trim(text.substr(7));
            if (saw_cmodule) return fail(lineno, "duplicate 'cmodule'");
            if (rest != name)
                return fail(lineno, std::format("cmodule '{}' does not match cimport '{}'", rest,
                                                name));
            saw_cmodule = true;
            continue;
        }
        if (!saw_cmodule) return fail(lineno, "'cmodule' must appear before other declarations");

        if (text.starts_with("link")) {
            std::string rest = trim(text.substr(4));
            if (rest.empty()) return fail(lineno, "'link' requires an argument");
            std::string arg = normalize_link(rest);
            if (seen_links.insert(arg).second) decl.links.push_back(arg);
            continue;
        }

        if (text.starts_with("const")) {
            std::string rest = trim(text.substr(5));
            size_t colon = rest.find(':');
            if (colon == std::string::npos)
                return fail(lineno, "expected 'const NAME: type = value'");
            std::string cname = trim(rest.substr(0, colon));
            if (!is_ident(cname)) return fail(lineno, "invalid const name");
            std::string tail = trim(rest.substr(colon + 1));
            size_t eq = tail.find('=');
            if (eq == std::string::npos)
                return fail(lineno, "expected '=' in const declaration");
            std::string ty_text = trim(tail.substr(0, eq));
            std::string val_text = trim(tail.substr(eq + 1));
            auto kind = parse_ckind(ty_text);
            if (!kind) return fail(lineno, std::format("unknown type '{}'", ty_text));
            CConst c;
            c.name = cname;
            c.kind = *kind;
            switch (*kind) {
                case CKind::Int:
                case CKind::Int32:
                case CKind::UInt32: {
                    try {
                        size_t used = 0;
                        c.i = std::stoll(val_text, &used, 0);
                        if (used != val_text.size()) throw std::invalid_argument("trailing");
                    } catch (...) {
                        return fail(lineno, std::format("invalid integer literal '{}'", val_text));
                    }
                    break;
                }
                case CKind::Float: {
                    try {
                        size_t used = 0;
                        c.f = std::stod(val_text, &used);
                        if (used != val_text.size()) throw std::invalid_argument("trailing");
                    } catch (...) {
                        return fail(lineno, std::format("invalid float literal '{}'", val_text));
                    }
                    break;
                }
                case CKind::Bool:
                    if (val_text == "True") c.b = true;
                    else if (val_text == "False") c.b = false;
                    else return fail(lineno, "bool literal must be True or False");
                    break;
                case CKind::Str:
                    if (val_text.size() < 2 || val_text.front() != '"' || val_text.back() != '"')
                        return fail(lineno, "str literal must be quoted");
                    c.s = unquote(val_text);
                    break;
                default:
                    return fail(lineno, "const only supports int/int32/uint32/float/bool/str");
            }
            if (seen_py.count(cname)) return fail(lineno, std::format("duplicate name '{}'", cname));
            seen_py.insert(cname);
            decl.consts.push_back(std::move(c));
            continue;
        }

        if (text.starts_with("extern")) {
            std::string rest = trim(text.substr(6));
            if (!rest.starts_with("def "))
                return fail(lineno, "expected 'extern def <symbol>(...)'");
            rest = trim(rest.substr(4));
            size_t lp = rest.find('(');
            size_t rp = rest.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp < lp)
                return fail(lineno, "expected '(' and ')' in extern declaration");
            std::string head = trim(rest.substr(0, lp));
            std::string params_text = rest.substr(lp + 1, rp - lp - 1);
            std::string ret_text = trim(rest.substr(rp + 1));
            if (ret_text.starts_with("->")) ret_text = trim(ret_text.substr(2));
            else if (!ret_text.empty()) return fail(lineno, "expected '->' before return type");
            if (ret_text.empty()) ret_text = "None";

            std::string symbol_part = head;
            std::string py_part;
            size_t aspos = head.rfind(" as ");
            if (aspos != std::string::npos) {
                symbol_part = trim(head.substr(0, aspos));
                py_part = trim(head.substr(aspos + 4));
            }
            std::string symbol = unquote(symbol_part);
            std::string py_name = py_part.empty() ? symbol : py_part;
            if (symbol.empty()) return fail(lineno, "missing C symbol name");
            if (!is_ident(py_name)) return fail(lineno, std::format("invalid name '{}'", py_name));

            CExtern ex;
            ex.symbol = symbol;
            ex.py_name = py_name;
            for (auto& p : split_commas(params_text)) {
                size_t colon = p.find(':');
                if (colon == std::string::npos)
                    return fail(lineno, std::format("parameter '{}' needs a type", p));
                std::string pname = trim(p.substr(0, colon));
                std::string ptype = trim(p.substr(colon + 1));
                if (!is_ident(pname))
                    return fail(lineno, std::format("invalid parameter name '{}'", pname));
                CParam cp;
                cp.name = pname;
                if (ptype == "ptr") {
                    cp.kind = CKind::Ptr;
                } else if (ptype.starts_with("ptr[") && ptype.back() == ']') {
                    std::string inner = trim(ptype.substr(4, ptype.size() - 5));
                    auto elem = parse_ckind(inner);
                    if (!elem || *elem == CKind::None)
                        return fail(lineno,
                                    std::format("unknown pointer element type '{}'", inner));
                    cp.kind = CKind::Ptr;
                    cp.ptr_elem = *elem;
                } else {
                    auto k = parse_ckind(ptype);
                    if (!k)
                        return fail(lineno, std::format("unknown parameter type '{}'", ptype));
                    cp.kind = *k;
                }
                ex.params.push_back(std::move(cp));
            }
            auto rk = parse_ckind(ret_text);
            if (!rk) return fail(lineno, std::format("unknown return type '{}'", ret_text));
            ex.ret = *rk;

            if (seen_py.count(py_name))
                return fail(lineno, std::format("duplicate name '{}'", py_name));
            seen_py.insert(py_name);
            decl.externs.push_back(std::move(ex));
            continue;
        }

        return fail(lineno, std::format("unknown declaration '{}'", text));
    }

    if (!saw_cmodule) {
        err = std::format("{}: missing 'cmodule {}'", found.string(), name);
        return std::nullopt;
    }
    return decl;
}

}  // namespace pyadamas
