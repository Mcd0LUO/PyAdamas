// irdump —— IR 结构可视化辅助（离线工具，不进编译器主流程）
//
// 用途：把枚举编号对回名字。调试优化遍时最耗时的一步就是这个 ——
// 实测踩过的坑：
//   1. 日志打出 op=36，不知道那是 CallRt（IrOp 是 uint8_t 枚举，只有数值）；
//   2. 日志打出 rt=58，以为是 RcMark，实际是 RcInc（RtFn 有 200+ 项）；
//   3. 块号连续不等于 CFG 连续（内联后 for.body 会 branch 到很远的块）。
//
// 用法：
//   tools/irdump ops            # 全部 IrOp
//   tools/irdump rt             # 全部 RtFn
//   tools/irdump rt 58 79       # 查指定 rt 编号（含前后各 3 个邻居）
//   tools/irdump op 36          # 查指定 op 编号
//
// 编译（不进 CMake，避免污染主构建）：
//   g++ -std=c++23 -I src tools/irdump.cpp -o /tmp/irdump
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ir/ops.h"

namespace {

using namespace pyadamas;

// irop_name / rt_name 越界都返回 "?"。靠「连续未知名」判断枚举结束。
constexpr int kUnknownRunLimit = 2;

bool named(const char* n) { return n && *n && std::strcmp(n, "?") != 0; }

template <typename E>
std::vector<std::pair<int, std::string>> collect(const char* (*name_fn)(E), int limit) {
    std::vector<std::pair<int, std::string>> out;
    int unknown_run = 0;
    for (int i = 0; i < limit; ++i) {
        const char* n = name_fn(static_cast<E>(i));
        if (!named(n)) {
            if (++unknown_run >= kUnknownRunLimit) break;
            continue;
        }
        unknown_run = 0;
        out.emplace_back(i, n);
    }
    return out;
}

void print_table(const char* title, const std::vector<std::pair<int, std::string>>& rows) {
    std::printf("=== %s (%zu 项) ===\n", title, rows.size());
    for (const auto& r : rows) std::printf("%4d  %s\n", r.first, r.second.c_str());
}

void print_nearby(const char* title, const std::vector<std::pair<int, std::string>>& rows,
                  int want) {
    std::printf("=== %s: %d 附近 ===\n", title, want);
    bool hit = false;
    for (const auto& r : rows) {
        if (r.first >= want - 3 && r.first <= want + 3) {
            std::printf("%4d  %s%s\n", r.first, r.second.c_str(),
                        r.first == want ? "   <== 命中" : "");
            if (r.first == want) hit = true;
        }
    }
    if (!hit) std::printf("  （该编号未定义）\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "用法:\n"
                     "  irdump ops            列出全部 IrOp 编号\n"
                     "  irdump rt             列出全部 RtFn 编号\n"
                     "  irdump op <n>...      查 IrOp 编号附近\n"
                     "  irdump rt <n>...      查 RtFn 编号附近\n");
        return 2;
    }
    const std::string mode = argv[1];
    // IrOp 是 uint8_t，上限 256；RtFn 是 uint16_t，上限取 1024。
    if (mode == "ops") {
        print_table("IrOp", collect(&irop_name, 256));
    } else if (mode == "rt") {
        auto rows = collect(&rt_name, 1024);
        if (argc == 2) {
            print_table("RtFn", rows);
        } else {
            for (int i = 2; i < argc; ++i) print_nearby("RtFn", rows, std::atoi(argv[i]));
        }
    } else if (mode == "op") {
        auto rows = collect(&irop_name, 256);
        for (int i = 2; i < argc; ++i) print_nearby("IrOp", rows, std::atoi(argv[i]));
    } else {
        std::fprintf(stderr, "未知模式: %s\n", mode.c_str());
        return 2;
    }
    return 0;
}
