// cdecl_parse.cpp —— C 头文件解析：类型、struct/enum 与声明输出
#include "cdecl.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cdecl_internal.h"

namespace pyadamas {
namespace {

// ── 类型 ───────────────────────────────────────────────────────
struct CType {
    enum K {
        Void, Bool, I8, U8, I16, U16, I32, U32, I64, U64, Double, Float,
        Ptr, Struct, Union, Func, Unknown
    } k = Unknown;
    std::string name;               // Struct/Union
    std::shared_ptr<CType> inner;   // Ptr
    std::vector<CType> ps;          // Func
    std::shared_ptr<CType> ret;     // Func
    bool varargs = false;
    bool is_const = false;
};

CType make_ptr(CType inner) {
    CType t;
    t.k = CType::Ptr;
    t.inner = std::make_shared<CType>(std::move(inner));
    return t;
}

bool is_qualifier(const std::string& s) {
    return s == "const" || s == "volatile" || s == "restrict" || s == "__restrict" ||
           s == "static" || s == "extern" || s == "register" || s == "inline" ||
           s == "__inline" || s == "__inline__" || s == "_Noreturn";
}

bool is_builtin_type_name(const std::string& s) {
    static const std::set<std::string> names = {
        "void", "bool", "_Bool", "char", "short", "int", "long", "double", "float",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
        "int64_t", "uint64_t", "intptr_t", "uintptr_t", "size_t", "ssize_t",
        "ptrdiff_t",
    };
    return names.count(s) > 0;
}

// 解析 base type（不含声明符名字）。返回消费到的位置。
size_t parse_base_type(const std::vector<Tok>& v, size_t i, const std::map<std::string, CType>& alias,
                       CType& out) {
    bool is_const = false;
    bool is_unsigned = false;
    while (i < v.size() && v[i].k == Tok::Id) {
        const std::string& s = v[i].s;
        if (is_qualifier(s)) {
            if (s == "const") is_const = true;
            ++i;
            continue;
        }
        if (s == "signed") {
            ++i;
            continue;
        }
        if (s == "unsigned") {
            is_unsigned = true;
            ++i;
            continue;
        }
        break;
    }
    if (i >= v.size() || v[i].k != Tok::Id) {
        // `unsigned` / `signed` 单独出现等价于 unsigned int / int。
        if (is_unsigned) out.k = CType::U32;
        else if (is_const) out.k = CType::Unknown;
        out.is_const = is_const;
        return i;
    }
    std::string base = v[i].s;
    bool has_base = is_builtin_type_name(base) || base == "struct" || base == "union" ||
                    base == "enum" || alias.count(base) > 0;
    if (!has_base) {
        if (is_unsigned) {
            out.k = CType::U32;
            out.is_const = is_const;
            return i;  // 不吞掉声明符名字
        }
        out.k = CType::Unknown;
        out.is_const = is_const;
        return i;
    }
    ++i;

    auto scalar = [&](CType::K sk) {
        out.k = sk;
        out.is_const = is_const;
    };
    if (base == "void") scalar(CType::Void);
    else if (base == "bool" || base == "_Bool") scalar(CType::Bool);
    else if (base == "char") scalar(is_unsigned ? CType::U8 : CType::I8);
    else if (base == "short") scalar(is_unsigned ? CType::U16 : CType::I16);
    else if (base == "int") scalar(is_unsigned ? CType::U32 : CType::I32);
    else if (base == "long") {
        if (i < v.size() && v[i].k == Tok::Id && v[i].s == "long") ++i;
        scalar(is_unsigned ? CType::U64 : CType::I64);
    } else if (base == "double") scalar(CType::Double);
    else if (base == "float") scalar(CType::Float);
    else if (base == "int8_t") scalar(CType::I8);
    else if (base == "uint8_t") scalar(CType::U8);
    else if (base == "int16_t") scalar(CType::I16);
    else if (base == "uint16_t") scalar(CType::U16);
    else if (base == "int32_t") scalar(CType::I32);
    else if (base == "uint32_t") scalar(CType::U32);
    else if (base == "int64_t" || base == "intptr_t" || base == "ptrdiff_t" ||
             base == "ssize_t")
        scalar(CType::I64);
    else if (base == "uint64_t" || base == "size_t" || base == "uintptr_t")
        scalar(CType::I64);  // .cpya 只有有符号 64 位，宽度一致
    else if (base == "struct" || base == "union") {
        out.k = base == "struct" ? CType::Struct : CType::Union;
        if (i < v.size() && v[i].k == Tok::Id) out.name = v[i++].s;
    } else if (base == "enum") {
        if (i < v.size() && v[i].k == Tok::Id) ++i;
        scalar(CType::I32);
    } else {
        auto it = alias.find(base);
        if (it != alias.end()) out = it->second;
        else out.k = CType::Unknown;
    }
    return i;
}

// 把紧随其后的 '*' 逐个包成指针；const 修饰符跳过。
size_t consume_stars(const std::vector<Tok>& v, size_t i, CType& t) {
    while (i < v.size() && v[i].k == Tok::Punc && v[i].s == "*") {
        t = make_ptr(t);
        ++i;
        while (i < v.size() && v[i].k == Tok::Id &&
               (v[i].s == "const" || v[i].s == "volatile" || v[i].s == "restrict"))
            ++i;
    }
    return i;
}

const char* kInt32 = "int32";
const char* kUInt32 = "uint32";

// 函数参数/返回值类型 → .cpya。失败时 why 说明原因。
std::optional<std::string> cpya_type(const CType& t, bool param, std::string& why) {
    switch (t.k) {
        case CType::Void:
            if (param) {
                why = "void parameter";
                return std::nullopt;
            }
            return std::string("None");
        case CType::Bool: return std::string("bool");
        case CType::I8:
        case CType::I16:
        case CType::I32: return std::string(kInt32);
        case CType::U8:
        case CType::U16:
        case CType::U32: return std::string(kUInt32);
        case CType::U64:
        case CType::I64: return std::string("int");
        case CType::Double: return std::string("float");
        case CType::Float: return std::string("float32");
        case CType::Struct: return t.name;
        case CType::Union:
            why = "union by value is not supported";
            return std::nullopt;
        case CType::Ptr: {
            if (!t.inner) return std::string("ptr");
            const CType& in = *t.inner;
            if (in.k == CType::Void) return std::string("ptr");
            if (in.k == CType::Struct) return std::format("ptr[{}]", in.name);
            if (in.k == CType::Union) return std::string("ptr");
            if (in.k == CType::Func) return std::string("ptr");
            if (in.k == CType::I8 && in.is_const) return std::string("str");
            if (in.k == CType::I8 || in.k == CType::U8) return std::string("ptr");
            if (in.k == CType::I16 || in.k == CType::U16) return std::string("ptr");
            if (in.k == CType::Double) return std::string("ptr[float]");
            if (in.k == CType::Bool) return std::string("ptr[bool]");
            if (in.k == CType::I32) return std::string("ptr[int32]");
            if (in.k == CType::U32) return std::string("ptr[uint32]");
            if (in.k == CType::I64 || in.k == CType::U64) return std::string("ptr[int]");
            return std::string("ptr");
        }
        case CType::Func: {
            // 回调跳板暂不做单精度提升：含 C float 的回调签名整体跳过，
            // 避免生成 .cpya 无法解析的 fn(float32, ...)。
            for (const auto& p : t.ps)
                if (p.k == CType::Float) {
                    why = "C float in callback signature";
                    return std::nullopt;
                }
            if (t.ret && t.ret->k == CType::Float) {
                why = "C float callback return";
                return std::nullopt;
            }
            // C 回调 → Python 的 Callable[[params], ret]（.cpya 注解里没有 -> 语法）。
            std::string s = "Callable[[";
            for (size_t i = 0; i < t.ps.size(); ++i) {
                std::string why2;
                auto p = cpya_type(t.ps[i], true, why2);
                if (!p) {
                    why = "callback parameter: " + why2;
                    return std::nullopt;
                }
                if (i) s += ", ";
                s += *p;
            }
            if (t.varargs) {
                why = "varargs callback";
                return std::nullopt;
            }
            s += "], ";
            if (t.ret && t.ret->k != CType::Void) {
                std::string why2;
                auto r = cpya_type(*t.ret, false, why2);
                if (!r) {
                    why = "callback return: " + why2;
                    return std::nullopt;
                }
                s += *r;
            } else {
                s += "None";
            }
            s += "]";
            return s;
        }
        default:
            why = "unknown type";
            return std::nullopt;
    }
}

// struct 字段：只允许布局与 .cpya 完全一致的标量/指针。
std::optional<std::string> cpya_field_type(const CType& t, std::string& why) {
    switch (t.k) {
        case CType::Bool: return std::string("bool");
        case CType::I32: return std::string(kInt32);
        case CType::U32: return std::string(kUInt32);
        case CType::I64:
        case CType::U64: return std::string("int");
        case CType::Double: return std::string("float");
        case CType::Ptr: return std::string("ptr");
        default:
            why = "field type is not a fixed-width scalar/pointer";
            return std::nullopt;
    }
}

// ── 常量表达式求值（枚举 / #define）─────────────────────────────
struct Eval {
    const std::vector<Tok>& t;
    size_t i = 0;
    size_t end = 0;
    const std::map<std::string, int64_t>& env;
    bool ok = true;

