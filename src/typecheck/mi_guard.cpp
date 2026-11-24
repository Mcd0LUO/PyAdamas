// typecheck/mi_guard.cpp —— 多继承字段布局护栏（3.9，docs/mi-layout.md）
//
// 非主基类的字段在子类布局里会平移，而方法体的 self.<字段> 按**声明类**的
// 布局算偏移，会静默读到别人的字段。这里只记录「方法体碰过字段」并在类定义
// 处拒绝确实会算错的组合；纯 mixin / 单继承 / 主基类完全不受影响。
#include "typecheck/typecheck.h"

#include <cstdio>
#include <cstdlib>
#include <format>

namespace pyadamas {

// 方法体里出现 self.<字段> 的读或写：记在方法所属类上。只做静态访问记录。
void TypeChecker::mark_self_field(const Attribute& a) {
    if (cur_class_ && is_self_attr(a)) {
        cur_class_->methods_access_own_fields = true;
        if (std::getenv("PYADAMAS_MI_DEBUG"))
            std::fprintf(stderr, "MI mark %s\n", cur_class_->name.c_str());
    }
}

// layout base（0.504）：MRO 顺序里第一个**有字段**的基类。它的字段前缀在子类里
// 保持原偏移，所以「mixin 在前、状态基类在后」的方法体按声明类布局算偏移是对的。
// 没有带字段的基类 → 无前缀（与 0.503 相同）。C3 保序，[base]+extra 就是 MRO 里
// 基类的先后。
void TypeChecker::set_layout_base(ClassInfo& cls, ClassInfo* base,
                                  const std::vector<ClassInfo*>& extra_bases) {
    std::vector<ClassInfo*> cand;
    if (base) cand.push_back(base);
    for (ClassInfo* e : extra_bases) cand.push_back(e);
    ClassInfo* layout = nullptr;
    for (ClassInfo* b : cand)
        if (b->n_fields() > 0) {
            layout = b;
            break;
        }
    cls.layout_base = layout;
    cls.base_fields = layout ? layout->n_fields() : 0;
}

void TypeChecker::check_mi_layout(const ClassDef& c, const ClassInfo* self, ClassInfo* base,
                                  const std::vector<ClassInfo*>& extra_bases, bool open) {
    // 精确护栏：拒绝条件三条同时成立 ——
    //   ① 该非主基类的方法体碰过 self.<字段>（methods_access_own_fields）；
    //   ② 它的字段在子类布局里真的挪了位置（菱形共享祖先偏移不变 → 放行）；
    //   ③ 它不是导入类/看不全的类（那两种静态判不准，保守放过）。
    // 纯 mixin（无字段 / 方法不碰字段）永远允许；单继承与主基类不受影响。
    if (std::getenv("PYADAMAS_MI_DEBUG"))
        std::fprintf(stderr, "MI check %s base=%s extra=%zu open=%d\n", self ? self->name.c_str() : "?",
                     base ? base->name.c_str() : "?", extra_bases.size(), open ? 1 : 0);
    // 临时验证开关（0.508）：thunk 落地前不要用 —— 只有直调路径做了 delta，
    // 虚调用/基类转型仍会读错字段。仅用于本轮的阶段验收与下次调试。
    if (std::getenv("PYADAMAS_MI_UNSAFE")) return;
    if (open || !base || !self) return;
    const ClassInfo* cls = self;
    for (ClassInfo* eb : extra_bases) {
        if (!eb || eb->imported() || eb->is_open()) continue;
        if (std::getenv("PYADAMAS_MI_DEBUG"))
            std::fprintf(stderr, "MI  eb=%s fields=%zu flag=%d\n", eb->name.c_str(), eb->fields.size(),
                         eb->methods_access_own_fields ? 1 : 0);
        if (!eb->methods_access_own_fields) continue;
        bool moved = false;
        for (const ClassInfo* k = eb; k && !moved; k = k->base) {
            for (const FieldInfo& fld : k->fields) {
                int eidx = eb->field_index(fld.name);
                int pidx = base->field_index(fld.name);
                // 子类自己把同名字段放在同一偏移上也安全（C 显式声明了它）。
                if (pidx < 0) pidx = cls->field_index(fld.name);
                // pidx 是字段在「主基类 / 子类」布局里的位置；-1 表示子类布局里
                // 根本没有这个字段 —— 那方法体读到的是别的字段，同样要拒绝。
                if (eidx != pidx) {
                    moved = true;
                    break;
                }
            }
        }
        if (!moved) continue;
        err(c.line, c.col,
            std::format("base '{}' of '{}' is not the primary base and its methods "
                        "access instance fields; put {} first or make it a stateless "
                        "mixin (see docs/mi-layout.md)",
                        eb->name, cls->name, eb->name));
    }
}

// 布局实体化（0.506）：把每个带字段的非 layout base 的字段块按 MRO 顺序追加进
// 子类布局。布局布局 = [layout_base 的前缀] + [各非 layout base 的块] + [本类声明]。
// 记录每个块在子类布局里的起始下标（thunk 的 delta 与 field_index 都靠它）。
void TypeChecker::materialize_base_fields(ClassInfo& cls, ClassInfo* base,
                                          const std::vector<ClassInfo*>& extra_bases) {
    cls.base_field_offsets.clear();
    std::vector<ClassInfo*> cand;
    if (base) cand.push_back(base);
    for (ClassInfo* e : extra_bases) cand.push_back(e);
    for (ClassInfo* b : cand) {
        if (!b || b == cls.layout_base) continue;
        if (b->n_fields() == 0) continue;
        // 该基类链上的字段按「根 → 派生」顺序追加（与它自己的布局顺序一致）。
        std::vector<const ClassInfo*> chain;
        for (const ClassInfo* k = b; k; k = k->base) chain.push_back(k);
        int start = cls.n_fields();
        int appended = 0;  // 实际追加的字段数：菱形共享祖先时可能一个都不追加，
                           // 这时不能记偏移（否则 delta 会把 self 挪到错误位置）。
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (const FieldInfo& f : (*it)->fields) {
                // 同名遮蔽：子类已声明 / 已追加过的名字不再追加（Python 里
                // 子类字段遮蔽基类字段，这里保持「先到先得」）。
                if (cls.field_index(f.name) >= 0) continue;
                FieldInfo copy;
                copy.name = f.name;
                copy.ty = f.ty;
                copy.implicit = true;
                cls.fields.push_back(std::move(copy));
                ++appended;
            }
        }
        // 只有真的追加了字段才记偏移：菱形（共享祖先）时字段已在前缀里，
        // 偏移差为 0，不能把 self 挪走。
        if (appended > 0) cls.base_field_offsets[b] = start;
    }
}

void TypeChecker::dump_layout(const ClassDef& c, const ClassInfo& cls) {
    if (!std::getenv("PYADAMAS_DUMP_LAYOUT")) return;
    std::fprintf(stderr, "layout %s base=%s layout_base=%s base_fields=%d\n", c.name.c_str(),
                 cls.base ? cls.base->name.c_str() : "-",
                 cls.layout_base ? cls.layout_base->name.c_str() : "-", cls.base_fields);
    for (auto& [b, off] : cls.base_field_offsets)
        std::fprintf(stderr, "  block %s -> start %d\n", b ? b->name.c_str() : "-", off);
    for (auto& f : cls.fields)
        std::fprintf(stderr, "  field %s -> %d%s\n", f.name.c_str(), cls.field_index(f.name),
                     f.implicit ? " (block/implicit)" : "");
}

}  // namespace pyadamas
