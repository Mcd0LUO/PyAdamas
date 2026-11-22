// vect_planner.cpp —— 见 docs/vect-planner-design.md
#include "vect_planner.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "codegen_impl.h"   // 只为 AsmGen::op_imm / op_reg / mreg_operand 这些静态工厂

namespace pyadamas {
namespace {

// 正 2 的幂 → m（除数 == 2^m，0 <= m <= 62）；其余（<= 0、非幂、负数）→ -1。
int pow2_shift_i64(int64_t d) {
    if (d < 1) return -1;
    if ((d & (d - 1)) != 0) return -1;
    int m = 0;
    while ((d >> m) != 1) ++m;
    return m;
}

// double 的位模式（从 codegen_vect_jvec.cpp 的匿名命名空间原样搬来，
// 与 codegen_vect.cpp 的同名 helper 同口径）。
uint64_t jvec_f64_bits(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return u;
}

}  // namespace

bool VectPlanner::vect_div_pow2(VReg b, int* sh) const {
    if (b == kNoReg || sh == nullptr) return false;
    const Inst* def = nullptr;
    for (int hop = 0; hop < 8 && b != kNoReg; ++hop) {
        def = val_.simd_def(b);
        if (def == nullptr) return false;
        if (def->op == IrOp::ConstInt) break;
        if (def->op != IrOp::Mov) return false;
        b = def->a;
    }
    if (def == nullptr || def->op != IrOp::ConstInt) return false;
    const int m = pow2_shift_i64(def->imm);
    if (m < 0 || m > 62) return false;
    *sh = m;
    return true;
}

void VectPlanner::vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh) {
    if (sh <= 0) {
        // m == 0：x // 1 == x（rd 已经是 x，什么都不发）；x % 1 == 0。
        if (is_mod) emit_.emit2(MOp::kPxor, mreg_operand(rd), mreg_operand(rd));
        return;
    }
    if (is_mod) {
        // x % 2^m = x & (2^m - 1)：左移把高 m 位推出去，再逻辑右移回来。
        emit_.emit_imm(MOp::kPsllq, 64 - sh, mreg_operand(rd));
        emit_.emit_imm(MOp::kPsrlq, 64 - sh, mreg_operand(rd));
        return;
    }
    // floor：sign = (x >> 31 每 dword 取符号) → pshufd 复制到 64 位 lane →
    // 左移 (64-m) 得到「高 m 位全 1 或 0」→ 与 (x 逻辑右移 m) 相或。
    emit_.emit2(MOp::kMovapd, mreg_operand(rd), mreg_operand(tmp));
    emit_.emit_imm(MOp::kPsrad, 31, mreg_operand(tmp));
    {
        MInstr ins;                          // pshufd 本 GAS 只认三操作数写法
        ins.op = MOp::kPshufd;
        ins.ops.push_back(AsmGen::op_imm(0xF5));
        ins.ops.push_back(AsmGen::op_reg(tmp));
        ins.ops.push_back(AsmGen::op_reg(tmp));
        emit_.emit_instr(std::move(ins));
    }
    emit_.emit_imm(MOp::kPsllq, 64 - sh, mreg_operand(tmp));
    emit_.emit_imm(MOp::kPsrlq, sh, mreg_operand(rd));
    emit_.line("por " + std::string(mreg_name(tmp)) + ", " + std::string(mreg_name(rd)));
}

void VectPlanner::vect_affine_kinit(MReg k, MReg jreg, int64_t coef) {
    emit_.emit2(MOp::kMovq, mreg_operand(jreg), mreg_operand(k));
    emit_.emit_imm(MOp::kImulq, coef * 8, mreg_operand(k));
}

void VectPlanner::vect_affine_step(MReg k, int64_t coef) {
    emit_.emit_imm(MOp::kAddq, coef * 16, mreg_operand(k));
}

void VectPlanner::vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                                   MReg dst, MReg scratch) {
    if (coef == 1) {
        emit_.emit2(MOp::kMovupd, AsmGen::op_mem(base, j, 8, off * 8), mreg_operand(dst));
        return;
    }
    emit_.emit2(MOp::kMovsd, AsmGen::op_mem(base, k, 1, off * 8), mreg_operand(dst));
    emit_.emit2(MOp::kMovsd, AsmGen::op_mem(base, k, 1, (off + coef) * 8), mreg_operand(scratch));
    emit_.emit2(MOp::kUnpcklpd, mreg_operand(scratch), mreg_operand(dst));
}

void VectPlanner::vect_affine_store(MReg base, MReg j, int64_t off, MReg src) {
    emit_.emit2(MOp::kMovupd, mreg_operand(src), AsmGen::op_mem(base, j, 8, off * 8));
}

void VectPlanner::vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr) {
    emit_.emit2(MOp::kLeaq, AsmGen::op_mem(jreg, off), mreg_operand(kRax));
    emit_.emit2(MOp::kMovq, mreg_operand(kRax), mreg_operand(dst));
    emit_.emit2(MOp::kLeaq, AsmGen::op_mem(jreg, off + 1), mreg_operand(kRax));
    emit_.emit2(MOp::kMovq, mreg_operand(kRax), mreg_operand(scr));
    emit_.emit2(MOp::kMovlhps, mreg_operand(scr), mreg_operand(dst));  // dst.hi = j+off+1
}