    int64_t number(const std::string& raw) {
        std::string s = raw;
        while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' ||
                              s.back() == 'L'))
            s.pop_back();
        if (s.empty()) {
            ok = false;
            return 0;
        }
        char* endp = nullptr;
        long long v = std::strtoll(s.c_str(), &endp, 0);
        if (!endp || *endp != '\0') {
            ok = false;
            return 0;
        }
        return static_cast<int64_t>(v);
    }

    int64_t primary() {
        if (i >= end) {
            ok = false;
            return 0;
        }
        const Tok& tok = t[i];
        if (tok.k == Tok::Num) {
            ++i;
            return number(tok.s);
        }
        if (tok.k == Tok::Id) {
            ++i;
            auto it = env.find(tok.s);
            if (it == env.end()) {
                ok = false;
                return 0;
            }
            return it->second;
        }
        if (tok.k == Tok::Punc && tok.s == "(") {
            ++i;
            int64_t v = expr();
            if (i < end && t[i].k == Tok::Punc && t[i].s == ")") ++i;
            else ok = false;
            return v;
        }
        ok = false;
        return 0;
    }

    int64_t unary() {
        if (i < end && t[i].k == Tok::Punc) {
            const std::string& op = t[i].s;
            if (op == "-") {
                ++i;
                return -unary();
            }
            if (op == "+") {
                ++i;
                return unary();
            }
            if (op == "~") {
                ++i;
                return ~unary();
            }
            if (op == "!") {
                ++i;
                return unary() == 0 ? 1 : 0;
            }
        }
        return primary();
    }

