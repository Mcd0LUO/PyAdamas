// vect_planner.h —— 向量化器协作对象（阶段 3 第一块，逐方法搬）
#pragma once

#include <cstddef>
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

// 体块一条指令在向量循环里的角色（阶段 A4 第 1 步从 AsmGen 类体搬来：
// Loop 要按它发射，codegen_vect.h 里留别名，类内写法不变）。
enum class VRole : uint8_t { Skip, Load, Bin, MapStore, RedStore, Fail };

// 一条下标访问的仿射描述（coef*j + inv + off）。
struct VAccess {
    VReg list = kNoReg;
    int64_t off = 0;
    int64_t coef = 1;
    VReg inv = kNoReg;
    bool write = false;
    int pos = 0;              // 体块内指令序号（读在写前判定）
    int bidx = -1;            // 基址键下标（AffBase）
};

// 一个 reduction 累加器（int 走 4-lane 时 IACC 当 ymm 用）。
struct VRed {
    int slot = -1;
    VReg acc = kNoReg, expr = kNoReg, add = kNoReg;
    int store_pos = -1;
    bool is_float = false;
    MOperand loc{};                 // 槽位置
    bool acc_stack = false;         // float acc 在栈（scratch+每轮写回）
    MReg XT = kMRegNone;            // float acc 寄存器
    MReg IACC = kMRegNone;          // int acc 寄存器
};

// 4-lane 发射上下文：全部是 try_vect_loop 已定死的分析/分配结果（只读）。
struct VAvx2Ctx {
    const std::vector<Inst>* body = nullptr;
    const std::vector<VRole>* role = nullptr;
    const std::vector<VAccess>* acc = nullptr;
    const std::vector<int>* acc_at = nullptr;
    const std::vector<AffBase>* bases = nullptr;
    const std::vector<VRed>* reds = nullptr;
    const std::function<MReg(VReg)>* xmm_of = nullptr;
    const std::vector<VReg>* uniq_inv = nullptr;
    const std::unordered_map<VReg, MReg>* inv_reg = nullptr;
    const std::vector<int64_t>* kcoefs = nullptr;
    const std::unordered_map<int64_t, MReg>* kreg = nullptr;
    MReg jreg = kMRegNone;
    bool j_stack = false;
    MOperand jloc;
    MOperand n_op;
    MReg gscr = kMRegNone;    // coef>1 装载的打包 scratch（VPINSRQ 用）
    MReg seq1 = kMRegNone;    // int 乘 pmuludq 重建 scratch
    MReg seq2 = kMRegNone;
    MReg hscr = kMRegNone;    // int reduction 水平求和 scratch（折叠也用）
    // 归纳变量物化寄存器（P5b，2-lane 侧由 vect_jvec2 物化 [j, j+1]）：
    // 4-lane 循环自己把它扩成 [j, j+1, j+2, j+3]（低 128 位 + vpinsrq
    // 拼高 128 位）。jvec == kMRegNone → 体块里没有 j 参与算术。
    MReg jvec = kMRegNone;
    bool has_j = false;
    const std::string* vec = nullptr;   // SSE2 2-lane 循环标签（回退目标）
    bool int_ok = false;      // 体块全是 int 值（无任何 VecF/InvF/float acc）
};

class VectPlanner {
public:
    // 一次 try_vect_loop 调用的「非 lambda 捕获」局部状态（阶段 A4 第 1 步）。
    // 来源：src/codegen_vect.cpp 的 try_vect_loop。被 [&] lambda 捕获的局部量
    // 本轮仍留在函数里（不改 lambda），所以不在这里；其余状态逐字段搬来。
    struct Loop {
        // ── 循环结构 ──
        int sJ = -1;              // 归纳变量槽
        int end_bi = -1;          // cond 分支目标块（simd_incr_cond_ok 出参）
        int jupd_pos = -1;        // while 形状：体块尾部增量的起始指令位置
        VReg n = kNoReg;          // 循环上界 vreg
        MOperand n_op;            // 上界的入口可读操作数
        bool failed = false;      // 角色判定失败标志

        // ── 分析结果（角色 / 逻辑数组 / 守卫 / 基址）──
        std::unordered_map<VReg, char> nonindex_use;   // 非下标使用点（P5b）
        std::vector<VRole> role;                       // 体块指令角色
        std::unordered_map<std::string, std::vector<int>> by_list;  // 数组键 → 访问下标
        std::unordered_map<std::string, VReg> list_rep;             // 数组键 → 代表 vreg
        std::vector<std::string> lists;                // 数组键（确定性顺序）
        std::vector<AffEq> inv_eq;                     // 入口 inv 判等守卫
        std::vector<AffBase> bases;                    // 基址键
        std::vector<int64_t> kcoefs;                   // coef>1 的跨步计数器系数

        // ── 寄存器分配（dry-run）──
        bool j_stack = false;         // 归纳变量在栈（否则在 JREG）
        MReg JREG = kMRegNone;        // 归纳变量工作寄存器
        std::vector<MReg> gpr_pool{kRdi, kRdx, kRcx};
        std::size_t gpr_used = 0;
        std::unordered_map<int64_t, MReg> kreg;        // coef → 跨步计数器寄存器
        bool any_int_red = false;
        MReg SEQ1 = kMRegNone, SEQ2 = kMRegNone;
        MReg TWO32 = kMRegNone;
        MReg GSCR = kMRegNone;
        std::vector<VReg> uniq_inv;                    // 去重后的不变量广播清单
        std::unordered_map<VReg, int> last_use;        // 临时值末次使用位置
        int j_first = -1, j_last = -1;                 // 归纳变量算术使用区间
        MReg FSCR = kMRegNone;
        MReg HSCR = kMRegNone;

        // ── 发射 ──
        std::string tail;             // 标量尾标签
        std::string vec;              // 向量循环标签
        bool avx_int = true;          // 体块纯整数 → 可走 4-lane
        VAvx2Ctx avx;                 // AVX2 发射上下文
        std::string vexit;            // 两条循环的公共出口标签
        bool avx2 = false;            // 是否发了 AVX2 段
    };

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
