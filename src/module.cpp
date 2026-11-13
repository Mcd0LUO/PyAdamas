#include "module.h"
#include <cctype>

#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

namespace pyadamas {
namespace {

constexpr char kMagic[8] = {'P', 'Y', '2', '3', 'I', '\0', '\0', '\0'};

struct Enc {
    std::string buf;

    void u8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u64(bits);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.append(s);
    }
};

struct Dec {
    const unsigned char* p;
    const unsigned char* end;
    // 接口里类按名字引用；类表解码完之前先登记，最后统一解析。
    std::vector<std::pair<TypePtr, std::string>> pending_classes;

    void need(size_t n) {
        if (static_cast<size_t>(end - p) < n) throw std::runtime_error("truncated interface");
    }
    uint8_t u8() {
        need(1);
        return *p++;
    }
    uint16_t u16() {
        uint16_t v = u8();
        v |= static_cast<uint16_t>(u8()) << 8;
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
        return v;
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    double f64() {
        uint64_t bits = u64();
        double v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        need(n);
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

void enc_type(Enc& e, const TypePtr& t);
TypePtr dec_type(Dec& d);

// 默认值的公共编码：0 无默认，1..5/7 字面量，6 有默认但不可跨模块表示。
void enc_default_value(Enc& e, bool has_default,
                       const std::optional<LiteralValue>& lit) {
    if (!has_default) {
        e.u8(0);
        return;
    }
    if (!lit) {
        e.u8(6);
        return;
    }
    const auto& v = *lit;
    switch (v.kind) {
        case LiteralValue::Kind::None: e.u8(1); break;
        case LiteralValue::Kind::Bool: e.u8(2); e.u8(v.b ? 1 : 0); break;
        case LiteralValue::Kind::Int: e.u8(3); e.i64(v.i); break;
        case LiteralValue::Kind::Float: e.u8(4); e.f64(v.f); break;
        case LiteralValue::Kind::Str: e.u8(5); e.str(v.s); break;
        case LiteralValue::Kind::Bytes: e.u8(7); e.str(v.s); break;
    }
}

struct DecodedDefault {
    bool has = false;
    std::optional<LiteralValue> lit;
};
DecodedDefault dec_default_value(Dec& d) {
    DecodedDefault out;
    uint8_t tag = d.u8();
    out.has = tag != 0;
    if (tag >= 1 && tag <= 7) {
        LiteralValue v;
        switch (tag) {
            case 1: v.kind = LiteralValue::Kind::None; break;
            case 2:
                v.kind = LiteralValue::Kind::Bool;
                v.b = d.u8() != 0;
                break;
            case 3:
                v.kind = LiteralValue::Kind::Int;
                v.i = d.i64();
                break;
            case 4:
                v.kind = LiteralValue::Kind::Float;
                v.f = d.f64();
                break;
            case 5:
                v.kind = LiteralValue::Kind::Str;
                v.s = d.str();
                break;
            case 7:
                v.kind = LiteralValue::Kind::Bytes;
                v.s = d.str();
                break;
            default: break;
        }
        out.lit = std::move(v);
    }
    return out;
}

void enc_default(Enc& e, const Param& p) {
    enc_default_value(e, p.has_default, p.default_lit);
}

Param dec_default(Dec& d) {
    Param p;
    auto v = dec_default_value(d);
    p.has_default = v.has;
    p.default_lit = std::move(v.lit);
    return p;
}

void enc_sig(Enc& e, const FuncInfoPtr& fn) {
    if (!fn) {
        e.u32(0);
        e.u32(0);
        e.u32(0);
        e.u8(0);
        e.u8(0);
        e.u8(0);
        e.u8(0);
        return;
    }
    e.u32(static_cast<uint32_t>(fn->n_posonly));
    e.u32(static_cast<uint32_t>(fn->pos.size()));
    for (auto& p : fn->pos) {
        e.str(p.name);
        enc_type(e, p.ty);
        enc_default(e, p);
    }
    e.u32(static_cast<uint32_t>(fn->kwonly.size()));
    for (auto& p : fn->kwonly) {
        e.str(p.name);
        enc_type(e, p.ty);
        enc_default(e, p);
    }
    e.u8(fn->varargs ? 1 : 0);
    if (fn->varargs) {
        e.str(fn->vararg_name);
        enc_type(e, fn->vararg_ty);
    }
    e.u8(fn->kwargs ? 1 : 0);
    if (fn->kwargs) {
        e.str(fn->kwarg_name);
        enc_type(e, fn->kwarg_ty);
    }
    e.u8(fn->ret ? 1 : 0);
    if (fn->ret) enc_type(e, fn->ret);
    e.u8(fn->ret_known ? 1 : 0);
}

FuncInfoPtr dec_sig(Dec& d) {
    auto fn = std::make_shared<FuncInfo>();
    fn->n_posonly = d.u32();
    uint32_t npos = d.u32();
    for (uint32_t i = 0; i < npos; ++i) {
        Param p;
        p.name = d.str();
        p.ty = dec_type(d);
        Param dv = dec_default(d);
        p.has_default = dv.has_default;
        p.default_lit = std::move(dv.default_lit);
        fn->pos.push_back(std::move(p));
    }
    uint32_t nkw = d.u32();
    for (uint32_t i = 0; i < nkw; ++i) {
        Param p;
        p.name = d.str();
        p.ty = dec_type(d);
        Param dv = dec_default(d);
        p.has_default = dv.has_default;
        p.default_lit = std::move(dv.default_lit);
        p.kwonly = true;
        fn->kwonly.push_back(std::move(p));
    }
    if (d.u8() != 0) {
        fn->varargs = true;
        fn->vararg_name = d.str();
        fn->vararg_ty = dec_type(d);
    }
    if (d.u8() != 0) {
        fn->kwargs = true;
        fn->kwarg_name = d.str();
        fn->kwarg_ty = dec_type(d);
    }
    if (d.u8() != 0) fn->ret = dec_type(d);
    fn->ret_known = d.u8() != 0;
    fn->name = "<imported>";
    return fn;
}

void enc_type(Enc& e, const TypePtr& t) {
    if (!t) {
        e.u8(0);
        return;
    }
    switch (t->kind) {
        case TyKind::Unknown: e.u8(0); break;
        // 生成器/协程/异步生成器跨模块保留元素/结果类型；其余 asyncio 对象仍按 unknown。
        case TyKind::Generator: {
            e.u8(14);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            for (size_t i = 1; i < 3; ++i) {
                bool has = t->sub.size() > i && t->sub[i];
                e.u8(has ? 1 : 0);
                if (has) enc_type(e, t->sub[i]);
            }
            break;
        }
        case TyKind::Coroutine:
            e.u8(15);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::AsyncGenerator:
            e.u8(16);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::Task:
        case TyKind::Future:
        case TyKind::AsyncEvent:
        case TyKind::AsyncLock:
        case TyKind::AsyncQueue:
        case TyKind::TypeObj:
        // 类对象跨模块签名暂不序列化：按 unknown 处理。
        case TyKind::ClassObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:
        case TyKind::Complex:
        // range 跨模块接口暂不支持：按 unknown 处理
        case TyKind::Range:
        case TyKind::File:
        case TyKind::Random:
        case TyKind::CompletedProcess:
        case TyKind::Regex:
        case TyKind::Match:
        // Popen / datetime / pathlib / deque 跨模块签名暂不支持：按 unknown 处理
        case TyKind::Popen:
        case TyKind::DateTime:
        case TyKind::TimeDelta:
        case TyKind::Path:
        case TyKind::Deque:
        // struct 的布局不跨模块序列化：按 unknown 处理。
        case TyKind::Struct:
        case TyKind::StructPtr:
            e.u8(0);
            break;
        case TyKind::Bytes: e.u8(12); break;
        case TyKind::None: e.u8(1); break;
        case TyKind::Bool: e.u8(2); break;
        case TyKind::Int: e.u8(3); break;
        case TyKind::Float: e.u8(4); break;
        case TyKind::Str: e.u8(5); break;
        case TyKind::List:
            e.u8(6);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::Set:
            e.u8(7);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::Dict:
            e.u8(8);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            enc_type(e, t->sub.size() < 2 ? nullptr : t->sub[1]);
            break;
        case TyKind::Tuple:
            e.u8(9);
            e.u8(t->variadic ? 1 : 0);
            e.u32(static_cast<uint32_t>(t->sub.size()));
            for (auto& s : t->sub) enc_type(e, s);
            break;
        case TyKind::Optional:
            e.u8(10);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::Func:
            e.u8(11);
            enc_sig(e, t->fn);
            break;
        case TyKind::Class:
            // 类按名字引用，导入方在类表解码完后解析成 ClassInfo*。
            e.u8(13);
            e.str(t->cls ? t->cls->name : "");
            break;
        case TyKind::BoundMethod:
            e.u8(0);
            break;
    }
}

TypePtr dec_type(Dec& d) {
    uint8_t tag = d.u8();
    switch (tag) {
        case 0: return t_unknown();
        case 1: return t_none();
        case 2: return t_bool();
        case 3: return t_int();
        case 4: return t_float();
        case 5: return t_str();
        case 6: return mk_list(dec_type(d));
        case 7: return mk_set(dec_type(d));
        case 8: {
            TypePtr k = dec_type(d);
            TypePtr v = dec_type(d);
            return mk_dict(k, v);
        }
        case 9: {
            bool variadic = d.u8() != 0;
            uint32_t n = d.u32();
            std::vector<TypePtr> sub;
            sub.reserve(n);
            for (uint32_t i = 0; i < n; ++i) sub.push_back(dec_type(d));
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Tuple;
            t->sub = std::move(sub);
            t->variadic = variadic;
            return t;
        }
        case 10: return mk_optional(dec_type(d));
        case 11: {
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Func;
            t->fn = dec_sig(d);
            return t;
        }
        case 12: return t_bytes();
        case 14: {
            TypePtr y = dec_type(d);
            TypePtr s = d.u8() ? dec_type(d) : nullptr;
            TypePtr r = d.u8() ? dec_type(d) : nullptr;
            return mk_generator(y, s, r);
        }
        case 15: return mk_coroutine(dec_type(d));
        case 16: return mk_async_generator(dec_type(d));
        case 13: {
            // 类引用：先占位，等模块类表解码完再填 cls。
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Unknown;
            d.pending_classes.emplace_back(t, d.str());
            return t;
        }
        default: throw std::runtime_error("unknown type tag in interface");
    }
}

}  // namespace

uint64_t fnv1a64(const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(uint64_t v) { return std::format("{}", v); }

std::string module_symbol_prefix(const std::string& module) {
    std::string out;
    for (char c : module)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_');
    return out;
}

std::string module_global_symbol(const std::string& module, const std::string& name) {
    return std::format("pyadamas${}$g${}", module_symbol_prefix(module), name);
}
std::string module_ready_symbol(const std::string& module) {
    return std::format("pyadamas${}$ready", module_symbol_prefix(module));
}
std::string module_init_symbol(const std::string& module) {
    return std::format("pyadamas${}$init", module_symbol_prefix(module));
}
std::string module_roots_symbol(const std::string& module) {
    return std::format("pyadamas${}$roots", module_symbol_prefix(module));
}
std::string module_class_new_symbol(const std::string& module, const std::string& cls) {
    return std::format("pyadamas${}$n${}", module_symbol_prefix(module), cls);
}
std::string module_class_vtable_symbol(const std::string& module, const std::string& cls) {
    return std::format("pyadamas${}$vt${}", module_symbol_prefix(module), cls);
}
std::string module_class_own_slots_symbol(const std::string& module, const std::string& cls) {
    return std::format("pyadamas${}$os${}", module_symbol_prefix(module), cls);
}
std::string module_class_method_symbol(const std::string& module, const std::string& cls,
                                       int slot) {
    return std::format("pyadamas${}$ms${}${}", module_symbol_prefix(module), cls, slot);
}

std::expected<std::string, std::string> encode_module_interface(const ModuleInfo& info) {
    try {
        Enc e;
        e.buf.append(kMagic, sizeof kMagic);
        e.u16(kModuleFormatVersion);
        e.u16(kModuleAbiVersion);
        e.str(info.name);
        e.str(info.source_path);
        e.u32(static_cast<uint32_t>(info.globals.size()));
        for (auto& [name, ty] : info.globals) {
            e.str(name);
            enc_type(e, ty);
        }
        e.u32(static_cast<uint32_t>(info.classes.size()));
        for (auto& [name, ic] : info.classes) {
            const ClassInfo& c = *ic.cls;
            e.str(name);
            // 运行期限定名：导入方生成 mirror 元数据和 isinstance 比较都用它。
            e.str(ic.qualname.empty() ? info.name + "." + name : ic.qualname);
            // 完整祖先链（不含自身，MRO 顺序）：导入方据此补 mirror 元数据。
            e.u32(static_cast<uint32_t>(c.ext_ancestors.size()));
            for (auto& a : c.ext_ancestors) {
                e.str(a.module);
                e.str(a.name);
                e.str(a.qualname);
            }
            // 字段按布局顺序（根类在前）导出：导入方的字段下标与运行期偏移一致，
            // 模式匹配也能读到继承来的字段。
            std::vector<const FieldInfo*> layout;
            {
                std::vector<const ClassInfo*> chain;
                for (const ClassInfo* k = &c; k; k = k->base) chain.push_back(k);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    for (auto& f : (*it)->fields) layout.push_back(&f);
            }
            e.u32(static_cast<uint32_t>(layout.size()));
            for (auto* f : layout) {
                e.str(f->name);
                enc_type(e, f->ty);
                enc_default_value(e, f->has_default, f->default_lit);
            }
            e.u32(static_cast<uint32_t>(c.match_args.size()));
            for (auto& ma : c.match_args) e.str(ma);
            e.u32(static_cast<uint32_t>(c.methods.size()));
            for (auto& m : c.methods) {
                e.str(m.name);
                e.u8(static_cast<uint8_t>(m.kind));
                e.u32(static_cast<uint32_t>(m.slot));
                e.u32(static_cast<uint32_t>(m.aliases.size()));
                for (int a : m.aliases) e.u32(static_cast<uint32_t>(a));
                enc_sig(e, m.call_sig ? m.call_sig : m.sig);
            }
            e.u8(c.has_init ? 1 : 0);
            e.u8(c.complete ? 1 : 0);
            e.u8(c.open ? 1 : 0);
            enc_sig(e, ic.ctor_sig);
            e.str(ic.ctor_symbol);
        }
        uint64_t sum = fnv1a64(e.buf);
        e.u64(sum);
        return e.buf;
    } catch (const std::exception& ex) {
        return std::unexpected(ex.what());
    }
}

std::expected<ModuleInfoPtr, std::string> decode_module_interface(const std::string& bytes,
                                                                  const std::string& source_path) {
    try {
        if (bytes.size() < sizeof(kMagic) + 4 + 8)
            return std::unexpected("interface too short");
        if (std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0)
            return std::unexpected("bad interface magic");
        uint64_t want;
        std::memcpy(&want, bytes.data() + bytes.size() - 8, 8);
        uint64_t got = fnv1a64(bytes.data(), bytes.size() - 8);
        if (want != got) return std::unexpected("interface checksum mismatch");
        Dec d{reinterpret_cast<const unsigned char*>(bytes.data()),
              reinterpret_cast<const unsigned char*>(bytes.data()) + bytes.size() - 8, {}};
        d.p += sizeof kMagic;
        uint16_t fmt = d.u16();
        uint16_t abi = d.u16();
        if (fmt != kModuleFormatVersion || abi != kModuleAbiVersion)
            return std::unexpected(std::format("interface version mismatch: fmt {} abi {}", fmt, abi));
        auto info = std::make_shared<ModuleInfo>();
        info->name = d.str();
        std::string src = d.str();
        info->source_path = src.empty() ? source_path : src;
        uint32_t n = d.u32();
        for (uint32_t i = 0; i < n; ++i) {
            std::string name = d.str();
            info->globals[name] = dec_type(d);
        }
        uint32_t nc = d.u32();
        for (uint32_t i = 0; i < nc; ++i) {
            std::string name = d.str();
            std::string qualname = d.str();
            auto cls = std::make_shared<ClassInfo>();
            cls->name = name;
            cls->complete = true;
            cls->origin_module = info->name;
            cls->origin_qualname =
                qualname.empty() ? info->name + "." + name : std::move(qualname);
            uint32_t na = d.u32();
            for (uint32_t j = 0; j < na; ++j) {
                ClassAncestor a;
                a.module = d.str();
                a.name = d.str();
                a.qualname = d.str();
                cls->ext_ancestors.push_back(std::move(a));
            }
            uint32_t nf = d.u32();
            for (uint32_t j = 0; j < nf; ++j) {
                FieldInfo f;
                f.name = d.str();
                f.ty = dec_type(d);
                auto dv = dec_default_value(d);
                f.has_default = dv.has;
                f.default_lit = std::move(dv.lit);
                cls->fields.push_back(std::move(f));
            }
            uint32_t nma = d.u32();
            for (uint32_t j = 0; j < nma; ++j) cls->match_args.push_back(d.str());
            uint32_t nm = d.u32();
            for (uint32_t j = 0; j < nm; ++j) {
                MethodInfo m;
                m.name = d.str();
                m.kind = static_cast<MethodKind>(d.u8());
                m.slot = static_cast<int>(d.u32());
                uint32_t nal = d.u32();
                for (uint32_t k = 0; k < nal; ++k)
                    m.aliases.push_back(static_cast<int>(d.u32()));
                m.call_sig = dec_sig(d);
                m.sig = m.call_sig;
                cls->methods.push_back(std::move(m));
            }
            for (size_t mi = 0; mi < cls->methods.size(); ++mi) {
                MethodInfo& m = cls->methods[mi];
                if (!m.is_accessor()) continue;
                auto& slot = cls->properties[m.name];
                slot.owner = cls.get();
                switch (m.kind) {
                    case MethodKind::PropertyGetter: slot.getter = static_cast<int>(mi); break;
                    case MethodKind::PropertySetter: slot.setter = static_cast<int>(mi); break;
                    case MethodKind::PropertyDeleter: slot.deleter = static_cast<int>(mi); break;
                    default: break;
                }
            }
            cls->has_init = d.u8() != 0;
            cls->complete = d.u8() != 0;
            cls->open = d.u8() != 0;
            ImportedClass ic;
            ic.cls = cls;
            ic.ctor_sig = dec_sig(d);
            ic.ctor_symbol = d.str();
            ic.module = info->name;
            ic.qualname = cls->origin_qualname;
            info->classes[name] = std::move(ic);
        }
        for (auto& [t, cname] : d.pending_classes) {
            auto it = info->classes.find(cname);
            if (it == info->classes.end() || !it->second.cls) continue;
            t->kind = TyKind::Class;
            t->cls = it->second.cls.get();
        }
        info->interface_hash = hex64(fnv1a64(bytes));
        return info;
    } catch (const std::exception& ex) {
        return std::unexpected(ex.what());
    }
}

}  // namespace pyadamas
