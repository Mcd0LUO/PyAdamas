// cimport/cdecl_parse.cpp —— C 头文件解析：类型、struct/enum 与声明输出
#include "cimport/cdecl.h"

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

#include "cimport/cdecl_internal.h"
#include "cimport/cdecl_model.h"

namespace pyadamas {
namespace {
using pyadamas::cdetail::CType;
using pyadamas::cdetail::StructFieldDef;
using pyadamas::cdetail::StructDef;
using pyadamas::cdetail::cpya_type;
using pyadamas::cdetail::cpya_field_type;


CType make_ptr(CType inner) {
    CType t;
    t.k = CType::Ptr;
    t.inner = std::make_shared<CType>(std::move(inner));
    return t;
}

bool is_qualifier(const std::string& s) {
    return s == "const" || s == "volatile" || s == "restrict" || s == "__restrict" ||
           s == "__restrict__" || s == "__const__" || s == "__volatile__" ||
           s == "__extension__" || s == "static" || s == "extern" || s == "register" ||
           s == "inline" || s == "__inline" || s == "__inline__" || s == "_Noreturn";
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
// 限定符也可能出现在 base 与 '*' 之间（`void const *`），所以循环开头先跳。
size_t consume_stars(const std::vector<Tok>& v, size_t i, CType& t) {
    for (;;) {
        while (i < v.size() && v[i].k == Tok::Id &&
               (v[i].s == "const" || v[i].s == "volatile" || v[i].s == "restrict" ||
                v[i].s == "__restrict"))
            ++i;
        if (i < v.size() && v[i].k == Tok::Punc && v[i].s == "*") {
            t = make_ptr(t);
            ++i;
            continue;
        }
        break;
    }
    return i;
}

// 函数参数/返回值类型 → .cpya。失败时 why 说明原因。

// struct 字段：只允许布局与 .cpya 完全一致的标量/指针。

// ── 常量表达式求值（枚举 / #define）─────────────────────────────



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
        // 位域布局不可移植（依赖编译器），整体降级为不透明；数组字段支持。
        for (size_t i = db; i < de; ++i) {
            if (v[i].k != Tok::Punc) continue;
            if (v[i].s == ":") {
                sd.ok = false;
                sd.why = "bitfield field";
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
            std::vector<int64_t> dims;
            for (size_t q = j; q < ce; ++q) {
                if (v[q].k == Tok::Punc && (v[q].s == "(" || v[q].s == ")")) {
                    sd.ok = false;
                    sd.why = "function pointer field";
                    return;
                }
                if (v[q].k == Tok::Punc && v[q].s == "[") {
                    // 定长数组字段：只接受整数字面量（宏已在预处理阶段展开）。
                    size_t close = q + 1;
                    int64_t n = 0;
                    bool neg = false, got = false;
                    for (; close < ce && !(v[close].k == Tok::Punc && v[close].s == "]");
                         ++close) {
                        if (v[close].k == Tok::Num) {
                            try {
                                size_t used = 0;
                                n = std::stoll(v[close].s, &used, 0);
                                got = used == v[close].s.size();
                            } catch (...) {
                                got = false;
                            }
                        } else if (v[close].k == Tok::Punc && v[close].s == "-") {
                            neg = true;
                        } else {
                            got = false;
                            break;
                        }
                    }
                    if (!got || close >= ce || neg || n <= 0) {
                        sd.ok = false;
                        sd.why = "array field with a non-literal length";
                        return;
                    }
                    dims.push_back(n);
                    q = close;
                    continue;
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
            sd.fields.push_back(StructFieldDef{fname, ft, std::move(dims)});
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
    // 只有 class X: pass 的（没有布局）不能按值用；引用它们的地方必须跳过。
    std::set<std::string> opaque_structs;
    std::set<std::string> referenced_structs;
    // 函数/全局变量同名去重（系统头文件里常见重复声明）。
    std::set<std::string> emitted_externs;
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

    bool layout_known(const std::string& name) const {
        return !name.empty() && defined_structs.count(name) && !opaque_structs.count(name);
    }
    // 按值使用 struct/union 时必须有完整布局。
    bool by_value_known(const CType& t) const {
        if (t.k == CType::Struct || t.k == CType::Union) return layout_known(t.name);
        return true;
    }
    // 返回函数指针（含指针链）在 .cpya 里无法表达，只能跳过。
    static bool is_func_ptr(const CType& t) {
        const CType* p = &t;
        while (p->k == CType::Ptr && p->inner) p = p->inner.get();
        return p->k == CType::Func;
    }
    // 与 cimport 的回调寄存器上限保持一致（整数 ≤6、浮点 ≤8），超了就跳过。
    static bool cb_too_many(const CType& f) {
        int ni = 0, nf = 0;
        for (const auto& p : f.ps) {
            if (p.k == CType::Float) ++nf;
            else ++ni;
        }
        return ni > 6 || nf > 8;
    }

    // 指针/回调里嵌套引用的 struct 也要补一个不透明 class，否则
    // `ptr[X]` 在 .cpya 里找不到元素类型。
    void note_refs(const CType& t) {
        if (t.k == CType::Struct || t.k == CType::Union) referenced_structs.insert(t.name);
        if (t.k == CType::Ptr && t.inner) note_refs(*t.inner);
        if (t.k == CType::Func) {
            if (t.ret) note_refs(*t.ret);
            for (auto& p : t.ps) note_refs(p);
        }
    }

    void emit_struct(const StructDef& sd, const std::string& name) {
        if (name.empty() || defined_structs.count(name)) return;
        defined_structs.insert(name);
        if (!sd.ok) {
            opaque_structs.insert(name);
            structs.push_back(std::format("# {} (opaque: {})\nclass {}: pass\n", name, sd.why, name));
            return;
        }
        std::string block;
        if (sd.packed && !sd.is_union)
            block += std::format("# packed {} (no padding; layout matches C __attribute__((packed)))\n", name);
        else if (sd.is_union)
            block += std::format("# union {} (all members at offset 0)\n", name);
        if (sd.is_union) block += "@union\n";
        if (sd.packed) block += "@packed\n";
        block += std::format("class {}:\n", name);
        for (auto& f : sd.fields) {
            if (!by_value_known(f.type)) {
                opaque_structs.insert(name);
                structs.push_back(std::format(
                    "# {} (opaque: field '{}' has struct type '{}' with no layout)\nclass {}: pass\n",
                    name, f.name, f.type.name, name));
                return;
            }
            std::string why;
            auto s = cpya_field_type(f.type, why);
            if (!s) {
                // 字段类型不支持（如 C float 字段）：整体降级为不透明。
                opaque_structs.insert(name);
                structs.push_back(
                    std::format("# {} (opaque: {})\nclass {}: pass\n", name, why, name));
                return;
            }
            std::string dims;
            for (int64_t d : f.dims) dims += std::format("[{}]", d);
            block += std::format("    {}: {}{}\n", f.name, *s, dims);
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
                bool attr_packed = false;
                for (size_t q = close + 1; q < d.size(); ++q) {
                    // __attribute__((packed))：跳过属性 token，不能当别名。
                    if (d[q].k == Tok::Id && d[q].s == "__attribute__") {
                        int depth = 0;
                        for (++q; q < d.size(); ++q) {
                            if (d[q].k == Tok::Punc && d[q].s == "(") ++depth;
                            else if (d[q].k == Tok::Punc && d[q].s == ")") {
                                if (--depth == 0) break;
                            } else if (d[q].k == Tok::Id && d[q].s == "packed") {
                                attr_packed = true;
                            }
                        }
                        continue;
                    }
                    if (d[q].k == Tok::Id) alias_name = d[q].s;
                }
                std::string name = alias_name.empty() ? tag : alias_name;
                if (is_enum) {
                    parse_enum_body(d, j + 1, close, d[i]);
                    CType t;
                    t.k = CType::I32;
                    if (!alias_name.empty()) alias[alias_name] = t;
                    return;
                }
                StructDef sd;
                sd.name = name;
                sd.line = d[i].line;
                sd.is_union = is_union;
                sd.packed = attr_packed;
                parse_struct_body(d, j + 1, close, alias, sd);
                emit_struct(sd, name);
                CType t;
                t.k = is_union ? CType::Union : CType::Struct;
                // 类体只有一个名字（alias 优先）；tag 也指向同一个类，否则
                // `typedef struct z_stream_s {...} z_stream;` 之后的
                // `struct z_stream_s *` 会指向不存在的类。
                t.name = name;
                // `} *Alias;`（typedef 到指针）要把 '*' 也算进去。
                CType ta = t;
                for (size_t q = close + 1; q < d.size(); ++q)
                    if (d[q].k == Tok::Punc && d[q].s == "*") ta = make_ptr(ta);
                if (!alias_name.empty()) alias[alias_name] = ta;
                if (!tag.empty()) alias[tag] = t;
                return;
            }
            // typedef struct Tag Alias;  /  typedef struct Tag *Alias;
            std::string alias_name;
            for (size_t q = j; q < d.size(); ++q)
                if (d[q].k == Tok::Id) alias_name = d[q].s;
            CType t;
            t.k = is_enum ? CType::I32 : (is_union ? CType::Union : CType::Struct);
            t.name = tag;
            // `typedef struct sqlite3_stmt sqlite3_stmt;`（别名与 tag 同名）必须
            // 把 tag 自身也注册成类型，否则后续 `sqlite3_stmt*` 参数全是 unknown。
            CType ta = t;
            for (size_t q = j; q < d.size(); ++q)
                if (d[q].k == Tok::Punc && d[q].s == "*") ta = make_ptr(ta);
            if (!alias_name.empty()) alias[alias_name] = ta;
            if (!tag.empty()) {
                if (alias_name != tag) alias[tag] = t;
                referenced_structs.insert(tag);
            }
            return;
        }
        // typedef RET (*NAME)(params);
        {
            size_t lp = d.size();
            for (size_t q = 1; q < d.size(); ++q) {
                if (d[q].k == Tok::Punc && d[q].s == "(" && q + 1 < d.size() &&
                    d[q + 1].k == Tok::Punc && d[q + 1].s == "*") {
                    lp = q;
                    break;
                }
            }
            if (lp < d.size()) {
                size_t rp = lp + 1;
                int depth = 0;
                for (size_t q = lp; q < d.size(); ++q) {
                    if (d[q].k != Tok::Punc) continue;
                    if (d[q].s == "(") ++depth;
                    else if (d[q].s == ")" && --depth == 0) {
                        rp = q;
                        break;
                    }
                }
                std::string name;
                for (size_t q = lp + 2; q < rp; ++q)
                    if (d[q].k == Tok::Id) name = d[q].s;
                if (!name.empty() && rp + 1 < d.size() && d[rp + 1].k == Tok::Punc &&
                    d[rp + 1].s == "(") {
                    size_t close = find_matching(d, rp + 1, "(", ")");
                    if (close < d.size()) {
                        CType cb;
                        cb.k = CType::Func;
                        CType ret;
                        parse_base_type(d, 1, alias, ret);
                        cb.ret = std::make_shared<CType>(ret);
                        if (parse_params(d, rp + 2, close, cb, d[0])) {
                            alias[name] = std::move(cb);
                            return;
                        }
                    }
                }
            }
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
        StructDef sd;
        sd.name = tag;
        sd.line = d[0].line;
        sd.is_union = is_union;
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
        // 系统头文件可能被多次包含（不同条件分支），同一名字只保留第一份。
        if (!emitted_externs.insert(name).second) return;
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
        if (!by_value_known(ret)) {
            warn(d[0], std::format("skipped '{}': return struct '{}' has no layout", name, ret.name));
            ++skipped;
            return;
        }
        if (is_func_ptr(ret)) {
            warn(d[0], std::format("skipped '{}': function pointer return", name));
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
        // 结构体按值/指针：记录引用，稍后补不透明声明
        note_refs(ret);
        std::string params;
        for (size_t i = 0; i < fn.ps.size(); ++i) {
            if (!by_value_known(fn.ps[i])) {
                warn(d[0], std::format("skipped '{}': parameter struct '{}' has no layout", name,
                                       fn.ps[i].name));
                ++skipped;
                return;
            }
            if (fn.ps[i].k == CType::Func && cb_too_many(fn.ps[i])) {
                warn(d[0], std::format("skipped '{}': callback has too many parameters", name));
                ++skipped;
                return;
            }
            std::string pw;
            auto ps = cpya_type(fn.ps[i], true, pw);
            if (!ps) {
                warn(d[0], std::format("skipped '{}': {}", name, pw));
                ++skipped;
                return;
            }
            note_refs(fn.ps[i]);
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
            if (!emitted_externs.insert(name).second) continue;
            if (is_func_ptr(t)) {
                warn(d[0], std::format("skipped function pointer variable '{}'", name));
                ++skipped;
                continue;
            }
            if (array) {
                // 0.564：数组变量生成 `T[N]`（C 的 int vals[3] 同序）。
                // 元素指针统一写 ptr（char* 之类在 .cpya 里是 ptr 而不是 str，
                // str 表示 PyAdamas 侧字符串对象，做数组元素会被误当成 8 字节槽）。
                std::string ewhy;
                auto elem = cpya_type(t, false, ewhy);
                std::string elem_txt = elem ? *elem : std::string("ptr");
                if (elem_txt == "str") elem_txt = "ptr";
                int64_t n = 0;
                for (size_t q = j; q < ve; ++q) {
                    if (!(d[q].k == Tok::Punc && d[q].s == "[")) continue;
                    size_t close = find_matching(d, q, "[", "]");
                    if (close >= ve) break;
                    if (auto v = eval_const(d, q + 1, close, macro_env)) n = *v;
                    break;
                }
                if (n <= 0) n = 1;
                vars.push_back(std::format("{}: {}[{}]", name, elem_txt, static_cast<long long>(n)));
                emitted_names.insert(name);
                continue;
            }
            if (!by_value_known(t)) {
                warn(d[0], std::format("skipped variable '{}': struct type has no layout", name));
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
            if (t.k == CType::Struct) note_refs(t);
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
    // 0.561：编译器内建类型先登记成不透明指针 —— 最常见的是 `__builtin_va_list`
    // （stdio.h 的 vprintf 家族全靠它），不登记就会被当成「unsupported typedef」整批跳过。
    {
        CType vp;
        vp.k = CType::Ptr;
        vp.inner = std::make_shared<CType>();
        cd.alias["__builtin_va_list"] = vp;
        cd.alias["__builtin_ms_va_list"] = vp;
    }
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
