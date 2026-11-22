// vect_planner.h —— 向量化器协作对象（阶段 3 第一块，逐方法搬）
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "emit_sink.h"
#include "ir.h"
#include "mach_instr.h"
#include "slot_query.h"
#include "value_query.h"
#include "vect_rules.h"

namespace pyadamas {

struct AffIdx {
    bool ok = false;
    int64_t coef = 1;
    VReg inv = kNoReg;
    int64_t off = 0;
};
constexpr int64_t kAffMaxCoef = int64_t{1} << 24;
constexpr int64_t kAffMaxOff = int64_t{1} << 24;
struct AffBase {
    std::string key;
    VReg inv = kNoReg;
    MOperand inv_src;
    MReg reg = kMRegNone;
    int64_t coef = 1;
    int64_t off_max = 0;
    int64_t off_min = 0;
};
struct AffEq {
    VReg x = kNoReg;
    VReg y = kNoReg;
    MOperand xm;
    MOperand ym;
};

// 向量值分类（try_vect_loop 的分析结果）。原为 AsmGen 的内嵌类型，阶段 3
// 第 6 步随常量归并方法搬到 VectPlanner：codegen_vect_jvec.cpp 的寄存器规划
// （同值常量归并 / 归纳变量生命周期 / 临时值线性扫描）要读同一份分类，判据
// 只有一处。codegen_vect.h 里留别名，类内 VClass 写法不变。
// InvF/InvI 是循环不变量（setup 广播）；J 是归纳变量；Affine 是 j+常量
//（只用于下标位移）；VecF/VecI 是逐 lane 计算的 2-lane 值；Acc 是
// reduction 累加器旧值；Fail 不可向量化。
// 分类枚举与判据的唯一出处在 vect_rules.h（具名 + 编译期真值表）。
struct VClass {
    vect::VK kind = vect::VK::Fail;
    int64_t off = 0;          // Affine：j+off；Convert j_src：[j+off, j+off+1]
    VReg list = kNoReg;       // VecF/VecI 来自 IndexLoad 时的 list vreg
    bool is_load = false;     // VecF/VecI 是内存装载（vs 算术）
    VReg mov_src = kNoReg;    // Mov 透明别名 / 同值常量归并的规范 vreg
    bool j_src = false;       // Convert 输入是归纳变量 j（或 j+常量）
};

class VectPlanner {
public:
    VectPlanner(EmitSink& e, ValueQuery& v, SlotQuery& s) : emit_(e), val_(v), slot_(s) {}

    // 除数是不是正的 2 的幂常量；是则写回移位量 sh。
    bool vect_div_pow2(VReg b, int* sh) const;
    // // 与 %（除数为 2 的幂常量）的 2-lane packed 发射：rd 已持有被除数，原地写回。
    void vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh);

    // ── 仿射下标的发射（阶段 3 第 3 步：纯发射，无捕获状态）──────────
    // K = coef*8*j：进循环前算一次，循环里每轮 += coef*16。
    void vect_affine_kinit(MReg k, MReg jreg, int64_t coef);
    void vect_affine_step(MReg k, int64_t coef);
    // coef==1 用 movupd 整装；coef>1 逐 lane movsd + unpcklpd（SSE2 无 gather）。
    void vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                          MReg dst, MReg scratch);
    // 写出只支持 coef==1（scatter 在角色判定处就拒绝）。
    void vect_affine_store(MReg base, MReg j, int64_t off, MReg src);

    // ── 归纳变量物化（阶段 3 第 4 步）──
    // dst = [j+off, j+off+1]：与 Convert 的 j_src 分支逐字节同一序列。
    void vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr);

    // ── 仿射下标解析（阶段 3 第 6 步：纯分析，无捕获状态）──
    // 递归解析下标表达式；depth 仅防病态深链。成功 = 可折进寻址的仿射式。
    AffIdx vect_affine_index(VReg v, const std::vector<int>& jalias,
                             const std::function<bool(VReg)>& inv_ok,
                             int depth = 0) const;
    // 规划期校验（发射前）：常量编不进 imm32/disp32 → 整段保守跳过。
    bool vect_affine_plan_ok(const std::vector<AffBase>& bases) const;

    // ── 同值常量归并（阶段 3 第 6 步）──
    // 按值把重复的 InvI/InvF 常量 vreg 接到首个 vreg 的 mov_src 上（广播
    // 寄存器只占一个）。语义等价，只为省寄存器。
    void vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls);

    // ── 仿射守卫与基址（阶段 3 第 5 步）──
    bool vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                           const MOperand& n_op, const std::string& tail);
    void vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg);
    void vect_affine_inv_eq(const AffEq& eq, const std::string& tail);

private:
    EmitSink& emit_;
    ValueQuery& val_;
    SlotQuery& slot_;
};

}  // namespace pyadamas
