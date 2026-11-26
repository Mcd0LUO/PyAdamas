#include "opt/vect_planner.h"
#include "backend/impl.h"
#include <algorithm>
#include <string>
#include <vector>

namespace pyadamas {
bool VectPlanner::plan_aliases(Loop& L) {
    using Access = VAccess;
    // ── 别名/依赖分析 ────────────────────────────────────────────────────
    // 按逻辑数组分组。同对象读+写：只允许「同一 (coef, off)、inv 运行期
    // 相等、读在写前」的逐元素原地 map —— 此时每个 lane 读写的都是同一个
    // 元素，跨 lane / 跨迭代都不存在依赖（coef>=1，相邻 lane 组互不相交）。
    // 其余同对象读写（偏移不同、inv 证不等）→ 保守跳过。inv 不同但可能相等
    // 的（如 c[i*N+j] 的读与写由两次等价计算得到）走运行期判等守卫。
    for (size_t ai = 0; ai < L.accesses.size(); ++ai) {
        std::string k = L.list_key(L.accesses[ai].list);
        L.by_list[k].push_back(static_cast<int>(ai));
        L.list_rep[k] = L.accesses[ai].list;
    }
    for (auto& [lv, accs] : L.by_list) L.lists.push_back(lv);
    // ── 别名 / 跨 lane 依赖分析（阶段 B5 阶段 4）：实现在
    // opt/vect_planner.cpp 的 Loop::vect_alias_plan（按 (list, coef) 分组
    // 证明跨 lane 无依赖：同组偏移差判定 + 异组运行期区间不相交守卫）。
    if (!L.vect_alias_plan()) return false;

    // 基址键 = (逻辑数组, inv)：一个 GPR 存 data + inv*8；coef>1 的访问共用
    // 一个按 coef 去重的运行期跨步计数器 K。
    for (Access& a : L.accesses) {
        int bi = -1;
        std::string k = L.list_key(a.list);
        for (size_t j = 0; j < L.bases.size(); ++j)
            if (L.bases[j].inv == a.inv && L.bases[j].key == k) { bi = static_cast<int>(j); break; }
        if (bi < 0) {
            AffBase nb;
            nb.key = k; nb.inv = a.inv;
            if (a.inv != kNoReg) nb.inv_src = L.list_base_mop(a.inv);
            nb.coef = a.coef; nb.off_max = a.off; nb.off_min = a.off;
            L.bases.push_back(nb);
            bi = static_cast<int>(L.bases.size()) - 1;
        } else {
            L.bases[static_cast<size_t>(bi)].coef = std::max(L.bases[static_cast<size_t>(bi)].coef, a.coef);
            L.bases[static_cast<size_t>(bi)].off_max = std::max(L.bases[static_cast<size_t>(bi)].off_max, a.off);
            L.bases[static_cast<size_t>(bi)].off_min = std::min(L.bases[static_cast<size_t>(bi)].off_min, a.off);
        }
        a.bidx = bi;
    }
    for (const Access& a : L.accesses)
        // coef==0 是循环内不变下标（广播装载），没有跨步，不占 K 计数器。
        if (a.coef != 1 && a.coef != 0 &&
            std::find(L.kcoefs.begin(), L.kcoefs.end(), a.coef) == L.kcoefs.end())
            L.kcoefs.push_back(a.coef);
    std::sort(L.kcoefs.begin(), L.kcoefs.end());
    if (!vect_affine_plan_ok(L.bases)) return false;

    return true;
}
}  // namespace pyadamas
