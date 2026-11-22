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
#include "codegen_impl.h"

#include <climits>

namespace pyadamas {

// ── 1) 分析 ────────────────────────────────────────────────────────────
// 解析结果用「coef*j + inv + off」三元组表示，其中最多一个运行期项 inv。
// 常量解析成 {0, 无, c}：这样 Add 规则自然把常量折进 off，不用特判。
AsmGen::AffIdx AsmGen::vect_affine_index(VReg v, const std::vector<int>& jalias,
                                         const std::function<bool(VReg)>& inv_ok,
                                         int depth) const {
    AffIdx bad;
    if (v == kNoReg || depth > 24) return bad;
    const Inst* d = simd_def(v);
    auto is_jalias = [&](int s) {
        for (int x : jalias) if (x == s) return true;
        return false;
    };
    // 归纳变量（j 本身或 j 的副本槽）：仿射的基准项。
    if (d && d->op == IrOp::LoadSlot && is_jalias(d->idx)) {
        AffIdx r; r.ok = true; r.coef = 1; r.inv = kNoReg; r.off = 0;
        return r;
    }
    // 常量：{0, 无, c}。单独出现时 coef==0 → 调用方拒绝（不是逐 lane 访问）。
    // 常量允许暂时为负（后续 Add 可能抵消），最终 off>=0 由调用方校验。
    if (d && d->op == IrOp::ConstInt) {
        if (d->imm < -kAffMaxOff || d->imm > kAffMaxOff) return bad;
        AffIdx r; r.ok = true; r.coef = 0; r.off = d->imm;
        return r;
    }
    // 跨块 Mov 当不变量用**它自己**的 home：源 vreg 的活跃区间可能已在循环外
    // 结束（栈槽被复用），入口读源槽会取到别的值。体块内的 Mov 还没发射，
    // 只能往下解析。
    if (d && d->op == IrOp::Mov) {
        if (inv_ok(v)) {
            AffIdx r; r.ok = true; r.coef = 0; r.inv = v; r.off = 0;
            return r;
        }
        return vect_affine_index(d->a, jalias, inv_ok, depth + 1);
    }
    if (d && d->op == IrOp::Bin && d->bop == BinKind::Add) {
        AffIdx a = vect_affine_index(d->a, jalias, inv_ok, depth + 1);
        AffIdx b = vect_affine_index(d->b, jalias, inv_ok, depth + 1);
        // 两个运行期项相加不是仿射（那是运行期加法，不能折进寻址位移）。
        if (a.ok && b.ok && (a.inv == kNoReg || b.inv == kNoReg)) {
            int64_t coef = 0, off = 0;
            if (!__builtin_add_overflow(a.coef, b.coef, &coef) &&
                !__builtin_add_overflow(a.off, b.off, &off)) {
                AffIdx r;
                r.ok = true; r.coef = coef; r.off = off;
                r.inv = a.inv != kNoReg ? a.inv : b.inv;
                return r;
            }
        }
    }
    if (d && d->op == IrOp::Bin && d->bop == BinKind::Mul) {
        AffIdx a = vect_affine_index(d->a, jalias, inv_ok, depth + 1);
        AffIdx b = vect_affine_index(d->b, jalias, inv_ok, depth + 1);
        // 常量侧 = 纯常量（coef 0、无 inv）；被放大侧必须是纯仿射。
        const bool ac = a.ok && a.coef == 0 && a.inv == kNoReg;
        const bool bc = b.ok && b.coef == 0 && b.inv == kNoReg;
        const AffIdx& k = ac ? a : b;          // 常量
        const AffIdx& x = ac ? b : a;          // 被放大
        if ((ac || bc) && k.off >= 0 && k.off <= kAffMaxCoef && x.ok && x.inv == kNoReg) {
            int64_t coef = 0, off = 0;
            if (!__builtin_mul_overflow(x.coef, k.off, &coef) &&
                !__builtin_mul_overflow(x.off, k.off, &off) &&
                coef <= kAffMaxCoef && off <= kAffMaxOff) {
                AffIdx r; r.ok = true; r.coef = coef; r.off = off;
                return r;
            }
        }
    }
    // 兜底：循环不变 int 值（跨块计算 / 体内不写槽的装载）→ {0, v, 0}。
    // 它不依赖 j，整体折进基址；这里不要求是常量（那是 i*N 这类外层次计算）。
    if (inv_ok(v)) {
        AffIdx r; r.ok = true; r.coef = 0; r.inv = v; r.off = 0;
        return r;
    }
    return bad;
}

// 规划期校验（发射前，失败则一个字节不发）：所有基址键的 coef/off 都必须
// 小到能用 imulq 立即数与 disp32 表示。解析期已经卡过上界，这里再兜一次。
bool AsmGen::vect_affine_plan_ok(const std::vector<AffBase>& bases) const {
    for (const AffBase& b : bases) {
        if (b.coef < 1 || b.coef > kAffMaxCoef) return false;
        if (b.off_max < 0 || b.off_max > kAffMaxOff) return false;
        if (b.off_min < 0 || b.off_min > kAffMaxOff) return false;
        // coef>1 的装载需要两次标量 movsd + unpcklpd 的打包 scratch；同 coef
        // 的访问共用一个跨步计数器，理论上界由调用方按寄存器池另行检查。
        if (b.coef * 16 > INT32_MAX || b.coef * 8 > INT32_MAX) return false;
    }
    return true;
}

// ── 2) 守卫 ────────────────────────────────────────────────────────────
// 末 lane 下标 = coef*(n-1) + inv + off，必须 >= 0 且 < len。
// 循环入口已保证 j >= 0 且 j+1 < n（n >= 2），coef > 0 → 下标随 j 单调增，
// 所以「最小下标 = inv + off_min（j=0 处取得下界）」与「最大下标 =
// coef*(n-1) + inv + off_max」就是全部需要守的东西。
bool AsmGen::vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                               const MOperand& n_op, const std::string& tail) {
    const int64_t coef = b.coef;
    const int64_t off = b.off_max;
    if (coef < 1 || coef > kAffMaxCoef || off < 0 || off > kAffMaxOff) return false;
    if (off - coef < INT32_MIN || off - coef > INT32_MAX) return false;
    // inv 的入口可读来源由调用方用 list_base_mop 解析好（跨块值取 vreg home
    // 的冻结值、体内装载取槽现值）—— 直接 src_mop(v) 会在 vreg 的 home 还没
    // 写出来的情况下读到垃圾。
    const MOperand& iv = b.inv_src;
    if (b.inv != kNoReg && !simd_scratch_safe(iv)) return false;
    // 非负守卫：inv + off_min >= 0。inv 可能为负（i*N 在 i 为负时不保证），
    // 负下标会变成 Python 的从尾部索引 → 语义不同，必须走标量。
    if (b.inv != kNoReg) {
        emit2(MOp::kMovq, iv, kRax);
        if (b.off_min != 0) emit_imm(MOp::kAddq, b.off_min, kRax);
        emit2(MOp::kTestq, kRax, kRax);
        emit_jmp(MOp::kJs, tail);
    }
    // 上界守卫：value = coef*(n-1) + inv + off_max，要求 value < len。
    // 逐步 jo：任一溢出都说明下标算不准 → 直接标量（不猜）。
    emit2(MOp::kMovq, n_op, kRcx);
    if (coef != 1) {
        emit_imm(MOp::kImulq, coef, kRcx);                 // rcx = coef*n
        emit_jmp("jo", tail);                              // jo 不在 MOp 表：文本兜底
    }
    if (b.inv != kNoReg) {
        emit2(MOp::kAddq, iv, kRcx);                       // + inv
        emit_jmp("jo", tail);
    }
    if (off != coef) {
        emit_imm(MOp::kAddq, off - coef, kRcx);            // - coef + off
        emit_jmp("jo", tail);
    }
    emit2(MOp::kMovq, list_obj, kRax);
    emit2(MOp::kMovq, op_mem(kRax, 0), kRdx);              // len
    emit2(MOp::kCmpq, kRcx, kRdx);
    emit_jmp(MOp::kJle, tail);                             // len <= value → 标量
    return true;
}

