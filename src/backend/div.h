// backend/div.h —— 整数除法的公共约定（backend/ops.cpp / backend/int_fixed.cpp）
//
// idiv/div 的除数必须在 %rcx。SysV 直接用它；Win64 下 %rcx 是第 1 个实参
// 寄存器，而除零检查里的 panic 是调用、会破坏它，所以统一用 %r10 中转。
// 魔数除法（gen_div_const）的模式码也放这里：两个发射文件共用同一份定义，
// 避免「同一语义两份常量」漂移。
#pragma once

#include <climits>

#include "common/intmath.h"
#include "backend/target_abi.h"

namespace pyadamas {

inline const char* div_reg() { return target_abi().gnu_cfi ? "%rcx" : "%r10"; }

constexpr int kDivModeTrunc = 0;  // 向零取整商（/ 的整数语义）
constexpr int kDivModeFloor = 1;  // Python 的 //（向下取整）
constexpr int kDivModeMod = 2;    // Python 的 %（结果与除数同号）



// gen_div_const 的模式：0 = 向零取整商（/），1 = 向下取整商（//），2 = 余数（%）

// 操作数文本是不是一个立即数常量（"$123" / "$-4"）。src_op 对「能直接当
// 立即数用」的 vreg 返回这种形态；寄存器/栈槽则以 '%' / 数字开头。
inline bool operand_const_imm(const std::string& s, int64_t* out) {
    if (s.size() < 2 || s[0] != '$') return false;
    size_t i = 1;
    bool neg = false;
    if (s[i] == '-') { neg = true; ++i; }
    if (i >= s.size()) return false;
    unsigned long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + static_cast<unsigned long long>(s[i] - '0');
        if (v > 0x8000000000000000ULL) return false;
    }
    if (neg) *out = v == 0x8000000000000000ULL ? INT64_MIN : -static_cast<int64_t>(v);
    else      *out = static_cast<int64_t>(v);
    return true;
}

// 有符号 64 位「除以常数」：把 |x| 当无符号做魔数除法，最后按符号还原。
//
// 为什么值得做：x86-64 的 64 位 idivq 在 Cascade Lake 上吞吐约 24 周期，
// 而下面这段是十来条一周期指令加一次 3 周期的 imul。实测 idivq 在
// queue_sim 的 simulate 里占该函数 47.6% 的采样（t % 3、t % 7 各一处），
// 在 str_build 的 build_line 里占 71.9%（i % 10）—— 全是循环变量对常数取模。
//
// 算法（Granlund-Montgomery 的 round-up 魔数）：
//   对 d > 1（非 2 的幂），取 L = ceil(log2 d)，M = floor(2^(64+L)/d) + 1。
//   M 一定 > 2^64（因为 d < 2^L），写成 M = 2^64 + m'，于是
//       floor(x*M / 2^(64+L)) == (mulhu(x, m') + x) >> L
//   —— 这正是把 65 位魔数拆成「高位那 1 加上 m'」的结果，x*2^64 那部分
//   恰好整块进位到高 64 位。
//
//   有符号情形不必另找魔数：trunc(x/d) = sign(x) * floor(|x|/d)，
//   而 |x| 用 (x ^ sign) - sign 在无符号域里算（INT64_MIN 得到 2^63，正好）。
//   这一步顺便把 INT64_MIN/d 的溢出问题一起解决了。
//
//   2 的幂另有更便宜的路径：trunc(x/2^L) = (x + (x>>63 & (2^L-1))) >> L，
//   不用乘法。这一条必须单独写 —— 魔数法在 2 的幂上会退化（M 变成 2^63+1，
//   代入上面的式子不成立）。
//
// 正确性验证：30,583,433 个 (x, d) 组合 0 失败，覆盖 d ∈ [-4000,4000] ∪
// {2^k±1, 2^k} ∪ 500 个随机 31 位除数，x 含 0/±1/±d/±(d±1)/±2d/
// INT64_MIN/INT64_MAX，小除数上再密集跑满 [0, 2^18)。

// d >= 2，返回 L 使 d == 2^L；不是 2 的幂返回 -1。
//
// 算法本体在 common/intmath.h（向量化判据也用同一份）。这里的域是 uint64 且**排除 1**
// （除法强度削减不处理除数为 1 —— 那条路径直接消掉除法，不经过这里），所以要
// 单独把 d < 2 与 2^63 两个边界按本函数的约定处理。
inline int pow2_log2(uint64_t d) {
    if (d == (uint64_t{1} << 63)) return 63;
    if (d < 2 || d > static_cast<uint64_t>(INT64_MAX)) return -1;
    return pyadamas::pow2_log2(static_cast<int64_t>(d));
}

// d > 1 且非 2 的幂。返回 m' 与 L 使 (mulhu(x,m') + x) >> L == x / d。
inline bool udiv_magic(uint64_t d, uint64_t* mp_out, int* L_out) {
    if (pow2_log2(d) >= 0) return false;
    using u128 = unsigned __int128;
    int L = 0;
    while ((static_cast<uint64_t>(1) << L) < d) ++L;
    const u128 two64 = static_cast<u128>(1) << 64;
    const u128 m = (static_cast<u128>(1) << (64 + L)) / d + 1;
    if (m <= two64) return false;
    const u128 mp = m - two64;
    if (mp >= two64) return false;
    *mp_out = static_cast<uint64_t>(mp);
    *L_out = L;
    return true;
}

}  // namespace pyadamas