    int64_t mul() {
        int64_t v = unary();
        while (i < end && t[i].k == Tok::Punc &&
               (t[i].s == "*" || t[i].s == "/" || t[i].s == "%")) {
            std::string op = t[i++].s;
            int64_t r = unary();
            if (op == "*") v *= r;
            else if (r == 0) {
                ok = false;
                return 0;
            } else if (op == "/") v /= r;
            else v %= r;
        }
        return v;
    }

    int64_t add() {
        int64_t v = mul();
        while (i < end && t[i].k == Tok::Punc && (t[i].s == "+" || t[i].s == "-")) {
            std::string op = t[i++].s;
            int64_t r = mul();
            v = op == "+" ? v + r : v - r;
        }
        return v;
    }

    int64_t shift() {
        int64_t v = add();
        while (i < end && t[i].k == Tok::Punc && (t[i].s == "<<" || t[i].s == ">>")) {
            std::string op = t[i++].s;
            int64_t r = add();
            if (r < 0 || r > 63) {
                ok = false;
                return 0;
            }
            v = op == "<<" ? (v << r) : (v >> r);
        }
        return v;
    }

    int64_t band() {
        int64_t v = shift();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "&") {
            ++i;
            v &= shift();
        }
        return v;
    }

    int64_t bxor() {
        int64_t v = band();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "^") {
            ++i;
            v ^= band();
        }
        return v;
    }

    int64_t bor() {
        int64_t v = bxor();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "|") {
            ++i;
            v |= bxor();
        }
        return v;
    }

    int64_t expr() { return bor(); }
};

std::optional<int64_t> eval_const(const std::vector<Tok>& t, size_t b, size_t e,
                                  const std::map<std::string, int64_t>& env) {
    Eval ev{t, b, e, env};
    int64_t v = ev.expr();
    if (!ev.ok || ev.i != e) return std::nullopt;
    return v;
}

// ── 结构体 / 枚举定义 ──────────────────────────────────────────
struct StructDef {
    std::string name;
    std::vector<std::pair<std::string, CType>> fields;
    bool ok = true;
    std::string why;
    int line = 0;
};

