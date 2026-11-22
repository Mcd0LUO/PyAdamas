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
    if (coef == 0) {
        // 循环内不变的下标元素（b[i*7+1] / b[3]）：每个 lane 读同一个元素 →
        // 一次 movsd 装载、movlhps 把低 lane 复制到高 lane 变成 [v, v]。
        emit_.emit2(MOp::kMovsd, AsmGen::op_mem(base, off * 8), mreg_operand(dst));
        emit_.emit2(MOp::kMovlhps, mreg_operand(dst), mreg_operand(dst));
        return;
    }
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
    if (coef < 0 || coef > kAffMaxCoef || off < 0 || off > kAffMaxOff) return false;
    if (off - coef < INT32_MIN || off - coef > INT32_MAX) return false;
    if (coef == 0) {
        // 循环内不变的下标（b[i*7+1] / b[3]）：只做**一次性**区间检查
        //   0 <= inv + off  且  inv + off < len
        // 不随 j 变化，所以既不用 n、也不用 j 守卫。任一不成立 → 跳标量尾：
        // 越界 IndexError 由标量按原时机抛出；负下标（Python 从尾部索引）
        // 语义不同，一律交给标量。
        const MOperand& iv = b.inv_src;
        if (b.inv != kNoReg) {
            if (!AsmGen::simd_scratch_safe(iv)) return false;
            emit_.emit2(MOp::kMovq, iv, kRax);
            emit_.emit2(MOp::kTestq, kRax, kRax);
            emit_.emit_jmp(MOp::kJs, tail);              // inv < 0 → 标量
            emit_.emit2(MOp::kMovq, iv, kRcx);
            if (off != 0) {
                emit_.emit_imm(MOp::kAddq, off, kRcx);   // rcx = inv + off_max
                emit_.emit_jmp("jo", tail);              // 溢出 → 标量
            }
        } else {
            emit_.emit_imm(MOp::kMovq, off, kRcx);       // 常量下标
        }
        emit_.emit2(MOp::kMovq, list_obj, kRax);
        emit_.emit2(MOp::kMovq, AsmGen::op_mem(kRax, 0), kRdx);   // len
        emit_.emit2(MOp::kCmpq, kRcx, kRdx);
        emit_.emit_jmp(MOp::kJle, tail);                 // len <= idx → 标量
        return true;
    }
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
        // coef==0：整条基址键只有循环内不变的下标访问（一次装载 + 广播），
        // 守卫换成一次性区间检查（见 vect_affine_guard），不参与跨步计数器。
        if (b.coef < 0 || b.coef > kAffMaxCoef) return false;
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


    // ── 方法：槽 / 归约 / 临时值 / 寄存器分配（阶段 A4 第 2 步，原捕获 lambda）──
bool VectPlanner::Loop::in_loop(int bi) const {
    return bi == cond_bi || bi == body_bi || bi == incr_bi;
}

bool VectPlanner::Loop::slot_is_jalias(int slot) const {
    for (int s : jalias) if (s == slot) return true;
    return false;
}

bool VectPlanner::Loop::slot_is_red(int slot) const {
    for (const Red& r : reds) if (r.slot == slot) return true;
    return false;
}

VectPlanner::Loop::Red* VectPlanner::Loop::red_of_slot(int slot) {
    for (Red& r : reds) if (r.slot == slot) return &r;
    return nullptr;
}

bool VectPlanner::Loop::is_red_add(VReg v) const {
    for (const Red& r : reds) if (r.add == v) return true;
    return false;
}

bool VectPlanner::Loop::slot_is_temp(int slot) const {
    for (int s : temp_slots) if (s == slot) return true;
    return false;
}

VReg VectPlanner::Loop::temp_store_val(int slot) const {
    for (const Inst& in : BI)
        if (in.op == IrOp::StoreSlot && in.idx == slot) return in.a;
    return kNoReg;
}

MReg VectPlanner::Loop::take(MReg prefer) {
    for (size_t i = 0; i < xmm_pool.size(); ++i)
        if (xmm_pool[i] == prefer) { MReg r = xmm_pool[i]; xmm_pool.erase(xmm_pool.begin() + static_cast<long>(i)); return r; }
    if (xmm_pool.empty()) return kMRegNone;
    MReg r = xmm_pool.front(); xmm_pool.erase(xmm_pool.begin()); return r;
}

