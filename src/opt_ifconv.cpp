// opt_ifconv.cpp —— 简单菱形 if-conversion（scoped，0.419）
//
// **问题**：向量化器的循环体要求「单块、直线、Jump 结尾」（`vect_planner_loop.cpp:53`）。
// 循环体里一个 `if` 就把体块拆成 cond/then/merge 多块，整段循环被拒。实测 9 个
// 0 触发基准里 sieve/nqueens/collatz/fasta/wordfreq 的第一道门都是它。
//
// **本 pass 只做一种最简单的菱形**（then 块 = 累加器自增/自减一个常量，else 为空）：
//
//     C: ... branch cond, T, M          C: ... %c = cond(0/1)
//     T: %l = load acc                  %l = load acc
//        %v = %l ± k        ───────►     %v = %l ± %c      （k = ±1）
//        store acc, %v                   store acc, %v
//        jump M                          jump M
//     （T 变空块，C 直达 M）
//
// **为什么安全**（每条都对应一个拒绝条件）：
//  1. then 块内只允许「不抛异常、无副作用、不通过可能非法的指针读内存」的指令：
//     ConstInt/ConstFloat/Mov/LoadSlot/LoadGlobal/Cmp/Un/整数 Add/Sub/Mul/位运算。
//     调用、IndexLoad（可能 IndexError）、FieldLoad/PtrLoad/Len（可能非法指针）、
//     除法/取模（可能 ZeroDivisionError）、任何 store（除唯一的 acc 写）一律拒绝。
//  2. then 块的唯一前驱必须是 cond 块（`single_pred == C`），否则可能有别的路径
//     跳过条件直接进 then。
//  3. acc 的读和写必须是同一个槽/全局，且 then 块里只有这一个写；常量 k 必须是
//     ±1（其它 k 需要乘法，指令数不划算，暂不做）。
//  4. 条件值必须是 bool 类型的 vreg（机器上是 0/1）—— `acc ± cond` 与
//     `cond ? acc±1 : acc` 在 acc 为整数时逐位一致（cond=0 时 acc+0 == acc）。
//  5. 函数带异常 handler / 是生成器时整段跳过（CFG 与 landing pad 复杂）。
//
// 收益：sieve 的计数循环 `if flags[i] == 1: count += 1` 的体块变单块（是否可向量化
// 还取决于其它判据，见 docs/vectorization-coverage.md）。
#include "opt_detail.h"

#include <unordered_map>
#include <vector>

