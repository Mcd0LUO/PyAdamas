// cimport/cimport.cpp —— .cpya 声明文件解析（Python 语法 stub）
//
// 声明文件统一是 Python 语法（见 src/cimport/pydecl.h），例如：
//
//   # stdlib/zlib.cpya
//   cmodule("zlib")            # 可省略：默认从 cimport 名字推导
//   link("z")                  # -> -lz；给路径/以 '-' 开头则原样传给链接器
//
//   Z_OK: int = 0              # 常量：带注解的赋值，编译期内联
//
//   class FILE: pass           # 不透明句柄：class 体只有 pass
//   class pair:                # 带字段的 C struct：注解无值赋值 = 字段
//       a: float
//       b: float
//
//   @extern("strlen")          # 改名时才要装饰器
//   def slen(s: str) -> int: pass
//   def f32_mul(x: float32, y: float32) -> float32: pass   # C 符号 = Python 名
//
//   stdout: ptr[FILE]                        # 无值 = extern var（C 名 = py 名）
//   optind: int32 = extern_var("optind")     # 有值且是 extern_var(...) 调用 = 改名
//
// 解析复用编译器自己的 Lexer + Parser（src/cimport/pydecl.h），旧的行式语法已删除
// （pre-1.0，不留兼容）。类型注解是本文件特有的 C 边界类型：int/int32/uint32/
// float/float32/bool/str/char/int8/short/.../uint64/ptr/ptr[T]/Optional[str]/
// Callable[[...], R] 以及本文件声明的 struct 名。
#include "cimport/cimport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/ast.h"
#include "cimport/pydecl.h"

namespace pyadamas {

std::optional<CKind> parse_ckind(const std::string& text) {
    std::string t = text;
    // 去掉两端空白（注解名来自标识符，正常没有空白；防御一下）
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    if (t == "None") return CKind::None;
    if (t == "int") return CKind::Int;
    // 0.564：定宽别名也认（int64 是 long，C 的 long 在 LP64 上就是 8 字节）。
    if (t == "int64" || t == "long") return CKind::Int;
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
    return std::nullopt;
}

namespace {

// 注解解析后的 C 边界类型。
struct CTypeRef {
    CKind kind = CKind::None;
    CKind elem = CKind::None;      // ptr[T] 的 T（标量时）
    std::string stname;            // ptr[Struct] / 按值 Struct 的结构体名
    bool opaque = false;           // 指向的结构体是不是不透明句柄
    std::vector<CKind> cb_params;  // kind == Callback：回调形参种类
    CKind cb_ret = CKind::None;    // kind == Callback：回调返回种类
    std::vector<int64_t> dims;     // 非空 = 定长数组（行优先，dims[0] 最外层）

    CTypeRef() = default;
    explicit CTypeRef(CKind k, CKind e = CKind::None, std::string s = {}, bool o = false,
                      std::vector<CKind> cbp = {}, CKind cbr = CKind::None)
        : kind(k),
          elem(e),
          stname(std::move(s)),
          opaque(o),
          cb_params(std::move(cbp)),
          cb_ret(cbr) {}
};

// 回调签名只允许能在 C 与 PyAdamas 之间按机器字直接传递的类型。
// None 只用于返回（C 的 void）；跳板把 rax 原样返回，C 侧不会读它。
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
        case CKind::Ptr:
        case CKind::None: return true;
        default: return false;
    }
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
        case CKind::Float32: size = 4; align = 4; return true;  // C float
        case CKind::Bool: size = 1; align = 1; return true;     // _Bool
        case CKind::Ptr: size = 8; align = 8; return true;      // void* / T*
        default: return false;
    }
}

int64_t round_up(int64_t v, int64_t a) { return (v + a - 1) / a * a; }

// 本文件声明的 struct：名字 → 布局（字段布局用，opaque 判定用）。
using StructMap = std::unordered_map<std::string, const CStruct*>;

// 注解表达式 → C 边界类型。只认 .cpya 的词汇：
//   int/int32/uint32/float/float32/bool/str/char/int8/short/int16/uchar/
//   uint8/ushort/uint16/ulong/uint64/None/ptr/ptr[T]/Optional[str]/
//   Callable[[P...], R]/<本文件 struct 名>
// 注解解析上下文：Pass 1（原始名字，允许前向引用）或 Pass 2（已定型的 struct 表）。
struct TypeCtx {
    const StructMap* structs = nullptr;
    const std::unordered_map<std::string, size_t>* raw = nullptr;
};

