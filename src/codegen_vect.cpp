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
//   * reduction：s += f(...)（float 保序提取累加，逐位一致；int paddq 2-lane
//     模 2^64 精确 + 出口水平求和）。
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
//  lowering：SSE2 2-lane packed（movupd/addpd/subpd/mulpd/divpd）。尾循环：
// 向量循环退出 jmp cond——j==n 直接退出（计数器精确等于标量），j==n-1 经
// cond 回体块，入口 j+1<n 守卫失败 → 原标量体处理余 1（gen_block 照常发射）。
//
// 寄存器约定：scratch 只用永不提升的 GPR(rax/rcx/rdx/rsi/rdi) 与 xmm0-7；
// j 原地 addq $2 在提升 GPR 或 rsi 工作+写回；float acc 原地累加在提升
// xmm8-15 或 scratch+每轮写回。所有分析（含寄存器分配可行性）在发射前完成，
// 失败则一个字节不发。与标量发射机器衔接同特化原型（见 codegen_simd.cpp §4.2）。
#include "codegen_impl.h"

#include <cstring>
#include <unordered_set>

namespace pyadamas {

namespace {

// 向量值分类。InvF/InvI 是循环不变量（setup 广播）；J 是归纳变量；Affine 是
// j+常量（只用于下标位移）；VecF/VecI 是逐 lane 计算的 2-lane 值；Acc 是
// reduction 累加器旧值；Fail 不可向量化。
enum class VK : uint8_t { Fail, InvF, InvI, InvPtr, J, Affine, VecF, VecI, Acc };

struct VClass {
    VK kind = VK::Fail;
    int64_t off = 0;          // Affine：j+off
    VReg list = kNoReg;       // VecF/VecI 来自 IndexLoad 时的 list vreg
    bool is_load = false;     // VecF/VecI 是内存装载（vs 算术）
    VReg mov_src = kNoReg;    // Mov 透明别名
};

struct Access {
    VReg list = kNoReg;
    int64_t off = 0;
    bool write = false;
    int pos = 0;              // 体块内指令序号（读在写前判定）
};

uint64_t f64_bits(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return u;
}

}  // namespace

bool AsmGen::vect_func_ok() const {
    if (!vect_enabled_ || opt_ < 2 || gen_ || ra_graph_ || tags_enabled_) return false;
    if (!slot_loc_.empty()) return false;                 // 只做默认寄存器分配
    if (std::string_view(target_abi().name) != "x86_64-sysv") return false;
    if (f_->has_handlers) return false;
    return true;
}

bool AsmGen::try_vect_loop(const Block& b) {
    if (!vect_func_ok()) return false;
    const int body_bi = cur_block_;
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    const auto& BI = b.insts;
    if (BI.size() < 4 || BI.size() > 64) return false;    // 体块规模上限（保守）
    if (BI.back().op != IrOp::Jump) return false;
    const int incr_bi = BI.back().target;

    // ── 循环结构：从 incr 块反推 sJ，再校验 cond/incr 形状（步长 1）──────
    if (incr_bi < 0 || incr_bi >= static_cast<int>(f_->blocks.size()) || incr_bi == body_bi)
        return false;
    const auto& INC = f_->blocks[static_cast<size_t>(incr_bi)].insts;
    if (INC.empty() || INC[0].op != IrOp::LoadSlot) return false;
    const int sJ = INC[0].idx;
    int cond_bi = -1, end_bi = -1;
    VReg n = kNoReg;
    if (!simd_incr_cond_ok(incr_bi, body_bi, sJ, cond_bi, end_bi, n)) return false;
    if (!simd_outside(n, cond_bi, body_bi, incr_bi) ||
        !simd_slot_stable(n, cond_bi, body_bi, incr_bi)) return false;
    if (sJ < 0 || sJ >= static_cast<int>(f_->slot_ty.size())) return false;
    { TypePtr tj = f_->slot_ty[static_cast<size_t>(sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    if (!simd_slot_unaddressed(sJ)) return false;
    // 循环三块 op 白名单：无调用/全局写/取地址（下标读写放行，位置另判）。
    if (!simd_loop_ops_ok(cond_bi, body_bi, incr_bi, false, true)) return false;

    const MOperand n_op = src_mop(n);
    if (!simd_scratch_safe(n_op)) return false;

    // ── 槽分析：j-alias / reduction / 体局部临时 / 其余写一律拒绝 ─────────
    // 收集三块内所有槽写/读位置。
    struct SlotInfo { int writers = 0; int store_pos = -1; VReg store_val = kNoReg;
                      bool read_before_store = false; int loads = 0; bool outside_read = false;
                      int min_load_pos = 1 << 30; };
    std::unordered_map<int, SlotInfo> si;
    auto in_loop = [&](int bi) { return bi == cond_bi || bi == body_bi || bi == incr_bi; };
    for (int bi = 0; bi < static_cast<int>(f_->blocks.size()); ++bi)
        for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts) {
            if (in.op == IrOp::SlotAddr) {
                // 任何被取地址的槽都不能跟踪（间接读写）→ 若涉及本循环槽则拒绝。
                si[in.idx].outside_read = true;           // 保守标记
            }
            if (in.op == IrOp::StoreSlot) {
                auto& s = si[in.idx];
                if (in_loop(bi)) { ++s.writers; if (bi == body_bi) { s.store_pos = static_cast<int>(&in - BI.data()); s.store_val = in.a; } }
            }
            if (in.op == IrOp::LoadSlot) {
                auto& s = si[in.idx];
                if (in_loop(bi)) {
                    ++s.loads;
                    if (bi == body_bi)
                        s.min_load_pos = std::min(s.min_load_pos, static_cast<int>(&in - BI.data()));
                } else {
                    s.outside_read = true;
                }
            }
        }
    // 读在写前判定须在完整扫描后做（store_pos 此时才齐全）：任何体块装载
    // 早于该槽的唯一存储 → read_before_store（否则误判临时槽 → classify 自环）。
    for (auto& [slot, info] : si)
        info.read_before_store = (info.store_pos >= 0 && info.min_load_pos < info.store_pos);
    // sJ：只被 incr 写一次。
    if (si[sJ].writers != 1) return false;

    // j-alias 槽：sJ 自身 + 「体块内单写、写值来自 j-alias 装载、读全在写后、
    // 无外部读、不取地址」的副本槽。副本槽存储跳过（外部无读 → 死值）。
    std::vector<int> jalias{sJ};
    auto is_jalias_load = [&](VReg v) {
        const Inst* d = simd_def(v);
        if (!d || d->op != IrOp::LoadSlot) return false;
        for (int s : jalias) if (d->idx == s) return true;
        return false;
    };
    // 迭代到不动点（副本链通常只有一层）。
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [slot, info] : si) {
            if (slot == sJ) continue;
            bool already = false;
            for (int s : jalias) if (s == slot) already = true;
            if (already) continue;
            if (info.writers != 1 || info.store_pos < 0 || info.outside_read ||
                info.read_before_store || !simd_slot_unaddressed(slot)) continue;
            if (!is_jalias_load(info.store_val)) continue;
            TypePtr ty = slot < static_cast<int>(f_->slot_ty.size())
                             ? f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
            if (!ty || ty->kind != TyKind::Int) continue;
            jalias.push_back(slot);
            changed = true;
        }
    }
    auto slot_is_jalias = [&](int slot) {
        for (int s : jalias) if (s == slot) return true;
        return false;
    };

    // reduction 槽：float/int，体块内恰一写一读，写值 = 读值 + expr（或 expr +
    // 读值），读在写前（SSA 必然），无其它循环内读；外部读允许（终值要维护）。
    int red_slot = -1; VReg red_acc = kNoReg, red_expr = kNoReg, red_add = kNoReg;
    bool red_float = false; int red_store_pos = -1;
    for (auto& [slot, info] : si) {
        if (slot_is_jalias(slot) || slot == sJ) continue;
        if (info.writers != 1 || info.store_pos < 0 || info.loads != 1) continue;
        if (info.read_before_store || !simd_slot_unaddressed(slot)) continue;
        TypePtr ty = slot < static_cast<int>(f_->slot_ty.size())
                         ? f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
        if (!ty || (ty->kind != TyKind::Float && ty->kind != TyKind::Int)) continue;
        if (ir_is_heap(ty)) continue;
        // 找体块内的 load（acc 旧值）与 store。
        VReg acc = kNoReg; const Inst* st = nullptr;
        for (const Inst& in : BI) {
            if (in.op == IrOp::LoadSlot && in.idx == slot) acc = in.dst;
            if (in.op == IrOp::StoreSlot && in.idx == slot) st = &in;
        }
        if (acc == kNoReg || !st) continue;
        const Inst* add = simd_def(st->a);
        if (!add || add->op != IrOp::Bin || add->bop != BinKind::Add) continue;
        VReg expr = kNoReg;
        if (add->a == acc) expr = add->b;
        else if (add->b == acc) expr = add->a;
        else continue;
        if (expr == kNoReg || expr == acc) continue;
        if (red_slot != -1) return false;                 // 本轮只支持单 reduction
        red_slot = slot; red_acc = acc; red_expr = expr; red_add = st->a;
        red_store_pos = static_cast<int>(st - BI.data());
        red_float = ty->kind == TyKind::Float;
    }

    // 体局部临时槽：单写、读全在写后、无外部读、非堆、不取地址 → 跳过存储，
    // 其 load 替换为所存值。其余有循环内写的槽 → 拒绝（无法维护）。
    std::vector<int> temp_slots;
    for (auto& [slot, info] : si) {
        if (slot == sJ || slot_is_jalias(slot) || slot == red_slot) continue;
        if (info.writers == 0) continue;                  // 只读槽：不变量候选
        if (info.writers == 1 && info.store_pos >= 0 && !info.outside_read &&
            !info.read_before_store && simd_slot_unaddressed(slot)) {
            TypePtr ty = slot < static_cast<int>(f_->slot_ty.size())
                             ? f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
            if (ty && !ir_is_heap(ty) &&
                (ty->kind == TyKind::Float || ty->kind == TyKind::Int)) {
                temp_slots.push_back(slot);
                continue;
            }
        }
        return false;                                     // 未处理的槽写 → 保守跳过
    }
    auto slot_is_temp = [&](int slot) {
        for (int s : temp_slots) if (s == slot) return true;
        return false;
    };
    // 临时槽的 store_val（替换用）。
    auto temp_store_val = [&](int slot) -> VReg {
        for (const Inst& in : BI)
            if (in.op == IrOp::StoreSlot && in.idx == slot) return in.a;
        return kNoReg;
    };

    // ── 表达式 DAG 分类（ memoized 递归）─────────────────────────────────
    std::unordered_map<VReg, VClass> cls;
    std::vector<Access> accesses;
    std::vector<VReg> inv_floats;                         // 需要广播的不变量 float
    bool failed = false;

    // 下标仿射偏移：j → 0；j+c（c>=0 常量）→ c；否则 -1。
    std::function<int64_t(VReg)> index_off = [&](VReg v) -> int64_t {
        if (v == kNoReg) return -1;
        const Inst* d = simd_def(v);
        if (!d) return -1;
        if (d->op == IrOp::LoadSlot && slot_is_jalias(d->idx)) return 0;
        if (d->op == IrOp::Bin && d->bop == BinKind::Add) {
            const Inst* ca = simd_def(d->a);
            const Inst* cb = simd_def(d->b);
            int64_t oa = index_off(d->a), ob = index_off(d->b);
            if (oa >= 0 && cb && cb->op == IrOp::ConstInt && cb->imm >= 0) return oa + cb->imm;
            if (ob >= 0 && ca && ca->op == IrOp::ConstInt && ca->imm >= 0) return ob + ca->imm;
        }
        return -1;
    };
    // 规范化数组基址：body 内 load.slot/load.global/Mov 解析到稳定来源；
    // 同一槽/全局/跨块值的不同 vreg 归并为同一逻辑数组（非内联函数体内装载）。
    std::function<MOperand(VReg)> list_base_mop = [&](VReg lv) -> MOperand {
        const Inst* d = simd_def(lv);
        if (d && d->op == IrOp::LoadSlot) return slot_mop(d->idx);
        if (d && d->op == IrOp::LoadGlobal) return global_mop(d->idx);
        if (d && d->op == IrOp::Mov) return list_base_mop(d->a);
        return src_mop(lv);
    };
    std::function<std::string(VReg)> list_key = [&](VReg lv) -> std::string {
        const Inst* d = simd_def(lv);
        if (d && d->op == IrOp::LoadSlot) return "s" + std::to_string(d->idx);
        if (d && d->op == IrOp::LoadGlobal) return "g" + std::to_string(d->idx);
        if (d && d->op == IrOp::Mov) return list_key(d->a);
        return "v" + std::to_string(lv);
    };
    std::function<bool(VReg)> list_base_ok = [&](VReg lv) -> bool {
        const Inst* d = simd_def(lv);
        if (!d) return false;
        if (d->op == IrOp::LoadSlot)
            return si[d->idx].writers == 0 && simd_slot_unaddressed(d->idx);
        if (d->op == IrOp::LoadGlobal) return true;
        if (d->op == IrOp::Mov) return list_base_ok(d->a);
        return simd_outside(lv, cond_bi, body_bi, incr_bi);
    };
    auto list_elem_float = [&](VReg lv) -> int {
        // 返回 0=float 1=int -1=其它。list vreg 必须是不变量（跨块/稳定槽）。
        TypePtr ty = ty_of(lv);
        if (!ty || ty->kind != TyKind::List || ty->sub.empty()) return -1;
        if (!list_base_ok(lv)) return -1;
        if (!simd_scratch_safe(list_base_mop(lv))) return -1;
        TypePtr et = ty->sub[0];
        if (!et) return -1;
        if (et->kind == TyKind::Float) return 0;
        if (et->kind == TyKind::Int) return 1;
        return -1;
    };

    std::unordered_set<VReg> visiting;
    std::function<VK(VReg)> classify = [&](VReg v) -> VK {
        if (v == kNoReg) return VK::Fail;
        auto it = cls.find(v);
        if (it != cls.end()) return it->second.kind;
        if (!visiting.insert(v).second) return VK::Fail;  // 环 → 保守失败（防栈溢出）
        VClass c;
        const Inst* d = simd_def(v);
        if (!d) { cls[v] = c; return VK::Fail; }
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
                if (d->idx == red_slot && v == red_acc) { c.kind = VK::Acc; break; }
                if (slot_is_temp(d->idx)) {
                    VReg sv = temp_store_val(d->idx);
                    c.kind = classify(sv);
                    c.mov_src = sv;
                    break;
                }
                // 不变量槽：循环内无写、不取地址。
                if (si[d->idx].writers != 0 || !simd_slot_unaddressed(d->idx)) { c.kind = VK::Fail; break; }
                TypePtr ty = ty_of(v);
                if (ir_is_float(ty)) { c.kind = VK::InvF; inv_floats.push_back(v); }
                else if (ty && ty->kind == TyKind::Int) c.kind = VK::InvI;
                else if (ty && ty->kind == TyKind::List) c.kind = VK::InvPtr;  // 透明不变量基址
                else c.kind = VK::Fail;
                break;
            }
            case IrOp::LoadGlobal: {
                TypePtr ty = ty_of(v);
                if (ir_is_float(ty)) { c.kind = VK::InvF; inv_floats.push_back(v); }
                else if (ty && ty->kind == TyKind::Int) c.kind = VK::InvI;
                else if (ty && ty->kind == TyKind::List) c.kind = VK::InvPtr;  // 透明不变量基址
                else c.kind = VK::Fail;
                break;
            }
            case IrOp::Bin: {
                VK ka = classify(d->a), kb = classify(d->b);
                bool fa = ir_is_float(ty_of(d->a)), fb = ir_is_float(ty_of(d->b));
                auto float_ok = [](VK k) { return k == VK::InvF || k == VK::VecF; };
                if (fa && fb && float_ok(ka) && float_ok(kb)) {
                    switch (d->bop) {
                        case BinKind::Add: case BinKind::Sub:
                        case BinKind::Mul: case BinKind::Div:
                            c.kind = VK::VecF; break;
                        default: c.kind = VK::Fail;
                    }
                } else {
                    c.kind = VK::Fail;                    // int 运算在增量 2 支持
                }
                break;
            }
            case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked: {
                int ek = list_elem_float(d->a);
                int64_t off = index_off(d->b);
                if (ek < 0 || off < 0) { c.kind = VK::Fail; break; }
                if (!ir_is_float(ty_of(v)) && ek == 0) { c.kind = VK::Fail; break; }
                c.kind = ek == 0 ? VK::VecF : VK::VecI;
                c.list = d->a; c.off = off; c.is_load = true;
                accesses.push_back({d->a, off, false, static_cast<int>(d - BI.data())});
                break;
            }
            case IrOp::Convert:
                c.kind = VK::Fail;                        // int→float 在增量 2 支持
                break;
            default:
                c.kind = VK::Fail;
        }
        cls[v] = c;
        return c.kind;
    };

    // 体块指令角色判定 + 分类。任何非结构指令必须可向量化或可跳过。
    enum class Role : uint8_t { Skip, Load, Bin, MapStore, RedStore, Fail };
    std::vector<Role> role(BI.size(), Role::Skip);
    for (size_t i = 0; i < BI.size(); ++i) {
        const Inst& in = BI[i];
        switch (in.op) {
            case IrOp::ConstInt: case IrOp::ConstFloat: case IrOp::Jump:
                role[i] = Role::Skip; break;
            case IrOp::StoreSlot:
                if (slot_is_jalias(in.idx) || slot_is_temp(in.idx)) role[i] = Role::Skip;
                else if (in.idx == red_slot) role[i] = Role::RedStore;
                else role[i] = Role::Fail;
                break;
            case IrOp::LoadSlot: {
                // j-alias/临时/不变量/acc 装载都是透明引用，不单独发射。
                VK k = classify(in.dst);
                role[i] = (k == VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::Mov: {
                VK k = classify(in.dst);
                role[i] = (k == VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::LoadGlobal: {
                VK k = classify(in.dst);
                role[i] = (k == VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked: {
                VK k = classify(in.dst);
                role[i] = (k == VK::VecF || k == VK::VecI) ? Role::Load : Role::Fail;
                break;
            }
            case IrOp::Bin: {
                if (in.dst == red_add) { role[i] = Role::Skip; break; }  // reduction add 折叠进 RedStore
                if (index_off(in.dst) >= 0) { role[i] = Role::Skip; break; }  // 仿射下标计算折叠进寻址位移
                VK k = classify(in.dst);
                role[i] = (k == VK::VecF || k == VK::VecI) ? Role::Bin : Role::Fail;
                break;
            }
            case IrOp::IndexStore: case IrOp::IndexStoreUnchecked: {
                int ek = list_elem_float(in.a);
                int64_t off = index_off(in.b);
                VK kc = classify(in.c);
                bool ok = ek >= 0 && off >= 0 &&
                          ((ek == 0 && (kc == VK::VecF || kc == VK::InvF)) ||
                           (ek == 1 && (kc == VK::VecI || kc == VK::InvI)));
                if (!ok) { role[i] = Role::Fail; break; }
                // 存储值类型须与元素类型一致。
                if (ek == 0 && !ir_is_float(ty_of(in.c))) { role[i] = Role::Fail; break; }
                if (ek == 1 && (!ty_of(in.c) || ty_of(in.c)->kind != TyKind::Int)) { role[i] = Role::Fail; break; }
                role[i] = Role::MapStore;
                accesses.push_back({in.a, off, true, static_cast<int>(i)});
                break;
            }
            default:
                role[i] = Role::Fail;
        }
        if (role[i] == Role::Fail) { failed = true; break; }
    }
    if (failed) return false;
    // reduction expr 必须可向量化且不含 acc。
    if (red_slot != -1) {
        VK ke = classify(red_expr);
        bool ok = red_float ? (ke == VK::VecF || ke == VK::InvF)
                            : (ke == VK::VecI || ke == VK::InvI);
        if (!ok) return false;
    }
    // 至少要有一个 map 存储或 reduction（否则循环没有可向量化副作用）。
    bool has_side = red_slot != -1;
    for (Role r : role) if (r == Role::MapStore) has_side = true;
    if (!has_side) return false;

    // ── 别名/依赖分析 ────────────────────────────────────────────────────
    // 按 list 分组。同对象读+写：只允许全偏移 0 且读在写前（逐元素原地）。
    std::unordered_map<std::string, std::vector<Access>> by_list;
    std::unordered_map<std::string, VReg> list_rep;
    for (const Access& a : accesses) {
        std::string k = list_key(a.list);
        by_list[k].push_back(a);
        list_rep[k] = a.list;
    }
    for (auto& [lv, accs] : by_list) {
        bool has_r = false, has_w = false;
        int64_t maxoff = 0; int min_read_pos = 1 << 30, max_write_pos = -1;
        for (const Access& a : accs) {
            maxoff = std::max(maxoff, a.off);
            if (a.write) { has_w = true; max_write_pos = std::max(max_write_pos, a.pos); }
            else { has_r = true; min_read_pos = std::min(min_read_pos, a.pos); }
        }
        if (has_r && has_w) {
            for (const Access& a : accs) if (a.off != 0) return false;
            if (min_read_pos > max_write_pos) return false;   // 读必须在写前
        }
        if (has_w && maxoff != 0) {
            // 写偏移非 0：跨迭代依赖距离 = 写偏移 - 读偏移，保守只允许纯写且
            // 偏移 0（本轮）。纯写非 0 偏移（如 out[j+1]=...）跨迭代写写不相交
            // 但读侧复杂 → 保守跳过。
            return false;
        }
    }
    // 不同 list vreg 两两运行时判等（循环版本化别名守卫）。
    std::vector<std::string> lists;
    for (auto& [lv, accs] : by_list) lists.push_back(lv);

    // ── 寄存器分配可行性（发射前 dry-run）────────────────────────────────
    // GPR：j（提升 GPR 或 rsi 工作）；data 指针每 list 一个；rax 循环临时。
    const MOperand& jloc = slot_mop(sJ);
    const bool j_stack = jloc.kind != MOperandKind::Reg;
    if (!j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    const MReg JREG = j_stack ? kRsi : jloc.reg;
    // data GPR 池：rdi/rdx/rcx +（rsi 若 j 提升）。rax 留作循环条件临时。
    std::vector<MReg> gpr_pool{kRdi, kRdx, kRcx};
    if (!j_stack) gpr_pool.push_back(kRsi);
    if (lists.size() > gpr_pool.size()) return false;
    std::unordered_map<std::string, MReg> data_reg;
    for (size_t i = 0; i < lists.size(); ++i) data_reg[lists[i]] = gpr_pool[i];

    // float reduction acc 位置。
    const MOperand tloc = red_slot != -1 ? slot_mop(red_slot) : MOperand{};
    bool acc_stack = false; MReg XT = kXmm7;
    if (red_slot != -1 && red_float) {
        acc_stack = tloc.kind != MOperandKind::Reg;
        if (!acc_stack) {
            if (tloc.reg < kXmm8 || tloc.reg > kXmm15) return false;
            XT = tloc.reg;
        }
    }
    // int reduction acc：2-lane xmm scratch + 出口水平求和写回槽。
    bool red_int = red_slot != -1 && !red_float;
    MReg IACC = kXmm7;
    const MOperand iloc = red_int ? slot_mop(red_slot) : MOperand{};
    if (red_int && iloc.kind == MOperandKind::Reg &&
        (iloc.reg < kRax || iloc.reg > kR15)) return false;

    // xmm 池：xmm0-7。先分配持久不变量广播 + acc，再给临时值线性扫描。
    std::vector<MReg> xmm_pool{kXmm0, kXmm1, kXmm2, kXmm3, kXmm4, kXmm5, kXmm6, kXmm7};
    auto take = [&](MReg prefer) -> MReg {
        for (size_t i = 0; i < xmm_pool.size(); ++i)
            if (xmm_pool[i] == prefer) { MReg r = xmm_pool[i]; xmm_pool.erase(xmm_pool.begin() + static_cast<long>(i)); return r; }
        if (xmm_pool.empty()) return kMRegNone;
        MReg r = xmm_pool.front(); xmm_pool.erase(xmm_pool.begin()); return r;
    };
    // 去重不变量 float（Mov/临时槽别名解析到同一生产者）。
    auto resolve_src = [&](VReg v) -> VReg {
        while (true) {
            auto it = cls.find(v);
            if (it == cls.end() || it->second.mov_src == kNoReg) return v;
            v = it->second.mov_src;
        }
    };
    std::vector<VReg> uniq_inv;
    for (VReg v : inv_floats) {
        VReg r = resolve_src(v);
        bool dup = false;
        for (VReg u : uniq_inv) if (u == r) dup = true;
        if (!dup) uniq_inv.push_back(r);
    }
    std::unordered_map<VReg, MReg> inv_reg;
    for (VReg v : uniq_inv) {
        MReg r = take(kMRegNone);
        if (r == kMRegNone) return false;
        inv_reg[v] = r;
    }
    if (red_float && acc_stack) { XT = take(kXmm7); if (XT == kMRegNone) return false; }
    if (red_int) { IACC = take(kXmm7); if (IACC == kMRegNone) return false; }

    // 临时值线性扫描：按体块顺序，VecF/VecI 生产者分配 xmm，操作数末次使用后释放。
    std::unordered_map<VReg, int> last_use;
    for (size_t i = 0; i < BI.size(); ++i) {
        const Inst& in = BI[i];
        VReg ops[3] = {in.a, in.b, in.c};
        for (VReg op : ops) if (op != kNoReg) last_use[resolve_src(op)] = static_cast<int>(i);
    }
    // reduction expr 的末次使用在 RedStore（add 已折叠，不算独立使用点）。
    if (red_slot != -1) last_use[resolve_src(red_expr)] = red_store_pos;
    std::unordered_map<VReg, MReg> tmp_reg;
    auto xmm_of = [&](VReg v) -> MReg {
        VReg r = resolve_src(v);
        auto it = cls.find(r);
        if (it == cls.end()) return kMRegNone;
        if (it->second.kind == VK::InvF || it->second.kind == VK::InvI) {
            auto ir = inv_reg.find(r);
            return ir == inv_reg.end() ? kMRegNone : ir->second;
        }
        auto tr = tmp_reg.find(r);
        return tr == tmp_reg.end() ? kMRegNone : tr->second;
    };
    for (size_t i = 0; i < BI.size(); ++i) {
        const Inst& in = BI[i];
        if (role[i] == Role::Load || role[i] == Role::Bin) {
            VReg r = resolve_src(in.dst);
            MReg rd = take(kMRegNone);
            if (rd == kMRegNone) return false;
            tmp_reg[r] = rd;
        }
        // 释放本指令操作数中末次使用的临时值。
        VReg ops[3] = {in.a, in.b, in.c};
        for (VReg op : ops) {
            if (op == kNoReg) continue;
            VReg r = resolve_src(op);
            auto lu = last_use.find(r);
            if (lu != last_use.end() && lu->second == static_cast<int>(i)) {
                auto tr = tmp_reg.find(r);
                if (tr != tmp_reg.end()) xmm_pool.push_back(tr->second);  // tmp_reg 永久保留供发射
            }
        }
    }

    // ── 全部分析通过 → 发射向量前奏 ──────────────────────────────────────
    const std::string tail = fresh_label("vecttail");
    const std::string vec = fresh_label("vectvec");
    if (std::getenv("PYADAMAS_VECT_DBG"))
        std::fprintf(stderr, "[vect] vectorize loop body bb%d (sJ=%d red=%d lists=%zu inv=%zu)\n",
                     body_bi, sJ, red_slot, lists.size(), uniq_inv.size());
    comment("VECT generic loop (PYADAMAS_VECT): SSE2 2-lane map/reduction");
    if (j_stack) emit2(MOp::kMovq, jloc, JREG);

    // 守卫：j+1<n、j>=0。
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJge, tail);
    emit2(MOp::kTestq, JREG, JREG);
    emit_jmp(MOp::kJs, tail);
    // 每 list：len>=n+maxoff（溢出检查）、kind==0、data 指针。
    for (const std::string& lk : lists) {
        int64_t maxoff = 0;
        for (const Access& a : by_list[lk]) maxoff = std::max(maxoff, a.off);
        emit2(MOp::kMovq, list_base_mop(list_rep[lk]), kRax);
        emit2(MOp::kMovq, op_mem(kRax, 0), kRcx);          // len
        if (maxoff == 0) {
            emit2(MOp::kCmpq, n_op, kRcx);
            emit_jmp(MOp::kJl, tail);
        } else {
            emit2(MOp::kMovq, n_op, kRcx);
            emit_imm(MOp::kAddq, maxoff, kRcx);
            emit_jmp(MOp::kJs, tail);
            emit2(MOp::kMovq, op_mem(kRax, 0), kRdx);
            emit2(MOp::kCmpq, kRcx, kRdx);
            emit_jmp(MOp::kJl, tail);
        }
        emit_imm(MOp::kCmpq, 0, op_mem(kRax, 24));
        emit_jmp(MOp::kJne, tail);
        emit2(MOp::kMovq, op_mem(kRax, 16), data_reg[lk]);  // data 指针常驻
    }
    // 别名守卫：不同 list 对象两两判等。
    for (size_t i = 0; i < lists.size(); ++i)
        for (size_t j = i + 1; j < lists.size(); ++j) {
            emit2(MOp::kMovq, list_base_mop(list_rep[lists[i]]), kRax);
            emit2(MOp::kCmpq, list_base_mop(list_rep[lists[j]]), kRax);
            // 相等 → 不同 vreg 可能持同一对象，无法静态证明偏移安全 → 保守标量。
            emit_jmp(MOp::kJe, tail);
        }
    // setup：不变量 float 广播。
    for (VReg v : uniq_inv) {
        MReg rr = inv_reg[v];
        const Inst* d = simd_def(v);
        if (d && d->op == IrOp::ConstFloat) {
            emit_imm_hex(MOp::kMovq, f64_bits(d->fimm), kRax);
            emit2(MOp::kMovq, kRax, rr);
        } else {
            emit2(MOp::kMovsd, src_mop(v), rr);
        }
        emit2(MOp::kMovlhps, rr, rr);
    }
    if (red_float && acc_stack) emit2(MOp::kMovsd, tloc, XT);
    if (red_int) emit2(MOp::kPxor, IACC, IACC);

    // 向量循环体。
    label(vec);
    for (size_t i = 0; i < BI.size(); ++i) {
        const Inst& in = BI[i];
        switch (role[i]) {
            case Role::Load: {
                VReg r = resolve_src(in.dst);
                MReg rd = tmp_reg[r];
                int64_t off = cls[r].off;
                emit2(MOp::kMovupd, op_mem(data_reg[list_key(cls[r].list)], JREG, 8, off * 8), rd);
                break;
            }
            case Role::Bin: {
                VReg r = resolve_src(in.dst);
                MReg rd = tmp_reg[r];
                MReg ra = xmm_of(in.a), rb = xmm_of(in.b);
                if (ra == kMRegNone || rb == kMRegNone) return false;  // 不应发生
                if (rd != ra) emit2(MOp::kMovapd, ra, rd);
                MOp op = MOp::kAddpd;
                switch (in.bop) {
                    case BinKind::Add: op = MOp::kAddpd; break;
                    case BinKind::Sub: op = MOp::kSubpd; break;
                    case BinKind::Mul: op = MOp::kMulpd; break;
                    case BinKind::Div: op = MOp::kDivpd; break;
                    default: return false;
                }
                emit2(op, rb, rd);
                break;
            }
            case Role::MapStore: {
                int64_t off = index_off(in.b);
                MReg rc = xmm_of(in.c);
                if (rc == kMRegNone) return false;
                emit2(MOp::kMovupd, rc, op_mem(data_reg[list_key(in.a)], JREG, 8, off * 8));
                break;
            }
            case Role::RedStore: {
                MReg re = xmm_of(red_expr);
                if (re == kMRegNone) return false;
                if (red_float) {
                    // 保序提取累加：acc += e0; acc += e1（与标量逐位一致）。
                    emit2(MOp::kAddsd, re, XT);
                    emit2(MOp::kMovhlps, re, re);
                    emit2(MOp::kAddsd, re, XT);
                    if (acc_stack) emit2(MOp::kMovsd, XT, tloc);
                } else {
                    // int：paddq 2-lane 累加（模 2^64 精确），出口水平求和。
                    emit2(MOp::kPaddq, re, IACC);
                }
                break;
            }
            default: break;
        }
    }
    // j += 2（+ 写回栈）。
    emit_imm(MOp::kAddq, 2, JREG);
    if (j_stack) emit2(MOp::kMovq, JREG, jloc);
    // 循环条件：j+1<n。
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJl, vec);
    // 出口：int reduction 水平求和写回槽（标量尾/cond/end 读最新）。
    if (red_int) {
        const MReg hscratch = (IACC == kXmm0) ? kXmm1 : kXmm0;
        emit2(MOp::kMovhlps, IACC, hscratch);
        emit2(MOp::kPaddq, hscratch, IACC);
        emit2(MOp::kMovq, IACC, kRax);
        emit2(MOp::kMovq, kRax, iloc);
    }
    // jmp cond：j==n 精确退出；j==n-1 经 cond 回体块走标量尾（入口守卫失败）。
    emit_jmp(MOp::kJmp, block_label(cond_bi));
    label(tail);                                          // 其后 gen_block 照常发标量体
    return true;
}

}  // namespace pyadamas
