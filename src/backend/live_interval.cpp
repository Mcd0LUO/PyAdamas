// backend/live_interval.cpp —— 精确活跃区间（带洞）
//
// 设计说明见 backend/live_interval.h 与 docs/regalloc-architecture.md。
#include "backend/live_interval.h"

namespace pyadamas {

void LiveInterval::add(int64_t start, int64_t end, int vn) {
    if (end <= start) return;

    // 1) 先插入到正确位置（按 start 升序）。
    auto it = ranges.begin();
    while (it != ranges.end() && it->start < start) ++it;
    LiveRange nr{start, end, vn};
    it = ranges.insert(it, nr);

    // 2) 向后合并：相邻或重叠且值编号相同的段合成一个。
    //
    // **只有 vn 相同才合并**：vn 不同说明段之间的值定义不同（phi 合并），
    // 中间必须留洞，否则会误判「同一时刻只存一个值」。
    auto cur = it;
    while (cur + 1 != ranges.end() && cur->end >= (cur + 1)->start &&
           cur->vn == (cur + 1)->vn) {
        cur->end = std::max(cur->end, (cur + 1)->end);
        ranges.erase(cur + 1);
    }

    // 3) 向前合并。
    if (cur != ranges.begin()) {
        auto prev = cur - 1;
        if (prev->end >= cur->start && prev->vn == cur->vn) {
            prev->end = std::max(prev->end, cur->end);
            ranges.erase(cur);
        }
    }
}

bool LiveInterval::liveAt(int64_t p) const {
    // 二分查找：ranges 按 start 升序。
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), p,
        [](int64_t pos, const LiveRange& r) { return pos < r.start; });
    if (it == ranges.begin()) return false;
    --it;
    return it->contains(p);
}

int64_t LiveInterval::totalLength() const {
    int64_t n = 0;
    for (const auto& r : ranges) n += r.end - r.start;
    return n;
}

bool LiveInterval::conflictsWith(const LiveInterval& o) const {
    // 双指针扫描两个有序段列表，找任意重叠。
    size_t i = 0, j = 0;
    while (i < ranges.size() && j < o.ranges.size()) {
        if (ranges[i].overlaps(o.ranges[j])) return true;
        if (ranges[i].end <= o.ranges[j].end) ++i;
        else ++j;
    }
    return false;
}

int64_t LiveInterval::wastedByEnvelope() const {
    if (ranges.empty()) return 0;
    const int64_t env = ranges.back().end - ranges.front().start;
    return env > 0 ? env - totalLength() : 0;
}

}  // namespace pyadamas