std::optional<CTypeRef> ctype_from_ast(const Expr* e, const TypeCtx& ctx, std::string& why) {
    if (!e) {
        why = "missing type annotation";
        return std::nullopt;
    }
    // 定长数组：T[N] / T[N][M]（下标必须是正整数字面量）。
    if (const auto* sub = dynamic_cast<const Subscript*>(e)) {
        if (const auto* c = dynamic_cast<const Constant*>(sub->slice.get());
            c && c->ck == Constant::CK::Int) {
            auto inner = ctype_from_ast(sub->value.get(), ctx, why);
            if (!inner) return std::nullopt;
            if (c->u64_lit || c->i <= 0) {
                why = "array length must be a positive integer";
                return std::nullopt;
            }
            if (inner->dims.size() >= 8) {
                why = "too many array dimensions (max 8)";
                return std::nullopt;
            }
            inner->dims.push_back(c->i);
            return inner;
        }
    }
    if (const auto* c = dynamic_cast<const Constant*>(e)) {
        // -> None：无返回值（Python 里 None 是常量表达式，不是 Name）。
        if (c->ck == Constant::CK::None) return CTypeRef{CKind::None};
        why = "unsupported type annotation";
        return std::nullopt;
    }
    if (const auto* n = dynamic_cast<const Name*>(e)) {
        if (ctx.structs) {
            if (auto it = ctx.structs->find(n->id); it != ctx.structs->end())
                return CTypeRef{CKind::Struct, CKind::None, n->id,
                                it->second->fields.empty()};
        }
        if (ctx.raw && ctx.raw->count(n->id))
            return CTypeRef{CKind::Struct, CKind::None, n->id, false};
        if (auto k = parse_ckind(n->id))
            return CTypeRef{*k, CKind::None, {}, false, {}, CKind::None};
        why = "unknown type '" + n->id + "'";
        return std::nullopt;
    }
    const auto* s = dynamic_cast<const Subscript*>(e);
    if (!s) {
        why = "unsupported type annotation";
        return std::nullopt;
    }
    const std::string base = pydecl::annotation_base_name(s->value.get());
    if (base == "ptr") {
        const auto* in = dynamic_cast<const Name*>(s->slice.get());
        if (!in) {
            why = "ptr[...] needs a plain type name as the element type";
            return std::nullopt;
        }
        if (ctx.structs) {
            if (auto it = ctx.structs->find(in->id); it != ctx.structs->end())
                return CTypeRef{CKind::Ptr, CKind::None, in->id,
                                it->second->fields.empty(), {}, CKind::None};
        }
        if (ctx.raw && ctx.raw->count(in->id))
            return CTypeRef{CKind::Ptr, CKind::None, in->id, false};
        if (auto k = parse_ckind(in->id))
            return CTypeRef{CKind::Ptr, *k, {}, false, {}, CKind::None};
        why = "unknown pointer element type '" + in->id + "'";
        return std::nullopt;
    }
    if (base == "Optional") {
        const auto* in = dynamic_cast<const Name*>(s->slice.get());
        if (in && in->id == "str") return CTypeRef{CKind::OptStr, CKind::None, {}, false, {}, CKind::None};
        why = "Optional[...] only supports Optional[str] in .cpya";
        return std::nullopt;
    }
    if (base == "Callable") {
        // Callable[[P1, P2, ...], R] —— C 回调参数（C fn(P1, P2) -> R）。
        const auto* tup = dynamic_cast<const TupleExpr*>(s->slice.get());
        if (!tup || tup->elts.size() != 2) {
            why = "Callable needs the form Callable[[param, ...], ret]";
            return std::nullopt;
        }
        CTypeRef r;
        r.kind = CKind::Callback;
        if (const auto* plist = dynamic_cast<const ListExpr*>(tup->elts[0].get())) {
            for (auto& pe : plist->elts) {
                auto pk = ctype_from_ast(pe.get(), ctx, why);
                if (!pk || pk->kind == CKind::Callback) {
                    if (why.empty()) why = "callback parameters must be scalars or ptr";
                    return std::nullopt;
                }
                // 回调里的指针一律按地址传（ptr[T] 的 T 不参与 ABI）。
                const CKind k = pk->kind == CKind::Ptr ? CKind::Ptr : pk->kind;
                if (!callback_kind_ok(k)) {
                    why = "callback parameter type is not supported";
                    return std::nullopt;
                }
                r.cb_params.push_back(k);
            }
        } else if (!dynamic_cast<const TupleExpr*>(tup->elts[0].get())) {
            why = "Callable parameter list must be a list";
            return std::nullopt;
        }
        auto rk = ctype_from_ast(tup->elts[1].get(), ctx, why);
        if (!rk || !callback_kind_ok(rk->kind == CKind::Ptr ? CKind::Ptr : rk->kind)) {
            why = "callback return type is not supported";
            return std::nullopt;
        }
        r.cb_ret = rk->kind == CKind::Ptr ? CKind::Ptr : rk->kind;
        int ni = 0, nf = 0;
        for (CKind k : r.cb_params) {
            if (k == CKind::Float) ++nf;
            else ++ni;
        }
        if (ni > 6 || nf > 8) {
            why = "callback has too many parameters (no stack-passed callback args)";
            return std::nullopt;
        }
        return r;
    }
    why = "unknown type '" + base + "'";
    return std::nullopt;
}

