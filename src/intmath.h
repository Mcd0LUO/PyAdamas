// intmath.h —— 整数数学原语（无处不用的极小工具）
//
// 之所以单独成文件：这些判据被**标量代码生成**（codegen_div.h 的强度削减）和
// **向量化判据**（vect_rules.h）同时使用。以前两边各写一份，属于「同一规则两个
// 出处」—— 0.391 的故障就是这类问题的变体（规则藏在 lambda 里没人知道）。
#pragma once

#include <cstdint>

namespace pyadamas {

// k > 0 且是 2 的幂（1 = 2^0 算）。0 不算（同时是除零）。
constexpr bool is_pow2(int64_t k) { return k > 0 && (k & (k - 1)) == 0; }

// 2 的幂的指数；不是 2 的幂返回 -1。
constexpr int pow2_log2(int64_t k) {
    if (!is_pow2(k)) return -1;
    int m = 0;
    while ((int64_t{1} << m) < k) ++m;
    return m;
}

}  // namespace pyadamas
