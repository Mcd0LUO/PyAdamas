// vect_planner_loop.cpp —— 通用循环向量化的入口 VectPlanner::try_loop
//
// 阶段 A4 第 3 步：从 AsmGen::try_vect_loop（原 src/codegen_vect.cpp）整体搬来。
// 只做机械改写：AsmGen 成员/私有辅助访问 → gen_. / AsmGen::（VectPlanner 是
// AsmGen 的 friend class），Loop 的方法调用保持 L.xxx。判据/阈值/发射顺序不变。
#include "vect_planner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "codegen_impl.h"   // AsmGen 完整定义（gen_ 的成员访问）

namespace pyadamas {

namespace {

// double 的位模式（与 codegen_vect_jvec.cpp 的同名 helper 同口径）。
uint64_t f64_bits(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return u;
}

}  // namespace

bool VectPlanner::try_loop(const Block& b) {
    if (!gen_.vect_func_ok()) return false;
    // 阶段 A4 第 1/2 步：本次调用的全部局部状态收进 VectPlanner::Loop。
    Loop L(*this, gen_, b.insts);
    L.body_bi = gen_.cur_block_;
    if (L.body_bi < 0 || L.body_bi >= static_cast<int>(gen_.f_->blocks.size())) return false;
    if (L.BI.size() < 4 || L.BI.size() > 64) return false;    // 体块规模上限（保守）
    if (L.BI.back().op != IrOp::Jump) return false;

    // ── 循环结构 ────────────────────────────────────────────────────
    // 两种形状（0.387 起都认）：
    //   for  ：cond / body / incr 三块，体块跳 incr、incr 跳 cond；
    //   while：cond / body 两块，体块直接跳回 cond，增量在体块尾部三条
    //          （load sJ / add 1 / store sJ）。手写的 `while i < n: …; i += 1`
    //          降成后者，以前整段被跳过 —— 而它是最常见的手写形式。
    L.incr_bi = L.BI.back().target;
    if (L.incr_bi < 0 || L.incr_bi >= static_cast<int>(gen_.f_->blocks.size()) || L.incr_bi == L.body_bi)
        return false;
    {
        const auto& INC = gen_.f_->blocks[static_cast<size_t>(L.incr_bi)].insts;
        bool shaped = false;
        if (!INC.empty() && INC[0].op == IrOp::LoadSlot) {
            L.sJ = INC[0].idx;
            shaped = gen_.simd_incr_cond_ok(L.incr_bi, L.body_bi, L.sJ, L.cond_bi, L.end_bi, L.n);
        }
        if (!shaped && gen_.simd_while_shape_ok(L.incr_bi, L.body_bi, L.sJ, L.end_bi, L.n, L.jupd_pos)) {
            // while：增量在体块里，后续「循环三块」检查用体块顶上（重复访问无害）。
            L.cond_bi = L.incr_bi;
            L.incr_bi = L.body_bi;
            shaped = true;
        }
        if (!shaped) return false;
    }
    if (!gen_.simd_outside(L.n, L.cond_bi, L.body_bi, L.incr_bi) ||
        !gen_.simd_slot_stable(L.n, L.cond_bi, L.body_bi, L.incr_bi)) return false;
    if (L.sJ < 0 || L.sJ >= static_cast<int>(gen_.f_->slot_ty.size())) return false;
    { TypePtr tj = gen_.f_->slot_ty[static_cast<size_t>(L.sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    if (!gen_.simd_slot_unaddressed(L.sJ)) return false;
    // 循环三块 op 白名单：除 math.sqrt 外无调用/全局写/取地址（下标读写放行，
    // 位置另判）。sqrt 的发射序列见下面 CallRt 分支（负数守卫跳标量尾）。
    if (!gen_.simd_loop_ops_ok(L.cond_bi, L.body_bi, L.incr_bi, true, true)) return false;

    L.n_op = gen_.src_mop(L.n);
    if (!AsmGen::simd_scratch_safe(L.n_op)) return false;

    // ── 槽分析：j-alias / reduction / 体局部临时 / 其余写一律拒绝 ─────────
    // 收集三块内所有槽写/读位置。
    for (int bi = 0; bi < static_cast<int>(gen_.f_->blocks.size()); ++bi)
        for (const Inst& in : gen_.f_->blocks[static_cast<size_t>(bi)].insts) {
            if (in.op == IrOp::SlotAddr) {
                // 任何被取地址的槽都不能跟踪（间接读写）→ 若涉及本循环槽则拒绝。
                L.si[in.idx].outside_read = true;           // 保守标记
            }
            if (in.op == IrOp::StoreSlot) {
                auto& s = L.si[in.idx];
                if (L.in_loop(bi)) { ++s.writers; if (bi == L.body_bi) { s.store_pos = static_cast<int>(&in - L.BI.data()); s.store_val = in.a; } }
            }
            if (in.op == IrOp::LoadSlot) {
                auto& s = L.si[in.idx];
                if (L.in_loop(bi)) {
                    ++s.loads;
                    if (bi == L.body_bi)
                        s.min_load_pos = std::min(s.min_load_pos, static_cast<int>(&in - L.BI.data()));
                } else {
                    s.outside_read = true;
                }
            }
        }
    // 读在写前判定须在完整扫描后做（store_pos 此时才齐全）：任何体块装载
    // 早于该槽的唯一存储 → read_before_store（否则误判临时槽 → classify 自环）。
    for (auto& [slot, info] : L.si)
        info.read_before_store = (info.store_pos >= 0 && info.min_load_pos < info.store_pos);
    // sJ：只被 incr 写一次。
    if (L.si[L.sJ].writers != 1) return false;

    // j-alias 槽：sJ 自身 + 「体块内单写、写值来自 j-alias 装载、读全在写后、
    // 无外部读、不取地址」的副本槽。副本槽存储跳过（外部无读 → 死值）。
    L.jalias = {L.sJ};
    // 迭代到不动点（副本链通常只有一层）。
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [slot, info] : L.si) {
            if (slot == L.sJ) continue;
            bool already = false;
            for (int s : L.jalias) if (s == slot) already = true;
            if (already) continue;
            if (info.writers != 1 || info.store_pos < 0 || info.outside_read ||
                info.read_before_store || !gen_.simd_slot_unaddressed(slot)) continue;
            if (!L.is_jalias_load(info.store_val)) continue;
            TypePtr ty = slot < static_cast<int>(gen_.f_->slot_ty.size())
                             ? gen_.f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
            if (!ty || ty->kind != TyKind::Int) continue;
            L.jalias.push_back(slot);
            changed = true;
        }
    }

    // reduction 槽（可多个并行）：float/int，体块内恰一写一读，写值 = 读值 +
    // expr（或 expr + 读值）或读值 - expr（float 减号归约），读在写前（SSA 必然），
    // 无其它循环内读；外部读允许（终值要维护）。各累加器独立向量寄存器，出口
    // 各自写回（float 保序提取，int paddq 模 2^64 + 水平求和）。expr 含另一 acc
    // 的读 → loads!=1 或 classify Acc → 保守拒绝（跨累加器依赖不证）。
    using Red = VRed;                   // 定义在 vect_planner.h（4-lane 也要读）
    using Access = VAccess;             // 同上：下标访问描述只有一份
    for (auto& [slot, info] : L.si) {
        if (L.slot_is_jalias(slot) || slot == L.sJ) continue;
        if (info.writers != 1 || info.store_pos < 0 || info.loads != 1) continue;
        // acc 槽的读**必须**在写前（读旧值→写新值）；loads==1 保证无其它循环内读。
        // 注意与临时槽判据相反：临时槽要求读全在写后（!read_before_store）。
        if (!info.read_before_store || !gen_.simd_slot_unaddressed(slot)) continue;
        TypePtr ty = slot < static_cast<int>(gen_.f_->slot_ty.size())
                         ? gen_.f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
        if (!ty || (ty->kind != TyKind::Float && ty->kind != TyKind::Int)) continue;
        if (ir_is_heap(ty)) continue;
        // 找体块内的 load（acc 旧值）与 store。
        VReg acc = kNoReg; const Inst* st = nullptr;
        for (const Inst& in : L.BI) {
            if (in.op == IrOp::LoadSlot && in.idx == slot) acc = in.dst;
            if (in.op == IrOp::StoreSlot && in.idx == slot) st = &in;
        }
        if (acc == kNoReg || !st) continue;
        // 归约算符：s += expr（expr 在任一侧）或 s -= expr（只支持 float，
        // 且必须是「acc - expr」方向；发射期按 lane 顺序 subsd，与标量逐位一致）。
        // int 的 Sub 不做：paddq 那条线只覆盖加，neg 还要额外 scratch，保守留给标量。
        const Inst* add = gen_.simd_def(st->a);
        if (!add || add->op != IrOp::Bin) continue;
        const bool is_sub =
            add->bop == BinKind::Sub && ty->kind == TyKind::Float && add->a == acc;
        if (!is_sub && add->bop != BinKind::Add) continue;
        VReg expr = kNoReg;
        if (is_sub) expr = add->b;
        else if (add->a == acc) expr = add->b;
        else if (add->b == acc) expr = add->a;
        else continue;
        if (expr == kNoReg || expr == acc) continue;
        Red rd;
        rd.slot = slot; rd.acc = acc; rd.expr = expr; rd.add = st->a;
        rd.store_pos = static_cast<int>(st - L.BI.data());
        rd.is_float = ty->kind == TyKind::Float;
        rd.is_sub = is_sub;
        L.reds.push_back(rd);
    }
    // 确定性顺序（unordered_map 遍历序不影响正确性，但稳定 asm 便于比对）。
    std::sort(L.reds.begin(), L.reds.end(), [](const Red& x, const Red& y) { return x.slot < y.slot; });

    // 体局部临时槽：单写、读全在写后、无外部读、非堆、不取地址 → 跳过存储，
    // 其 load 替换为所存值。其余有循环内写的槽 → 拒绝（无法维护）。
    for (auto& [slot, info] : L.si) {
        if (slot == L.sJ || L.slot_is_jalias(slot) || L.slot_is_red(slot)) continue;
        if (info.writers == 0) continue;                  // 只读槽：不变量候选
        if (info.writers == 1 && info.store_pos >= 0 && !info.outside_read &&
            !info.read_before_store && gen_.simd_slot_unaddressed(slot)) {
            TypePtr ty = slot < static_cast<int>(gen_.f_->slot_ty.size())
                             ? gen_.f_->slot_ty[static_cast<size_t>(slot)] : nullptr;
            if (ty && !ir_is_heap(ty) &&
                (ty->kind == TyKind::Float || ty->kind == TyKind::Int)) {
                L.temp_slots.push_back(slot);
                continue;
            }
        }
        return false;                                     // 未处理的槽写 → 保守跳过
    }
    // 临时槽的 store_val（替换用）。

    // ── 表达式 DAG 分类（ memoized 递归）─────────────────────────────────
    L.acc_at.assign(L.BI.size(), -1);                    // 体块位置 → accesses 下标

    // 下标仿射偏移：j → 0；j+c（c>=0 常量）→ c；否则 -1。
    // 规范化数组基址：body 内 load.slot/load.global/Mov 解析到稳定来源；
    // 同一槽/全局/跨块值的不同 vreg 归并为同一逻辑数组（非内联函数体内装载）。
    // 跨块装载的值在装载时刻冻结：槽/全局其后可能被改写（内联 RC move-from
    // 把槽清 0 是真实形态——mov 链解析到槽现值会取到 NULL → 段错误）。跨块
    // 一律取 vreg 自己的 home（分配器保证使用点值在 home）；循环内装载的 dst
    // 此刻尚未发射，只能取槽/全局现值（循环内无写 → 循环期间稳定）。
    // 逻辑数组身份：循环内装载同槽/全局 ≡ 同一数组（循环期间稳定）；跨块
    // 装载按 vreg 身份（同槽不同时刻的装载可能不同值，不得归并免别名守卫）。

    // 仿射下标的「不变量」候选：循环不变 int 值。跨块值（外层次计算/LICM
    // 外提）与「体内不写、不取地址」的槽装载都算。体局部临时槽的值每轮都变
    // （循环携带），不算 —— 否则基址会被固定成入口那一轮的值。


    // 使用点分类（P5b）：仿射式折叠进寻址的前提是「只当下标用」。
    gen_.vect_mark_nonindex_uses(&L.nonindex_use);
    // 体块指令角色判定 + 分类。任何非结构指令必须可向量化或可跳过。
    // 角色定义在 codegen_vect.h：AVX2 4-lane 发射器按同一张表逐条重放。
    using Role = VRole;
    L.role.assign(L.BI.size(), Role::Skip);
    bool saw_map_store = false;   // sqrt 之前不允许有 map 写（见 CallRt 分支）
    for (size_t i = 0; i < L.BI.size(); ++i) {
        const Inst& in = L.BI[i];
        // while 形状：体块尾部三条是归纳变量的增量，由向量循环自己维护 j，
        // 不参与逐元素计算（不跳过的话 `add` 会落进 classify 的 Fail）。
        if (L.jupd_pos >= 0 && i >= static_cast<size_t>(L.jupd_pos) &&
            i < static_cast<size_t>(L.jupd_pos) + 3) {
            L.role[i] = Role::Skip;
            continue;
        }
        switch (in.op) {
            case IrOp::ConstInt: case IrOp::ConstFloat: case IrOp::Jump:
                L.role[i] = Role::Skip; break;
            case IrOp::StoreSlot:
                if (L.slot_is_jalias(in.idx) || L.slot_is_temp(in.idx)) L.role[i] = Role::Skip;
                else if (L.slot_is_red(in.idx)) L.role[i] = Role::RedStore;
                else L.role[i] = Role::Fail;
                break;
            case IrOp::LoadSlot: {
                // j-alias/临时/不变量/acc 装载都是透明引用，不单独发射。
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::Mov: {
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::LoadGlobal: {
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::Fail) ? Role::Fail : Role::Skip;
                break;
            }
            case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked: {
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::VecF || k == vect::VK::VecI) ? Role::Load : Role::Fail;
                break;
            }
            case IrOp::Bin: {
                if (L.is_red_add(in.dst)) { L.role[i] = Role::Skip; break; }  // reduction add 折叠进 RedStore
                // 仿射下标计算（j+c / coef*j+inv+off）折叠进寻址位移/基址，不发射。
                // 两个前提：coef>0（真的含 j；coef==0 的仿射式是不变量，可能被
                // 别的算式使用，必须照常 classify/发射），且**全部使用点都是
                // 下标**（P5b：j 参与算术后，i+j 这类值可能既当下标又当算术语；
                // 折叠掉却仍被算术引用 → 发射期查不到寄存器 → 半截 asm）。
                { AffIdx af = vect_affine_index(in.dst, L.jalias, L.inv_int_pred());
                  if (af.ok && af.coef > 0 &&
                      L.nonindex_use.find(in.dst) == L.nonindex_use.end()) {
                      L.role[i] = Role::Skip; break;
                  } }
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::VecF || k == vect::VK::VecI) ? Role::Bin : Role::Fail;
                break;
            }
            case IrOp::Convert: {
                vect::VK k = L.classify(in.dst);
                L.role[i] = (k == vect::VK::VecF) ? Role::Bin : Role::Fail;  // 发射按 op 分支
                break;
            }
            case IrOp::IndexStore: case IrOp::IndexStoreUnchecked: {
                int ek = L.list_elem_float(in.a);
                AffIdx af = vect_affine_index(in.b, L.jalias, L.inv_int_pred());
                vect::VK kc = L.classify(in.c);
                // 写出仍只支持 coef==1：两 lane 相邻才能 movupd 整存，跨步写
                // 是 scatter（SSE2 没有，且没有收益）→ 保守跳过整段。
                bool ok = ek >= 0 && af.ok && af.coef == 1 &&
                          af.off >= 0 && af.off <= kAffMaxOff &&
                          ((ek == 0 && (kc == vect::VK::VecF || kc == vect::VK::InvF)) ||
                           (ek == 1 && (kc == vect::VK::VecI || kc == vect::VK::InvI)));
                if (!ok) { L.role[i] = Role::Fail; break; }
                // 存储值类型须与元素类型一致。
                if (ek == 0 && !ir_is_float(gen_.ty_of(in.c))) { L.role[i] = Role::Fail; break; }
                if (ek == 1 && (!gen_.ty_of(in.c) || gen_.ty_of(in.c)->kind != TyKind::Int)) { L.role[i] = Role::Fail; break; }
                L.role[i] = Role::MapStore;
                Access ac;
                ac.list = in.a; ac.off = af.off; ac.coef = 1; ac.inv = af.inv;
                ac.write = true; ac.pos = static_cast<int>(i);
                L.acc_at[i] = static_cast<int>(L.accesses.size());
                L.accesses.push_back(ac);
                break;
            }
            case IrOp::CallRt: {
                // math.sqrt：只允许出现在所有 reduction 写与所有 map 写之前。
                // 负数守卫会跳标量尾重放本轮：本轮若已有 RedStore 会重复累加
                //（int reduction 的历史值还在向量累加器里、未写回槽，退出会丢）；
                // 本轮若已有 map 写，向量循环已把相邻 lane（j+1）也写了，而标量
                // 重放可能在本轮抛 ValueError → 那一 lane 的写泄漏（语义不同）。
                // 临时槽/ja-alias 槽的写在标量重放时幂等且不可观测，不受限。
                bool pos_ok = !saw_map_store;
                for (const Red& rr : L.reds)
                    if (!rr.is_float || rr.store_pos <= static_cast<int>(i)) pos_ok = false;
                vect::VK k = L.classify(in.dst);
                L.role[i] = (pos_ok && k == vect::VK::VecF) ? Role::Bin : Role::Fail;
                break;
            }
            default:
                L.role[i] = Role::Fail;
        }
        if (L.role[i] == Role::Fail) { L.failed = true; break; }
        if (L.role[i] == Role::MapStore) saw_map_store = true;
    }
    if (L.failed) return false;
    // 每个 reduction expr 必须可向量化且不含 acc（Acc 分类不在允许集 →
    // 跨累加器依赖保守拒绝）。
    for (const Red& r : L.reds) {
        vect::VK ke = L.classify(r.expr);
        bool ok = r.is_float ? (ke == vect::VK::VecF || ke == vect::VK::InvF)
                             : (ke == vect::VK::VecI || ke == vect::VK::InvI);
        if (!ok) return false;
    }
    // 至少要有一个 map 存储或 reduction（否则循环没有可向量化副作用）。
    bool has_side = !L.reds.empty();
    for (Role r : L.role) if (r == Role::MapStore) has_side = true;
    if (!has_side) return false;
    // ── 死值消除（只对含广播装载的新路径生效）─────────────────────────────
    // 含 coef==0（循环内不变下标）装载的循环里，常见「下标中间量被保守标记为
    // 非下标用途 → 发射成向量整数运算」的死值（如 `j*7` 只被折叠的仿射下标
    // 使用，却要 JVEC/SEQ1/SEQ2 三个 xmm）。它们会把 dry-run 的临时寄存器池
    // 挤爆，让整个循环退回标量。这里按 IR 使用关系做一次纯值消除：
    //   * 只删无副作用且不会抛异常的 Bin/Mov/Convert；除法/取模/幂可能抛
    //     ZeroDivisionError、sqrt 有 ValueError 语义 → 一律保留；
    //   * 删完重算 has_int_mul/has_convert/has_intdiv/has_j_operand，让序列
    //     scratch/JVEC 的分配跟着缩；
    //   * 不含 coef==0 访问的循环完全不走这段 → 现有产物 asm 逐字节不变。
    bool has_bcast = false;
    for (const Access& a : L.accesses) if (a.coef == 0) has_bcast = true;
    std::unordered_set<VReg> needed;
    if (has_bcast) {
        auto collect_needed = [&]() {
            needed.clear();
            for (size_t i = 0; i < L.BI.size(); ++i) {
                if (L.role[i] == Role::Skip) continue;
                const Inst& in = L.BI[i];
                const bool index_op = in.op == IrOp::IndexLoad ||
                                      in.op == IrOp::IndexLoadUnchecked ||
                                      in.op == IrOp::IndexStore ||
                                      in.op == IrOp::IndexStoreUnchecked;
                const VReg ops[3] = {in.a, in.b, in.c};
                for (int k = 0; k < 3; ++k) {
                    if (ops[k] == kNoReg) continue;
                    if (index_op && k == 1) continue;   // 下标操作数折进寻址，不占寄存器
                    needed.insert(L.resolve_src(ops[k]));
                }
                for (VReg op : in.args) if (op != kNoReg) needed.insert(L.resolve_src(op));
            }
            for (const Red& r : L.reds)
                if (r.expr != kNoReg) needed.insert(L.resolve_src(r.expr));
        };
        collect_needed();
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < L.BI.size(); ++i) {
                if (L.role[i] != Role::Bin) continue;
                const Inst& in = L.BI[i];
                if (in.dst == kNoReg) continue;
                if (in.op == IrOp::Bin) {
                    // 只删无副作用、不会抛异常的算符；除法/取模/幂/移位可能抛
                    // 异常或语义敏感（Python 负移位/负除数值语义），一律保留。
                    switch (in.bop) {
                        case BinKind::Add: case BinKind::Sub: case BinKind::Mul:
                        case BinKind::BitAnd: case BinKind::BitOr: case BinKind::BitXor:
                            break;
                        default: continue;
                    }
                } else if (in.op != IrOp::Mov && in.op != IrOp::Convert) {
                    continue;                                  // 其余（含 CallRt/sqrt）不动
                }
                if (needed.count(L.resolve_src(in.dst))) continue;
                L.role[i] = Role::Skip;
                collect_needed();
                changed = true;
            }
        }
        // 序列 scratch / JVEC 的分配 flag 按存活指令重算（死值不再占寄存器）。
        L.has_int_mul = L.has_convert = L.has_intdiv = false;
        L.has_j_operand = false;
        for (size_t i = 0; i < L.BI.size(); ++i) {
            if (L.role[i] != Role::Bin) continue;
            const Inst& in = L.BI[i];
            if (in.op == IrOp::Convert) { L.has_convert = true; continue; }
            if (in.op != IrOp::Bin) continue;
            TypePtr t = gen_.ty_of(in.dst);
            if (!t || t->kind != TyKind::Int) continue;
            if (in.bop == BinKind::Mul) L.has_int_mul = true;
            if (in.bop == BinKind::FloorDiv || in.bop == BinKind::Mod) L.has_intdiv = true;
            if (L.classify(in.a) == vect::VK::J || L.classify(in.b) == vect::VK::J)
                L.has_j_operand = true;
        }
    }
    // 不变量广播清单补全：classify 只在 float 常量/float 槽/int Bin 操作数处
    // 收集，但 ConstInt 还可直接出现在 map 存储值（c[i]=1）或 reduction expr
    // （s+=1）——这些 InvI 同样要广播。漏收 → 发射期 xmm_of 查无寄存器 →
    // 前奏已发却 return false → 悬空 tail 标签（链接失败）。统一扫 cls 补全，
    // uniq_inv 去重；发射期寄存器查询从此不可能失败（dry-run 完备）。
    for (auto& [v, c] : L.cls) {
        // 死值不占广播寄存器（只有含广播装载的循环会做上面的死值消除）。
        if (has_bcast && !needed.count(L.resolve_src(v))) continue;
        if (c.kind == vect::VK::InvF) L.inv_floats.push_back(v);
        else if (c.kind == vect::VK::InvI) L.inv_ints.push_back(v);
    }

    // ── 别名/依赖分析 ────────────────────────────────────────────────────
    // 按逻辑数组分组。同对象读+写：只允许「同一 (coef, off)、inv 运行期
    // 相等、读在写前」的逐元素原地 map —— 此时每个 lane 读写的都是同一个
    // 元素，跨 lane / 跨迭代都不存在依赖（coef>=1，相邻 lane 组互不相交）。
    // 其余同对象读写（偏移不同、inv 证不等）→ 保守跳过。inv 不同但可能相等
    // 的（如 c[i*N+j] 的读与写由两次等价计算得到）走运行期判等守卫。
    for (size_t ai = 0; ai < L.accesses.size(); ++ai) {
        std::string k = L.list_key(L.accesses[ai].list);
        L.by_list[k].push_back(static_cast<int>(ai));
        L.list_rep[k] = L.accesses[ai].list;
    }
    for (auto& [lv, accs] : L.by_list) L.lists.push_back(lv);
    for (const std::string& lk : L.lists) {
        const std::vector<int>& ix = L.by_list[lk];
        bool has_r = false, has_w = false;
        int64_t maxoff = 0; int min_read_pos = 1 << 30, max_write_pos = -1;
        for (int ai : ix) {
            const Access& a = L.accesses[static_cast<size_t>(ai)];
            maxoff = std::max(maxoff, a.off);
            if (a.write) { has_w = true; max_write_pos = std::max(max_write_pos, a.pos); }
            else { has_r = true; min_read_pos = std::min(min_read_pos, a.pos); }
        }
        if (has_w && maxoff != 0) {
            // 写偏移非 0：跨迭代依赖距离 = 写偏移 - 读偏移，保守只允许纯写且
            // 偏移 0（本轮）。纯写非 0 偏移（如 out[j+1]=...）跨迭代写写不相交
            // 但读侧复杂 → 保守跳过。
            return false;
        }
        if (!has_r || !has_w) continue;
        const Access& a0 = L.accesses[static_cast<size_t>(ix[0])];
        VReg rep = a0.inv;
        for (int ai : ix) {
            Access& a = L.accesses[static_cast<size_t>(ai)];
            if (a.coef != a0.coef || a.off != a0.off) return false;
            if (a.inv != rep) {
                bool seen = false;
                for (const AffEq& pr : L.inv_eq)
                    if ((pr.x == rep && pr.y == a.inv) ||
                        (pr.x == a.inv && pr.y == rep)) seen = true;
                if (!seen) {
                    AffEq eq;
                    eq.x = rep; eq.y = a.inv;
                    if (rep != kNoReg) eq.xm = L.list_base_mop(rep);
                    if (a.inv != kNoReg) eq.ym = L.list_base_mop(a.inv);
                    L.inv_eq.push_back(eq);
                }
                a.inv = rep;                              // 守卫后等价 → 归一到代表
            }
        }
        if (min_read_pos > max_write_pos) return false;   // 读必须在写前
    }
    // 基址键 = (逻辑数组, inv)：一个 GPR 存 data + inv*8；coef>1 的访问共用
    // 一个按 coef 去重的运行期跨步计数器 K。
    for (Access& a : L.accesses) {
        int bi = -1;
        std::string k = L.list_key(a.list);
        for (size_t j = 0; j < L.bases.size(); ++j)
            if (L.bases[j].inv == a.inv && L.bases[j].key == k) { bi = static_cast<int>(j); break; }
        if (bi < 0) {
            AffBase nb;
            nb.key = k; nb.inv = a.inv;
            if (a.inv != kNoReg) nb.inv_src = L.list_base_mop(a.inv);
            nb.coef = a.coef; nb.off_max = a.off; nb.off_min = a.off;
            L.bases.push_back(nb);
            bi = static_cast<int>(L.bases.size()) - 1;
        } else {
            L.bases[static_cast<size_t>(bi)].coef = std::max(L.bases[static_cast<size_t>(bi)].coef, a.coef);
            L.bases[static_cast<size_t>(bi)].off_max = std::max(L.bases[static_cast<size_t>(bi)].off_max, a.off);
            L.bases[static_cast<size_t>(bi)].off_min = std::min(L.bases[static_cast<size_t>(bi)].off_min, a.off);
        }
        a.bidx = bi;
    }
    for (const Access& a : L.accesses)
        // coef==0 是循环内不变下标（广播装载），没有跨步，不占 K 计数器。
        if (a.coef != 1 && a.coef != 0 &&
            std::find(L.kcoefs.begin(), L.kcoefs.end(), a.coef) == L.kcoefs.end())
            L.kcoefs.push_back(a.coef);
    std::sort(L.kcoefs.begin(), L.kcoefs.end());
    if (!vect_affine_plan_ok(L.bases)) return false;

    // ── 寄存器分配可行性（发射前 dry-run）────────────────────────────────
    // GPR：j（提升 GPR 或 rsi 工作）；data 指针每 list 一个；rax 循环临时。
    const MOperand& jloc = gen_.slot_mop(L.sJ);
    L.j_stack = jloc.kind != MOperandKind::Reg;
    if (!L.j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    L.JREG = L.j_stack ? kRsi : jloc.reg;
    // GPR 池：rdi/rdx/rcx +（rsi 若 j 提升）。rax 留作循环临时。基址键与
    // 跨步计数器都从这里取；放不下 → 保守跳过（不溢出到 r8-r15：那些可能
    // 是跨块 vreg 的 home）。
    if (!L.j_stack) L.gpr_pool.push_back(kRsi);
    if (L.bases.size() + L.kcoefs.size() > L.gpr_pool.size()) return false;
    for (AffBase& bb : L.bases) bb.reg = L.gpr_pool[L.gpr_used++];
    for (int64_t c : L.kcoefs) L.kreg[c] = L.gpr_pool[L.gpr_used++];
    // inv 的运行期来源必须能在入口被安全读取：不能落在我们要覆写的池寄存器
    // 或 rax 上（simd_scratch_safe 恰好排掉 rax/rcx/rdx/rsi/rdi + xmm0-7）。
    for (const AffBase& bb : L.bases) {
        if (bb.inv == kNoReg) continue;
        if (!AsmGen::simd_scratch_safe(bb.inv_src)) return false;
        if (bb.inv_src.kind == MOperandKind::Reg && bb.inv_src.reg == L.JREG) return false;
    }
    for (const AffEq& eq : L.inv_eq) {
        if (eq.x != kNoReg && !AsmGen::simd_scratch_safe(eq.xm)) return false;
        if (eq.y != kNoReg && !AsmGen::simd_scratch_safe(eq.ym)) return false;
    }

    // 各 reduction acc 位置：float 提升 xmm8-15 原地累加，否则 scratch+每轮
    // 写回；int 一律 2-lane xmm scratch + 出口水平求和写回槽。
    for (Red& r : L.reds) {
        r.loc = gen_.slot_mop(r.slot);
        if (r.is_float) {
            r.acc_stack = r.loc.kind != MOperandKind::Reg;
            if (!r.acc_stack) {
                if (r.loc.reg < kXmm8 || r.loc.reg > kXmm15) return false;
                r.XT = r.loc.reg;
            }
        } else {
            L.any_int_red = true;
            if (r.loc.kind == MOperandKind::Reg &&
                (r.loc.reg < kRax || r.loc.reg > kR15)) return false;
        }
    }

    // xmm 池：xmm0-7。先分配持久不变量广播 + acc，再给临时值线性扫描。
    // int 乘 pmuludq 重建 / Convert cvtdq2pd 折算的 2 个序列 scratch：序列在
    // 单条 Bin 发射内原子完成，跨指令不 live（两序列可共用），先占使不变量/
    // acc/临时值线性扫描不会占用。放不下保守跳过。
    // 归纳变量物化（P5b）也要 SEQ1 当第二条 lane 的临时落点。
    if (L.has_int_mul || L.has_convert || L.has_intdiv || L.has_j_operand) {
        L.SEQ1 = L.take(kMRegNone);
        if (L.SEQ1 == kMRegNone) return false;
        // 只有乘/Convert 要第二条（两序列在同一条 Bin 里成对使用）。
        if (L.has_int_mul || L.has_convert) {
            L.SEQ2 = L.take(kMRegNone);
            if (L.SEQ2 == kMRegNone) return false;
        }
    }
    // Convert 折算常量 2^32（double 广播，见发射处推导）。
    if (L.has_convert) {
        L.TWO32 = L.take(kMRegNone);
        if (L.TWO32 == kMRegNone) return false;
    }
    // coef>1 的 gather 打包 scratch（两次 movsd + unpcklpd 的第二源，跨指令
    // 不 live；先占避免被临时值线性扫描挤掉）。
    if (!L.kcoefs.empty()) {
        L.GSCR = L.take(kMRegNone);
        if (L.GSCR == kMRegNone) return false;
    }
    // math.sqrt：高 lane 打包临时 + 负数守卫的 0.0（循环前清零，跨指令不 live
    // 的部分只有单条 sqrt 内部的打包序列；放不下保守跳过）。
    if (L.has_sqrt) {
        // 高 lane 打包临时 + 0.0 比较量都只在**单条指令**的 sqrt 序列里原子使用，
        // 可以借用其它序列 scratch（SEQ1/SEQ2/GSCR 同样"单条指令内原子使用、
        // 跨指令不 live"）：这样 sqrt 不再占专用寄存器（nbody energy 内层原来
        // 正好差这 1~2 个 xmm）。借不到才从池里独占分配。
        L.SQRTMP = L.SEQ1 != kMRegNone ? L.SEQ1 : L.take(kMRegNone);
        if (L.SQRTMP == kMRegNone) return false;
        if (L.SEQ2 != kMRegNone && L.SEQ2 != L.SQRTMP) L.SQRZ = L.SEQ2;
        else if (L.GSCR != kMRegNone && L.GSCR != L.SQRTMP) L.SQRZ = L.GSCR;
        else if (L.SQRZ = L.take(kMRegNone), L.SQRZ == kMRegNone) return false;
    }
    // 去重不变量 float（Mov/临时槽别名解析到同一生产者）。
    // 同值常量归并（实现见 codegen_vect_jvec.cpp）：广播寄存器只占一个。
    vect_merge_const_invs(&L.cls);
    for (VReg v : L.inv_floats) {
        VReg r = L.resolve_src(v);
        bool dup = false;
        for (VReg u : L.uniq_inv) if (u == r) dup = true;
        if (!dup) L.uniq_inv.push_back(r);
    }
    for (VReg v : L.inv_ints) {
        VReg r = L.resolve_src(v);
        bool dup = false;
        for (VReg u : L.uniq_inv) if (u == r) dup = true;
        if (!dup) L.uniq_inv.push_back(r);
    }
    for (VReg v : L.uniq_inv) {
        if (!AsmGen::simd_scratch_safe(L.list_base_mop(v))) return false;  // 不变量来源不能落在 scratch
        MReg r = L.take(kMRegNone);
        if (r == kMRegNone) return false;
        L.inv_reg[v] = r;
    }
    for (Red& r : L.reds) {
        if (r.is_float) {
            if (r.acc_stack) { r.XT = L.take(kXmm7); if (r.XT == kMRegNone) return false; }
        } else {
            r.IACC = L.take(kXmm7); if (r.IACC == kMRegNone) return false;
        }
    }
    // int reduction 出口水平求和 scratch：先占保证不与任何 IACC 冲突
    //（多 int acc 时旧 xmm0/1 择一可能撞上另一个 IACC）。
    if (L.any_int_red) { L.HSCR = L.take(kMRegNone); if (L.HSCR == kMRegNone) return false; }

    // 临时值线性扫描：按体块顺序，VecF/VecI 生产者分配 xmm，操作数末次使用后释放。
    for (size_t i = 0; i < L.BI.size(); ++i) {
        const Inst& in = L.BI[i];
        VReg ops[3] = {in.a, in.b, in.c};
        for (VReg op : ops) if (op != kNoReg) L.last_use[L.resolve_src(op)] = static_cast<int>(i);
        // CallRt（sqrt）的实参也是使用点：不记录会让源寄存器提前还池。
        for (VReg op : in.args) if (op != kNoReg) L.last_use[L.resolve_src(op)] = static_cast<int>(i);
    }
    // reduction expr 的末次使用在 RedStore（add 已折叠，不算独立使用点）；
    // 多 reduction 共享同一 expr 时取最晚的 store 位置。
    for (const Red& r : L.reds) {
        VReg er = L.resolve_src(r.expr);
        auto lu = L.last_use.find(er);
        int pos = (lu == L.last_use.end()) ? r.store_pos : std::max(lu->second, r.store_pos);
        L.last_use[er] = pos;
    }
    // 归纳变量物化寄存器（P5b）：生命周期只由算术使用点决定（纯下标用的 j
    // 不需要它）。分配/释放见 vect_alloc_temps。
    gen_.vect_j_span(L.BI, L.role, L.cls, &L.j_first, &L.j_last);
    // float 保序提取会 movhlps 破坏 expr 寄存器：expr 末次使用晚于本 RedStore
    //（被另一 reduction/map 存储共享）时必须用 scratch 副本提取。先占 FSCR。
    for (const Red& r : L.reds) {
        if (!r.is_float) continue;
        auto lu = L.last_use.find(L.resolve_src(r.expr));
        if (lu != L.last_use.end() && lu->second > r.store_pos) {
            L.FSCR = L.take(kMRegNone);
            if (L.FSCR == kMRegNone) return false;
            break;
        }
    }
    // 临时值线性扫描（含归纳变量物化寄存器的分配/释放），见 vect_alloc_temps。
    {
        AsmGen::VTempPlan plan;
        plan.body = &L.BI; plan.role = &L.role; plan.last_use = &L.last_use; plan.cls = &L.cls;
        plan.pool = &L.xmm_pool; plan.tmp_reg = &L.tmp_reg;
        plan.jvec = L.JVEC; plan.j_first = L.j_first; plan.j_last = L.j_last;
        if (!gen_.vect_alloc_temps(&plan)) return false;
        L.JVEC = plan.jvec;
    }

    // ── 全部分析通过 → 发射向量前奏 ──────────────────────────────────────
    L.tail = gen_.fresh_label("vecttail");
    L.vec = gen_.fresh_label("vectvec");
    if (std::getenv("PYADAMAS_VECT_DBG"))
        std::fprintf(stderr, "[vect] vectorize loop body bb%d (sJ=%d reds=%zu lists=%zu inv=%zu)\n",
                     L.body_bi, L.sJ, L.reds.size(), L.lists.size(), L.uniq_inv.size());
    gen_.comment("VECT generic loop (PYADAMAS_VECT): SSE2 2-lane map/reduction");
    if (L.j_stack) gen_.emit2(MOp::kMovq, jloc, L.JREG);

    // 守卫：j+1<n、j>=0。
    gen_.emit2(MOp::kLeaq, AsmGen::op_mem(L.JREG, 1), kRax);
    gen_.emit2(MOp::kCmpq, L.n_op, kRax);
    gen_.emit_jmp(MOp::kJge, L.tail);
    gen_.emit2(MOp::kTestq, L.JREG, L.JREG);
    gen_.emit_jmp(MOp::kJs, L.tail);
    // 每 list：kind==0（守护 IndexError/类型升级语义：向量路径不做逐元素检查，
    // 所以元素类型/kind 必须在入口一次证完）。
    for (const std::string& lk : L.lists) {
        gen_.emit2(MOp::kMovq, L.list_base_mop(L.list_rep[lk]), kRax);
        gen_.emit_imm(MOp::kCmpq, 0, AsmGen::op_mem(kRax, 24));
        gen_.emit_jmp(MOp::kJne, L.tail);
    }
    // 每基址键：0 <= coef*(n-1)+inv+off < len（逐步 jo 溢出检查 → 跳标量）。
    for (const AffBase& bb : L.bases)
        if (!vect_affine_guard(L.list_base_mop(L.list_rep[bb.key]), bb, L.n_op, L.tail)) return false;
    // 同对象读写的 inv 判等守卫（循环版本化：证不出相等就走标量）。
    for (const AffEq& eq : L.inv_eq) vect_affine_inv_eq(eq, L.tail);
    // 基址常驻寄存器：base = data + inv*8。
    for (const AffBase& bb : L.bases)
        vect_affine_base(L.list_base_mop(L.list_rep[bb.key]), bb, bb.reg);
    // 别名守卫：不同 list 对象两两判等。
    for (size_t i = 0; i < L.lists.size(); ++i)
        for (size_t j = i + 1; j < L.lists.size(); ++j) {
            gen_.emit2(MOp::kMovq, L.list_base_mop(L.list_rep[L.lists[i]]), kRax);
            gen_.emit2(MOp::kCmpq, L.list_base_mop(L.list_rep[L.lists[j]]), kRax);
            // 相等 → 不同 vreg 可能持同一对象，无法静态证明偏移安全 → 保守标量。
            gen_.emit_jmp(MOp::kJe, L.tail);
        }
    // setup：不变量广播（float 用 movsd/位模式，int 用 movq）。
    for (VReg v : L.uniq_inv) {
        MReg rr = L.inv_reg[v];
        const Inst* d = gen_.simd_def(v);
        if (d && d->op == IrOp::ConstFloat) {
            gen_.emit_imm_hex(MOp::kMovq, f64_bits(d->fimm), kRax);
            gen_.emit2(MOp::kMovq, kRax, rr);
        } else if (d && d->op == IrOp::ConstInt) {
            gen_.emit_imm(MOp::kMovq, d->imm, kRax);
            gen_.emit2(MOp::kMovq, kRax, rr);
        } else if (ir_is_float(gen_.ty_of(v))) {
            gen_.emit2(MOp::kMovsd, L.list_base_mop(v), rr);
        } else {
            gen_.emit2(MOp::kMovq, L.list_base_mop(v), rr);
        }
        gen_.emit2(MOp::kMovlhps, rr, rr);
    }
    if (L.has_convert) {
        gen_.emit_imm_hex(MOp::kMovq, f64_bits(4294967296.0), kRax);  // 2^32
        gen_.emit2(MOp::kMovq, kRax, L.TWO32);
        gen_.emit2(MOp::kMovlhps, L.TWO32, L.TWO32);
    }
    // sqrt 的 0.0 比较量在每次 sqrt 序列里现清（借用其它 scratch，不能假设
    // 循环期间保持不变），见发射处的 CallRt 分支。
    for (const Red& r : L.reds) {
        if (r.is_float) {
            if (r.acc_stack) gen_.emit2(MOp::kMovsd, r.loc, r.XT);
        } else {
            // int acc 必须装载槽的**当前值**进低 lane（高 lane 自动清 0）：
            // 循环前累加器可能非 0（t=49; for...: t+=n）。pxor 清 0 + 出口覆盖写
            // 会丢前值（静默算错）。出口水平求和 = 前值 + 向量和（模 2^64 结合律
            // 精确），写回槽后标量尾续加。
            gen_.emit2(MOp::kMovq, r.loc, r.IACC);
        }
    }
    // 跨步计数器初始化：K = coef*8*j（j 已从栈提到 JREG）。
    for (int64_t c : L.kcoefs) vect_affine_kinit(L.kreg[c], L.JREG, c);

    // ── AVX2 4-lane（整数路径，运行时派发）──────────────────────────────
    // 体块里只要出现浮点值（VecF/InvF）或浮点累加器，就整段留给 SSE2 2-lane：
    // 浮点 reduction 必须保序（逐位一致），4-lane 拿不到收益（文档 §P3）。
    // 判据只有 IR 结构（分类结果），没有任何基准名/基准字面量。
    for (auto& [v, cc] : L.cls)
        if (cc.kind == vect::VK::VecF || cc.kind == vect::VK::InvF) L.avx_int = false;
    for (const Red& r : L.reds) if (r.is_float) L.avx_int = false;
    // coef==0 的不变量下标元素装载只在 SSE2 2-lane 里做了广播；4-lane 发射器
    // 没有这条路径 → 含这类访问的循环整段留给 SSE2。
    for (const Access& a : L.accesses) if (a.coef == 0) L.avx_int = false;
    L.avx.body = &L.BI; L.avx.role = &L.role; L.avx.acc = &L.accesses; L.avx.acc_at = &L.acc_at;
    L.avx.bases = &L.bases; L.avx.reds = &L.reds; L.avx.xmm_of = &L.xmm_of_fn;
    L.avx.uniq_inv = &L.uniq_inv; L.avx.inv_reg = &L.inv_reg;
    L.avx.kcoefs = &L.kcoefs; L.avx.kreg = &L.kreg;
    L.avx.jreg = L.JREG; L.avx.j_stack = L.j_stack; L.avx.jloc = jloc; L.avx.n_op = L.n_op;
    L.avx.gscr = L.GSCR; L.avx.seq1 = L.SEQ1; L.avx.seq2 = L.SEQ2; L.avx.hscr = L.HSCR;
    L.avx.jvec = L.JVEC; L.avx.has_j = (L.JVEC != kMRegNone);
    L.avx.vec = &L.vec; L.avx.int_ok = L.avx_int;
    L.avx2 = gen_.vect_avx2_emit(L.avx, &L.vexit);

    // pshufd 三操作数形式（本 GAS 不接受两操作数写法，同 codegen_simd.cpp）。
    // 向量循环体。
    gen_.label(L.vec);
    for (size_t i = 0; i < L.BI.size(); ++i) {
        const Inst& in = L.BI[i];
        switch (L.role[i]) {
            case Role::Load: {
                VReg r = L.resolve_src(in.dst);
                MReg rd = L.tmp_reg[r];
                const Access& a = L.accesses[static_cast<size_t>(L.acc_at[i])];
                // coef==0（循环内不变下标）不需要跨步计数器，发射侧走广播装载。
                MReg kb = (a.coef == 1 || a.coef == 0) ? kMRegNone : L.kreg[a.coef];
                vect_affine_load(L.bases[static_cast<size_t>(a.bidx)].reg, L.JREG, kb,
                                 a.coef, a.off, rd, L.GSCR);
                break;
            }
            case Role::Bin: {
                VReg r = L.resolve_src(in.dst);
                MReg rd = L.tmp_reg[r];
                // P5b：归纳变量当整数算术操作数 —— 本轮首个使用点物化一次
                // [j, j+1]（序列与 Convert 的 j_src 分支同一份）。
                if (L.JVEC != kMRegNone && static_cast<int>(i) == L.j_first)
                    vect_jvec2(L.JREG, 0, L.JVEC, L.SEQ1);
                if (in.op == IrOp::Convert) {
                    // i64→f64 折算（与标量 cvtsi2sdq 逐位一致，任意 i64）：
                    //   x = hi_s*2^32 + lo_u（hi_s 算术高32 ∈ [-2^31,2^31)，
                    //   lo_u 无符号低32 ∈ [0,2^32)）。hi_f=cvtdq2pd(hi_s) 精确
                    //   （|hi_s|<2^53），×2^32 是 2 的幂缩放仍精确；lo 修正：
                    //   psrad(lo,31) 符号掩码 → cvtdq2pd = -1.0/0.0 → ×2^32 =
                    //   -2^32/+0.0 → lo_s_f 减之 = lo_u（精确，<2^53；lo=0 时
                    //   +0-+0=+0）。末条 addpd 两精确操作数之和恰为整数 x →
                    //   单次 RN 舍入 = cvtsi2sdq(x)（任何 MXCSR 舍入模式同理，
                    //   中间量全精确）。INT64_MIN：hi_s=-2^31, lo_u=0 → -2^63 精确。
                    const VClass& cc = L.cls[r];
                    if (cc.j_src) {
                        // 构造 [j+off, j+off+1]（j 任意 i64，折算无需范围守卫）。
                        vect_jvec2(L.JREG, cc.off, rd, L.SEQ1);
                    } else {
                        MReg ra = L.xmm_of(in.a);
                        if (ra == kMRegNone) return false;  // 不应发生（dry-run 完备）
                        if (rd != ra) gen_.emit2(MOp::kMovapd, ra, rd);
                    }
                    // 注意 cvtdq2pd 源是 m64：转 dword0/dword1（低 64 位内
                    // 两个 i32），不是每 lane 低 32。2-lane i64 向量 dword 布局
                    // [lo0,hi0,lo1,hi1] → pshufd 0xDD 聚 [hi0,hi1]、0x88 聚
                    // [lo0,lo1] 到低 64 位再转。
                    gen_.emit2(MOp::kMovapd, rd, L.SEQ1);
                    L.pshufd(0xDD, L.SEQ1);                    // [hi0,hi1,hi0,hi1]
                    gen_.emit2(MOp::kCvtdq2pd, L.SEQ1, L.SEQ1);     // hi_s 按 signed i32，精确
                    gen_.emit2(MOp::kMulpd, L.TWO32, L.SEQ1);       // hi_f*2^32 精确
                    gen_.emit2(MOp::kMovapd, rd, L.SEQ2);
                    gen_.emit_imm(MOp::kPsrad, 31, L.SEQ2);       // 逐 dword 符号掩码
                    L.pshufd(0x88, L.SEQ2);                    // [s(lo0),s(lo1),...]
                    gen_.emit2(MOp::kCvtdq2pd, L.SEQ2, L.SEQ2);     // -1.0 / 0.0
                    gen_.emit2(MOp::kMulpd, L.TWO32, L.SEQ2);       // -2^32 / +0.0
                    L.pshufd(0x88, rd);                      // [lo0,lo1,lo0,lo1]
                    gen_.emit2(MOp::kCvtdq2pd, rd, rd);         // lo_s_f（signed i32）
                    gen_.emit2(MOp::kSubpd, L.SEQ2, rd);          // lo_u_f 精确
                    gen_.emit2(MOp::kAddpd, L.SEQ1, rd);          // 单次舍入 = cvtsi2sdq
                    break;
                }
                if (in.op == IrOp::CallRt) {
                    // math.sqrt 的 2-lane 序列（与标量 sqrtsd fast path 逐位一致）：
                    // 低/高 lane 分别 sqrtsd，movlhps 拼回。任一 lane 为负（或
                    // NaN → 走标量只是慢一点，结果同 NaN）跳标量尾；x<0 的
                    // ValueError 由标量路径按原时机抛出。
                    MReg ra = L.xmm_of(in.args[0]);
                    if (ra == kMRegNone) return false;   // 不应发生（dry-run 完备）
                    if (rd != ra) gen_.emit2(MOp::kMovapd, ra, rd);
                    gen_.emit2(MOp::kXorpd, L.SQRZ, L.SQRZ);       // 0.0（借用 scratch，现清）
                    gen_.emit2(MOp::kComisd, L.SQRZ, rd);          // rd ? 0
                    gen_.emit_jmp(MOp::kJb, L.tail);               // rd<0 或 NaN → 标量尾
                    gen_.emit2(MOp::kMovapd, rd, L.SQRTMP);
                    gen_.emit2(MOp::kMovhlps, L.SQRTMP, L.SQRTMP); // 高 lane 复制到低
                    gen_.emit2(MOp::kComisd, L.SQRZ, L.SQRTMP);
                    gen_.emit_jmp(MOp::kJb, L.tail);
                    gen_.emit2(MOp::kSqrtsd, rd, rd);              // 低 lane sqrt
                    gen_.emit2(MOp::kSqrtsd, L.SQRTMP, L.SQRTMP);  // 高 lane sqrt
                    gen_.emit2(MOp::kMovlhps, L.SQRTMP, rd);       // rd.hi = sqrt(hi)
                    break;
                }
                // int // 与 %（除数为 2 的幂常量）：只读被除数，除数只用
                // 编译期移位量（不占寄存器、不参与广播）。
                int dsh = -1;
                const bool intdiv2 =
                    (in.bop == BinKind::FloorDiv || in.bop == BinKind::Mod) &&
                    vect_div_pow2(in.b, &dsh);
                MReg ra = L.xmm_of(in.a);
                MReg rb = intdiv2 ? kMRegNone : L.xmm_of(in.b);
                if (ra == kMRegNone || (!intdiv2 && rb == kMRegNone)) return false;  // 不应发生
                if (intdiv2 && in.bop == BinKind::FloorDiv && dsh > 0 &&
                    L.SEQ1 == kMRegNone) return false;                     // 不应发生
                if (rd != ra) gen_.emit2(MOp::kMovapd, ra, rd);
                if (intdiv2) {
                    vect_intdiv2(rd, L.SEQ1, in.bop == BinKind::Mod, dsh);
                    break;
                }
                const bool is_int = gen_.ty_of(in.dst) && gen_.ty_of(in.dst)->kind == TyKind::Int;
                MOp op = MOp::kAddpd;
                if (is_int) {
                    if (in.bop == BinKind::Mul) {
                        // 全 i64 乘重建（rd=A 已装好，rb=B 保持不动）：
                        //   a*b mod 2^64 = a_lo*b_lo + ((a_lo*b_hi + a_hi*b_lo) mod 2^32)<<32
                        // a_lo/a_hi 是 64 位 lane 的低/高 32 位（无符号拆半，对负数
                        // 补码同样成立）。pmuludq 逐 lane 取低 32 位无符号乘 → 精确
                        // 64 位积（<2^64）；paddq/psllq 的模 2^64/丢高位恰好实现上式
                        // 的 mod 2^32 与 <<32 → 与标量 imulq 低 64 位逐位一致，
                        // 任意 i64（含负数/INT64_MIN）无需值域守卫。
                        gen_.emit2(MOp::kMovapd, rb, L.SEQ1);
                        gen_.emit_imm(MOp::kPsrlq, 32, L.SEQ1);      // SEQ1 = b_hi
                        gen_.emit2(MOp::kPmuludq, rd, L.SEQ1);       // SEQ1 = a_lo*b_hi
                        gen_.emit2(MOp::kMovapd, rd, L.SEQ2);
                        gen_.emit_imm(MOp::kPsrlq, 32, L.SEQ2);      // SEQ2 = a_hi
                        gen_.emit2(MOp::kPmuludq, rb, L.SEQ2);       // SEQ2 = b_lo*a_hi
                        gen_.emit2(MOp::kPaddq, L.SEQ2, L.SEQ1);       // 交叉项和（mod 2^64）
                        gen_.emit_imm(MOp::kPsllq, 32, L.SEQ1);      // 只留低 32 并 <<32
                        gen_.emit2(MOp::kPmuludq, rb, rd);         // rd = a_lo*b_lo（精确）
                        gen_.emit2(MOp::kPaddq, L.SEQ1, rd);         // rd = a*b mod 2^64
                        break;
                    }
                    switch (in.bop) {
                        case BinKind::Add: op = MOp::kPaddq; break;
                        case BinKind::Sub: op = MOp::kPsubq; break;
                        default: return false;
                    }
                } else {
                    switch (in.bop) {
                        case BinKind::Add: op = MOp::kAddpd; break;
                        case BinKind::Sub: op = MOp::kSubpd; break;
                        case BinKind::Mul: op = MOp::kMulpd; break;
                        case BinKind::Div: op = MOp::kDivpd; break;
                        default: return false;
                    }
                }
                gen_.emit2(op, rb, rd);
                break;
            }
            case Role::MapStore: {
                const Access& a = L.accesses[static_cast<size_t>(L.acc_at[i])];
                MReg rc = L.xmm_of(in.c);
                if (rc == kMRegNone) return false;
                vect_affine_store(L.bases[static_cast<size_t>(a.bidx)].reg, L.JREG, a.off, rc);
                break;
            }
            case Role::RedStore: {
                const Red* rp = L.red_of_slot(in.idx);
                if (!rp) return false;                    // 不应发生
                MReg re = L.xmm_of(rp->expr);
                if (re == kMRegNone) return false;
                if (rp->is_float) {
                    // 保序提取累加：acc += e0; acc += e1（或 acc -= e0; acc -= e1，
                    // 减号归约按同一 lane 顺序 subsd → 与标量逐位一致）。
                    // expr 被共享（末次使用晚于本 store）→ scratch 副本提取，
                    // 不破坏后续使用者。
                    MReg ex = re;
                    auto lu = L.last_use.find(L.resolve_src(rp->expr));
                    if (lu != L.last_use.end() && lu->second > rp->store_pos) {
                        if (L.FSCR == kMRegNone) return false;  // 不应发生
                        gen_.emit2(MOp::kMovapd, re, L.FSCR);
                        ex = L.FSCR;
                    }
                    const MOp fop = rp->is_sub ? MOp::kSubsd : MOp::kAddsd;
                    gen_.emit2(fop, ex, rp->XT);
                    gen_.emit2(MOp::kMovhlps, ex, ex);
                    gen_.emit2(fop, ex, rp->XT);
                    if (rp->acc_stack) gen_.emit2(MOp::kMovsd, rp->XT, rp->loc);
                } else {
                    // int：paddq 2-lane 累加（模 2^64 精确），出口水平求和。
                    gen_.emit2(MOp::kPaddq, re, rp->IACC);
                }
                break;
            }
            default: break;
        }
    }
    // 跨步计数器推进：一条向量迭代消费 2 个 lane，各跨 coef 个元素。
    for (int64_t c : L.kcoefs) vect_affine_step(L.kreg[c], c);
    // j += 2（+ 写回栈）。
    gen_.emit_imm(MOp::kAddq, 2, L.JREG);
    if (L.j_stack) gen_.emit2(MOp::kMovq, L.JREG, jloc);
    // 循环条件：j+1<n。
    gen_.emit2(MOp::kLeaq, AsmGen::op_mem(L.JREG, 1), kRax);
    gen_.emit2(MOp::kCmpq, L.n_op, kRax);
    gen_.emit_jmp(MOp::kJl, L.vec);
    if (L.avx2) gen_.label(L.vexit);            // AVX2 段与 SSE2 段在此汇合
    // 出口：各 int reduction 水平求和写回槽（标量尾/cond/end 读最新）。
    // HSCR 先占且不与任何 IACC 冲突；逐个求和写回，相互独立。
    for (const Red& r : L.reds) {
        if (r.is_float) continue;
        gen_.emit2(MOp::kMovhlps, r.IACC, L.HSCR);
        gen_.emit2(MOp::kPaddq, L.HSCR, r.IACC);
        gen_.emit2(MOp::kMovq, r.IACC, kRax);
        gen_.emit2(MOp::kMovq, kRax, r.loc);
    }
    // jmp cond：j==n 精确退出；j==n-1 经 cond 回体块走标量尾（入口守卫失败）。
    gen_.emit_jmp(MOp::kJmp, gen_.block_label(L.cond_bi));
    gen_.label(L.tail);                                          // 其后 gen_block 照常发标量体
    return true;
}

}  // namespace pyadamas