std::vector<std::pair<size_t, size_t>> split_top(const std::vector<Tok>& v, size_t b, size_t e,
                                                 const std::string& sep) {
    std::vector<std::pair<size_t, size_t>> out;
    int depth = 0;
    size_t start = b;
    for (size_t i = b; i < e; ++i) {
        const Tok& t = v[i];
        if (t.k != Tok::Punc) continue;
        if (t.s == "(" || t.s == "[" || t.s == "{") ++depth;
        else if (t.s == ")" || t.s == "]" || t.s == "}") --depth;
        else if (t.s == sep && depth == 0) {
            out.emplace_back(start, i);
            start = i + 1;
        }
    }
    if (start < e) out.emplace_back(start, e);
    return out;
}

size_t find_matching(const std::vector<Tok>& v, size_t open, const std::string& o,
                     const std::string& c) {
    int depth = 0;
    for (size_t i = open; i < v.size(); ++i) {
        if (v[i].k != Tok::Punc) continue;
        if (v[i].s == o) ++depth;
        else if (v[i].s == c) {
            if (--depth == 0) return i;
        }
    }
    return v.size();
}

void parse_struct_body(const std::vector<Tok>& v, size_t b, size_t e,
                       const std::map<std::string, CType>& alias, StructDef& sd) {
    for (auto [db, de] : split_top(v, b, e, ";")) {
        if (db >= de) continue;
        // 位域、数组、函数指针、匿名成员：布局不可表示，整体降级为不透明。
        for (size_t i = db; i < de; ++i) {
            if (v[i].k != Tok::Punc) continue;
            if (v[i].s == ":" || v[i].s == "[") {
                sd.ok = false;
                sd.why = "bitfield/array field";
                return;
            }
        }
        size_t i = db;
        CType base;
        size_t after = parse_base_type(v, i, alias, base);
        if (after == i || base.k == CType::Unknown || base.k == CType::Func) {
            sd.ok = false;
            sd.why = "unsupported field type";
            return;
        }
        // 一条声明可能有多个字段：int x, y;
        for (auto [cb, ce] : split_top(v, after, de, ",")) {
            if (cb >= ce) continue;
            CType ft = base;
            size_t j = cb;
            while (j < ce && v[j].k == Tok::Punc && v[j].s == "*") {
                ft = make_ptr(ft);
                ++j;
            }
            // 字段名是最后一个标识符；括号说明是函数指针/嵌套声明。
            std::string fname;
            for (size_t q = j; q < ce; ++q) {
                if (v[q].k == Tok::Punc && (v[q].s == "(" || v[q].s == ")")) {
                    sd.ok = false;
                    sd.why = "function pointer field";
                    return;
                }
                if (v[q].k == Tok::Id) fname = v[q].s;
            }
            if (fname.empty()) {
                sd.ok = false;
                sd.why = "anonymous field";
                return;
            }
            std::string why;
            if (!cpya_field_type(ft, why)) {
                sd.ok = false;
                sd.why = why;
                return;
            }
            sd.fields.emplace_back(fname, ft);
        }
    }
}

// ── 主转换器 ───────────────────────────────────────────────────
struct Cdecl {
    std::vector<std::string> consts;
    std::vector<std::string> externs;
    std::vector<std::string> vars;
    std::vector<std::string> structs;      // 定义好的 struct 块
    std::set<std::string> defined_structs;
    std::set<std::string> referenced_structs;
    std::set<std::string> emitted_names;
    std::map<std::string, CType> alias;
    std::map<std::string, int64_t> enum_env;
    std::map<std::string, int64_t> macro_env;
    int warnings = 0;
    int skipped = 0;

    void warn(const Tok& at, const std::string& msg) {
        ++warnings;
        std::fprintf(stderr, "cdecl: warning: line %d: %s\n", at.line, msg.c_str());
    }

    void emit_struct(const StructDef& sd, const std::string& name) {
        if (name.empty() || defined_structs.count(name)) return;
        defined_structs.insert(name);
        if (!sd.ok) {
            structs.push_back(std::format("# {} (opaque: {})\nclass {}: pass\n", name, sd.why, name));
            return;
        }
        std::string block = std::format("class {}:\n", name);
        for (auto& [fname, ft] : sd.fields) {
            std::string why;
            auto s = cpya_field_type(ft, why);
            block += std::format("    {}: {}\n", fname, *s);
        }
        structs.push_back(std::move(block));
    }

    void emit_const(const std::string& name, const std::string& type, const std::string& value) {
        if (!emitted_names.insert(name).second) return;
        consts.push_back(std::format("{}: {} = {}", name, type, value));
    }

