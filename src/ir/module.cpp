#include "ir/module.h"
#include <cctype>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

namespace pyadamas {
namespace {

constexpr char kMagic[8] = {'P', 'Y', '2', '3', 'I', '\0', '\0', '\0'};

// 类型编码的递归深度上限：容器嵌套/自引用类型理论上可以无限深，超过就按
// unknown 收口（解码端把 tag 0 读成 nullptr = unknown，语义上安全）。
constexpr int kMaxEncTypeDepth = 64;

struct Enc {
    std::string buf;
    // 循环进口的递归保护（3.13）：函数类型可以互相引用（回调、自引用签名，
    // 例如 threading 里的 lambda/装饰器包装），enc_sig ↔ enc_type 会成环。
    // 在环上第二次遇到同一个 FuncInfo 时按「签名未知」编码 —— 循环依赖本身
    // 合法，接口必须能终止，不能靠拒绝。
    std::vector<const FuncInfo*> sig_stack;
    int type_depth = 0;
    // 编码期当前模块名（判断类引用是本模块还是别的模块）。
    std::string module_name;

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
    // 接口里类按 (归属模块, 类名) 引用；类表解码完之前先登记，最后统一解析。
    // module 为空表示「本接口的模块」（tag 13 的旧口径）。
    struct PendingClass {
        TypePtr ty;
        std::string module;
        std::string name;
    };
    std::vector<PendingClass> pending_classes;
    // 0.543：struct 段在 globals 之后解码，globals 里的 tag 27/28 先登记、末尾回填 Type::st。
    std::vector<std::pair<TypePtr, std::string>> pending_structs;
    // 0.540：跨模块类引用（tag 29）要靠它去取别的模块的接口。
    std::function<ModuleInfoPtr(const std::string&)> loader;
    std::unordered_map<std::string, ModuleInfoPtr> loaded;
    std::set<std::string> loading;  // 环保护：正在加载中的模块名

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

// 默认值的公共编码：0 无默认，1..5/7 字面量，6 有默认但不可跨模块表示，
// 8 u64 范围整数字面量（位模式；旧解码器按 6 的口径降级）。
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
        case LiteralValue::Kind::Int:
            // tag 8：u64 范围大字面量默认值（位模式）。旧解码器对 >7 的
            // tag 按「有默认但不可跨模块表示」降级，不会误读。
            if (v.u64) { e.u8(8); e.i64(v.i); }
            else { e.u8(3); e.i64(v.i); }
            break;
        case LiteralValue::Kind::Float: e.u8(4); e.f64(v.f); break;
        case LiteralValue::Kind::Str: e.u8(5); e.str(v.s); break;
        case LiteralValue::Kind::Bytes: e.u8(7); e.str(v.s); break;
        // P3：big 默认值按十进制串序列化（tag 9；旧解码器对 >8 降级）。
        case LiteralValue::Kind::Big: e.u8(9); e.str(v.s); break;
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
    if (tag >= 1 && tag <= 9) {
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
            case 8:
                v.kind = LiteralValue::Kind::Int;
                v.i = d.i64();
                v.u64 = true;
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
            case 9:
                v.kind = LiteralValue::Kind::Big;
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
    // 已经在这个签名的展开栈上：环上再展开就无限递归（3.13，check --imports
    // 在 sched/random/threading 上栈溢出）。按「签名未知」收口，调用方拿到的
    // 是 unknown 调用约定 —— 宁可漏报，不可崩。
    bool cyclic = false;
    if (fn) {
        for (auto* f : e.sig_stack)
            if (f == fn.get()) cyclic = true;
        if (!cyclic) e.sig_stack.push_back(fn.get());
    }
    if (cyclic) {
        // 编码成「unknown 签名」：与 fn == nullptr 同形，但保留位置/关键字
        // 计数为 0 —— 解码端按 unknown 处理。
        e.u32(0);
        e.u32(0);
        e.u32(0);
        e.u8(0);
        e.u8(0);
        e.u8(0);
        e.u8(0);
        return;
    }
    struct PopSig {
        Enc& e;
        bool armed;
        ~PopSig() {
            if (armed) e.sig_stack.pop_back();
        }
    } pop{e, fn != nullptr};
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
        e.u8(p.ann_unknown ? 1 : 0);
    }
    e.u32(static_cast<uint32_t>(fn->kwonly.size()));
    for (auto& p : fn->kwonly) {
        e.str(p.name);
        enc_type(e, p.ty);
        enc_default(e, p);
        e.u8(p.ann_unknown ? 1 : 0);
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
        p.ann_unknown = d.u8() != 0;
        // Unknown 在 enc_type 里与 nullptr 同形（tag 0）；显式 object/Any
        // 标注解码后必须恢复成 unknown，调用点才会按 unknown 域装箱 float。
        if (p.ann_unknown && !p.ty) p.ty = t_unknown();
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
        p.ann_unknown = d.u8() != 0;
        if (p.ann_unknown && !p.ty) p.ty = t_unknown();
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
    // 深度上限（3.13）：类型图里可能出现自引用（容器套自己、函数签名互相
    // 引用）。超过上限按 unknown 收口，接口编码必须能终止。
    if (e.type_depth >= kMaxEncTypeDepth) {
        e.u8(0);
        return;
    }
    ++e.type_depth;
    struct PopDepth {
        Enc& e;
        ~PopDepth() { --e.type_depth; }
    } pop_depth{e};
    if (!t) {
        e.u8(0);
        return;
    }
    switch (t->kind) {
        // 联合（Unknown + 成员表）不能退化成裸 unknown（0.527 P0-B）：跨模块
        // 形参的 `Value | float` 丢掉成员表后，调用点会按第一个实参反推类型，
        // 轻则误报「used with both float and non-float values」，重则 ABI 错位。
        // tag 25 = 联合，成员依次编码；0..24 逐字沿用，旧 modcache 解码不变。
        // 0.544：buffer 有 tag 37（元素类型 + 元素宽度）。
        case TyKind::Buffer:
            e.u8(37);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            e.u8(static_cast<uint8_t>(t->celem));
            break;
        case TyKind::Unknown:
            if (!t->sub.empty()) {
                e.u8(25);
                e.u8(static_cast<uint8_t>(t->sub.size()));
                for (auto& m : t->sub) enc_type(e, m);
            } else {
                e.u8(0);
            }
            break;
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
        // 类对象（构造器）跨模块保留：tag 26 + 内部 Class 引用（tag 13）。
        // 导入方解码后拿到 ClassObj，才能正确区分「类」与「实例」。
        case TyKind::ClassObj:
            e.u8(26);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        case TyKind::TypeObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:
        case TyKind::Complex:
        // 0.544：值/句柄类型各有 tag（30..45），不再退化成 unknown。
        // 0.544 修正：Deque **不**跨模块（tag 30 停用）。开它会让
        //   已排除的假设（0.549 调查）：
        //   - 编解码不对称：❌ 最小用例 `d: deque[int]` 全局 + `def f(x: deque[int])`
        //     跨模块正常工作（`len(d)` / `f(d)` 都对），enc/dec 的 tag 30 + 元素类型
        //     是严格对称的；
        //   - `isinstance(x, (types.CoroutineType, collections.abc.Coroutine))`：❌ 单独
        //     复现**不**触发（ok），必须整文件 `check --imports asyncio/coroutines.py`；
        //   - **依赖链二分（0.551）**：把 tag 30 打开后逐个 `check --imports` 直接依赖：
        //     `collections/__init__.py` / `inspect.py` / `os.py` / `sys/__init__.py` /
        //     `types.py` 都没有 isinstance 报错（只有各自的无关错误，如
        //     `_collections_abc.py:89 sys._getframe` 未建模）；只有
        //     `asyncio/coroutines.py` 复现 ⇒ 与该文件的 import 图无关，是**文件自身**
        //     的 `_COROUTINE_TYPES` 元组在 tag 30 打开后元素类型退化。
        //   - 仍然成立的现象：只有 `asyncio/coroutines.py` 报「second argument must be a
        //     class」，怀疑与 asyncio 的 import 图里某个模块的接口在 Deque tag 开启后
        //     解出的类表被破坏有关（下一步：对 asyncio 的 import 做子集二分）。
        // `asyncio/coroutines.py` 的 `isinstance(obj, _COROUTINE_TYPES)` 报
        // 「second argument must be a class」——该文件里 tuple 元素类型
        // （`types.CoroutineType` / `collections.abc.Coroutine`）经 imports 接口
        // 解析后不再是类。根因待查，先保持 unknown（与 0.543 之前一致）。
        case TyKind::Deque:
            e.u8(0);
            break;
        case TyKind::DateTime: e.u8(31); break;
        case TyKind::TimeDelta: e.u8(32); break;
        case TyKind::Path: e.u8(33); break;
        case TyKind::Regex: e.u8(34); break;
        case TyKind::Match: e.u8(35); break;
        case TyKind::Popen: e.u8(36); break;
        case TyKind::StructTime: e.u8(38); break;
        case TyKind::UnameInfo: e.u8(39); break;
        case TyKind::FloatInfo: e.u8(40); break;
        case TyKind::SysFlags: e.u8(41); break;
        case TyKind::StatResult: e.u8(42); break;
        case TyKind::CompletedProcess: e.u8(43); break;
        case TyKind::Random: e.u8(44); break;
        case TyKind::Range:  // 45：带元素类型
            e.u8(45);
            enc_type(e, t->sub.empty() ? nullptr : t->sub[0]);
            break;
        // 以下**刻意保持 unknown**（跨模块传裸句柄会 UAF 或语义不安全）：
        //   File   —— 文件句柄绑定进程内 fd；
        //   DYLIB  —— 动态库句柄；
        //   VThread/Chan —— 运行期线程/通道句柄，跨模块会悬垂。
        case TyKind::File:
        // 0.543：用户 struct 按名字导出（27 = 值、28 = 指针）。.cpya 的 c_struct 继续按
        // unknown —— 布局由 C 头决定，导入方不该按值语义重建。
        case TyKind::Struct:
            if (t->st && !t->st->c_struct) { e.u8(27); e.str(t->st->name); } else e.u8(0);
            break;
        case TyKind::StructPtr:
            if (t->st && !t->st->c_struct) { e.u8(28); e.str(t->st->name); } else e.u8(0);
            break;
        // dylib 句柄不跨模块传递：按 unknown 处理。
        case TyKind::DYLIB:
        // vthread handle / chan 跨模块签名暂不支持：按 unknown 处理。
        case TyKind::VThread:
        case TyKind::Chan:
            e.u8(0);
            break;
        case TyKind::Bytes: e.u8(12); break;
        // 定宽整数（P1）：跨模块保留宽度/符号，编码 17..23 为新码
        // （0..16 逐字沿用，旧 modcache 解码不受影响）。
        case TyKind::I8: e.u8(17); break;
        case TyKind::I16: e.u8(18); break;
        case TyKind::I32: e.u8(19); break;
        case TyKind::U8: e.u8(20); break;
        case TyKind::U16: e.u8(21); break;
        case TyKind::U32: e.u8(22); break;
        case TyKind::U64: e.u8(23); break;
        // P3：big 跨模块编码 24（0..23 逐字沿用，旧 modcache 解码不变）。
        case TyKind::Big: e.u8(24); break;
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
            // 0.540：别的模块的类按 (归属模块, 类名) 引用（tag 29）—— 只写裸名
            // 时导入方只能在自己的类表里找，`contextlib._GeneratorContextManager`
            // 这类必然解析失败退化成 unknown。本模块的类仍走 tag 13（逐字沿用）。
            if (t->cls && !t->cls->origin_module.empty() &&
                t->cls->origin_module != e.module_name) {
                e.u8(29);
                e.str(t->cls->origin_module);
                e.str(t->cls->name);
            } else {
                e.u8(13);
                e.str(t->cls ? t->cls->name : "");
            }
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
        case 17: return t_i8();
        case 18: return t_i16();
        case 19: return t_i32();
        case 20: return t_u8();
        case 21: return t_u16();
        case 22: return t_u32();
        case 23: return t_u64();
        case 24: return t_big();
        case 14: {
            TypePtr y = dec_type(d);
            TypePtr s = d.u8() ? dec_type(d) : nullptr;
            TypePtr r = d.u8() ? dec_type(d) : nullptr;
            return mk_generator(y, s, r);
        }
        case 15: return mk_coroutine(dec_type(d));
        case 16: return mk_async_generator(dec_type(d));
        case 25: {
            uint8_t n = d.u8();
            if (n == 0) return t_unknown();
            // 直接构造联合，**不走 mk_union**：类引用先解码成 Unknown 占位（tag 13），
            // mk_union / union_flatten 会把占位当成 unknown，把整个联合退化成裸
            // unknown —— 那正是 P0-B 要修的丢信息。占位稍后由 pending_classes
            // 原地填成 Class，成员表始终有效。
            std::vector<TypePtr> members;
            members.reserve(n);
            for (uint8_t i = 0; i < n; ++i) members.push_back(dec_type(d));
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Unknown;
            t->sub = std::move(members);
            return t;
        }
        case 26: return mk_class_obj(dec_type(d));
        case 27:
        case 28: {
            std::string nm = d.str();
            TypePtr t = std::make_shared<Type>();
            t->kind = tag == 27 ? TyKind::Struct : TyKind::StructPtr;
            d.pending_structs.emplace_back(t, nm);
            return t;
        }
        case 13: {
            // 本模块类引用：先占位，等模块类表解码完再填 cls。
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Unknown;
            Dec::PendingClass pc;
            pc.ty = t;
            pc.name = d.str();
            d.pending_classes.push_back(std::move(pc));
            return t;
        }
        // 0.544：值/句柄类型 tag 30..45（与 enc_type 一一对应）。
        case 30: return mk_deque(dec_type(d));
        case 31: return new_ty(TyKind::DateTime);
        case 32: return new_ty(TyKind::TimeDelta);
        case 33: return new_ty(TyKind::Path);
        case 34: return t_regex();
        case 35: return t_match();
        case 36: return t_popen();
        case 37: {
            auto buf = mk_buffer(dec_type(d));
            buf->celem = static_cast<CKind>(d.u8());
            return buf;
        }
        case 38: return t_struct_time();
        case 39: return t_uname();
        case 40: return t_float_info();
        case 41: return t_sys_flags();
        case 42: return t_stat();
        case 43: return t_completed();
        case 44: return t_random();
        case 45: return mk_range(dec_type(d));
        case 29: {
            // 0.540：别的模块的类引用 —— (归属模块, 类名)，解析推迟到类表解码后。
            TypePtr t = std::make_shared<Type>();
            t->kind = TyKind::Unknown;
            Dec::PendingClass pc;
            pc.ty = t;
            pc.module = d.str();
            pc.name = d.str();
            d.pending_classes.push_back(std::move(pc));
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

// 模块名/版本号 → 符号片段：**必须单射**（不同输入绝不映射到同一片段）。
//
// 旧实现把非字母数字一律换成 '_'，于是 "a.b" 与 "a_b" 撞成同一个符号。
// 这两者可以同时出现在一个程序里（`import a.b` 与 `import a_b`），
// 结果是两个 .o 定义同名符号：链接期 duplicate symbol，或者（符号不冲突时）
// 更糟的静默串线。包名将来还会带 '-' 和 '@<版本>'，冲突面更大。
//
// 编码规则（字面 '_' 一律被转义，所以输出里每个 '_' 都开始一个定长转义码，
// 解码唯一）：
//   '_' → "_1"、'.' → "_2"、'-' → "_3"、'@' → "_5"；
//   其它非字母数字字节 → "_4" + 两位十六进制；
//   空名 → "_0"（真实编码不会产生 "_0"，仅作占位）。
// 字母数字原样保留（ASCII 判定，不依赖 locale）。
std::string mangle_component(const std::string& module) {
    if (module.empty()) return "_0";
    auto is_ascii_alnum = [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    std::string out;
    out.reserve(module.size() + 8);
    for (unsigned char c : module) {
        if (is_ascii_alnum(c)) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        switch (c) {
            case '_': out += "_1"; break;
            case '.': out += "_2"; break;
            case '-': out += "_3"; break;
            case '@': out += "_5"; break;
            default:
                out += "_4";
                out.push_back("0123456789abcdef"[c >> 4]);
                out.push_back("0123456789abcdef"[c & 0xF]);
                break;
        }
    }
    return out;
}

// 当前模块版本坐标。P0：全局常量（默认为 "0"）。
//
// 为什么先做「全局版本」而不是每包版本：版本进 mangling 是 P3 缓存与 P4
// 多版本共存的前置。先把**编码路径**（符号带版本段、缓存键含版本、接口
// 记录版本）定死，等 P2 有包坐标再把 module_version() 换成逐模块值即可，
// 不必再动符号规范。PYADAMAS_MODULE_VERSION 覆盖口只用于 CI/单测。
std::string module_version() {
    static const std::string v = [] {
        const char* e = std::getenv("PYADAMAS_MODULE_VERSION");
        if (e != nullptr && e[0] != '\0') return std::string(e);
        return std::string("0");
    }();
    return v;
}

// 符号前缀 = pyadamas$<enc(module)>$v$<enc(version)>。
// "$v$" 是版本段标记：'$' 是既有分隔符，模块名/版本的单射编码永远不含
// '$'，所以新前缀不可能和旧的 pyadamas$<enc(mod)>$g$... 互相误认。
std::string module_symbol_prefix(const std::string& module) {
    std::string out = "pyadamas$";
    out += mangle_component(module);
    out += "$v$";
    out += mangle_component(module_version());
    return out;
}

std::string module_global_symbol(const std::string& module, const std::string& name) {
    return std::format("{}$g${}", module_symbol_prefix(module), name);
}
std::string module_ready_symbol(const std::string& module) {
    return std::format("{}$ready", module_symbol_prefix(module));
}
std::string module_init_symbol(const std::string& module) {
    return std::format("{}$init", module_symbol_prefix(module));
}
std::string module_roots_symbol(const std::string& module) {
    return std::format("{}$roots", module_symbol_prefix(module));
}
std::string module_class_new_symbol(const std::string& module, const std::string& cls) {
    return std::format("{}$n${}", module_symbol_prefix(module), cls);
}
std::string module_class_vtable_symbol(const std::string& module, const std::string& cls) {
    return std::format("{}$vt${}", module_symbol_prefix(module), cls);
}
std::string module_class_own_slots_symbol(const std::string& module, const std::string& cls) {
    return std::format("{}$os${}", module_symbol_prefix(module), cls);
}
std::string module_class_method_symbol(const std::string& module, const std::string& cls,
                                       int slot) {
    return std::format("{}$ms${}${}", module_symbol_prefix(module), cls, slot);
}

std::expected<std::string, std::string> encode_module_interface(const ModuleInfo& info) {
    try {
        Enc e;
        e.module_name = info.name;  // 0.540：判断类引用是不是本模块的
        e.buf.append(kMagic, sizeof kMagic);
        e.u16(kModuleFormatVersion);
        e.u16(kModuleAbiVersion);
        e.str(info.name);
        // 路径**不进**接口：它只是诊断信息，不影响语义；写进编码会让
        // interface_hash 依赖绝对路径（换目录/换机器即变），
        // 破坏「同内容同哈希」的可复现性。解码端用调用方给的路径兜底
        // （decode_module_interface 的 source_path 参数）。
        e.str(std::string());
        e.u32(static_cast<uint32_t>(info.globals.size()));
        for (auto& [name, ty] : info.globals) {
            e.str(name);
            enc_type(e, ty);
        }
        // 0.543：用户 struct 定义段（globals 之后、classes 之前）。
        e.u32(static_cast<uint32_t>(info.structs.size()));
        for (auto& [sname, sp] : info.structs) {
            e.str(sname);
            e.u8(sp->c_union ? 1 : 0);
            e.u32(static_cast<uint32_t>(sp->fields.size()));
            for (auto& f : sp->fields) {
                e.str(f.name);
                enc_type(e, f.ty);
                e.u64(static_cast<uint64_t>(f.offset));
                e.u64(static_cast<uint64_t>(f.size));
            }
            e.u64(static_cast<uint64_t>(sp->size));
            e.u64(static_cast<uint64_t>(sp->align));
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
            // 顺序必须是**运行期布局**顺序（根 → layout base → 本类）：导入方
            // 的 field_index 就是这里的下标，顺序错了跨模块读字段会错位。
            std::vector<const FieldInfo*> layout;
            {
                std::vector<const ClassInfo*> chain;
                for (const ClassInfo* k = c.layout_base; k; k = k->base) chain.push_back(k);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    for (auto& f : (*it)->fields) layout.push_back(&f);
                for (auto& f : c.fields) layout.push_back(&f);
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
                // 0.536：装饰过的方法必须把标记带过接口。以前不带，
                // 导入方按普通方法走 vtable，槽里是装饰器的 wrapper/空槽，
                // 跨模块调用直接段错误（temp-proj/DECORATORS.md 的 J2）。
                e.u8(m.decorated ? 1 : 0);
                e.u32(static_cast<uint32_t>(m.slot));
                e.u32(static_cast<uint32_t>(m.aliases.size()));
                for (int a : m.aliases) e.u32(static_cast<uint32_t>(a));
                enc_sig(e, m.call_sig ? m.call_sig : m.sig);
            }
            e.u8(c.has_init ? 1 : 0);
            e.u8(c.complete ? 1 : 0);
            e.u8(c.open ? 1 : 0);
            e.u8(c.needs_dict ? 1 : 0);
            // 异常类标记：导入方的 except/raise 名字位置与 except as e 绑定
            // 形态（扁平异常值 / 富实例）都依赖这三项。
            e.u8(c.is_exception_class ? 1 : 0);
            e.u8(c.is_rich_exception ? 1 : 0);
            e.str(c.exc_parent);
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

std::expected<ModuleInfoPtr, std::string> decode_module_interface(
    const std::string& bytes, const std::string& source_path,
    std::function<ModuleInfoPtr(const std::string&)> loader) {
    try {
        if (bytes.size() < sizeof(kMagic) + 4 + 8)
            return std::unexpected("interface too short");
        if (std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0)
            return std::unexpected("bad interface magic");
        uint64_t want;
        std::memcpy(&want, bytes.data() + bytes.size() - 8, 8);
        uint64_t got = fnv1a64(bytes.data(), bytes.size() - 8);
        if (want != got) return std::unexpected("interface checksum mismatch");
        Dec d;
        d.p = reinterpret_cast<const unsigned char*>(bytes.data());
        d.end = d.p + bytes.size() - 8;
        d.loader = std::move(loader);
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
        // 0.543：用户 struct 段（两遍：先建占位登记进 info->structs，再填字段）。
        uint32_t ns = d.u32();
        for (uint32_t i = 0; i < ns; ++i) {
            auto sp = std::make_shared<StructInfo>();
            sp->name = d.str();
            sp->c_union = d.u8() != 0;
            uint32_t nf = d.u32();
            for (uint32_t k = 0; k < nf; ++k) {
                StructFieldInfo f;
                f.name = d.str();
                f.ty = dec_type(d);
                f.offset = static_cast<int64_t>(d.u64());
                f.size = static_cast<int64_t>(d.u64());
                sp->fields.push_back(std::move(f));
            }
            sp->size = static_cast<int64_t>(d.u64());
            sp->align = static_cast<int64_t>(d.u64());
            info->structs[sp->name] = std::move(sp);
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
                m.decorated = d.u8() != 0;
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
            cls->needs_dict = d.u8() != 0;
            cls->is_exception_class = d.u8() != 0;
            cls->is_rich_exception = d.u8() != 0;
            cls->exc_parent = d.str();
            ImportedClass ic;
            ic.cls = cls;
            ic.ctor_sig = dec_sig(d);
            ic.ctor_symbol = d.str();
            ic.module = info->name;
            ic.qualname = cls->origin_qualname;
            info->classes[name] = std::move(ic);
        }
        for (auto& [t, sname] : d.pending_structs) {
            auto it = info->structs.find(sname);
            if (it == info->structs.end() || !it->second) continue;
            t->st = it->second.get();
        }

        for (auto& pc : d.pending_classes) {
            const ModuleInfo* owner = info.get();
            if (!pc.module.empty() && pc.module != info->name) {
                // 别的模块：用 loader 取它的接口（缓存 + 环保护）。
                auto cache_it = d.loaded.find(pc.module);
                if (cache_it != d.loaded.end()) {
                    owner = cache_it->second.get();
                } else if (d.loader && !d.loading.count(pc.module)) {
                    d.loading.insert(pc.module);
                    auto loaded = d.loader(pc.module);
                    d.loading.erase(pc.module);
                    if (loaded) {
                        d.loaded[pc.module] = loaded;
                        owner = loaded.get();
                    } else {
                        owner = nullptr;
                    }
                } else {
                    owner = nullptr;  // 环：对方正在加载，交给它自己解析
                }
            }
            if (!owner) continue;
            auto it = owner->classes.find(pc.name);
            if (it == owner->classes.end() || !it->second.cls) {
                if (std::getenv("PYADAMAS_DIAG_TYPEREF"))
                    std::fprintf(stderr, "[typeref] unresolved class %s.%s\n",
                                 pc.module.empty() ? info->name.c_str() : pc.module.c_str(),
                                 pc.name.c_str());
                continue;
            }
            pc.ty->kind = TyKind::Class;
            pc.ty->cls = it->second.cls.get();
        }
        info->interface_hash = hex64(fnv1a64(bytes));
        return info;
    } catch (const std::exception& ex) {
        return std::unexpected(ex.what());
    }
}

}  // namespace pyadamas