bool VectPlanner::vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                               const MOperand& n_op, const std::string& tail) {
    const int64_t coef = b.coef;
    const int64_t off = b.off_max;
    if (coef < 1 || coef > kAffMaxCoef || off < 0 || off > kAffMaxOff) return false;
    if (off - coef < INT32_MIN || off - coef > INT32_MAX) return false;
    // inv 的入口可读来源由调用方用 list_base_mop 解析好（跨块值取 vreg home
    // 的冻结值、体内装载取槽现值）—— 直接 src_mop(v) 会在 vreg 的 home 还没
    // 写出来的情况下读到垃圾。
    const MOperand& iv = b.inv_src;
    if (b.inv != kNoReg && !AsmGen::simd_scratch_safe(iv)) return false;
    // 非负守卫：inv + off_min >= 0。inv 可能为负（i*N 在 i 为负时不保证），
    // 负下标会变成 Python 的从尾部索引 → 语义不同，必须走标量。
    if (b.inv != kNoReg) {
        emit_.emit2(MOp::kMovq, iv, kRax);
        if (b.off_min != 0) emit_.emit_imm(MOp::kAddq, b.off_min, kRax);
        emit_.emit2(MOp::kTestq, kRax, kRax);
        emit_.emit_jmp(MOp::kJs, tail);
    }
    // 上界守卫：value = coef*(n-1) + inv + off_max，要求 value < len。
    // 逐步 jo：任一溢出都说明下标算不准 → 直接标量（不猜）。
    emit_.emit2(MOp::kMovq, n_op, kRcx);
    if (coef != 1) {
        emit_.emit_imm(MOp::kImulq, coef, kRcx);                 // rcx = coef*n
        emit_.emit_jmp("jo", tail);                              // jo 不在 MOp 表：文本兜底
    }
    if (b.inv != kNoReg) {
        emit_.emit2(MOp::kAddq, iv, kRcx);                       // + inv
        emit_.emit_jmp("jo", tail);
    }
    if (off != coef) {
        emit_.emit_imm(MOp::kAddq, off - coef, kRcx);            // - coef + off
        emit_.emit_jmp("jo", tail);
    }
    emit_.emit2(MOp::kMovq, list_obj, kRax);
    emit_.emit2(MOp::kMovq, AsmGen::op_mem(kRax, 0), kRdx);              // len
    emit_.emit2(MOp::kCmpq, kRcx, kRdx);
    emit_.emit_jmp(MOp::kJle, tail);                             // len <= value → 标量
    return true;
}

void VectPlanner::vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg) {
    emit_.emit2(MOp::kMovq, list_obj, kRax);
    emit_.emit2(MOp::kMovq, AsmGen::op_mem(kRax, 16), reg);              // data 指针常驻
    if (b.inv == kNoReg) return;
    emit_.emit2(MOp::kMovq, b.inv_src, kRax);                    // 入口可读的 inv 来源
    emit_.emit2(MOp::kLeaq, AsmGen::op_mem(reg, kRax, 8), reg);          // reg = data + inv*8
}

void VectPlanner::vect_affine_inv_eq(const AffEq& eq, const std::string& tail) {
    if (eq.x == kNoReg && eq.y == kNoReg) return;
    if (eq.x == kNoReg || eq.y == kNoReg) {                // 与「无 inv」比较 = 比 0
        emit_.emit2(MOp::kMovq, eq.x == kNoReg ? eq.ym : eq.xm, kRax);
        emit_.emit2(MOp::kTestq, kRax, kRax);
        emit_.emit_jmp(MOp::kJne, tail);
        return;
    }
    emit_.emit2(MOp::kMovq, eq.xm, kRax);
    emit_.emit2(MOp::kCmpq, eq.ym, kRax);
    emit_.emit_jmp(MOp::kJne, tail);
}

// ── 仿射下标解析（阶段 3 第 6 步：从 codegen_vect_affine.cpp 搬来）──────
// 解析结果用「coef*j + inv + off」三元组表示，其中最多一个运行期项 inv。
// 常量解析成 {0, 无, c}：这样 Add 规则自然把常量折进 off，不用特判。
AffIdx VectPlanner::vect_affine_index(VReg v, const std::vector<int>& jalias,
                                      const std::function<bool(VReg)>& inv_ok,
                                      int depth) const {
    AffIdx bad;
    if (v == kNoReg || depth > 24) return bad;
    const Inst* d = val_.simd_def(v);
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
bool VectPlanner::vect_affine_plan_ok(const std::vector<AffBase>& bases) const {
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

// ── 同值常量归并（阶段 3 第 6 步：从 codegen_vect_jvec.cpp 搬来）──────
// 常量常被 LICM 外提成多个独立 vreg —— 同一个 1 出现两次就占两个广播寄存器。
// 按值归并到首个 vreg（ConstInt 同 imm、ConstFloat 同位模式，且结果类型相同），
// 语义完全等价；给 j 算术腾出寄存器。顺序按 vreg 号，生成代码确定。
void VectPlanner::vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls) {
    if (cls == nullptr) return;
    std::vector<VReg> vs;
    for (auto& [v, c] : *cls)
        if ((c.kind == vect::VK::InvI || c.kind == vect::VK::InvF) && c.mov_src == kNoReg)
            vs.push_back(v);
    std::sort(vs.begin(), vs.end());
    std::unordered_map<int64_t, VReg> ci;
    std::unordered_map<uint64_t, VReg> cf;
    for (VReg v : vs) {
        const Inst* d = val_.simd_def(v);
        if (d == nullptr) continue;
        auto same_ty = [&](VReg u) {
            TypePtr tu = val_.ty_of(u), tv = val_.ty_of(v);
            return tu && tv && tu->kind == tv->kind;
        };
        if (d->op == IrOp::ConstInt) {
            auto [it, fresh] = ci.emplace(d->imm, v);
            if (!fresh && same_ty(it->second)) (*cls)[v].mov_src = it->second;
        } else if (d->op == IrOp::ConstFloat) {
            auto [it, fresh] = cf.emplace(jvec_f64_bits(d->fimm), v);
            if (!fresh && same_ty(it->second)) (*cls)[v].mov_src = it->second;
        }
    }
}

}  // namespace pyadamas
