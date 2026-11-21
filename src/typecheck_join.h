// typecheck_join.h —— 类型合并/赋值兼容/容器元素合并
//
// typecheck_types.h 的组成部分（纯代码移动，声明顺序不变）：
// 由 typecheck_types.h 末尾统一包含，不要单独 include。
#pragma once

#include "typecheck_types.h"

namespace pyadamas {

// ── 类型合并 ──────────────────────────────────────────────────────
// 两个分支的类型不同时取公共上界；同一数值域内做提升，其余情况
// 只要有一边是 Unknown 就取另一边，实在合不了就退化成 Unknown。

inline TypePtr join(const TypePtr& a, const TypePtr& b);

inline void fill_hole(const TypePtr& hole, const TypePtr& with) {
    if (!hole || hole->kind != TyKind::Unknown) return;
    if (!with || with->kind == TyKind::Unknown) return;
    // t_unknown() 是全程序共享的单例，不是「洞」。原地写它会把所有
    // unknown 一起改掉（曾把全程序的 unknown 变成某个函数类型）。
    if (hole.get() == t_unknown().get()) return;
    if (type_contains(with, hole)) return;  // 会形成环，保持 unknown
    *hole = *with;
}

/* 递归检查类型里有没有「类型洞」（可变的 Unknown，不是全局单例）。
 *
 * 裸 `list` / `set` / `dict` / `tuple` 标注解析成含 fresh_hole 的类型，
 * 这些洞等着被调用点实参或函数体用法填上。判断时要排除 t_unknown()
 * 单例 —— 那是「推断失败」，不是「待填」。 */
inline bool ty_contains_hole(const TypePtr& t, int depth) {
    if (!t || depth > 6) return false;
    if (t->kind == TyKind::Unknown) return t.get() != t_unknown().get();
    for (const auto& s : t->sub)
        if (ty_contains_hole(s, depth + 1)) return true;
    return false;
}

/* 把 `from` 里已知的容器元素类型递归填进 `to` 的类型洞里。
 *
 * 只填洞，不覆盖 `to` 已有的具体类型 —— 用户明确写的 `list[str]` 不会
 * 被推断结果改掉。 */
inline void fill_holes_recursive(const TypePtr& to, const TypePtr& from, int depth) {
    if (!to || !from || depth > 6) return;
    if (to->kind == TyKind::Unknown) {
        fill_hole(to, from);
        return;
    }
    if (to->kind != from->kind) return;
    size_t n = to->sub.size() < from->sub.size() ? to->sub.size() : from->sub.size();
    for (size_t i = 0; i < n; ++i) fill_holes_recursive(to->sub[i], from->sub[i], depth + 1);
}

inline TypePtr join_into(const TypePtr& a, const TypePtr& b) {
    if (!a) return b;
    if (!b) return a;
    // **洞（fresh_hole）参与 join 时不能退化成全局 unknown 单例。**
    //
    // 洞是「待填」，不是「推断失败」：它应该被另一边填上。原来的判据
    // `is_unknown(a) || is_unknown(b)` 对两者一视同仁，于是洞一参与 join
    // 就变成 t_unknown() 单例 —— 从此再也不能被 fill_hole 填上（fill_hole
    // 显式跳过单例），洞被销毁。
    //
    // 典型后果：
    //     out = []            # list[hole]
    //     if c: out.append(1) # 分支合并时 join(list[hole], list[int])
    //     else: out.append(2) # → 洞没了，变成 list[unknown]
    //
    // 判据：洞是「独占的、非单例的 Unknown」。单例 Unknown 仍是「信息已丢」，
    // 遇到就退化成 unknown（原逻辑保留）。
    const bool a_hole = a->kind == TyKind::Unknown && a.get() != t_unknown().get();
    const bool b_hole = b->kind == TyKind::Unknown && b.get() != t_unknown().get();
    if (a_hole && !b_hole) return b;
    if (b_hole && !a_hole) return a;
    if (is_unknown(a) || is_unknown(b)) return t_unknown();
    if (type_eq(a, b)) return a;
    if (is_num(a) && is_num(b)) {
        // 定宽整数参与合并时走 §1.2 的整数提升（取较宽、同宽无符号胜出）。
        // 其余数值组合保持旧行为（int/float 混 → float），**不要**顺手
        // 改成整数提升：那会改变既有程序的分支合并结果与生成代码。
        // P3：big 顶层（big 与任何数值合并 → big）。
        if (a->kind == TyKind::Big || b->kind == TyKind::Big) return t_big();
        if (is_fixed_int(a) || is_fixed_int(b)) {
            if (a->kind == TyKind::Float || b->kind == TyKind::Float) return t_float();
            return int_promote(a, b);
        }
        return t_float();
    }
    if (a->kind == TyKind::List && b->kind == TyKind::List)
        return mk_list(join(a->sub[0], b->sub[0]));
    if (a->kind == TyKind::Range && b->kind == TyKind::Range)
        return mk_range(join(a->sub.empty() ? t_unknown() : a->sub[0],
                             b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Set && b->kind == TyKind::Set)
        return mk_set(join(a->sub[0], b->sub[0]));
    if (a->kind == TyKind::Dict && b->kind == TyKind::Dict)
        return mk_dict(join(a->sub[0], b->sub[0]), join(a->sub[1], b->sub[1]));
    if (a->kind == TyKind::Generator && b->kind == TyKind::Generator)
        return mk_generator(
            join(a->sub.empty() ? t_unknown() : a->sub[0],
                 b->sub.empty() ? t_unknown() : b->sub[0]),
            (a->sub.size() > 1 && b->sub.size() > 1) ? join(a->sub[1], b->sub[1]) : nullptr,
            (a->sub.size() > 2 && b->sub.size() > 2) ? join(a->sub[2], b->sub[2]) : nullptr);
    if (a->kind == TyKind::Coroutine && b->kind == TyKind::Coroutine)
        return mk_coroutine(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                 b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Task && b->kind == TyKind::Task)
        return mk_task(join(a->sub.empty() ? t_unknown() : a->sub[0],
                            b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::AsyncGenerator && b->kind == TyKind::AsyncGenerator)
        return mk_async_generator(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                       b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Future && b->kind == TyKind::Future)
        return mk_future(join(a->sub.empty() ? t_unknown() : a->sub[0],
                              b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::TypeObj && b->kind == TyKind::TypeObj)
        return mk_typeobj(join(a->sub.empty() ? t_unknown() : a->sub[0],
                               b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::ClassObj && b->kind == TyKind::ClassObj) {
        // 两个不同的类对象合并后没法静态表示，只能退 unknown。
        return t_unknown();
    }
    if (a->kind == TyKind::AsyncQueue && b->kind == TyKind::AsyncQueue)
        return mk_async_queue(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                   b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::AsyncTimeout && b->kind == TyKind::AsyncTimeout) return a;
    if (a->kind == TyKind::AsyncTaskGroup && b->kind == TyKind::AsyncTaskGroup) return a;
    if (a->kind == TyKind::AsyncSemaphore && b->kind == TyKind::AsyncSemaphore) return a;
    if (a->kind == TyKind::AsyncEvent && b->kind == TyKind::AsyncEvent) return a;
    if (a->kind == TyKind::AsyncLock && b->kind == TyKind::AsyncLock) return a;
    if (a->kind == TyKind::VThread && b->kind == TyKind::VThread) return a;
    if (a->kind == TyKind::Chan && b->kind == TyKind::Chan)
        return mk_chan(join(a->sub.empty() ? t_unknown() : a->sub[0],
                            b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Exception && b->kind == TyKind::Exception) return a;
    if (a->kind == TyKind::Complex && b->kind == TyKind::Complex) return a;
    if (a->kind == TyKind::Tuple && b->kind == TyKind::Tuple) {
        if (a->variadic || b->variadic) {
            // 有一边长度不定，结果也只能是长度不定：元素取公共上界
            TypePtr acc = nullptr;
            for (auto& e : a->sub) acc = acc ? join(acc, e) : e;
            for (auto& e : b->sub) acc = acc ? join(acc, e) : e;
            return mk_tuple_rest(acc ? acc : t_unknown());
        }
        if (a->sub.size() == b->sub.size()) {
            std::vector<TypePtr> elems;
            for (size_t i = 0; i < a->sub.size(); ++i) elems.push_back(join(a->sub[i], b->sub[i]));
            return mk_tuple(std::move(elems));
        }
    }
    if (a->kind == TyKind::Optional) {
        if (b->kind == TyKind::None) return a;
        return mk_optional(join(a->sub.empty() ? t_unknown() : a->sub[0], b));
    }
    if (a->kind == TyKind::Class && b->kind == TyKind::Class) {
        // 单继承下两个类实例的公共上界就是较浅的那个；不同分支合并只能放弃
        if (!a->cls || !b->cls) return t_unknown();
        if (a->cls->is_subclass_of(b->cls)) return b;
        if (b->cls->is_subclass_of(a->cls)) return a;
        return t_unknown();
    }
    if (b->kind == TyKind::Optional) {
        if (a->kind == TyKind::None) return b;
        return mk_optional(join(a, b->sub.empty() ? t_unknown() : b->sub[0]));
    }
    // join(T, None) = Optional[T]。
    //
    // 这一行原来直接返回 unknown，于是三元表达式、if-else 合并、列表字面量
    // 里只要掺了 None 就全退化成 unknown：
    //     v = 1 if flag else None        -> unknown（应为 int | None）
    //     xs = [1, None]                 -> list[unknown]（应为 list[int | None]）
    // 而 Optional[T] 本身是**完整支持**的类型（标注路径 IR 正确、`is not None`
    // 收窄也正确），只是这条 join 没走它 —— 是实现缺口，不是设计权衡。
    //
    // 实测退化：300 万次热循环，unknown 34-36ms vs int|None 6-7ms（4.7x）。
    if (a->kind == TyKind::None) return mk_optional(b);
    if (b->kind == TyKind::None) return mk_optional(a);
    return t_unknown();
}

inline TypePtr join(const TypePtr& a, const TypePtr& b) { return join_into(a, b); }

// P4 门控（PYADAMAS_LANG_GAPS）的全局开关：assignable 是自由函数，拿不到
// TypeChecker 成员；整个进程共享同一个门控值，由 set_lang_gaps 写入。
// 门控关时恒 false，下面的 FileLike 放宽分支永不命中（asm/行为不变）。
inline bool& lang_gaps_enabled() { static bool v = false; return v; }

// 赋值的兼容性：允许 int -> float 的隐式提升，Unknown 与任何类型兼容
inline bool assignable(const TypePtr& dst, const TypePtr& src) {
    if (is_unknown(dst) || is_unknown(src)) return true;
    if (type_eq(dst, src)) return true;
    if (dst->kind == TyKind::Float &&
        (src->kind == TyKind::Int || is_fixed_int(src)))
        return true;  // int/定宽整数 → float 提升
    // ── P3：big 的隐式转换 ─────────────────────────────────────
    // int/定宽/bool → big：无信息损失的加宽（big 是格顶层）。
    // big → int/定宽：必须显式 cast（cast_i64/cast_int 截断回绕）。
    // big → float：Python int→float 隐式（有损但口径一致）。
    // float → big：必须 cast_big（int(float) 截断）。
    if (dst->kind == TyKind::Big &&
        (src->kind == TyKind::Int || is_fixed_int(src) || src->kind == TyKind::Bool))
        return true;
    if (dst->kind == TyKind::Float && src->kind == TyKind::Big) return true;
    // bool 是 int 的子类型（Python 里 True + 1 == 2）
    if (is_int_kind(dst->kind) && src->kind == TyKind::Bool) return true;
    // ── 定宽整数的隐式转换（P1）──────────────────────────────────
    //
    // 宽→窄**一律要显式 cast**（设计 §1.2：静默截断是最难查的算错）。
    // 字面量在范围内的情形不靠这里：调用点（手里有表达式）先把字面量的
    // 静态类型标成目标类型，所以那条路根本不进这条判断。
    //
    // 允许的隐式路径只有三条无信息损失的加宽：
    //   i8/i16/i32 → int（默认 int 是 i64，任何定宽有符号都放得下）
    //   窄无符号 → 宽无符号
    //   窄有符号 → 宽有符号
    // 其余组合（i32→i8、u8→i8、int→u8、u64←i64 等）要求 cast。
    if (is_fixed_int(dst) || is_int_kind(src->kind)) {
        auto di = int_info_of(dst->kind);
        auto si = int_info_of(src->kind);
        if (di && si) {
            if (di->width == 64 && di->is_signed) return true;   // → int(i64)
            if (di->width > si->width && di->is_signed == si->is_signed) return true;
            return false;
        }
    }
    if (dst->kind == TyKind::Optional)
        return src->kind == TyKind::None ||
               (dst->sub.empty() ? true : assignable(dst->sub[0], src));
    // 子类实例可以放进父类类型的槽（布局前缀相同，方法槽位相同）
    if (dst->kind == TyKind::Class && src->kind == TyKind::Class)
        return dst->cls && src->cls && src->cls->is_subclass_of(dst->cls);
    // 反向放宽：Optional[T] 当作 T 用不报错（类型收窄需要数据流分析，
    // 在这个阶段做了只会误杀）
    if (src->kind == TyKind::Optional)
        return src->sub.empty() ? true : assignable(dst, src->sub[0]);
    if (dst->kind == TyKind::List && src->kind == TyKind::List)
        return assignable(dst->sub[0], src->sub[0]);
    // range 不是 list：不能互相赋值（CPython 也是两回事）。
    if (dst->kind == TyKind::Range && src->kind == TyKind::Range)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    // 协程/生成器是只读产出，元素类型按协变放宽（coroutine[int] 可传给
    // coroutine[unknown] 的 asyncio.gather/run）。
    if (dst->kind == TyKind::Coroutine && src->kind == TyKind::Coroutine)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Generator && src->kind == TyKind::Generator)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Task && src->kind == TyKind::Task)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::AsyncGenerator && src->kind == TyKind::AsyncGenerator)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Future && src->kind == TyKind::Future)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::TypeObj && src->kind == TyKind::TypeObj) return true;
    if (dst->kind == TyKind::ClassObj && src->kind == TyKind::ClassObj) {
        ClassInfo* dc = class_obj_cls(dst);
        ClassInfo* sc = class_obj_cls(src);
        if (!dc || !sc) return true;
        return sc->is_subclass_of(dc);
    }
    if (dst->kind == TyKind::AsyncTimeout && src->kind == TyKind::AsyncTimeout) return true;
    if (dst->kind == TyKind::AsyncTaskGroup && src->kind == TyKind::AsyncTaskGroup)
        return true;
    if (dst->kind == TyKind::AsyncSemaphore && src->kind == TyKind::AsyncSemaphore)
        return true;
    if (dst->kind == TyKind::AsyncEvent && src->kind == TyKind::AsyncEvent) return true;
    if (dst->kind == TyKind::AsyncLock && src->kind == TyKind::AsyncLock) return true;
    if (dst->kind == TyKind::VThread && src->kind == TyKind::VThread) return true;
    if (dst->kind == TyKind::Chan && src->kind == TyKind::Chan)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::AsyncQueue && src->kind == TyKind::AsyncQueue)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Exception && src->kind == TyKind::Exception) return true;
    // int/float 可以隐式提升为 complex
    if (dst->kind == TyKind::Complex && src->kind == TyKind::Complex) return true;
    if (dst->kind == TyKind::Complex && is_num(src)) return true;
    if (dst->kind == TyKind::Set && src->kind == TyKind::Set)
        return assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Dict && src->kind == TyKind::Dict)
        return assignable(dst->sub[0], src->sub[0]) && assignable(dst->sub[1], src->sub[1]);
    if (dst->kind == TyKind::Tuple && src->kind == TyKind::Tuple) {
        if (dst->variadic && src->variadic)
            return dst->sub.empty() || src->sub.empty() ||
                   assignable(dst->sub[0], src->sub[0]);
        // 长度不定的一端按元素类型逐个核对，不因为「长度不同」直接报错
        if (dst->variadic) {
            for (auto& s : src->sub)
                if (!assignable(dst->sub[0], s)) return false;
            return true;
        }
        if (src->variadic) {
            for (auto& d : dst->sub)
                if (!assignable(d, src->sub[0])) return false;
            return true;
        }
        if (dst->sub.size() == src->sub.size()) {
            for (size_t i = 0; i < dst->sub.size(); ++i)
                if (!assignable(dst->sub[i], src->sub[i])) return false;
            return true;
        }
    }
    // 容器字面量的空类型洞
    if (dst->kind == TyKind::Tuple && src->kind == TyKind::Tuple && src->sub.empty()) return true;
    // P4（lang_gaps）file 协议泛化：file 槽收带 read/write 方法面的类实例
    // （FileLike，io.StringIO/BytesIO 等）。运行期 pa_file_* 入口按值分派
    // （真 file 直调 / 实例走方法名→槽登记表），静态类型仍是 file。
    if (lang_gaps_enabled() && dst->kind == TyKind::File &&
        src->kind == TyKind::Class && src->cls &&
        src->cls->method("read") && src->cls->method("write"))
        return true;
    return false;
}