VReg VectPlanner::Loop::resolve_src(VReg v) const {
    while (true) {
        auto it = cls.find(v);
        if (it == cls.end() || it->second.mov_src == kNoReg) return v;
        v = it->second.mov_src;
    }
}


    // ── 方法：仿射下标解析 / 逻辑数组身份 / 基址 / 4-lane 上下文（阶段 A4 第 2 步）──
bool VectPlanner::Loop::is_jalias_load(VReg v) const {
    const Inst* d = gen_.simd_def(v);
    if (!d || d->op != IrOp::LoadSlot) return false;
    for (int s : jalias) if (d->idx == s) return true;
    return false;
}

int64_t VectPlanner::Loop::index_off(VReg v) const {
    if (v == kNoReg) return -1;
    const Inst* d = gen_.simd_def(v);
    if (!d) return -1;
    if (d->op == IrOp::LoadSlot && slot_is_jalias(d->idx)) return 0;
    if (d->op == IrOp::Bin && d->bop == BinKind::Add) {
        const Inst* ca = gen_.simd_def(d->a);
        const Inst* cb = gen_.simd_def(d->b);
        int64_t oa = index_off(d->a), ob = index_off(d->b);
        if (oa >= 0 && cb && cb->op == IrOp::ConstInt && cb->imm >= 0) return oa + cb->imm;
        if (ob >= 0 && ca && ca->op == IrOp::ConstInt && ca->imm >= 0) return ob + ca->imm;
    }
    return -1;
}

bool VectPlanner::Loop::cross_block(VReg v) const {
    return gen_.simd_outside(v, cond_bi, body_bi, incr_bi);
}

MOperand VectPlanner::Loop::list_base_mop(VReg lv) const {
    const Inst* d = gen_.simd_def(lv);
    if (d && d->op == IrOp::LoadSlot)
        return cross_block(lv) ? gen_.src_mop(lv) : gen_.slot_mop(d->idx);
    if (d && d->op == IrOp::LoadGlobal)
        return cross_block(lv) ? gen_.src_mop(lv) : gen_.global_mop(d->idx);
    // 跨块 Mov 取**自己的** home：源 vreg 的活跃区间可能已经在循环外
    // 结束（内联 RC move-from 清槽后，分配器会把那个栈槽复用给别的值），
    // 取源槽会在入口读到垃圾——本文件头注释里的同一类陷阱。跨块 Mov 的
    // home 由循环外的发射写定，入口读它才是冻结值。
    if (d && d->op == IrOp::Mov)
        return cross_block(lv) ? gen_.src_mop(lv) : list_base_mop(d->a);
    return gen_.src_mop(lv);
}

std::string VectPlanner::Loop::list_key(VReg lv) const {
    const Inst* d = gen_.simd_def(lv);
    if (d && d->op == IrOp::LoadSlot)
        return cross_block(lv) ? "v" + std::to_string(lv) : "s" + std::to_string(d->idx);
    if (d && d->op == IrOp::LoadGlobal)
        return cross_block(lv) ? "v" + std::to_string(lv) : "g" + std::to_string(d->idx);
    if (d && d->op == IrOp::Mov) return list_key(d->a);
    return "v" + std::to_string(lv);
}

bool VectPlanner::Loop::list_base_ok(VReg lv) {
    const Inst* d = gen_.simd_def(lv);
    if (!d) return false;
    if (d->op == IrOp::LoadSlot) {
        if (cross_block(lv)) return true;           // home 持冻结值，与槽现值无关
        return si[d->idx].writers == 0 && gen_.simd_slot_unaddressed(d->idx);
    }
    if (d->op == IrOp::LoadGlobal) return true;
    if (d->op == IrOp::Mov) return list_base_ok(d->a);
    return gen_.simd_outside(lv, cond_bi, body_bi, incr_bi);
}

