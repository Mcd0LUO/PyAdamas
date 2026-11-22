// vect_rules.h —— 向量化判据的唯一出处
//
// 这些判据以前是 try_vect_loop 里的匿名 lambda（int_ok / float_ok），没有名字、
// 没有注释、没有测试 —— 0.391 的故障就出在这里：归纳变量 VK::J 曾经不在
// int_ok 里，于是「j 参与算术」的循环整段判 Fail，而没有任何地方记录过这条规则。
//
// 现在它们是具名、带文档、带编译期测试（vect_rules_test.cpp）的纯函数。
// 新增一条规则时：先在这里加判据 + 注释（为什么），再在测试文件里补齐真值表。
#pragma once

#include <cstdint>

namespace pyadamas::vect {

// 一个 vreg 在向量循环里的分类。
//
//   Fail    不可向量化
//   InvF    循环不变的 float（setup 期广播成一个 xmm）
//   InvI    循环不变的 int（setup 期广播）
//   InvPtr  循环不变的指针/对象（只用于基址，不参与算术）
//   J       归纳变量本身（结构身份，不是某个值）
//   Affine  j + 常量（只用于下标位移，折叠进寻址）
//   VecF    逐 lane 的 float 值
//   VecI    逐 lane 的 int 值
//   Acc     reduction 累加器的旧值
enum class VK : uint8_t { Fail, InvF, InvI, InvPtr, J, Affine, VecF, VecI, Acc };

// 整数二元算式（Add/Sub/Mul、除以 2 的幂的 // 与 %）的操作数。
//
// J 算在内：归纳变量按 VecI 参与整数算术，发射期物化成 [j, j+1]（4-lane 时
// [j..j+3]）。它的值域是任意 i64，逐 lane 加/减/乘与标量逐位一致，不需要守卫。
// Affine 不算：它是「j + 常量」这个**下标位移**中间态，不直接参与算术。
constexpr bool int_operand_ok(VK k) {
    return k == VK::InvI || k == VK::VecI || k == VK::J;
}

// 浮点二元算式（Add/Sub/Mul/Div）的操作数。
// J/Affine 不在内：归纳变量是整数，混进浮点算式要先 Convert。
constexpr bool float_operand_ok(VK k) { return k == VK::InvF || k == VK::VecF; }

// 可以折进寻址位移的下标形态（j 本身或 j+常量）。
constexpr bool index_affine_ok(VK k) { return k == VK::J || k == VK::Affine; }

// 可以承载 reduction 累加器的分类。
constexpr bool reducible_ok(VK k) { return k == VK::Acc || k == VK::VecF || k == VK::VecI; }

}  // namespace pyadamas::vect