// ── 3) 基址 / 跨步计数器 / 装载 / 存储 ────────────────────────────────
// base = data + inv*8。inv 折进基址后，coef==1 的访问就是原来的
// (base, JREG, 8, off*8)，coef>1 的访问则只差一个跨步计数器。
void AsmGen::vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg) {
    emit2(MOp::kMovq, list_obj, kRax);
    emit2(MOp::kMovq, op_mem(kRax, 16), reg);              // data 指针常驻
    if (b.inv == kNoReg) return;
    emit2(MOp::kMovq, b.inv_src, kRax);                    // 入口可读的 inv 来源
    emit2(MOp::kLeaq, op_mem(reg, kRax, 8), reg);          // reg = data + inv*8
}

// 同对象读写的 inv 判等守卫：只有两个下标序列完全相同（inv 运行期相等）
// 时才是逐元素原地 map；不等 → 标量（跨迭代依赖证不出来）。
void AsmGen::vect_affine_inv_eq(const AffEq& eq, const std::string& tail) {
    if (eq.x == kNoReg && eq.y == kNoReg) return;
    if (eq.x == kNoReg || eq.y == kNoReg) {                // 与「无 inv」比较 = 比 0
        emit2(MOp::kMovq, eq.x == kNoReg ? eq.ym : eq.xm, kRax);
        emit2(MOp::kTestq, kRax, kRax);
        emit_jmp(MOp::kJne, tail);
        return;
    }
    emit2(MOp::kMovq, eq.xm, kRax);
    emit2(MOp::kCmpq, eq.ym, kRax);
    emit_jmp(MOp::kJne, tail);
}

}  // namespace pyadamas