namespace pyadamas {
namespace optdetail {
namespace {

// then 块里允许出现的「确定性安全」指令：不抛异常、无副作用、不读可能非法的指针。
bool ifconv_safe_inst(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::Mov:
        case IrOp::LoadSlot:
        case IrOp::LoadGlobal:
        case IrOp::Cmp:
            return true;
        case IrOp::Un:
            return true;  // 取负只回绕，不抛
        case IrOp::Bin:
            // 只收整数、无除法/取模（bin_is_safe_int 已排除 Div/Mod/FloorDiv）。
            return in.ty && !ir_is_float(in.ty) && bin_is_safe_int(in.bop);
        case IrOp::Convert:
            // 只收整数/布尔目标（float→int 截断也不抛，但保守只收 int/bool）。
            return in.ty && (in.ty->kind == TyKind::Int || in.ty->kind == TyKind::Bool);
        default:
            return false;
    }
}

// 函数内所有 ConstInt：dst -> imm。常量可能被 LICM 外提到循环前置块。
std::unordered_map<VReg, int64_t> collect_int_consts(const IRFunction& fn) {
    std::unordered_map<VReg, int64_t> m;
    for (const Block& b : fn.blocks)
        for (const Inst& in : b.insts)
            if (in.op == IrOp::ConstInt && in.dst != kNoReg) m[in.dst] = in.imm;
    return m;
}

// vreg 是否在块内某条指令里被定义；返回下标或 -1。
int def_index_in_block(const Block& b, VReg v) {
    for (int i = 0; i < static_cast<int>(b.insts.size()); ++i)
        if (b.insts[static_cast<size_t>(i)].dst == v) return i;
    return -1;
}

// 统计「所有终点来自哪些块」的入边数。
int pred_count(const IRFunction& fn, int bi) {
    int n = 0;
    for (const Block& b : fn.blocks) {
        if (b.insts.empty()) continue;
        const Inst& t = b.insts.back();
        if (t.op == IrOp::Jump) {
            if (t.target == bi) ++n;
        } else if (t.op == IrOp::Branch) {
            if (t.target == bi) ++n;
            if (t.target2 == bi) ++n;
        }
    }
    return n;
}

VReg alloc_vreg(IRFunction& fn, TypePtr ty) {
    VReg v = static_cast<VReg>(fn.vreg_ty.size());
    fn.vreg_ty.push_back(std::move(ty));
    if (fn.vreg_nonneg.size() < fn.vreg_ty.size()) fn.vreg_nonneg.push_back(0);
    return v;
}

}  // namespace

bool if_convert_accumulators(IRFunction& fn) {
    // 带异常 handler / 生成器 / 任何 EH 标记：CFG 与 landing pad 复杂，整段不做。
    if (fn.has_handlers || fn.is_generator) return false;
    for (const Block& b : fn.blocks)
        for (const Inst& in : b.insts)
            if (in.op == IrOp::EhBegin || in.op == IrOp::EhEnd ||
                in.op == IrOp::EhLandingPad)
                return false;
    const auto consts = collect_int_consts(fn);
    bool changed = false;
    const int nb = static_cast<int>(fn.blocks.size());
    for (int ci = 0; ci < nb; ++ci) {
        Block& C = fn.blocks[static_cast<size_t>(ci)];
        if (C.insts.empty()) continue;
        const Inst term = C.insts.back();
        if (term.op != IrOp::Branch) continue;
        const int tb = term.target;
        const int mb = term.target2;
        if (tb < 0 || mb < 0 || tb >= nb || mb >= nb) continue;
        if (tb == mb || tb == ci || mb == ci) continue;
        Block& T = fn.blocks[static_cast<size_t>(tb)];
        // then 块：非空、恰好以「jump 到 merge」结尾。
        if (T.insts.size() < 2) continue;
        if (T.insts.back().op != IrOp::Jump || T.insts.back().target != mb) continue;
        // then 块的唯一前驱必须是 cond 块。
        if (pred_count(fn, tb) != 1) continue;
        // 条件必须是 bool vreg（0/1 语义）。
        const VReg cond = term.a;
        if (cond == kNoReg || cond >= fn.vreg_ty.size()) continue;
        const TypePtr cond_ty = fn.vreg_ty[static_cast<size_t>(cond)];
        if (!cond_ty || cond_ty->kind != TyKind::Bool) continue;
        // 扫描 then 块：全部安全纯指令，且恰有一个 store（必须是最后一条非终止）。
        int store_i = -1;
        bool ok = true;
        for (int k = 0; k + 1 < static_cast<int>(T.insts.size()); ++k) {
            const Inst& in = T.insts[static_cast<size_t>(k)];
            const bool is_store = in.op == IrOp::StoreSlot || in.op == IrOp::StoreGlobal;
            if (is_store) {
                if (store_i >= 0) { ok = false; break; }
                store_i = k;
                continue;
            }
            if (!ifconv_safe_inst(in)) { ok = false; break; }
        }
        if (!ok || store_i < 0) continue;
        // 唯一 store 必须在块尾（终止符前一条），否则后面还有指令要执行。
        if (store_i != static_cast<int>(T.insts.size()) - 2) continue;
        const Inst& st = T.insts[static_cast<size_t>(store_i)];
        const bool st_global = st.op == IrOp::StoreGlobal;
        // 最小可用版只处理**槽**累加器：分支化后 load+add+store 变成每轮无条件
        // 执行，而全局访问是内存操作（实测 sieve 计数循环 +2.2% 指令数）——
        // 槽累加器才有机会被 RA 提升/后续向量化吃掉。全局累加器等 0.419 之后
        // 的「全局变量寄存器化」能覆盖到循环（promote 目前只认 body→exit 边）
        // 再放开。
        if (st_global) continue;
        // store 的值必须由 then 块里的 Bin 产生：acc_load ± 常量。
        const int vd_i = def_index_in_block(T, st.a);
        if (vd_i < 0) continue;
        const Inst& vd = T.insts[static_cast<size_t>(vd_i)];
        if (vd.op != IrOp::Bin) continue;
        if (vd.bop != BinKind::Add && vd.bop != BinKind::Sub) continue;
        if (!vd.ty || ir_is_float(vd.ty)) continue;
        // 找出哪一侧是「读 acc 的 load」、哪一侧是常量 k。
        VReg load_v = kNoReg;
        int64_t k = 0;
        bool have_k = false;
        for (int side = 0; side < 2; ++side) {
            const VReg op = side == 0 ? vd.a : vd.b;
            if (op == kNoReg) continue;
            const int oi = def_index_in_block(T, op);
            if (oi >= 0) {
                const Inst& od = T.insts[static_cast<size_t>(oi)];
                if (od.op == IrOp::LoadSlot || od.op == IrOp::LoadGlobal) {
                    if (load_v != kNoReg) { ok = false; break; }
                    // 读的必须是同一个 acc 位置。
                    const bool same = (od.op == IrOp::LoadGlobal) == st_global && od.idx == st.idx;
                    if (!same) { ok = false; break; }
                    const TypePtr lt = od.ty;
                    if (!lt || ir_is_float(lt)) { ok = false; break; }
                    load_v = op;
                    continue;
                }
            }
            auto cit = consts.find(op);
            if (cit == consts.end()) { ok = false; break; }
            if (have_k) { ok = false; break; }
            k = cit->second;
            have_k = true;
        }
        if (!ok || load_v == kNoReg || !have_k) continue;
        // 最小可用版只处理 ±1（其它常量要乘法，指令数不划算）。
        if (k != 1 && k != -1) continue;
        // 减法不可交换：必须是「load ± 常量」而不是「常量 − load」。
        if (vd.bop == BinKind::Sub && load_v != vd.a) continue;
        // 变换：C 去掉 Branch，接上「load acc + (acc ± cond) + store acc + jump merge」。
        const Inst load_acc = T.insts[static_cast<size_t>(def_index_in_block(T, load_v))];
        Inst cl;
        cl.op = IrOp::Bin;
        // acc = acc + k*cond：k=±1，符号由原 Bin 的 Add/Sub 与 k 的符号共同决定。
        const bool plus = (vd.bop == BinKind::Add) == (k == 1);
        cl.bop = plus ? BinKind::Add : BinKind::Sub;
        cl.a = load_acc.dst;
        cl.b = cond;
        cl.dst = alloc_vreg(fn, t_int());
        cl.ty = t_int();
        Inst sj = st;  // 复用原 store（位置不变，只是不再条件执行）
        sj.a = cl.dst;
        Inst jmp;
        jmp.op = IrOp::Jump;
        jmp.target = mb;
        C.insts.pop_back();          // 去掉 branch
        C.insts.push_back(load_acc);
        C.insts.push_back(cl);
        C.insts.push_back(sj);
        C.insts.push_back(jmp);
        T.insts.clear();             // then 块变空（不可达；verifier 允许空块）
        changed = true;
    }
    // 尾块合并：C 以 jump M 结尾且 M 只有一个前驱 C 时，把 M 的指令接进 C。
    // 菱形压平后 merge 块（循环体剩余部分，如 sieve 的 i += 1）通常只剩一个
    // 前驱，合并后循环体才真正是单块（向量化器的硬性要求）。
    bool merged = true;
    while (merged) {
        merged = false;
        for (int ci = 0; ci < nb; ++ci) {
            Block& C = fn.blocks[static_cast<size_t>(ci)];
            if (C.insts.empty()) continue;
            const Inst& t = C.insts.back();
            if (t.op != IrOp::Jump) continue;
            const int mb = t.target;
            if (mb < 0 || mb >= nb || mb == ci) continue;
            // 只并**前向**边：回边目标（循环头）绝不能被并进体块，否则会把
            // while 的入口判断并掉（变成 do-while，N=0 时会多执行一轮）。
            if (mb < ci) continue;
            Block& M = fn.blocks[static_cast<size_t>(mb)];
            if (M.insts.empty()) continue;
            if (pred_count(fn, mb) != 1) continue;
            C.insts.pop_back();
            for (const Inst& in : M.insts) C.insts.push_back(in);
            M.insts.clear();
            merged = true;
            changed = true;
        }
    }
    if (changed) fn.rebuild_use_def();
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
