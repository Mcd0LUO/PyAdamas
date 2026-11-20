// cimport.cpp —— .cpya 声明文件解析
#include "cimport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
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
    // P1 收尾：窄/无符号 C 类型 → 定宽 TyKind（见 ckind_type）。
    if (t == "char" || t == "int8") return CKind::Int8;
    if (t == "short" || t == "int16") return CKind::Int16;
    if (t == "uchar" || t == "uint8") return CKind::UInt8;
    if (t == "ushort" || t == "uint16") return CKind::UInt16;
    if (t == "ulong" || t == "uint64") return CKind::UInt64;
    if (t == "float") return CKind::Float;
    if (t == "float32") return CKind::Float32;
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

namespace {

struct CTypeRef {
    CTypeRef() = default;
    CTypeRef(CKind k, CKind e, std::string s, bool o)
        : kind(k), elem(e), stname(std::move(s)), opaque(o) {}
    CKind kind = CKind::None;
    CKind elem = CKind::None;   // ptr[T] 的 T（标量时）
    std::string stname;         // ptr[Struct] 的结构体名
    bool opaque = false;        // ptr[Struct]：声明里没有字段
    // kind == Callback：fn(...) -> ret 的签名。
    std::vector<CKind> cb_params;
    CKind cb_ret = CKind::None;
};

// 回调签名只允许能在 C 与 PyAdamas 之间按机器字直接传递的类型。
bool callback_kind_ok(CKind k) {
    switch (k) {
        case CKind::Int:
        case CKind::Int32:
        case CKind::UInt32:
        case CKind::Int8:
        case CKind::Int16:
        case CKind::UInt8:
        case CKind::UInt16:
        case CKind::UInt64:
        case CKind::Float:
        case CKind::Bool:
        case CKind::Ptr: return true;
        default: return false;
    }
}

// 解析类型，允许 ptr[Struct] 和按值的 Struct 引用本文件声明的 struct。
std::optional<CTypeRef> parse_ctype(
    const std::string& text, const std::unordered_map<std::string, const CStruct*>& structs) {
    std::string t = trim(text);
    // fn(T1, T2, ...) -> R：C 回调参数。
    if (t.starts_with("fn(")) {
        size_t rp = t.find(')');
        if (rp == std::string::npos) return std::nullopt;
        CTypeRef r;
        r.kind = CKind::Callback;
        std::string inside = trim(t.substr(3, rp - 3));
        std::string tail = trim(t.substr(rp + 1));
        if (!tail.empty()) {
            if (!tail.starts_with("->")) return std::nullopt;
            auto rk = parse_ckind(trim(tail.substr(2)));
            if (!rk || !callback_kind_ok(*rk)) return std::nullopt;
            r.cb_ret = *rk;
        }
        if (!inside.empty()) {
            for (auto& part : split_commas(inside)) {
                // 回调参数允许标量和 ptr[T]；ptr[T] 在调用边界仍按地址传。
                auto ck = parse_ctype(part, structs);
                if (!ck || ck->kind == CKind::Callback) return std::nullopt;
                if (ck->kind == CKind::Ptr) {
                    r.cb_params.push_back(CKind::Ptr);
                    continue;
                }
                if (!callback_kind_ok(ck->kind)) return std::nullopt;
                r.cb_params.push_back(ck->kind);
            }
        }
        int ni = 0, nf = 0;
        for (CKind k : r.cb_params) {
            if (k == CKind::Float) ++nf;
            else ++ni;
        }
        if (ni > 6 || nf > 8) return std::nullopt;  // 回调参数不转发栈参数
        return r;
    }
    if (t.starts_with("ptr[") && t.back() == ']') {
        std::string inner = trim(t.substr(4, t.size() - 5));
        if (inner.empty()) return std::nullopt;
        if (auto it = structs.find(inner); it != structs.end())
            return CTypeRef{CKind::Ptr, CKind::None, inner, it->second->fields.empty()};
        if (auto k = parse_ckind(inner)) return CTypeRef{CKind::Ptr, *k, {}, false};
        return std::nullopt;
    }
    // 裸结构体名：只有带字段的 struct 能按值传递。
    if (auto it = structs.find(t); it != structs.end())
        return CTypeRef{CKind::Struct, CKind::None, t, it->second->fields.empty()};
    if (auto k = parse_ckind(t)) return CTypeRef{*k, CKind::None, {}, false};
    return std::nullopt;
}

// C 结构体字段的机器宽度。布局规则与 C 一致：offset 按字段对齐向上取整，
// 整体大小按最大对齐补齐。C 结构体只接受定宽标量，避免依赖 sizeof 探针。
bool cfield_layout(CKind k, int64_t& size, int64_t& align) {
    switch (k) {
        case CKind::Int: size = 8; align = 8; return true;      // long
        case CKind::Int8: size = 1; align = 1; return true;     // char
        case CKind::UInt8: size = 1; align = 1; return true;    // unsigned char
        case CKind::Int16: size = 2; align = 2; return true;    // short
        case CKind::UInt16: size = 2; align = 2; return true;   // unsigned short
        case CKind::UInt64: size = 8; align = 8; return true;   // unsigned long
        case CKind::Int32: size = 4; align = 4; return true;    // int
        case CKind::UInt32: size = 4; align = 4; return true;   // unsigned
        case CKind::Float: size = 8; align = 8; return true;    // double
        case CKind::Bool: size = 1; align = 1; return true;     // _Bool
        case CKind::Ptr: size = 8; align = 8; return true;      // void* / T*
        default: return false;
    }
}

int64_t round_up(int64_t v, int64_t a) { return (v + a - 1) / a * a; }

}  // namespace

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

    // 先收集所有 struct 名，允许 ptr[Struct] 出现在声明之前。
    std::vector<std::string> lines;
    {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    // 预扫描 struct：`struct NAME` 是不透明句柄，`struct NAME:` 后面跟缩进
    // 的字段声明，按 C 规则算好布局。字段行在正式解析时跳过。
    std::unordered_map<std::string, const CStruct*> structs;
    std::unordered_map<std::string, size_t> struct_pos;
    std::unordered_set<size_t> field_lines;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string text = trim(strip_comment(lines[i]));
        if (!text.starts_with("struct ")) continue;
        std::string sname = trim(text.substr(7));
        bool has_fields = !sname.empty() && sname.back() == ':';
        if (has_fields) sname.pop_back();
        sname = trim(sname);
        if (!is_ident(sname))
            return fail(static_cast<int>(i) + 1, std::format("invalid struct name '{}'", sname));
        if (structs.count(sname))
            return fail(static_cast<int>(i) + 1, std::format("duplicate struct '{}'", sname));

        CStruct cs;
        cs.name = sname;
        if (has_fields) {
            size_t j = i + 1;
            std::unordered_set<std::string> seen_fields;
            while (j < lines.size()) {
                std::string raw = strip_comment(lines[j]);
                std::string ft = trim(raw);
                if (ft.empty()) {
                    ++j;
                    continue;
                }
                bool indented = raw[0] == ' ' || raw[0] == '\t';
                if (!indented) break;
                size_t colon = ft.find(':');
                if (colon == std::string::npos)
                    return fail(static_cast<int>(j) + 1,
                                std::format("expected 'field: type' in struct '{}'", sname));
                std::string fname = trim(ft.substr(0, colon));
                std::string ftype = trim(ft.substr(colon + 1));
                if (!is_ident(fname))
                    return fail(static_cast<int>(j) + 1,
                                std::format("invalid field name '{}'", fname));
                if (!seen_fields.insert(fname).second)
                    return fail(static_cast<int>(j) + 1,
                                std::format("duplicate field '{}' in struct '{}'", fname, sname));
                auto fk = parse_ckind(ftype);
                int64_t fsize = 0, falign = 1;
                if (!fk || !cfield_layout(*fk, fsize, falign))
                    return fail(static_cast<int>(j) + 1,
                                std::format("unsupported struct field type '{}' (use int, "
                                            "int32, uint32, float, bool or ptr)", ftype));
                CStructField cf;
                cf.name = fname;
                cf.kind = *fk;
                cf.size = fsize;
                cf.align = falign;
                cs.fields.push_back(std::move(cf));
                field_lines.insert(j);
                ++j;
            }
            if (cs.fields.empty())
                return fail(static_cast<int>(i) + 1,
                            std::format("struct '{}' needs at least one field", sname));
            int64_t off = 0, align = 1;
            for (auto& f : cs.fields) {
                off = round_up(off, f.align);
                f.offset = off;
                off += f.size;
                if (f.align > align) align = f.align;
            }
            cs.align = align;
            cs.size = round_up(off, align);
        }
        decl.structs.push_back(std::move(cs));
        struct_pos[sname] = decl.structs.size() - 1;
    }
    // decl.structs 可能扩容，上面的裸指针在 push_back 后失效，重新绑定。
    structs.clear();
    for (auto& kv : struct_pos) structs[kv.first] = &decl.structs[kv.second];

    for (size_t li = 0; li < lines.size(); ++li) {
        if (field_lines.count(li)) continue;  // struct 的字段行
        int lineno = static_cast<int>(li) + 1;
        std::string text = trim(strip_comment(lines[li]));
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

        if (text.starts_with("struct ")) continue;  // 预扫描已校验

        if (text.starts_with("link")) {
            std::string rest = trim(text.substr(4));
            if (rest.empty()) return fail(lineno, "'link' requires an argument");
            std::string arg = normalize_link(rest);
            // 引号给的 .c 源文件（link "glue.c"） historically 按 cwd 解析；
            // stdlib 分发的 .cpya 不能依赖用户 cwd：cwd 下找不到时回退到
            // .cpya 所在目录（转绝对路径），两种口径都保住。
            if (!arg.empty() && arg[0] != '-' && arg.ends_with(".c")) {
                std::error_code ec;
                if (!fs::exists(arg, ec)) {
                    fs::path cand = found.parent_path() / arg;
                    if (fs::exists(cand, ec)) arg = fs::absolute(cand).string();
                }
            }
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
                case CKind::UInt32:
                case CKind::Int8:
                case CKind::Int16:
                case CKind::UInt8:
                case CKind::UInt16: {
                    try {
                        size_t used = 0;
                        c.i = std::stoll(val_text, &used, 0);
                        if (used != val_text.size()) throw std::invalid_argument("trailing");
                    } catch (...) {
                        return fail(lineno, std::format("invalid integer literal '{}'", val_text));
                    }
                    break;
                }
                case CKind::UInt64: {
                    // u64 常量按无符号解析（>INT64_MAX 存位模式；C 口径
                    // 允许 -1 这类负写法，strtoull 自动回绕）。
                    try {
                        size_t used = 0;
                        unsigned long long uv = std::stoull(val_text, &used, 0);
                        if (used != val_text.size()) throw std::invalid_argument("trailing");
                        c.i = static_cast<int64_t>(uv);
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
            // extern var NAME: type —— 读取 C 全局变量。
            if (rest.starts_with("var ")) {
                std::string dtext = trim(rest.substr(4));
                size_t colon = dtext.find(':');
                if (colon == std::string::npos)
                    return fail(lineno, "expected 'extern var NAME: type'");
                std::string head = trim(dtext.substr(0, colon));
                std::string ty_text = trim(dtext.substr(colon + 1));
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
                if (!is_ident(py_name))
                    return fail(lineno, std::format("invalid name '{}'", py_name));
                auto ct = parse_ctype(ty_text, structs);
                if (!ct) return fail(lineno, std::format("unknown variable type '{}'", ty_text));
                if (ct->kind == CKind::Callback)
                    return fail(lineno, "callback type is not supported for extern var");
                if (ct->kind == CKind::Struct)
                    return fail(lineno,
                                std::format("struct variable '{}' must be declared with ptr[{}]",
                                            py_name, ct->stname));
                if (seen_py.count(py_name))
                    return fail(lineno, std::format("duplicate name '{}'", py_name));
                seen_py.insert(py_name);
                CVar cv;
                cv.symbol = symbol;
                cv.py_name = py_name;
                cv.kind = ct->kind;
                cv.struct_name = ct->stname;
                cv.elem = ct->elem;
                decl.vars.push_back(std::move(cv));
                continue;
            }
            if (!rest.starts_with("def "))
                return fail(lineno,
                            "expected 'extern def <symbol>(...)' or 'extern var NAME: type'");
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
            auto param_parts = split_commas(params_text);
            if (!param_parts.empty() && param_parts.back() == "...") {
                ex.varargs = true;
                param_parts.pop_back();
            }
            for (auto& p : param_parts) {
                size_t colon = p.find(':');
                if (colon == std::string::npos)
                    return fail(lineno, std::format("parameter '{}' needs a type", p));
                std::string pname = trim(p.substr(0, colon));
                std::string ptype = trim(p.substr(colon + 1));
                if (!is_ident(pname))
                    return fail(lineno, std::format("invalid parameter name '{}'", pname));
                CParam cp;
                cp.name = pname;
                auto ct = parse_ctype(ptype, structs);
                if (!ct) {
                    if (ptype.starts_with("ptr[") && ptype.back() == ']') {
                        std::string inner = trim(ptype.substr(4, ptype.size() - 5));
                        return fail(lineno,
                                    std::format("unknown pointer element type '{}'", inner));
                    }
                    return fail(lineno, std::format("unknown parameter type '{}'", ptype));
                }
                cp.kind = ct->kind;
                cp.ptr_elem = ct->elem;
                cp.ptr_struct = ct->stname;
                cp.opaque = ct->opaque;
                cp.cb_params = ct->cb_params;
                cp.cb_ret = ct->cb_ret;
                if (cp.kind == CKind::Struct && cp.opaque)
                    return fail(lineno,
                                std::format("struct '{}' is opaque and cannot be passed by value",
                                            cp.ptr_struct));
                ex.params.push_back(std::move(cp));
            }
            auto rk = parse_ctype(ret_text, structs);
            if (!rk) return fail(lineno, std::format("unknown return type '{}'", ret_text));
            if (rk->kind == CKind::Callback)
                return fail(lineno, "returning a C function pointer is not supported");
            if (rk->kind == CKind::Struct && rk->opaque)
                return fail(lineno,
                            std::format("struct '{}' is opaque and cannot be returned by value",
                                        rk->stname));
            ex.ret = rk->kind;
            ex.ret_struct = rk->stname;
            ex.ret_elem = rk->elem;

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