    // 解析 enum { ... } 的 body，enumerate 常量。
    void parse_enum_body(const std::vector<Tok>& v, size_t b, size_t e, const Tok& at) {
        int64_t next = 0;
        for (auto [eb, ee] : split_top(v, b, e, ",")) {
            if (eb >= ee) continue;
            if (v[eb].k != Tok::Id) {
                warn(at, "skipped malformed enum entry");
                ++skipped;
                continue;
            }
            std::string name = v[eb].s;
            int64_t val = next;
            size_t eq = eb + 1;
            if (eq < ee && v[eq].k == Tok::Punc && v[eq].s == "=") {
                auto ev = eval_const(v, eq + 1, ee, enum_env);
                if (!ev) {
                    warn(at, std::format("cannot evaluate enum constant '{}'", name));
                    ++skipped;
                    next = 0;
                    continue;
                }
                val = *ev;
            }
            enum_env[name] = val;
            emit_const(name, "int", std::to_string(val));
            next = val + 1;
        }
    }

    // typedef struct/union/enum ...
    void handle_typedef(const std::vector<Tok>& d) {
        size_t i = 1;
        if (i < d.size() && (d[i].s == "struct" || d[i].s == "union" || d[i].s == "enum")) {
            bool is_union = d[i].s == "union";
            bool is_enum = d[i].s == "enum";
            std::string tag;
            size_t j = i + 1;
            if (j < d.size() && d[j].k == Tok::Id && d[j].s != "{") tag = d[j++].s;
            if (j < d.size() && d[j].k == Tok::Punc && d[j].s == "{") {
                size_t close = find_matching(d, j, "{", "}");
                if (close >= d.size()) {
                    warn(d[0], "unterminated aggregate body");
                    ++skipped;
                    return;
                }
                std::string alias_name;
                for (size_t q = close + 1; q < d.size(); ++q)
                    if (d[q].k == Tok::Id) alias_name = d[q].s;
                std::string name = alias_name.empty() ? tag : alias_name;
                if (is_enum) {
                    parse_enum_body(d, j + 1, close, d[i]);
                    CType t;
                    t.k = CType::I32;
                    if (!alias_name.empty()) alias[alias_name] = t;
                    return;
                }
                if (is_union) {
                    warn(d[i], "union is emitted as opaque struct");
                    ++skipped;
                    if (!name.empty()) emit_struct(StructDef{name, {}, false, "union", d[i].line},
                                                   name);
                    return;
                }
                StructDef sd;
                sd.name = name;
                sd.line = d[i].line;
                parse_struct_body(d, j + 1, close, alias, sd);
                emit_struct(sd, name);
                CType t;
                t.k = CType::Struct;
                t.name = name;
                if (!alias_name.empty()) alias[alias_name] = t;
                if (!tag.empty()) {
                    CType tg = t;
                    tg.name = tag;
                    alias[tag] = tg;
                }
                return;
            }
            // typedef struct Tag Alias;
            std::string alias_name;
            for (size_t q = j; q < d.size(); ++q)
                if (d[q].k == Tok::Id) alias_name = d[q].s;
            CType t;
            t.k = is_enum ? CType::I32 : (is_union ? CType::Union : CType::Struct);
            t.name = tag;
            if (!alias_name.empty() && alias_name != tag) alias[alias_name] = t;
            if (!tag.empty()) referenced_structs.insert(tag);
            return;
        }
        // typedef <type> Alias;
        CType base;
        size_t after = parse_base_type(d, 1, alias, base);
        after = consume_stars(d, after, base);
        std::string name;
        for (size_t q = after; q < d.size(); ++q)
            if (d[q].k == Tok::Id) name = d[q].s;
        if (name.empty() || base.k == CType::Unknown) {
            warn(d[0], "skipped unsupported typedef");
            ++skipped;
            return;
        }
        // typedef 的指针/函数指针目前只记录标量别名。
        alias[name] = base;
    }

    void handle_struct_or_enum(const std::vector<Tok>& d) {
        bool is_enum = d[0].s == "enum";
        bool is_union = d[0].s == "union";
        size_t j = 1;
        std::string tag;
        if (j < d.size() && d[j].k == Tok::Id && d[j].s != "{") tag = d[j++].s;
        if (j >= d.size() || d[j].k != Tok::Punc || d[j].s != "{") {
            if (!tag.empty() && !is_enum) referenced_structs.insert(tag);
            return;
        }
        size_t close = find_matching(d, j, "{", "}");
        if (close >= d.size()) {
            warn(d[0], "unterminated aggregate body");
            ++skipped;
            return;
        }
        if (is_enum) {
            parse_enum_body(d, j + 1, close, d[0]);
            return;
        }
        if (tag.empty()) {
            warn(d[0], "anonymous struct/union skipped");
            ++skipped;
            return;
        }
        if (is_union) {
            warn(d[0], std::format("union '{}' is emitted as opaque struct", tag));
            ++skipped;
            emit_struct(StructDef{tag, {}, false, "union", d[0].line}, tag);
            return;
        }
        StructDef sd;
        sd.name = tag;
        sd.line = d[0].line;
        parse_struct_body(d, j + 1, close, alias, sd);
        emit_struct(sd, tag);
        CType t;
        t.k = CType::Struct;
        t.name = tag;
        alias[tag] = t;
    }