// link 参数：字符串字面量。以 '-' 开头或像路径（含 '/' 或 .c/.a/.so 结尾）
// 原样传给链接器，其余按 -l<名字>。
std::string normalize_link_arg(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    if (v.empty()) return v;
    if (v.front() == '-') return v;
    if (v.find('/') != std::string::npos) return v;
    if (v.ends_with(".c") || v.ends_with(".a") || v.ends_with(".so") ||
        v.ends_with(".o") || v.ends_with(".dylib"))
        return v;
    return "-l" + v;
}

// 按值 ABI 只覆盖纯标量/指针字段：含数组、union、嵌套结构体的 struct 拒绝按值传。
bool struct_has_aggregate(const CStruct& cs, const StructMap& structs) {
    if (cs.is_union) return true;
    for (auto& f : cs.fields) {
        if (f.is_array()) return true;
        if (!f.ptr_struct.empty()) {
            auto it = structs.find(f.ptr_struct);
            if (it == structs.end()) return true;
            if (struct_has_aggregate(*it->second, structs)) return true;
        }
    }
    return false;
}

bool is_ident(const std::string& s) {
    if (s.empty()) return false;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c0) || c0 == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

}  // namespace

// ── .cpya 加载 ────────────────────────────────────────────────────────

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

    std::unique_ptr<Module> ast;
    try {
        ast = pydecl::parse_file(found.string());
    } catch (const pydecl::Error& e) {
        err = std::format("{}:{}: {}", found.string(), e.line, e.msg);
        return std::nullopt;
    }

    CImportDecl decl;
    decl.name = name;
    bool saw_cmodule = false;
    std::unordered_set<std::string> seen_py;
    std::unordered_set<std::string> seen_links;

    try {
        // ── Pass 1：class 声明 = C struct / union ──────────────────────
        // 先收集原始字段（类型可能引用本文件里稍后声明的 struct/union），
        // 再按依赖序递归算布局。按值环（struct 里放自己）在 C 里也非法。
        struct RawField {
            std::string name;
            CTypeRef type;
            int line = 0;
        };
        struct RawDecl {
            std::string name;
            bool is_union = false;
            bool packed = false;
            int line = 0;
            std::vector<RawField> fields;
            std::vector<CStructField> out;  // layout 后的字段（含 offset/size）
            int64_t size = 0, align = 1;
            int state = 0;  // 0=未算 1=算中 2=算完
        };
        std::vector<RawDecl> raws;
        std::unordered_map<std::string, size_t> raw_index;
        std::vector<size_t> order;  // 布局完成序：元素 struct 先于使用它的 struct

        for (auto& st : ast->body) {
            if (pydecl::is_docstring(*st)) continue;
            if (st->kind != StmtKind::ClassDef) continue;
            auto& cd = static_cast<ClassDef&>(*st);
            if (!is_ident(cd.name))
                pydecl::fail(cd.line, std::format("invalid struct name '{}'", cd.name));
            if (raw_index.count(cd.name))
                pydecl::fail(cd.line, std::format("duplicate struct '{}'", cd.name));
            RawDecl rd;
            rd.name = cd.name;
            rd.line = cd.line;
            for (auto& d : cd.decorators) {
                if (!d) continue;
                const auto dec = pydecl::decorator_of(*d, cd.name);
                if (dec.name == "union") {
                    if (dec.has_string_arg) pydecl::fail(dec.line, "@union takes no arguments");
                    if (rd.is_union) pydecl::fail(dec.line, "duplicate @union");
                    rd.is_union = true;
                } else if (dec.name == "packed") {
                    if (dec.has_string_arg) pydecl::fail(dec.line, "@packed takes no arguments");
                    if (rd.packed) pydecl::fail(dec.line, "duplicate @packed");
                    rd.packed = true;
                } else {
                    pydecl::fail(dec.line,
                                 std::format("unknown decorator '@{}' on class '{}' (only "
                                             "@union / @packed are allowed)", dec.name, cd.name));
                }
            }
            std::unordered_set<std::string> seen_fields;
            for (auto& bs : cd.body) {
                if (pydecl::is_docstring(*bs)) continue;
                if (bs->kind == StmtKind::Pass) continue;  // 不透明句柄
                if (bs->kind != StmtKind::AnnAssign)
                    pydecl::fail(bs->line,
                                 std::format("struct '{}' body may only contain field "
                                             "annotations (name: type) or pass; found {}",
                                             cd.name, pydecl::stmt_kind_name(bs->kind)));
                auto& aa = static_cast<AnnAssign&>(*bs);
                if (aa.value)
                    pydecl::fail(aa.line, std::format("struct field in '{}' must be an "
                                                      "annotation without a value", cd.name));
                const auto* fn = dynamic_cast<const Name*>(aa.target.get());
                if (!fn)
                    pydecl::fail(aa.line, std::format("struct field name in '{}' must be a "
                                                      "plain name", cd.name));
                if (!seen_fields.insert(fn->id).second)
                    pydecl::fail(aa.line, std::format("duplicate field '{}' in struct '{}'",
                                                      fn->id, cd.name));
                std::string why;
                auto ct = ctype_from_ast(aa.annotation.get(), TypeCtx{nullptr, &raw_index}, why);
                if (!ct)
                    pydecl::fail(aa.line,
                                 std::format("unsupported field type in '{}': {}", cd.name, why));
                if (ct->kind == CKind::Callback || ct->kind == CKind::Str ||
                    ct->kind == CKind::OptStr || ct->kind == CKind::None)
                    pydecl::fail(aa.line,
                                 std::format("unsupported struct field type '{}' in '{}' (use "
                                             "int, int32, uint32, float, float32, bool, ptr or a "
                                             "struct name; arrays via T[N])",
                                             pydecl::annotation_base_name(aa.annotation.get()),
                                             cd.name));
                RawField rf;
                rf.name = fn->id;
                rf.line = aa.line;
                rf.type = std::move(*ct);
                rd.fields.push_back(std::move(rf));
            }
            raw_index[rd.name] = raws.size();
            raws.push_back(std::move(rd));
        }

        std::function<void(size_t)> layout = [&](size_t idx) {
            RawDecl& rd = raws[idx];
            if (rd.state == 2) return;
            if (rd.state == 1)
                pydecl::fail(rd.line, std::format("recursive struct '{}' by value", rd.name));
            rd.state = 1;
            int64_t off = 0, align = 1, usize = 0;
            for (auto& rf : rd.fields) {
                const CTypeRef& t = rf.type;
                int64_t esize = 0, ealign = 1;
                if (!t.stname.empty()) {
                    auto it = raw_index.find(t.stname);
                    if (it == raw_index.end())
                        pydecl::fail(rf.line, std::format("unknown struct '{}'", t.stname));
                    layout(it->second);
                    esize = raws[it->second].size;
                    ealign = raws[it->second].align;
                    if (esize == 0)
                        pydecl::fail(rf.line, std::format("struct '{}' is opaque and cannot be "
                                                          "used by value", t.stname));
                } else if (!cfield_layout(t.kind, esize, ealign)) {
                    pydecl::fail(rf.line,
                                 std::format("unsupported struct field type in '{}'", rd.name));
                }
                int64_t total = esize;
                for (int64_t d : t.dims) {
                    if (total > INT64_MAX / d)
                        pydecl::fail(rf.line, std::format("array '{}' is too large", rf.name));
                    total *= d;
                }
                CStructField cf;
                cf.name = rf.name;
                cf.kind = t.kind;
                cf.ptr_elem = t.elem;
                cf.ptr_struct = t.stname;
                cf.dims = t.dims;
                cf.align = ealign;
                cf.size = total;
                if (rd.packed) cf.align = 1;
                if (rd.is_union) {
                    cf.offset = 0;
                    if (cf.size > usize) usize = cf.size;
                } else {
                    if (!rd.packed) off = round_up(off, cf.align);
                    cf.offset = off;
                    off += cf.size;
                }
                if (cf.align > align) align = cf.align;
                rd.out.push_back(std::move(cf));
            }
            if (rd.packed) {
                align = 1;
                rd.size = rd.is_union ? usize : off;
            } else {
                rd.size = round_up(rd.is_union ? usize : off, align);
            }
            rd.align = align;
            rd.state = 2;
            order.push_back(idx);
        };
        for (size_t i = 0; i < raws.size(); ++i) layout(i);

        // typecheck 侧按 decl.structs 的顺序逐个 build_cstruct 并靠 struct_info(名字)
        // 解析 ptr[Struct] / 数组元素 struct：先放不透明句柄（无依赖），再按
        // 布局完成序（元素 struct 一定先于使用它的 struct）。
        for (size_t i = 0; i < raws.size(); ++i) {
            if (!raws[i].fields.empty()) continue;
            CStruct cs;
            cs.name = raws[i].name;
            cs.is_union = raws[i].is_union;
            decl.structs.push_back(std::move(cs));
        }
        for (size_t idx : order) {
            RawDecl& rd = raws[idx];
            if (rd.fields.empty()) continue;
            CStruct cs;
            cs.name = rd.name;
            cs.is_union = rd.is_union;
            cs.size = rd.size;
            cs.align = rd.align;
            cs.fields = std::move(rd.out);
            decl.structs.push_back(std::move(cs));
        }
        // decl.structs 已定型（Pass 2 不再 push），指针稳定。
        StructMap structs;
        for (auto& s : decl.structs) structs[s.name] = &s;

        // ── Pass 2：cmodule / link / def / 常量 / extern var ──────────
        for (auto& st : ast->body) {
            if (pydecl::is_docstring(*st)) continue;
            switch (st->kind) {
                case StmtKind::ClassDef: continue;  // Pass 1 已处理
                case StmtKind::FunctionDef: {
                    auto& fd = static_cast<FunctionDef&>(*st);
                    if (fd.is_async)
                        pydecl::fail(fd.line, std::format("async def is not allowed in .cpya "
                                                          "(function '{}')", fd.name));
                    if (!is_ident(fd.name))
                        pydecl::fail(fd.line, std::format("invalid name '{}'", fd.name));
                    CExtern ex;
                    ex.py_name = fd.name;
                    ex.symbol = fd.name;  // 默认 C 符号 = Python 名
                    bool saw_extern = false;
                    for (auto& d : fd.decorators) {
                        if (!d) continue;
                        const auto dec = pydecl::decorator_of(*d, fd.name);
                        if (dec.name != "extern")
                            pydecl::fail(dec.line,
                                         std::format("unknown decorator '@{}' on '{}' (only "
                                                     "@extern(\"c_symbol\") is allowed)",
                                                     dec.name, fd.name));
                        if (!dec.has_string_arg)
                            pydecl::fail(dec.line, std::format("@extern on '{}' needs the C "
                                                               "symbol: @extern(\"c_name\")",
                                                               fd.name));
                        if (saw_extern)
                            pydecl::fail(dec.line, std::format("duplicate decorator on '{}'",
                                                               fd.name));
                        saw_extern = true;
                        ex.symbol = dec.arg;
                    }
                    pydecl::check_pass_body(fd);
                    if (!fd.args.kwonly.empty() || fd.args.kwarg)
                        pydecl::fail(fd.line, std::format("function '{}': keyword-only / **kwargs "
                                                          "parameters are not supported in .cpya",
                                                          fd.name));
                    auto add_param = [&](const Arg& a) {
                        if (!a.annotation)
                            pydecl::fail(fd.line, std::format("parameter '{}' of '{}' needs a type "
                                                              "annotation", a.name, fd.name));
                        std::string why;
                        auto ct = ctype_from_ast(a.annotation.get(), TypeCtx{&structs, nullptr}, why);
                        if (!ct)
                            pydecl::fail(fd.line, std::format("parameter '{}' of '{}': {}", a.name,
                                                              fd.name, why));
                        CParam cp;
                        cp.name = a.name;
                        cp.kind = ct->kind;
                        cp.ptr_elem = ct->elem;
                        cp.ptr_struct = ct->stname;
                        cp.opaque = ct->opaque;
                        cp.cb_params = ct->cb_params;
                        cp.cb_ret = ct->cb_ret;
                        if (cp.kind == CKind::Struct) {
                            if (cp.opaque)
                                pydecl::fail(fd.line,
                                             std::format("struct '{}' is opaque and cannot be "
                                                         "passed by value", cp.ptr_struct));
                            const CStruct* cs = structs.count(cp.ptr_struct)
                                                    ? structs.at(cp.ptr_struct)
                                                    : nullptr;
                            if (cs && cs->is_union)
                                pydecl::fail(fd.line,
                                             std::format("union '{}' cannot be passed by value "
                                                         "(C ABI classification is not "
                                                         "implemented); pass ptr[{}] instead",
                                                         cp.ptr_struct, cp.ptr_struct));
                            if (cs && struct_has_aggregate(*cs, structs))
                                pydecl::fail(fd.line,
                                             std::format("struct '{}' contains union/array/"
                                                         "nested-struct fields and cannot be "
                                                         "passed by value; pass ptr[{}] instead",
                                                         cp.ptr_struct, cp.ptr_struct));
                        }
                        ex.params.push_back(std::move(cp));
                    };
                    for (auto& a : fd.args.posonly) add_param(a);
                    for (auto& a : fd.args.args) add_param(a);
                    if (fd.args.vararg) ex.varargs = true;  // C 变参：*args 位置占位
                    std::string why;
                    auto rk = ctype_from_ast(fd.returns.get(), TypeCtx{&structs, nullptr}, why);
                    if (!rk)
                        pydecl::fail(fd.line, std::format("return type of '{}': {} (use -> None "
                                                          "for no return value)",
                                                          fd.name, why));
                    if (rk->kind == CKind::Callback)
                        pydecl::fail(fd.line, "returning a C function pointer is not supported");
                    if (rk->kind == CKind::Struct) {
                        if (rk->opaque)
                            pydecl::fail(fd.line,
                                         std::format("struct '{}' is opaque and cannot be "
                                                     "returned by value", rk->stname));
                        const CStruct* cs =
                            structs.count(rk->stname) ? structs.at(rk->stname) : nullptr;
                        if (cs && cs->is_union)
                            pydecl::fail(fd.line,
                                         std::format("union '{}' cannot be returned by value "
                                                     "(C ABI classification is not implemented)",
                                                     rk->stname));
                        if (cs && struct_has_aggregate(*cs, structs))
                            pydecl::fail(fd.line,
                                         std::format("struct '{}' contains union/array/"
                                                     "nested-struct fields and cannot be returned "
                                                     "by value", rk->stname));
                    }
                    ex.ret = rk->kind;
                    ex.ret_struct = rk->stname;
                    ex.ret_elem = rk->elem;
                    if (!seen_py.insert(ex.py_name).second)
                        pydecl::fail(fd.line, std::format("duplicate name '{}'", ex.py_name));
                    decl.externs.push_back(std::move(ex));
                    break;
                }
                case StmtKind::AnnAssign: {
                    auto& aa = static_cast<AnnAssign&>(*st);
                    const auto* nm = dynamic_cast<const Name*>(aa.target.get());
                    if (!nm)
                        pydecl::fail(aa.line, "declaration target must be a plain name");
                    const std::string pyname = nm->id;
                    if (!is_ident(pyname))
                        pydecl::fail(aa.line, std::format("invalid name '{}'", pyname));
                    if (!aa.annotation)
                        pydecl::fail(aa.line, std::format("'{}' needs a type annotation", pyname));
                    std::string why;
                    // 0.564：定长数组变量用 C/Python 同形的 `T[N]`（int32[3] / ptr[2]）。
                    // ctype_from_ast 已经把维度收进 dims，这里只解析类型。
                    std::optional<CTypeRef> ct =
                        ctype_from_ast(aa.annotation.get(), TypeCtx{&structs, nullptr}, why);
                    if (!ct) pydecl::fail(aa.line, std::format("'{}': {}", pyname, why));
                    // extern var：无值，或值是一个 extern_var("c_name") 调用。
                    std::string var_symbol = pyname;
                    bool is_var = !aa.value;
                    if (aa.value) {
                        if (const auto* call = dynamic_cast<const Call*>(aa.value.get())) {
                            const auto* cn = dynamic_cast<const Name*>(call->func.get());
                            if (cn && cn->id == "extern_var") {
                                if (call->args.size() != 1 || !call->keywords.empty())
                                    pydecl::fail(aa.value->line,
                                                 "extern_var(...) expects exactly one string "
                                                 "literal argument");
                                const auto* c =
                                    dynamic_cast<const Constant*>(call->args[0].get());
                                if (!c || c->ck != Constant::CK::Str || c->s.empty())
                                    pydecl::fail(aa.value->line,
                                                 "extern_var(...) expects a non-empty string "
                                                 "literal (C symbol name)");
                                is_var = true;
                                var_symbol = c->s;
                            }
                        }
                    }
                    if (is_var) {
                        CKind array_elem = CKind::None;
                        std::string array_stname;
                        if (ct->kind == CKind::Callback)
                            pydecl::fail(aa.line, "callback type is not supported for extern var");
                        // 0.562：array[T, N] —— 数组类型的 extern 变量，暴露成符号地址（ptr）。
                        // C 里数组名本来就退化成一个指向首元素的指针，语义一致。
                        bool addr_of = false;
                        if (auto* sub = dynamic_cast<const pyadamas::Subscript*>(aa.annotation.get())) {
                            if (auto* bn = dynamic_cast<const pyadamas::Name*>(sub->value.get());
                                bn && bn->id == "array") {
                                addr_of = true;
                            }
                        }
                        // 0.564：定长数组变量 `T[N]` —— 数组名在 C 里退化成 &arr[0]，
                        // 所以暴露符号地址，元素宽度取 T。
                        if (!ct->dims.empty()) {
                            addr_of = true;
                            array_elem = ct->kind;
                            array_stname = ct->stname;
                        }
                        if (ct->kind == CKind::Struct)
                            pydecl::fail(aa.line,
                                         std::format("struct/union variable '{}' must be declared "
                                                     "with ptr[{}]", pyname, ct->stname));
                        if (!seen_py.insert(pyname).second)
                            pydecl::fail(aa.line, std::format("duplicate name '{}'", pyname));
                        CVar cv;
                        cv.symbol = var_symbol;
                        cv.py_name = pyname;
                        cv.kind = ct->kind;
                        cv.struct_name = ct->stname;
                        cv.elem = ct->elem;
                        cv.addr_of = addr_of;
                        if (addr_of) {
                            cv.kind = CKind::Ptr;
                            if (array_elem != CKind::None) cv.elem = array_elem;
                            if (!array_stname.empty()) cv.struct_name = array_stname;
                        }
                        decl.vars.push_back(std::move(cv));
                        break;
                    }
                    // 常量：值必须是字面量，且种类与声明类型一致。
                    auto lit = pydecl::literal_of(aa.value.get());
                    if (!lit)
                        pydecl::fail(aa.value->line,
                                     std::format("constant '{}' initializer must be a literal "
                                                 "(int / float / str / bool / None)", pyname));
                    CConst c;
                    c.name = pyname;
                    c.kind = ct->kind;
                    switch (ct->kind) {
                        case CKind::Int:
                        case CKind::Int32:
                        case CKind::UInt32:
                        case CKind::Int8:
                        case CKind::Int16:
                        case CKind::UInt8:
                        case CKind::UInt16:
                        case CKind::UInt64:
                            if (lit->kind != pydecl::Literal::Kind::Int)
                                pydecl::fail(aa.value->line,
                                             std::format("constant '{}' needs an integer literal",
                                                         pyname));
                            c.i = lit->i;
                            break;
                        case CKind::Float:
                            if (lit->kind == pydecl::Literal::Kind::Float) c.f = lit->f;
                            else if (lit->kind == pydecl::Literal::Kind::Int)
                                c.f = static_cast<double>(lit->i);
                            else
                                pydecl::fail(aa.value->line,
                                             std::format("constant '{}' needs a float literal",
                                                         pyname));
                            break;
                        case CKind::Bool:
                            if (lit->kind != pydecl::Literal::Kind::Bool)
                                pydecl::fail(aa.value->line,
                                             std::format("constant '{}' needs True or False",
                                                         pyname));
                            c.b = lit->b;
                            break;
                        case CKind::Str:
                            if (lit->kind != pydecl::Literal::Kind::Str)
                                pydecl::fail(aa.value->line,
                                             std::format("constant '{}' needs a string literal",
                                                         pyname));
                            c.s = lit->s;
                            break;
                        default:
                            pydecl::fail(aa.line,
                                         "const only supports int/int32/uint32/uint64/char/"
                                         "short/uchar/ushort/float/bool/str");
                    }
                    if (!seen_py.insert(pyname).second)
                        pydecl::fail(aa.line, std::format("duplicate name '{}'", pyname));
                    decl.consts.push_back(std::move(c));
                    break;
                }
                case StmtKind::Expr: {
                    auto& es = static_cast<ExprStmt&>(*st);
                    const auto* call = dynamic_cast<const Call*>(es.value.get());
                    const auto* fn =
                        call ? dynamic_cast<const Name*>(call->func.get()) : nullptr;
                    if (!call || !fn) {
                        // 旧行式语法的残留（struct X / extern def ... / cmodule x）：
                        // 给一条明确的迁移提示，别只报「表达式语句」。
                        std::string what;
                        if (const auto* n = dynamic_cast<const Name*>(es.value.get()))
                            what = n->id;
                        pydecl::fail(es.line,
                                     "statement '" + what +
                                         "' is not valid: the .cpya format is Python syntax "
                                         "now (use cmodule(\"name\"), link(\"lib\"), "
                                         "class NAME: pass, def f(...) -> T: pass, "
                                         "NAME: T = <literal>, NAME: T)");
                    }
                    if (fn->id == "cmodule") {
                        if (call->args.size() != 1 || !call->keywords.empty())
                            pydecl::fail(es.line, "cmodule(...) expects exactly one string "
                                                  "literal");
                        const auto* c = dynamic_cast<const Constant*>(call->args[0].get());
                        if (!c || c->ck != Constant::CK::Str || c->s.empty())
                            pydecl::fail(es.line, "cmodule(...) expects a module name string");
                        if (saw_cmodule) pydecl::fail(es.line, "duplicate cmodule(...)");
                        if (c->s != name)
                            pydecl::fail(es.line,
                                         std::format("cmodule '{}' does not match cimport '{}'",
                                                     c->s, name));
                        saw_cmodule = true;
                        break;
                    }
                    if (fn->id == "link") {
                        if (call->args.size() != 1 || !call->keywords.empty())
                            pydecl::fail(es.line, "link(...) expects exactly one string literal");
                        const auto* c = dynamic_cast<const Constant*>(call->args[0].get());
                        if (!c || c->ck != Constant::CK::Str || c->s.empty())
                            pydecl::fail(es.line, "link(...) expects a non-empty string literal");
                        std::string arg = normalize_link_arg(c->s);
                        // 引号给的 .c 源文件按 cwd 解析；stdlib 分发的 .cpya 不能
                        // 依赖用户 cwd：cwd 下找不到时回退到 .cpya 所在目录。
                        if (!arg.empty() && arg[0] != '-' && arg.ends_with(".c")) {
                            std::error_code ec;
                            if (!fs::exists(arg, ec)) {
                                fs::path cand = found.parent_path() / arg;
                                if (fs::exists(cand, ec)) arg = fs::absolute(cand).string();
                            }
                        }
                        if (seen_links.insert(arg).second) decl.links.push_back(arg);
                        break;
                    }
                    pydecl::fail(es.line, std::format("unknown call statement '{}()' (expected "
                                                      "cmodule(...) or link(...))", fn->id));
                }
                default:
                    pydecl::fail(st->line,
                                 std::format("unknown statement in .cpya: {} (expected "
                                             "cmodule(...), link(...), def, class, annotated "
                                             "const or extern var)",
                                             pydecl::stmt_kind_name(st->kind)));
            }
        }
    } catch (const pydecl::Error& e) {
        err = std::format("{}:{}: {}", found.string(), e.line, e.msg);
        return std::nullopt;
    }
    return decl;
}

}  // namespace pyadamas