int VectPlanner::Loop::list_elem_float(VReg lv) {
    // 返回 0=float 1=int -1=其它。list vreg 必须是不变量（跨块/稳定槽）。
    TypePtr ty = gen_.ty_of(lv);
    if (!ty || ty->kind != TyKind::List || ty->sub.empty()) return -1;
    if (!list_base_ok(lv)) return -1;
    if (!AsmGen::simd_scratch_safe(list_base_mop(lv))) return -1;
    TypePtr et = ty->sub[0];
    if (!et) return -1;
    if (et->kind == TyKind::Float) return 0;
    if (et->kind == TyKind::Int) return 1;
    return -1;
}

bool VectPlanner::Loop::inv_int_ok(VReg v) {
    TypePtr ty = gen_.ty_of(v);
    if (!ty || ty->kind != TyKind::Int) return false;
    const Inst* d = gen_.simd_def(v);
    if (!d) return false;
    if (d->op != IrOp::LoadSlot) return cross_block(v);
    if (cross_block(v)) return true;
    auto it = si.find(d->idx);
    return (it == si.end() || it->second.writers == 0) && gen_.simd_slot_unaddressed(d->idx);
}

std::function<bool(VReg)> VectPlanner::Loop::inv_int_pred() {
    return [this](VReg v) { return inv_int_ok(v); };
}

void VectPlanner::Loop::pshufd(int64_t imm, MReg r) const {
    MInstr ins; ins.op = MOp::kPshufd;
    ins.ops.push_back(AsmGen::op_imm(imm));
    ins.ops.push_back(AsmGen::op_reg(r));
    ins.ops.push_back(AsmGen::op_reg(r));
    gen_.emit_.out.push_instr(std::move(ins));
}


    // ── 方法：表达式 DAG 分类（阶段 A4 第 2 步）──