// 开放容器扩宽元素类型。若扩宽结果指回容器自身（x[0] = x），
// 就退化成 unknown，避免形成类型环。
inline void widen_slot(TypePtr& slot, const TypePtr& container, const TypePtr& add) {
    TypePtr merged = join(slot, add);
    if (type_contains(merged, container)) {
        slot = t_unknown();
        return;
    }
    slot = std::move(merged);
}

// 把一个值并进容器的元素槽位。返回值 true 表示兼容（或已经扩宽），
// false 表示不兼容且容器被标注锁住，由调用方报错。
//
// 关键一条：来源是 unknown 时也要把槽位退化成 unknown。否则
// `iters = [None] * n; iters[0] = gs[i]().__next__`（值未知）之后，
// 槽位还停在 None 上，下一句 iters[i]() 就报「None 不可调用」。
inline bool merge_elem(TypePtr& slot, const TypePtr& container, const TypePtr& value, bool locked) {
    if (is_unknown(value)) {
        if (!locked && !is_unknown(slot)) slot = t_unknown();
        return true;
    }
    if (assignable(slot, value)) {
        if (is_unknown(slot)) fill_hole(slot, value);
        // 槽位具体、值带洞（d[k] = {} / xs.append({}) 这类空字面量）：
        // 把槽位类型填进值的洞，否则空容器按 unknown 种类建表，str 键
        // 退化成指针哈希，内容相同的键查不回来（configparser 实测）。
        else if (value && ty_contains_hole(value, 0))
            fill_holes_recursive(value, slot, 0);
        return true;
    }
    if (!locked) widen_slot(slot, container, value);
    return !locked;  // 扩宽过就算兼容，只有标注锁住时才是真错误
}