    // 参数表 → CType 列表；返回 false 表示无法表示。
    bool parse_params(const std::vector<Tok>& v, size_t b, size_t e, CType& fn,
                      const Tok& at) {
        if (b >= e) return true;
        if (e - b == 1 && v[b].k == Tok::Id && v[b].s == "void") return true;
        for (auto [pb, pe] : split_top(v, b, e, ",")) {
            if (pb >= pe) continue;
            std::vector<Tok> p(v.begin() + static_cast<long>(pb),
                               v.begin() + static_cast<long>(pe));
            // 可变参数
            if (p.size() == 1 && p[0].k == Tok::Punc && p[0].s == "...") {
                fn.varargs = true;
                continue;
            }
            // 函数指针参数：RET (*name)(params)
            size_t lp = p.size(), rp = 0;
            int depth = 0;
            for (size_t q = 0; q < p.size(); ++q) {
                if (p[q].k != Tok::Punc) continue;
                if (p[q].s == "(") {
                    if (depth == 0 && q + 1 < p.size() && p[q + 1].k == Tok::Punc &&
                        p[q + 1].s == "*") {
                        lp = q;
                    }
                    ++depth;
                } else if (p[q].s == ")") {
                    --depth;
                    if (depth == 0 && lp < q) {
                        rp = q;
                        break;
                    }
                }
            }
            if (lp < p.size() && rp > lp) {
                // name 在 (* name) 里
                std::string name;
                for (size_t q = lp + 2; q < rp; ++q)
                    if (p[q].k == Tok::Id) name = p[q].s;
                if (rp + 1 >= p.size() || p[rp + 1].k != Tok::Punc || p[rp + 1].s != "(") {
                    warn(at, "skipped unsupported function pointer parameter");
                    ++skipped;
                    return false;
                }
                size_t close2 = find_matching(p, rp + 1, "(", ")");
                if (close2 >= p.size()) return false;
                CType cb;
                cb.k = CType::Func;
                CType base;
                parse_base_type(p, 0, alias, base);
                cb.ret = std::make_shared<CType>(base);
                if (!parse_params(p, rp + 2, close2, cb, at)) return false;
                fn.ps.push_back(std::move(cb));
                continue;
            }
            // 普通参数：[const] TYPE [*...] [name] [ [ ] ]
            CType base;
            size_t after = parse_base_type(p, 0, alias, base);
            after = consume_stars(p, after, base);
            if (after == 0 || base.k == CType::Unknown) {
                warn(at, "skipped parameter with unknown type");
                ++skipped;
                return false;
            }
            bool is_array = false;
            for (size_t q = after; q < p.size(); ++q)
                if (p[q].k == Tok::Punc && p[q].s == "[") is_array = true;
            if (is_array && base.k != CType::Ptr) base = make_ptr(base);
            fn.ps.push_back(std::move(base));
        }
        return true;
    }

