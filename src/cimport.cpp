// cimport.cpp —— .cpya 声明文件解析（Python 语法 stub）
//
// 声明文件统一是 Python 语法（见 src/pydecl.h），例如：
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
// 解析复用编译器自己的 Lexer + Parser（src/pydecl.h），旧的行式语法已删除
// （pre-1.0，不留兼容）。类型注解是本文件特有的 C 边界类型：int/int32/uint32/
// float/float32/bool/str/char/int8/short/.../uint64/ptr/ptr[T]/Optional[str]/
// Callable[[...], R] 以及本文件声明的 struct 名。
#include "cimport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "pydecl.h"

namespace pyadamas {

std::optional<CKind> parse_ckind(const std::string& text) {
    std::string t = text;
    // 去掉两端空白（注解名来自标识符，正常没有空白；防御一下）
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
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

// 本文件声明的 struct：名字 → 布局（字段布局用，opaque 判定用）。
using StructMap = std::unordered_map<std::string, const CStruct*>;

// 注解表达式 → C 边界类型。只认 .cpya 的词汇：
//   int/int32/uint32/float/float32/bool/str/char/int8/short/int16/uchar/
//   uint8/ushort/uint16/ulong/uint64/None/ptr/ptr[T]/Optional[str]/
//   Callable[[P...], R]/<本文件 struct 名>
std::optional<CTypeRef> ctype_from_ast(const Expr* e, const StructMap& structs,
                                       std::string& why) {
    if (!e) {
        why = "missing type annotation";
        return std::nullopt;
    }
    if (const auto* c = dynamic_cast<const Constant*>(e)) {
        // -> None：无返回值（Python 里 None 是常量表达式，不是 Name）。
        if (c->ck == Constant::CK::None) return CTypeRef{CKind::None};
        why = "unsupported type annotation";
        return std::nullopt;
    }
    if (const auto* n = dynamic_cast<const Name*>(e)) {
        if (auto it = structs.find(n->id); it != structs.end())
            return CTypeRef{CKind::Struct, CKind::None, n->id,
                            it->second->fields.empty()};
        if (auto k = parse_ckind(n->id)) return CTypeRef{*k, CKind::None, {}, false, {}, CKind::None};
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
        if (auto it = structs.find(in->id); it != structs.end())
            return CTypeRef{CKind::Ptr, CKind::None, in->id, it->second->fields.empty(),
                            {}, CKind::None};
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
                auto pk = ctype_from_ast(pe.get(), structs, why);
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
        auto rk = ctype_from_ast(tup->elts[1].get(), structs, why);
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
        // ── Pass 1：class 声明 = C struct ──────────────────────────────
        // 先收集所有 struct 名，允许 ptr[Struct] 出现在声明之前；字段本身
        // 只允许定宽标量/指针（不引用 struct），所以布局可以在这一趟算完。
        for (auto& st : ast->body) {
            if (pydecl::is_docstring(*st)) continue;
            if (st->kind != StmtKind::ClassDef) continue;
            auto& cd = static_cast<ClassDef&>(*st);
            if (!is_ident(cd.name))
                pydecl::fail(cd.line, std::format("invalid struct name '{}'", cd.name));
            for (auto& d : cd.decorators)
                if (d)
                    pydecl::fail(d->line,
                                 std::format("decorators are not allowed on struct '{}'", cd.name));
            for (auto& s : decl.structs)
                if (s.name == cd.name)
                    pydecl::fail(cd.line, std::format("duplicate struct '{}'", cd.name));
            CStruct cs;
            cs.name = cd.name;
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
                for (auto& f : cs.fields)
                    if (f.name == fn->id)
                        pydecl::fail(aa.line, std::format("duplicate field '{}' in struct '{}'",
                                                          fn->id, cd.name));
                StructMap empty;  // 字段不能用 struct 名
                std::string why;
                auto ct = ctype_from_ast(aa.annotation.get(), empty, why);
                int64_t fsize = 0, falign = 1;
                if (!ct || !cfield_layout(ct->kind, fsize, falign)) {
                    const std::string fname = pydecl::annotation_base_name(aa.annotation.get());
                    pydecl::fail(aa.line,
                                 std::format("unsupported struct field type '{}' (use int, "
                                             "int32, uint32, float, bool or ptr)",
                                             fname.empty() ? (why.empty() ? "?" : why) : fname));
                }
                CStructField cf;
                cf.name = fn->id;
                cf.kind = ct->kind;
                cf.size = fsize;
                cf.align = falign;
                cs.fields.push_back(std::move(cf));
            }
            if (!cs.fields.empty()) {
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
                        auto ct = ctype_from_ast(a.annotation.get(), structs, why);
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
                        if (cp.kind == CKind::Struct && cp.opaque)
                            pydecl::fail(fd.line,
                                         std::format("struct '{}' is opaque and cannot be passed "
                                                     "by value", cp.ptr_struct));
                        ex.params.push_back(std::move(cp));
                    };
                    for (auto& a : fd.args.posonly) add_param(a);
                    for (auto& a : fd.args.args) add_param(a);
                    if (fd.args.vararg) ex.varargs = true;  // C 变参：*args 位置占位
                    std::string why;
                    auto rk = ctype_from_ast(fd.returns.get(), structs, why);
                    if (!rk)
                        pydecl::fail(fd.line, std::format("return type of '{}': {} (use -> None "
                                                          "for no return value)",
                                                          fd.name, why));
                    if (rk->kind == CKind::Callback)
                        pydecl::fail(fd.line, "returning a C function pointer is not supported");
                    if (rk->kind == CKind::Struct && rk->opaque)
                        pydecl::fail(fd.line,
                                     std::format("struct '{}' is opaque and cannot be returned "
                                                 "by value", rk->stname));
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
                    auto ct = ctype_from_ast(aa.annotation.get(), structs, why);
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
                        if (ct->kind == CKind::Callback)
                            pydecl::fail(aa.line, "callback type is not supported for extern var");
                        if (ct->kind == CKind::Struct)
                            pydecl::fail(aa.line,
                                         std::format("struct variable '{}' must be declared with "
                                                     "ptr[{}]", pyname, ct->stname));
                        if (!seen_py.insert(pyname).second)
                            pydecl::fail(aa.line, std::format("duplicate name '{}'", pyname));
                        CVar cv;
                        cv.symbol = var_symbol;
                        cv.py_name = pyname;
                        cv.kind = ct->kind;
                        cv.struct_name = ct->stname;
                        cv.elem = ct->elem;
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