// ── 所有权契约访问器 ──────────────────────────────────────────
// Type::cls / Type::st 是裸指针，指向 TypeChecker 拥有的对象。正常路径
// 下 TypeChecker 一定活得更久（ModuleUnit 同时持有 tc 和 ir）。但契约
// 一旦被破坏就是 use-after-free，而且普通构建下读到的垃圾指针很难追。
//
// 这几个访问器在 PYADAMAS_CHECK_TYPE_OWNERSHIP 打开时校验 alive 标志；
// 默认（生产）构建里是纯内联指针读取，零开销。
#if defined(PYADAMAS_CHECK_TYPE_OWNERSHIP)
#include <cstdio>
#include <cstdlib>
inline void pa_assert_type_alive(const void* p, bool alive, const char* what) {
    if (p && !alive) {
        std::fprintf(stderr,
                     "pyadamas: internal error: type owner accessed after its "
                     "TypeChecker was destroyed (use-after-free of %s)\n",
                     what);
        // 必须立即终止：这个访问器要返回的正是已经析构的对象指针，继续
        // 执行就是实实在在的 use-after-free。只打日志等于把断言降级成
        // 一行可能被 CI 日志淹没的输出。
        std::abort();
    }
}
inline ClassInfo* type_cls(const TypePtr& t) {
    if (!t) return nullptr;
    pa_assert_type_alive(t->cls, t->cls ? t->cls->alive : true, "ClassInfo");
    return t->cls;
}
inline StructInfo* type_struct(const TypePtr& t) {
    if (!t) return nullptr;
    pa_assert_type_alive(t->st, t->st ? t->st->alive : true, "StructInfo");
    return t->st;
}
#else
inline ClassInfo* type_cls(const TypePtr& t) { return t ? t->cls : nullptr; }
inline StructInfo* type_struct(const TypePtr& t) { return t ? t->st : nullptr; }
#endif

}  // namespace pyadamas