    void handle_function(const std::vector<Tok>& d, size_t lp, size_t rp) {
        // name 是 '(' 前最后一个标识符
        std::string name;
        size_t ni = 0;
        for (size_t q = 0; q < lp; ++q)
            if (d[q].k == Tok::Id && d[q].s != "extern" && d[q].s != "static" &&
                d[q].s != "inline") {
                name = d[q].s;
                ni = q;
            }
        if (name.empty()) {
            warn(d[0], "skipped function without a name");
            ++skipped;
            return;
        }
        bool is_static = false;
        for (size_t q = 0; q < lp; ++q)
            if (d[q].k == Tok::Id && (d[q].s == "static" || d[q].s == "inline")) is_static = true;
        if (is_static) return;  // 头文件里的 inline/static 函数不可链接
        CType fn;
        fn.k = CType::Func;
        CType ret;
        size_t rpos = parse_base_type(d, 0, alias, ret);
        rpos = consume_stars(d, rpos, ret);
        fn.ret = std::make_shared<CType>(ret);
        if (!parse_params(d, lp + 1, rp, fn, d[ni])) {
            warn(d[0], std::format("skipped function '{}'", name));
            ++skipped;
            return;
        }
        std::string why;
        auto rt = cpya_type(ret, false, why);
        if (!rt) {
            warn(d[0], std::format("skipped '{}': {}", name, why));
            ++skipped;
            return;
        }
        // 结构体按值：记录引用，稍后补不透明声明
        if (ret.k == CType::Struct) referenced_structs.insert(ret.name);
        std::string params;
        for (size_t i = 0; i < fn.ps.size(); ++i) {
            std::string pw;
            auto ps = cpya_type(fn.ps[i], true, pw);
            if (!ps) {
                warn(d[0], std::format("skipped '{}': {}", name, pw));
                ++skipped;
                return;
            }
            if (fn.ps[i].k == CType::Struct) referenced_structs.insert(fn.ps[i].name);
            if (i) params += ", ";
            params += std::format("p{}: {}", i, *ps);
        }
        if (fn.varargs) {
            if (!params.empty()) params += ", ";
            params += "*args: unknown";
        }
        std::string sig = std::format("def {}({}) -> {}: pass", name, params, *rt);
        externs.push_back(std::move(sig));
    }

    void handle_global(const std::vector<Tok>& d) {
        CType base;
        size_t after = parse_base_type(d, 0, alias, base);
        if (after == 0 || base.k == CType::Unknown) {
            warn(d[0], "skipped global with unknown type");
            ++skipped;
            return;
        }
        for (auto [vb, ve] : split_top(d, after, d.size(), ",")) {
            if (vb >= ve) continue;
            CType t = base;
            size_t j = vb;
            while (j < ve && d[j].k == Tok::Punc && d[j].s == "*") {
                t = make_ptr(t);
                ++j;
            }
            bool array = false;
            std::string name;
            for (size_t q = j; q < ve; ++q) {
                if (d[q].k == Tok::Punc && d[q].s == "[") array = true;
                if (d[q].k == Tok::Punc && d[q].s == "=") break;
                if (d[q].k == Tok::Id) name = d[q].s;
            }
            if (name.empty()) continue;
            if (array) {
                warn(d[0], std::format("skipped array variable '{}'", name));
                ++skipped;
                continue;
            }
            std::string why;
            auto ct = cpya_type(t, false, why);
            if (!ct) {
                warn(d[0], std::format("skipped variable '{}': {}", name, why));
                ++skipped;
                continue;
            }
            if (t.k == CType::Struct) referenced_structs.insert(t.name);
            vars.push_back(std::format("{}: {}", name, *ct));
        }
    }

    void handle_decl(const std::vector<Tok>& d) {
        if (d.empty()) return;
        if (d[0].k == Tok::Id && d[0].s == "typedef") {
            handle_typedef(d);
            return;
        }
        if (d[0].k == Tok::Id &&
            (d[0].s == "struct" || d[0].s == "union" || d[0].s == "enum")) {
            // 可能是定义，也可能是 `struct X x;` 变量声明
            size_t j = 1;
            if (j < d.size() && d[j].k == Tok::Id) ++j;
            if (j < d.size() && d[j].k == Tok::Punc && d[j].s == "{") {
                handle_struct_or_enum(d);
                return;
            }
        }
        // 函数原型：顶层 '('
        int depth = 0;
        size_t lp = d.size(), rp = 0;
        for (size_t i = 0; i < d.size(); ++i) {
            if (d[i].k != Tok::Punc) continue;
            if (d[i].s == "(") {
                if (depth == 0) {
                    lp = i;
                    rp = find_matching(d, i, "(", ")");
                    break;
                }
                ++depth;
            } else if (d[i].s == ")") --depth;
        }
        if (lp < d.size() && rp < d.size()) {
            // `int (*fp)(...)` 这种函数指针变量：跳过
            if (lp + 1 < d.size() && d[lp + 1].k == Tok::Punc && d[lp + 1].s == "*") {
                warn(d[0], "skipped function pointer variable");
                ++skipped;
                return;
            }
            handle_function(d, lp, rp);
            return;
        }
        handle_global(d);
    }

