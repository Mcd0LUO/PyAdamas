// codegen_vect_jvec.cpp —— 通用向量化：归纳变量 j 作为整数算术的操作数（P5b）
//
// 政策：这是**通用操作支持**，判据只有一条结构性质 —— 「这个操作数是归纳
// 变量（j-alias 槽的装载，含其副本槽）」。没有基准名、基准字面量或表达式
// 形状的特判。LLVM LoopVectorize / GCC tree-vect 对「归纳变量参与循环体内的
// 逐元素算术」做的是同一件事：把标量 j 广播成 [j, j+1, …] 的向量。
//
// 语义：j 在整型 Bin（Add/Sub/Mul/除以 2 的幂常量）里当 VecI 用 —— 2-lane
// 路径物化 [j, j+1]，4-lane 路径物化 [j, j+1, j+2, j+3]。j 本身是任意 i64
//（含负数），逐 lane 的模 2^64 加/减/乘与标量 addq/subq/imulq 逐位一致，
// 所以不需要任何值域守卫。
//
// 物化序列**复用** Convert 的 j_src 分支（本文件把它抽出来共用，不另发明）：
//
//     leaq off(j),  %rax ; movq %rax, dst   ; dst.lo = j+off
//     leaq off+1(j),%rax ; movq %rax, scr   ; scr.lo = j+off+1
//     movlhps scr, dst                      ; dst.hi = j+off+1
//
// 4-lane 版本（AVX2，见 codegen_vect_avx2.cpp）把 [j, j+1] 放在低 128 位，
// 再用同一个 GPR 序列 + vpinsrq 拼出 [j+2, j+3] 放到高 128 位。
//
// 寄存器：物化结果占 1 个 xmm（JVEC）+ 1 个序列 scratch（SEQ1，与 int 乘 /
// Convert / intdiv 的序列共用，单条 Bin 内原子完成、跨指令不 live）。JVEC
// 在**首个算术使用点**分配、末次使用后释放 —— j 在一轮迭代内不变，所以只需
// 物化一次；释放点让后面的临时值能复用这个寄存器（寄存器紧张的循环靠它
// 才装得下）。放不下 → 分配器保守放弃整个循环（一个字节不发）。
//
// 本文件同时放两件为它服务的分析（都在 codegen_vect.cpp 的 dry-run 里调用，
// 原来写在 1200 行硬限制的主文件里；搬到这里只是文件规模，判据不变）：
//
//   * vect_mark_nonindex_uses：使用点分类。这是 P5b 的正确性命门 —— 原来
//     「含 j 的仿射式」一律折叠进寻址、不发射；而 j 能参与算术后，同一个值
//     可能**既当下标又当算术语**（i+j 既可以是 a[i+j] 的下标，也可以是
//     (i+j)*(i+j+1) 的操作数）。折叠后发射期查不到寄存器 → 半截 asm。所以
//     折叠的前提改成「全部使用点都是下标操作数」。
//   * vect_merge_const_invs / vect_j_span / vect_alloc_temps：寄存器分配。
//     常量按值归并（同一个 1 只占一个广播寄存器）、归纳变量按算术使用区间
//     占一个寄存器、临时值线性扫描允许目标接管左操作数的寄存器。
#include "codegen_impl.h"

namespace pyadamas {

namespace {

// double 的位模式（与 codegen_vect.cpp 的同名 helper 同口径）。
uint64_t jvec_f64_bits(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    return u;
}

}  // namespace

// 2-lane（SSE2）：dst = [j+off, j+off+1]。scr 是 scratch（SEQ1），不得与 dst
// 相同。与 Convert 的 j_src 分支逐字节同一序列（那里 rd = 目标，SEQ1 =
// scratch，off = 仿射偏移）。
void AsmGen::vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr) {
    emit2(MOp::kLeaq, op_mem(jreg, off), kRax);
    emit2(MOp::kMovq, kRax, dst);
    emit2(MOp::kLeaq, op_mem(jreg, off + 1), kRax);
    emit2(MOp::kMovq, kRax, scr);
    emit2(MOp::kMovlhps, scr, dst);              // dst.hi = scr.lo = j+off+1
}

// 使用点分类：把「不是下标操作数」的用途记进 out（值本身无所谓，只看有没
// 有）。IndexLoad/IndexStore 的 in.b 是下标，其余操作数（含 list 基址、存储
// 值、Bin 操作数、Convert 源、Mov 源、槽写值）都算非下标用途。
// 仿射式折叠进寻址的前提 = 它**只**当下标用；否则必须照常发射（P5b）。
void AsmGen::vect_mark_nonindex_uses(std::unordered_map<VReg, char>* out) const {
    if (out == nullptr) return;
    for (const Block& blk : f_->blocks) {
        for (const Inst& in : blk.insts) {
            const bool index_op = in.op == IrOp::IndexLoad ||
                                  in.op == IrOp::IndexLoadUnchecked ||
                                  in.op == IrOp::IndexStore ||
                                  in.op == IrOp::IndexStoreUnchecked;
            const VReg ops[3] = {in.a, in.b, in.c};
            for (int k = 0; k < 3; ++k) {
                if (ops[k] == kNoReg) continue;
                if (index_op && k == 1) continue;     // 下标操作数：可折叠
                (*out)[ops[k]] = 1;
            }
        }
    }
}

