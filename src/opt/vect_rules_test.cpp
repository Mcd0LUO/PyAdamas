// opt/vect_rules_test.cpp —— opt/vect_rules.h / common/intmath.h 的编译期真值表。
//
// 这个翻译单元不产生任何代码，只靠 static_assert 在**每次构建**时验证判据。
// 改动任何一条规则，这里都会立刻报错或者需要同步补真值 —— 这正是 0.391 那类
// 故障（规则藏在 lambda 里、没人知道它的存在）的解药。
#include "common/intmath.h"
#include "opt/vect_rules.h"

namespace {
using pyadamas::is_pow2;
using pyadamas::pow2_log2;
using pyadamas::vect::float_operand_ok;
using pyadamas::vect::index_affine_ok;
using pyadamas::vect::int_operand_ok;
using pyadamas::vect::reducible_ok;
using pyadamas::vect::VK;

// ── 整数操作数：J 必须算（0.391 的教训）────────────────────────────
static_assert(int_operand_ok(VK::J), "归纳变量必须能参与整数算术");
static_assert(int_operand_ok(VK::VecI));
static_assert(int_operand_ok(VK::InvI));
static_assert(!int_operand_ok(VK::Affine), "Affine 是下标中间态，不直接参与算术");
static_assert(!int_operand_ok(VK::VecF), "浮点值不能进整数算式");
static_assert(!int_operand_ok(VK::InvF));
static_assert(!int_operand_ok(VK::Acc));
static_assert(!int_operand_ok(VK::InvPtr));
static_assert(!int_operand_ok(VK::Fail));

// ── 浮点操作数 ──────────────────────────────────────────────────
static_assert(float_operand_ok(VK::InvF));
static_assert(float_operand_ok(VK::VecF));
static_assert(!float_operand_ok(VK::J), "归纳变量是整数，混进浮点要先 Convert");
static_assert(!float_operand_ok(VK::VecI));
static_assert(!float_operand_ok(VK::InvI));
static_assert(!float_operand_ok(VK::Fail));

// ── 下标仿射 ────────────────────────────────────────────────────
static_assert(index_affine_ok(VK::J));
static_assert(index_affine_ok(VK::Affine));
static_assert(!index_affine_ok(VK::VecI), "算出来的值不再是仿射下标");
static_assert(!index_affine_ok(VK::InvI));

// ── reduction 承载者 ────────────────────────────────────────────
static_assert(reducible_ok(VK::Acc));
static_assert(reducible_ok(VK::VecF));
static_assert(reducible_ok(VK::VecI));
static_assert(!reducible_ok(VK::J));

// ── 2 的幂（common/intmath.h）──────────────────────────────────────────
static_assert(is_pow2(1));
static_assert(is_pow2(2));
static_assert(is_pow2(1024));
static_assert(is_pow2(int64_t{1} << 62));
static_assert(!is_pow2(0), "0 不是 2 的幂（也是除零）");
static_assert(!is_pow2(-2), "负数不算；除法路径按正除数处理");
static_assert(!is_pow2(3));
static_assert(!is_pow2(6));

static_assert(pow2_log2(1) == 0);
static_assert(pow2_log2(2) == 1);
static_assert(pow2_log2(1024) == 10);
static_assert(pow2_log2(int64_t{1} << 62) == 62);
static_assert(pow2_log2(3) == -1);
static_assert(pow2_log2(0) == -1);

}  // namespace
