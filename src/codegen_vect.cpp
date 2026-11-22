// codegen_vect.cpp —— 通用循环自动向量化（PYADAMAS_VECT 门控，默认关）
//
// 设计见 docs/simd-vectorization-design.md §11。与特化 matcher（codegen_simd*
// 认 spectral_norm/nbody/matmul 精确形状）互补：本文件做**通用**自动向量化，
// 基于 IR 结构 + 数据引用/别名分析识别任意可数 for 循环的逐元素 map /
// reduction，不依赖精确指令形状（参照 gcc tree-vect / LLVM LoopVectorize 的
// 依赖分析 + 循环版本化思路，本轮做 tractable 通用核心）。
//
// 识别（全部满足才向量化，任一不满足整段标量 fallback，一个字节不发）：
//   * 可数 for：cond/incr 结构形状（步长 1，所有 for-range 都降成这个形状，
//     非基准特化）；体块是纯算术/装载/槽写/下标读写的表达式 DAG。
//   * 归纳变量 j 及其副本槽（j-alias）；循环不变量（常量/稳定槽/跨块值）。
//   * 逐元素 map：out[j+c] = f(a[j+c'], b[j+c''], 不变量)，下标仿射 j+常量。
//   * reduction（可多个并行）：s += f(...)（float 保序提取累加，逐位一致；
//     int paddq 2-lane 模 2^64 精确 + 出口水平求和；int acc 装载槽前值）。
//   * int 乘：pmuludq 奇偶 lane 全 i64 重建（模 2^64 = imulq，任意 i64 无守卫）。
//   * Convert plain i64→float：cvtdq2pd 折算（pshufd 聚 hi/lo dword，中间量
//     全精确 + 末条单次舍入 = cvtsi2sdq，任意 i64 无守卫）。
//   * 体局部临时槽（单写、读全在写后、无外部读、非堆）→ SSA 替换，跳过存储。
//
// 别名/依赖安全判据（宁保守跳过，不误向量化）：
//   * 循环内唯一内存写是下标写（map）+ 槽写（栈帧，不与堆别名）；无调用、
//     无 store.global → 向量循环零调用，list/data/不变量期间不可能被改写。
//   * 不同 list 对象 data 数组必不相交（各自 malloc，无 view）→ 入口运行时
//     判等守卫（循环版本化）；同对象读+写只允许「同偏移 0 且读在写前」的
//     逐元素原地 map（跨迭代/跨 lane 无依赖证明）。其余同对象读写 → 跳过。
//   * len>=n+maxoff && kind==0 && j>=0 && j+1<n 守卫 → movupd 不越界、读裸
//     标量；不满足整段标量，IndexError/负索引/kind 升级语义与时机不变。
//
//  lowering：SSE2 2-lane packed（movupd/addpd/subpd/mulpd/divpd/paddq/psubq/
// pmuludq 重建/cvtdq2pd 折算）。尾循环：
// 向量循环退出 jmp cond——j==n 直接退出（计数器精确等于标量），j==n-1 经
// cond 回体块，入口 j+1<n 守卫失败 → 原标量体处理余 1（gen_block 照常发射）。
//
// 寄存器约定：scratch 只用永不提升的 GPR(rax/rcx/rdx/rsi/rdi) 与 xmm0-7；
// j 原地 addq $2 在提升 GPR 或 rsi 工作+写回；float acc 原地累加在提升
// xmm8-15 或 scratch+每轮写回。所有分析（含寄存器分配可行性）在发射前完成，
// 失败则一个字节不发。与标量发射机器衔接同特化原型（见 codegen_simd.cpp §4.2）。
//
// 阶段 A4 第 3 步：函数体已整体搬进 VectPlanner::try_loop
// （src/vect_planner_loop.cpp）；本文件只剩 AsmGen 侧入口与函数级前提。
#include "codegen_impl.h"

namespace pyadamas {

bool AsmGen::vect_func_ok() const {
    if (!vect_enabled_ || opt_ < 2 || gen_ || tag_.tags_enabled) return false;
    if (!slot_loc_.empty()) return false;                 // 只做默认寄存器分配
    if (std::string_view(target_abi().name) != "x86_64-sysv") return false;
    if (f_->has_handlers) return false;
    return true;
}

// 体块是否「全整数」：gen_block 的 P4 顺序分流判据（纯 IR 结构）。
//
// 只有「确定全整数」才返回 true —— 让通用向量化先试（它的整数路径有
// AVX2 4-lane，实测比特化 2-lane 模板快约 28%）。任何不确定都返回 false，
// 即浮点/混合/含调用的体块保持「特化先试」的现状（float 上特化快约 2 倍）。
// 允许的类型：int / bool / 定宽整数，以及元素为这些类型的 list（下标访问的
// 数组操作数）；浮点常量、float 任何出现、unknown 类型、其它容器/调用一律
// 视为「含浮点」。这个判据里没有任何基准名或循环形状特判。
bool AsmGen::block_int_only(const Block& b) const {
    auto scalar_int = [](const TypePtr& t) -> bool {
        if (!t) return false;
        switch (t->kind) {
            case TyKind::Int: case TyKind::Bool:
            case TyKind::I8: case TyKind::I16: case TyKind::I32:
            case TyKind::U8: case TyKind::U16: case TyKind::U32: case TyKind::U64:
                return true;
            default:
                return false;
        }
    };
    auto int_ok = [&](VReg r) -> bool {
        if (r == kNoReg) return true;
        TypePtr t = ty_of(r);
        if (!t) return false;
        if (t->kind == TyKind::List)
            return !t->sub.empty() && scalar_int(t->sub[0]);
        return scalar_int(t);
    };
    for (const Inst& in : b.insts) {
        if (in.op == IrOp::ConstFloat) return false;   // 浮点常量
        if (!int_ok(in.dst) || !int_ok(in.a) || !int_ok(in.b) || !int_ok(in.c))
            return false;
        for (VReg r : in.args)
            if (!int_ok(r)) return false;
    }
    return true;
}

bool AsmGen::xmm_high_free(MReg r) const {
    if (r < kXmm8 || r > kXmm15) return false;
    // 浮点槽提升（slot_reg）与跨块 vreg 提升（fp_promoted_vregs）是 xmm8-15 的
    // 两种使用者；只要某个寄存器出现在其中任一处，就说明整函数都有活跃值。
    for (const MOperand& loc : ra_.slot_reg)
        if (loc.kind == MOperandKind::Reg && loc.reg == r) return false;
    for (int v : ra_.fp_promoted_vregs) {
        if (v < 0 || static_cast<size_t>(v) >= vreg_reg_.size()) continue;
        const MOperand& loc = vreg_reg_[static_cast<size_t>(v)];
        if (loc.kind == MOperandKind::Reg && loc.reg == r) return false;
    }
    return true;
}

bool AsmGen::try_vect_loop(const Block& b) {
    // 阶段 A4 第 3 步：函数体已整体搬进 VectPlanner::try_loop。
    return vect_.try_loop(b);
}

}  // namespace pyadamas
