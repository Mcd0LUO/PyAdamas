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

}  // namespace pyadamas