VectPlanner::Loop::VK VectPlanner::Loop::classify(VReg v) {
    if (v == kNoReg) return VK::Fail;
    auto it = cls.find(v);
    if (it != cls.end()) return it->second.kind;
    if (!visiting.insert(v).second) return VK::Fail;  // 环 → 保守失败（防栈溢出）
    VClass c;
    const Inst* d = gen_.simd_def(v);
    if (!d) { cls[v] = c; return VK::Fail; }
    // 跨块值（LICM 外提的 Convert/Bin 等）按类型统一归不变量，不做逐 lane
    // 结构分类：外提值不在体块发射，若误判 VecF/VecI 则无临时寄存器分配
    // → 发射期查无寄存器（悬空 tail）。跨块值不依赖 j（循环携带值经槽
    // LoadSlot 进来，另路处理），按不变量广播是完备且保守的。
    if (gen_.simd_outside(v, cond_bi, body_bi, incr_bi)) {
        TypePtr ty = gen_.ty_of(v);
        if (ir_is_float(ty)) { c.kind = VK::InvF; inv_floats.push_back(v); }
        else if (ty && ty->kind == TyKind::Int) c.kind = VK::InvI;
        else if (ty && ty->kind == TyKind::List) c.kind = VK::InvPtr;
        else c.kind = VK::Fail;
        cls[v] = c;
        return c.kind;
    }
    switch (d->op) {
        case IrOp::ConstFloat:
            c.kind = VK::InvF;
            inv_floats.push_back(v);
            break;
        case IrOp::ConstInt:
            c.kind = VK::InvI;
            break;
        case IrOp::Mov:
            c.kind = classify(d->a);
            c.mov_src = d->a;
            break;
        case IrOp::LoadSlot: {
            if (slot_is_jalias(d->idx)) { c.kind = VK::J; break; }
            { bool is_acc = false;
              for (const Red& r : reds) if (d->idx == r.slot && v == r.acc) is_acc = true;
              if (is_acc) { c.kind = VK::Acc; break; } }
            if (slot_is_temp(d->idx)) {
                VReg sv = temp_store_val(d->idx);
                c.kind = classify(sv);
                c.mov_src = sv;
                break;
            }
            // 不变量槽：循环内无写、不取地址。
            if (si[d->idx].writers != 0 || !gen_.simd_slot_unaddressed(d->idx)) { c.kind = VK::Fail; break; }
            TypePtr ty = gen_.ty_of(v);
            if (ir_is_float(ty)) { c.kind = VK::InvF; inv_floats.push_back(v); }
            else if (ty && ty->kind == TyKind::Int) c.kind = VK::InvI;
            else if (ty && ty->kind == TyKind::List) c.kind = VK::InvPtr;  // 透明不变量基址
            else c.kind = VK::Fail;
            break;
        }
        case IrOp::LoadGlobal: {
            TypePtr ty = gen_.ty_of(v);
            if (ir_is_float(ty)) { c.kind = VK::InvF; inv_floats.push_back(v); }
            else if (ty && ty->kind == TyKind::Int) c.kind = VK::InvI;
            else if (ty && ty->kind == TyKind::List) c.kind = VK::InvPtr;  // 透明不变量基址
            else c.kind = VK::Fail;
            break;
        }
        case IrOp::Bin: {
            // P5：// 与 % 的除数是编译期正的 2 的幂常量时，它们和 Add/Sub/
            // Mul 一样是**通用的**逐元素整数操作（标量那边早就降成一条
            // sarq / andq；LLVM InstCombine、GCC tree-vect 同口径）。判据
            // 只有「除数是不是 2 的幂常量」，没有任何形状/程序特判。
            // 除数 vreg 不参与分类（不用广播它的值），只取移位量。
            int dsh = -1;
            const bool intdiv2 =
                (d->bop == BinKind::FloorDiv || d->bop == BinKind::Mod) &&
                owner_.vect_div_pow2(d->b, &dsh);
            VK ka = classify(d->a);
            VK kb = intdiv2 ? VK::InvI : classify(d->b);
            bool fa = ir_is_float(gen_.ty_of(d->a)), fb = ir_is_float(gen_.ty_of(d->b));
            // 判据的唯一出处在 vect_rules.h（具名、带注释、带编译期真值表）。
            // 归纳变量 J 算 VecI：发射期物化成 [j, j+1]，见 codegen_vect_jvec.cpp。
            using vect::float_operand_ok;
            using vect::int_operand_ok;
            bool ia = gen_.ty_of(d->a) && gen_.ty_of(d->a)->kind == TyKind::Int;
            bool ib = gen_.ty_of(d->b) && gen_.ty_of(d->b)->kind == TyKind::Int;
            if (fa && fb && float_operand_ok(ka) && float_operand_ok(kb)) {
                switch (d->bop) {
                    case BinKind::Add: case BinKind::Sub:
                    case BinKind::Mul: case BinKind::Div:
                        c.kind = VK::VecF; break;
                    default: c.kind = VK::Fail;
                }
            } else if (ia && ib && int_operand_ok(ka) && int_operand_ok(kb) &&
                       (d->bop == BinKind::Add || d->bop == BinKind::Sub ||
                        d->bop == BinKind::Mul || intdiv2)) {
                // int 逐元素加/减：paddq/psubq 模 2^64 与标量 addq/subq 逐位一致。
                // int 乘：pmuludq 奇偶 lane 组合全 i64 重建（无需值域守卫，
                // 任意 i64 逐位等于 imulq 低 64 位，推导见发射处注释）。
                // int // 与 %（除数为 2 的幂常量）：移位合成 / 取低位，见
                // codegen_vect_intdiv.cpp（floor 的 packed 合成要 SEQ1）。
                // 其余 int 除/位运算 → 保守跳过。
                c.kind = VK::VecI;
                if (d->bop == BinKind::Mul) has_int_mul = true;
                if (ka == VK::J || kb == VK::J) has_j_operand = true;
                // 除数常量不进不变量广播：发射期只用编译期移位量。
                if (intdiv2 && d->bop == BinKind::FloorDiv && dsh > 0) has_intdiv = true;
                if (ka == VK::InvI) inv_ints.push_back(d->a);
                if (!intdiv2 && kb == VK::InvI) inv_ints.push_back(d->b);
            } else {
                c.kind = VK::Fail;
            }
            break;
        }
        case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked: {
            // coef == 0 是**循环内不变的下标**元素装载（b[i*7+1] / b[3]）：
            // 每个 lane 读同一个元素 → 发射时一次装载 + 广播成 [v, v]
            // （见 vect_affine_load / vect_affine_guard 的 coef==0 分支）。
            int ek = list_elem_float(d->a);
            AffIdx af = owner_.vect_affine_index(d->b, jalias, inv_int_pred());
            if (ek < 0 || !af.ok || af.coef < 0 || af.coef > kAffMaxCoef ||
                af.off < 0 || af.off > kAffMaxOff) { c.kind = VK::Fail; break; }
            if (!ir_is_float(gen_.ty_of(v)) && ek == 0) { c.kind = VK::Fail; break; }
            c.kind = ek == 0 ? VK::VecF : VK::VecI;
            c.list = d->a; c.off = af.off; c.is_load = true;
            Access ac;
            ac.list = d->a; ac.off = af.off; ac.coef = af.coef; ac.inv = af.inv;
            ac.write = false; ac.pos = static_cast<int>(d - BI.data());
            acc_at[static_cast<size_t>(ac.pos)] = static_cast<int>(accesses.size());
            accesses.push_back(ac);
            break;
        }
        case IrOp::Convert: {
            // 只支持 plain i64 → float（标量发 cvtsi2sdq）。定宽/unsigned
            // 源走不同标量路径（norm/u64 绕道），float→int 需截断+溢出语义
            // → 一律保守跳过。
            TypePtr from = gen_.ty_of(d->a), to = d->ty;
            if (!to || to->kind != TyKind::Float || !from || from->kind != TyKind::Int) {
                c.kind = VK::Fail; break;
            }
            // 输入是 j / j+常量：先按仿射下标解析（避免 classify 把下标
            // 常量误收进不变量广播），发射期构造 [j+off, j+off+1] 向量。
            // 只有 off 能编进 leaq 的 disp32 才走这条快速路径（vect_jvec2 直接
            // 发 leaq off(j) / off+1(j)）；更大的常量交给下面的通用 VecI 折算
            // （不变量广播 + paddq + cvtdq2pd），否则会生成超范围位移、汇编失败。
            int64_t io = index_off(d->a);
            if (io >= 0 && io <= kAffMaxOff) {
                c.kind = VK::VecF; c.j_src = true; c.off = io;
                has_convert = true; break;
            }
            VK ka = classify(d->a);
            if (ka == VK::J) {
                c.kind = VK::VecF; c.j_src = true; c.off = 0;
                has_convert = true;
            } else if (ka == VK::VecI || ka == VK::InvI) {
                c.kind = VK::VecF;                    // 向量/不变量 i64 折算
                has_convert = true;
            } else {
                c.kind = VK::Fail;
            }
            break;
        }
        case IrOp::CallRt: {
            // 只支持 math.sqrt(x)（1 参 float→float）：标量侧是 sqrtsd 内联
            // fast path（x<0 抛 ValueError）；向量侧发 2-lane sqrtsd + 负数/NaN
            // 守卫跳标量尾，异常语义与时机不变。其余运行时调用可能碰容器/GC
            // → 一律拒绝。
            if (static_cast<RtFn>(d->imm) != RtFn::MathSqrt || d->args.size() != 1 ||
                !ir_is_float(d->ty)) {
                c.kind = VK::Fail; break;
            }
            VK ka = classify(d->args[0]);
            if (ka != VK::VecF && ka != VK::InvF) { c.kind = VK::Fail; break; }
            c.kind = VK::VecF;
            has_sqrt = true;
            break;
        }
        default:
            c.kind = VK::Fail;
    }
    cls[v] = c;
    return c.kind;
}

MReg VectPlanner::Loop::xmm_of(VReg v) const {
    VReg r = resolve_src(v);
    auto it = cls.find(r);
    if (it == cls.end()) return kMRegNone;
    if (it->second.kind == VK::InvF || it->second.kind == VK::InvI) {
        auto ir = inv_reg.find(r);
        return ir == inv_reg.end() ? kMRegNone : ir->second;
    }
    if (it->second.kind == VK::J) return JVEC;      // 归纳变量物化值
    auto tr = tmp_reg.find(r);
    return tr == tmp_reg.end() ? kMRegNone : tr->second;
}

}  // namespace pyadamas