    void handle_macros(const std::vector<MacroDef>& defs, const std::vector<Tok>& all) {
        for (auto& m : defs) {
            if (m.value.empty()) continue;
            std::string v = cdecl_trim(m.value);
            // 字符串常量
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                std::string inner = v.substr(1, v.size() - 2);
                emit_const(m.name, "str", std::format("\"{}\"", inner));
                continue;
            }
            if (v == "true" || v == "TRUE") {
                emit_const(m.name, "bool", "True");
                continue;
            }
            if (v == "false" || v == "FALSE") {
                emit_const(m.name, "bool", "False");
                continue;
            }
            // 纯浮点字面量
            bool has_dot = v.find('.') != std::string::npos;
            Tok fake;
            fake.k = Tok::Num;
            fake.s = v;
            fake.line = m.line;
            std::vector<Tok> one;
            one.push_back(fake);
            auto iv = eval_const(one, 0, 1, macro_env);
            if (iv) {
                macro_env[m.name] = *iv;
                emit_const(m.name, "int", std::to_string(*iv));
                continue;
            }
            if (has_dot) {
                char* endp = nullptr;
                double dv = std::strtod(v.c_str(), &endp);
                if (endp && *endp == '\0') {
                    emit_const(m.name, "float", std::format("{}", dv));
                    continue;
                }
            }
            // 其余宏（表达式、类型、函数式）不展开
        }
        (void)all;
    }

    std::string output(const std::string& header, const std::string& module,
                       const std::vector<std::string>& links) {
        // 引用了但从未定义的 struct 补不透明声明。
        std::vector<std::string> opaque;
        for (auto& name : referenced_structs)
            if (!defined_structs.count(name)) opaque.push_back(std::format("class {}: pass\n", name));
        std::string out = std::format("# generated by pyadamas cdecl from {}\n", header);
        if (!module.empty()) out += std::format("cmodule(\"{}\")\n", module);
        for (auto& l : links) out += std::format("link(\"{}\")\n", l);
        out += "\n";
        for (auto& c : consts) out += c + "\n";
        if (!consts.empty()) out += "\n";
        for (auto& s : structs) out += s + "\n";
        for (auto& s : opaque) out += s + "\n";
        if (!structs.empty() || !opaque.empty()) out += "\n";
        for (auto& v : vars) out += v + "\n";
        if (!vars.empty()) out += "\n";
        for (auto& e : externs) out += e + "\n";
        return out;
    }
};


}  // namespace

std::string cdecl_process(const std::vector<Tok>& toks, const std::vector<MacroDef>& defs,
                          const std::string& header_path, const std::string& module,
                          const std::vector<std::string>& links, std::string& err) {
    Cdecl cd;
    cd.handle_macros(defs, toks);

    size_t pos = 0;
    while (pos < toks.size()) {
        if (toks[pos].k == Tok::Punc && toks[pos].s == ";") {
            ++pos;
            continue;
        }
        size_t start = pos;
        int par = 0;
        bool skip_decl = false;
        while (pos < toks.size()) {
            const Tok& t = toks[pos];
            if (t.k == Tok::Punc) {
                if (t.s == "(" || t.s == "[") ++par;
                else if (t.s == ")" || t.s == "]") --par;
                else if (t.s == ";" && par == 0) break;
                else if (t.s == "{" && par == 0) {
                    // 聚合体 / 函数体：跳到匹配的 '}'
                    size_t close = find_matching(toks, pos, "{", "}");
                    if (close >= toks.size()) {
                        err = std::format("line {}: unterminated '{{'", t.line);
                        return {};
                    }
                    bool aggregate = false;
                    if (!toks.empty() && start < toks.size()) {
                        size_t q = start;
                        if (toks[q].k == Tok::Id && toks[q].s == "typedef") ++q;
                        if (q < toks.size() && toks[q].k == Tok::Id &&
                            (toks[q].s == "struct" || toks[q].s == "union" ||
                             toks[q].s == "enum"))
                            aggregate = true;
                    }
                    if (aggregate) {
                        pos = close + 1;
                        continue;
                    }
                    // 函数定义：整条丢弃
                    skip_decl = true;
                    pos = close + 1;
                    break;
                }
            }
            ++pos;
        }
        if (skip_decl) continue;
        std::vector<Tok> decl(toks.begin() + static_cast<long>(start),
                              toks.begin() + static_cast<long>(pos));
        if (pos < toks.size() && toks[pos].k == Tok::Punc && toks[pos].s == ";") ++pos;
        cd.handle_decl(decl);
    }

    std::fprintf(stderr, "cdecl: %zu constants, %zu structs, %zu vars, %zu functions, %d skipped\n",
                 cd.consts.size(), cd.structs.size(), cd.vars.size(), cd.externs.size(),
                 cd.skipped);
    return cd.output(header_path, module, links);
}

}  // namespace pyadamas