// 同值常量归并：常量常被 LICM 外提成多个独立 vreg —— 同一个 1 出现两次就占
// 两个广播寄存器。按值归并到首个 vreg（ConstInt 同 imm、ConstFloat 同位模式，
// 且结果类型相同），语义完全等价；给 j 算术腾出寄存器。顺序按 vreg 号，生成
// 代码确定。
void AsmGen::vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls) {
    if (cls == nullptr) return;
    std::vector<VReg> vs;
    for (auto& [v, c] : *cls)
        if ((c.kind == VK::InvI || c.kind == VK::InvF) && c.mov_src == kNoReg)
            vs.push_back(v);
    std::sort(vs.begin(), vs.end());
    std::unordered_map<int64_t, VReg> ci;
    std::unordered_map<uint64_t, VReg> cf;
    for (VReg v : vs) {
        const Inst* d = simd_def(v);
        if (d == nullptr) continue;
        auto same_ty = [&](VReg u) {
            TypePtr tu = ty_of(u), tv = ty_of(v);
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

// 归纳变量的算术使用区间：role==Bin 且某个操作数分类为 J 的首/末条指令。
// 只算**算术**使用点 —— 纯下标用的 j 不需要物化寄存器（生命周期越短，
// 临时值能复用的寄存器越多）。
void AsmGen::vect_j_span(const std::vector<Inst>& body, const std::vector<VRole>& role,
                         const std::unordered_map<VReg, VClass>& cls,
                         int* first, int* last) const {
    if (first == nullptr || last == nullptr) return;
    *first = -1;
    *last = -1;
    auto is_j = [&](VReg v) {
        auto it = cls.find(v);
        return it != cls.end() && it->second.kind == VK::J;
    };
    for (size_t i = 0; i < body.size(); ++i) {
        if (role[i] != VRole::Bin) continue;
        if (!is_j(body[i].a) && !is_j(body[i].b)) continue;
        if (*first < 0) *first = static_cast<int>(i);
        *last = static_cast<int>(i);
    }
}

// 临时值线性扫描（与 try_vect_loop 原来的内联实现同一套规则）：
//   * 每个 Load/Bin 的 dst 从池里取一个 xmm；
//   * dst 可以接管**左操作数**的寄存器（它的末次使用就是本条指令，发射期
//     第一条就是 movapd ra→rd，相同则省略，之后只读右操作数）；
//   * 操作数末次使用后还池（带重复检查：同一条指令的两个操作数可能是同一个
//     vreg（x*x），裸 push 会把同一寄存器放进池两次）；
//   * 归纳变量物化寄存器按 j_first..j_last 占一个，末次使用后还池。
// 池取空 → false（调用方一个字节都不发）。
bool AsmGen::vect_alloc_temps(VTempPlan* p) {
    if (p == nullptr || p->body == nullptr || p->role == nullptr ||
        p->last_use == nullptr || p->cls == nullptr || p->pool == nullptr ||
        p->tmp_reg == nullptr)
        return false;
    const std::vector<Inst>& BI = *p->body;
    const std::vector<VRole>& role = *p->role;
    auto resolve = [&](VReg v) -> VReg {
        while (true) {
            auto it = p->cls->find(v);
            if (it == p->cls->end() || it->second.mov_src == kNoReg) return v;
            v = it->second.mov_src;
        }
    };
    auto take = [&]() -> MReg {
        if (p->pool->empty()) return kMRegNone;
        MReg r = p->pool->front();
        p->pool->erase(p->pool->begin());
        return r;
    };
    auto give = [&](MReg r) {
        if (r == kMRegNone) return;
        for (MReg x : *p->pool) if (x == r) return;
        p->pool->push_back(r);
    };
    for (size_t i = 0; i < BI.size(); ++i) {
        const Inst& in = BI[i];
        // 归纳变量物化寄存器：首个算术使用点分配（它是那条 Bin 的操作数，
        // 所以要在 dst 之前），末次使用后还池。
        if (p->jvec == kMRegNone && role[i] == VRole::Bin &&
            static_cast<int>(i) == p->j_first) {
            p->jvec = take();
            if (p->jvec == kMRegNone) return false;
        }
        MReg dst_reg = kMRegNone;
        if (role[i] == VRole::Load || role[i] == VRole::Bin) {
            VReg r = resolve(in.dst);
            MReg rd = kMRegNone;
            if (role[i] == VRole::Bin && in.a != kNoReg) {
                VReg ra = resolve(in.a);
                auto lu = p->last_use->find(ra);
                if (lu != p->last_use->end() && lu->second == static_cast<int>(i)) {
                    auto tr = p->tmp_reg->find(ra);
                    if (tr != p->tmp_reg->end()) rd = tr->second;
                }
            }
            if (rd == kMRegNone) rd = take();
            if (rd == kMRegNone) return false;
            (*p->tmp_reg)[r] = rd;
            dst_reg = rd;
        }
        // 释放本指令操作数中末次使用的临时值（刚被目标接管的那个不再还池）。
        const VReg ops[3] = {in.a, in.b, in.c};
        for (VReg op : ops) {
            if (op == kNoReg) continue;
            VReg r = resolve(op);
            auto lu = p->last_use->find(r);
            if (lu != p->last_use->end() && lu->second == static_cast<int>(i)) {
                auto tr = p->tmp_reg->find(r);
                if (tr != p->tmp_reg->end() && tr->second != dst_reg) give(tr->second);
            }
        }
        if (p->jvec != kMRegNone && static_cast<int>(i) == p->j_last) give(p->jvec);
    }
    return true;
}

}  // namespace pyadamas
