// codegen_vect_affine.cpp —— 通用循环向量化的仿射下标（P1）
//
// 设计见 docs/vectorization-generalization.md §「P1 · 仿射下标分析」。
// codegen_vect.cpp 原来的下标模型只有 `j + 常量`；本文件把它扩成
//
//     coef * j + inv + off       coef > 0 编译期常量
//                                inv 循环不变 int 值（可为 kNoReg）
//                                off 编译期常量 >= 0
//
// 四块，全部只依赖 IR 结构（j-alias 槽 / Add / Mul 常量），不含任何基准名
// 或基准里的字面量 —— 0.384 的硬政策：禁止针对基准特化。
//
// 1) 分析（vect_affine_index）：递归解析下标表达式。j → {1,无,0}；
//    Add(x, 常量) → off 累加；Add(x, 不变量) → inv（最多一个运行期项）；
//    Mul(常量 C, x)（x 无 inv）→ {coef*C, 无, off*C}。inv 非空时不做 Mul
//    折叠 —— 那是运行期乘法，不能折进寻址。
// 2) 守卫（vect_affine_guard）：最后一个 lane 的下标必须 < len，即
//    coef*(n-1) + inv + off < len；同时要求最小下标 >= 0
//    （inv + off_min >= 0 —— 否则会落到 Python 的负索引/从尾部索引语义）。
//    每步算术都用 jo 检查溢出，任何一步溢出/越界 → 跳标量尾：宁可保守失败。
// 3) 基址/跨步计数器：coef==1 用 (base, JREG, 8, off*8)，base 在循环入口
//    算一次 data + inv*8（一条 leaq）；coef>1 时 x86 寻址 scale 只到 8，
//    故维护运行期跨步计数器 K = coef*8*j（每轮 += coef*16），装载用两次
//    movsd + unpcklpd 打包成 2-lane（SSE2 没有 gather）。
// 4) 只给 IndexLoad 做 coef>1。IndexStore 仍要 coef==1：写出没有 packed
//    scatter 可用，这轮明确不做（把装载先做完整，风险最小）。
//
// 前提（调用方 codegen_vect.cpp 在发射前证完）：循环内无调用、无
// store.global；同对象读写必须同 (coef, off) 且 inv 运行期相等（不等 →
// 入口守卫跳标量）、读在写前；data/len/kind 在循环期间不变。
//
// 阶段 3 第 6 步：本文件的实现已全部搬进 VectPlanner（src/vect_planner.cpp），
// 这里只留设计说明（AsmGen 侧的转发声明在 codegen_vect.h）。
#include "codegen_impl.h"

#include <climits>

namespace pyadamas {

// ── 1) 分析 ────────────────────────────────────────────────────────────
// 解析结果用「coef*j + inv + off」三元组表示，其中最多一个运行期项 inv。
// 常量解析成 {0, 无, c}：这样 Add 规则自然把常量折进 off，不用特判。
// （vect_affine_index / vect_affine_plan_ok 的实现已搬到 VectPlanner。）

// ── 2) 守卫 ────────────────────────────────────────────────────────────
// 末 lane 下标 = coef*(n-1) + inv + off，必须 >= 0 且 < len。
// 循环入口已保证 j >= 0 且 j+1 < n（n >= 2），coef > 0 → 下标随 j 单调增，
// 所以「最小下标 = inv + off_min（j=0 处取得下界）」与「最大下标 =
// coef*(n-1) + inv + off_max」就是全部需要守的东西。

// ── 3) 基址 / 跨步计数器 / 装载 / 存储 ────────────────────────────────
// base = data + inv*8。inv 折进基址后，coef==1 的访问就是原来的
// (base, JREG, 8, off*8)，coef>1 的访问则只差一个跨步计数器。

// 同对象读写的 inv 判等守卫：只有两个下标序列完全相同（inv 运行期相等）
// 时才是逐元素原地 map；不等 → 标量（跨迭代依赖证不出来）。

}  // namespace pyadamas
