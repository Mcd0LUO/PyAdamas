// codegen_regalloc.cpp —— 寄存器/栈槽分配与函数帧
#include "codegen_impl.h"

#include "block_bitset.h"
#include "live_interval.h"
#include "spill_placement.h"

namespace pyadamas {

// 调用序列之外不会被指令生成器用到的临时寄存器（%r8–%r11）。

// 帧布局：出参区一次留够，局部区落在它上面，callee-saved 再往上压。
    // 这样 rsp 在函数体里恒定，槽位和帧记录都能相对 rsp 寻址，帧里不需要
    // 帧指针。SysV 要求 call 时 rsp 是 16 的倍数：入口 rsp ≡ 8 (mod 16)，
    // 所以「push 的字节数 + subq 的字节数」要 ≡ 8 (mod 16)。
void AsmGen::layout_frame(int total_slots) { frame_.layout(total_slots, n_saved_regs_); }

void AsmGen::frame_alloc() {
        if (frame_.pad > 0) emit_imm(MOp::kSubq, frame_.pad, kRsp);
        frame_.cfa_off = frame_.bytes + 8;
        cfi_cfa_offset();
    }

// 尾声：先退局部区，再逐个弹回 callee-saved，最后 ret。
void AsmGen::emit_epilogue() {
        // CFI：尾声里 rsp 会一路退回去，先「记住」函数体的展开规则，
        // 把每一步都描述出来，ret 之后再还原——后面的块（异常落地、
        // 另一条 return）继续按函数体状态展开。
        cfi(".cfi_remember_state");
        const int body_cfa = frame_.cfa_off;
        // 先弹帧记录再退栈：记录本身在局部区底部，%rcx 此时是自由的。
        if (frame_rec_) {
            emit2(MOp::kMovq, rec_mem(0), kRcx);
            line("movq %rcx, pa_frame_top(%rip)");
        }
        if (frame_.pad > 0) {
            emit_imm(MOp::kAddq, frame_.pad, kRsp);
            frame_.cfa_off -= frame_.pad;
            cfi_cfa_offset();
        }
        for (int i = n_saved_regs_ - 1; i >= 0; --i) {
            const char* reg = gen_ ? "%r15" : kSlotRegs()[i];
            emit1(MOp::kPopq, mreg_from_name(reg));
            cfi_restore(reg);
            frame_.cfa_off -= 8;
            cfi_cfa_offset();
        }
        emit0(MOp::kRet);
        cfi(".cfi_restore_state");
        frame_.cfa_off = body_cfa;
    }

// ── 栈槽分配 ────────────────────────────────────────────────
    //
    // 线性扫描：给每个 vreg 算一个 [定义, 最后使用] 的区间，区间不重叠
    // 的可以共用一个栈槽。不做寄存器分配，所以不存在「值在寄存器里」
    // 的状态，规则非常简单；但槽数会从「vreg 总数」降到「最大同时活跃数」，
    // 对长函数是几十倍的差距。
    //
    // 关键前提：**只有「定义和使用都在同一基本块内」的值才能按线性区间
    // 复用**。跨块的值（尤其是循环头里被反复读取的）在静态上「已经用完」
    // 之后仍会在下一轮迭代被读，线性区间会把它的槽分给别人，然后循环
    // 第二次迭代就炸。这类值一律独占一个槽，区间拉满整个函数。
// RcMark 的返回值必须**独占**一个栈槽，且不参与任何寄存器分配。
//
// 它是 RC 临时栈的水位：入口 mark、出口 cleanup，中间隔着任意多次调用。
// 只要它被放进寄存器，某次 call 就会把它冲掉，cleanup 拿到错误水位 ——
// 运行时直接 panic（invalid temporary stack mark）。
//
// 原来的判定是「对每个 vreg 扫全函数找 RcMark」：nv × 指令数是 O(n²)
// （2150 vreg × 5000 指令的函数上实测占编译时间 3%+）。改成一次扫描
// 打标，两处调用点查表；判定的 vreg 集合完全相同。
namespace {
// RcMark 水位 vreg 一次扫描打标（供栈槽分配与全局寄存器提升两处查表）。
void collect_rc_marks(const IRFunction& f, BlockUseIndex& idx) {
    const size_t nv = f.vreg_ty.size();
    idx.rc_marks.assign(nv, 0);
    for (const auto& b : f.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::CallRt && static_cast<RtFn>(in.imm) == RtFn::RcMark &&
                in.dst != kNoReg && in.dst < nv)
                idx.rc_marks[in.dst] = 1;
}
}  // namespace

// ── vreg 栈槽分配 ───────────────────────────────────────────
//
// 目标：把「vreg 总数」个栈槽压到「最大同时活跃数」。
//
// **必须用精确的活跃性分析，不能用朴素的位置区间。** 原因是循环：
//
//     pos 10:  A = ...          ← 循环外定义
//     pos 50:  ... = A          ← 循环体内使用
//     pos 55:  B = ...          ← 循环体内定义
//     pos 58:  ... = B          ← 循环体内使用
//
// 朴素的 [定义, 最后使用] 给 A=[10,50]、B=[55,58]，两者不重叠 → 共享槽。
// 但循环第二轮在 pos 50 读 A 时，槽里是 B 在 pos 58 写进去的值 —— 直接算错。
//
// 正确做法：A 在循环体末尾仍然 live（下一轮还要读），所以 live_out[循环体]
// 含 A，A 的区间被扩展到块末尾，与 B 重叠 → 不共享。
//
// 这就是标准 linear scan 的 live interval：先做块级 live_in/live_out 数据流，
// 再在块内从后往前扫出精确的活跃位置。
void AsmGen::assign_vreg_slots() {
        const size_t nv = f_->vreg_ty.size();
        // RcMark 水位 vreg 一次扫描打标（供本函数与 assign_global_vreg_regs
        // 两处查表；原来是「每个 vreg 扫全函数」，O(n²)）。
        collect_rc_marks(*f_, block_use_);
        vreg_slot_.assign(nv, 0);
        n_vreg_slots_ = 0;
        use_count_.assign(nv, 0);
        cross_block_.assign(nv, false);
        def_block_.assign(nv, -1);
        if (nv == 0) return;
        const int nb = static_cast<int>(f_->blocks.size());
        if (nb == 0) return;

        // ── 1. 逐指令扫描：使用计数、定义位置、块级 use/def ──
        std::vector<int> def_pos(nv, -1);
        std::vector<int> def_count(nv, 0);
        std::vector<int> first_use(nv, INT32_MAX);
        std::vector<int> last_use(nv, -1);
        std::vector<int> block_start(static_cast<size_t>(nb), 0);
        // 块级 use/def 与下面的 live_in/live_out 都用字位集（block_bitset.h）。
        // 原实现是每块 nv 字节的 char 向量：626 块 × 2150 vreg 的函数单趟
        // 扫描就是 ~5MB 字节流量，不动点迭代还要乘轮数。只换容器，判定
        // 逻辑与结果逐位不变。
        std::vector<BlockBits> blk_use(static_cast<size_t>(nb));
        std::vector<BlockBits> blk_def(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            blk_use[static_cast<size_t>(b)].reset(nv);
            blk_def[static_cast<size_t>(b)].reset(nv);
        }
        int pos = 0;
        for (int b = 0; b < nb; ++b) {
            block_start[static_cast<size_t>(b)] = pos;
            auto& buse = blk_use[static_cast<size_t>(b)];
            auto& bdef = blk_def[static_cast<size_t>(b)];
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                auto touch = [&](VReg r) {
                    if (r == kNoReg || r >= nv) return;
                    ++use_count_[r];
                    if (pos < first_use[r]) first_use[r] = pos;
                    last_use[r] = pos;
                    if (!bdef.test(r)) buse.set(r);  // 块内先使用后定义 → live-in
                };
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
                if (in.dst != kNoReg && in.dst < nv) {
                    def_pos[in.dst] = pos;
                    def_block_[in.dst] = b;
                    ++def_count[in.dst];
                    bdef.set(in.dst);
                }
                ++pos;
            }
        }
        const int total_pos = pos;
        // 跨块判定：定义块 ≠ 使用块。
        //
        // 一趟顺扫（语义与原「逐 vreg 扫全表」逐位相同：对每个 vreg，命中
        // 第一个「不是定义块」的使用点即置位，之后保持）：把 O(nv × 指令数)
        // 的重复遍历降到 O(指令数)。大函数上这是 assign_vreg_slots 的主要
        // 开销（nbody 的 simulate/advance 各 3000+ 指令 × 4000+ vreg）。
        for (int b = 0; b < nb; ++b) {
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                auto hit = [&](VReg r) {
                    if (r == kNoReg || r >= nv) return;
                    if (def_pos[r] < 0) return;  // 未定义：与旧逻辑一样保持 false
                    if (b != def_block_[r]) cross_block_[r] = true;
                };
                hit(in.a); hit(in.b); hit(in.c);
                for (VReg r : in.args) hit(r);
            }
        }

        // ── 2. CFG 与块级活跃性（backward 数据流）──
        std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            if (insts.empty()) {
                if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
                continue;
            }
            const Inst& last = insts.back();
            if (last.op == IrOp::Jump) {
                if (last.target >= 0 && last.target < nb)
                    succ[static_cast<size_t>(b)].push_back(last.target);
            } else if (last.op == IrOp::Branch) {
                if (last.target >= 0 && last.target < nb)
                    succ[static_cast<size_t>(b)].push_back(last.target);
                if (last.target2 >= 0 && last.target2 < nb)
                    succ[static_cast<size_t>(b)].push_back(last.target2);
            } else {
                switch (last.op) {
                    case IrOp::Return:
                    case IrOp::GenYield:
                    case IrOp::GenAwait:
                    case IrOp::GenRethrow:
                    case IrOp::GenDone:
                        break;
                    default:
                        if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
                        break;
                }
            }
        }
        std::vector<BlockBits> live_in(static_cast<size_t>(nb));
        std::vector<BlockBits> live_out(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            live_in[static_cast<size_t>(b)].reset(nv);
            live_out[static_cast<size_t>(b)].reset(nv);
        }
        bool changed = true;
        int guard = 0;
        while (changed && guard++ < 2000) {
            changed = false;
            for (int b = nb - 1; b >= 0; --b) {
                // out 每次都从空集重算：继承上一轮的值会让 live_out 单调
                // 增长、永不收缩 —— 区间被越拉越长，最后所有 vreg 都变成
                // 全函数活跃，复用彻底失效（且没有后继的块会残留脏值）。
                BlockBits out(nv, false);
                for (int s : succ[static_cast<size_t>(b)]) out.or_with(live_in[static_cast<size_t>(s)]);
                BlockBits in(nv, false);
                const auto& buse = blk_use[static_cast<size_t>(b)];
                const auto& bdef = blk_def[static_cast<size_t>(b)];
                in.andnot_or(out, bdef, buse);
                if (!(in == live_in[static_cast<size_t>(b)]) ||
                    !(out == live_out[static_cast<size_t>(b)])) {
                    live_in[static_cast<size_t>(b)] = std::move(in);
                    live_out[static_cast<size_t>(b)] = std::move(out);
                    changed = true;
                }
            }
        }

        // ── 3. 精确 live interval（块内从后往前扫）──
        std::vector<int> live_first(nv, INT32_MAX);
        std::vector<int> live_last(nv, -1);
        auto mark_live = [&](VReg v, int p) {
            if (v >= nv) return;
            if (p < live_first[v]) live_first[v] = p;
            if (p > live_last[v]) live_last[v] = p;
        };
        for (int b = 0; b < nb; ++b) {
            BlockBits cur(nv, false);
            cur.assign(live_out[static_cast<size_t>(b)]);
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            const int bstart = block_start[static_cast<size_t>(b)];
            const int blen = static_cast<int>(insts.size());
            // live-out 的值一直活到块末尾
            if (blen > 0)
                cur.for_each_set([&](size_t v) {
                    mark_live(static_cast<VReg>(v), bstart + blen - 1);
                });
            for (int k = blen - 1; k >= 0; --k) {
                const Inst& in = insts[static_cast<size_t>(k)];
                const int p = bstart + k;
                if (in.dst != kNoReg && in.dst < nv) cur.clear(in.dst);
                auto use = [&](VReg v) {
                    if (v == kNoReg || v >= nv) return;
                    cur.set(v);
                    mark_live(v, p);
                };
                use(in.a);
                use(in.b);
                use(in.c);
                for (VReg r : in.args) use(r);
            }
            cur.for_each_set([&](size_t v) { mark_live(static_cast<VReg>(v), bstart); });
        }

        // ── 4. 线性扫描 ──
        struct Range {
            int start, end, vreg;
        };
        std::vector<Range> ranges;
        ranges.reserve(nv);
        std::vector<int> dead_vregs;
        for (size_t v = 0; v < nv; ++v) {
            // **既没定义也没使用的 vreg 也必须占一个独立槽。**
            //
            // vreg_slot_ 的默认值是 0，跳过它们会让这些「死 vreg」全部
            // 指向槽 0 —— 而槽 0 会被真实 vreg 用，于是同一个槽同时被
            // 两批 vreg 认领。读死 vreg 时拿到的就是别人的值。
            //
            // 原版给每个 vreg 都压了区间（包括死的），所以从没暴露这个
            // 问题；改成 live interval 后必须显式处理。
            // 既没定义也没使用的 vreg：**不参与线性扫描**，最后统一给
            // 一个哨兵槽。
            //
            // 它们出现在 vreg 表里但没有指令读写（降级器预留、死代码残留）。
            // 原版给每个压全函数区间，槽数被抬高；给单点区间也不行 ——
            // 它们全挤在位置 0、同时活跃。
            if (def_pos[v] < 0 && first_use[v] == INT32_MAX) {
                dead_vregs.push_back(static_cast<int>(v));
                continue;
            }
            // rc_mark：水位值跨整个函数体（入口 mark、出口 cleanup），独占。
            // 多次定义（生成器续跑 load.slot 回写）：区间不连续，独占。
            //
            // **跨块浮点 vreg 也必须独占。**
            //
            // 它们会被 assign_global_vreg_regs 提升进 xmm8–15，而调用点的
            // spill/reload 按 `vreg_off(v)` 写回栈副本。若两个 vreg 因活跃
            // 区间不重叠而共享槽，两个寄存器就会写到同一栈地址 ——
            // 实测 spectral_norm 生成 `movsd %xmm10, 80(%rsp)` 与
            // `movsd %xmm11, 80(%rsp)`，reload 时全读到后写的那个值。
            //
            // 数量上限是浮点池大小（8），代价可接受。
            if (block_use_.is_rc_mark(static_cast<VReg>(v)) || def_count[v] > 1 ||
                (cross_block_[v] && f_->vreg_ty[v] &&
                 f_->vreg_ty[v]->kind == TyKind::Float)) {
                ranges.push_back({0, total_pos + 1, static_cast<int>(v)});
                continue;
            }
            int start = live_first[v] == INT32_MAX
                            ? (def_pos[v] >= 0 ? def_pos[v] : 0)
                            : live_first[v];
            int end = live_last[v] >= 0 ? live_last[v] : start;
            if (def_pos[v] >= 0 && def_pos[v] < start) start = def_pos[v];
            if (last_use[v] > end) end = last_use[v];
            ranges.push_back({start, end, static_cast<int>(v)});
        }
        std::ranges::sort(ranges, [](const Range& a, const Range& b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        });
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>,
                            std::greater<>>
            active;
        std::vector<int> free_slots;
        for (const auto& r : ranges) {
            while (!active.empty() && active.top().first < r.start) {
                free_slots.push_back(active.top().second);
                active.pop();
            }
            int s;
            if (!free_slots.empty()) {
                s = free_slots.back();
                free_slots.pop_back();
            } else {
                s = n_vreg_slots_++;
            }
            vreg_slot_[r.vreg] = s;
            active.push({r.end, s});
        }
        // 死 vreg 统一指向一个额外哨兵槽（在所有活槽之后）。
        // 它们不被任何指令读写，内容无所谓；关键是**不能和活值共享槽** ——
        // 否则读死 vreg 会拿到别人的值。
        if (!dead_vregs.empty()) {
            const int sentinel = n_vreg_slots_++;
            for (int dv : dead_vregs) vreg_slot_[static_cast<size_t>(dv)] = sentinel;
        }
    }

// 变量提升：按被读写次数排序，把最热的几个局部变量放进寄存器。
// 没提升的变量继续住栈槽，语义完全一样，所以这一步错了最多是慢，
// 不会算错。
//
// 不做活跃性分析——一个变量要么整函数在寄存器里，要么整函数在栈上。
// 分支汇合、循环回边都因此天然正确，不需要活跃性 spill 逻辑。整数
// 用 callee-saved GPR，调用天然安全；浮点用 xmm8–15（xmm0–7 留给
// 参数和临时），在每次会正常返回的 call 前后统一写回/读出栈槽。
void AsmGen::spill_fp_slots() {
        // 整型 caller 池的槽也要在调用点写回（它们同样被 call 破坏）。
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            emit2(MOp::kMovsd, slot_reg_[slot], smem_mop(slot));
        }
        // 跨块 vreg 同样要写回：SysV 下 xmm8–15 是 caller-saved。
        // 它们没有块级掩码（活跃性按整个函数算），一律写回。
        //
        // 栈副本必须用 `vreg_off(v)` 直接算，不能用 `mem(v)` —— 后者在
        // vreg 已被提升到寄存器时返回寄存器本身，`movsd %xmm8, %xmm8`
        // 是非法操作数组合（实测 spectral_norm 汇编失败）。
        for (int v : fp_promoted_vregs_) {
            const MOperand& rg = vreg_reg_[static_cast<size_t>(v)];
            if (rg.empty()) continue;
            emit2(MOp::kMovsd, rg, op_mem("%rsp", vreg_off(static_cast<VReg>(v))));
        }
    }

void AsmGen::reload_fp_slots() {
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            emit2(MOp::kMovsd, smem_mop(slot), slot_reg_[slot]);
        }
        for (int v : fp_promoted_vregs_) {
            const MOperand& rg = vreg_reg_[static_cast<size_t>(v)];
            if (rg.empty()) continue;
            emit2(MOp::kMovsd, op_mem("%rsp", vreg_off(static_cast<VReg>(v))), rg);
        }
    }

// 浮点槽在调用点是否还需要：块级 live-in/live-out（只跟踪提升过的槽）。
void AsmGen::build_fp_liveness() {
        const int nb = static_cast<int>(f_->blocks.size());
        const size_t kf = fp_promoted_slots_.size();
        fp_live_in_.assign(static_cast<size_t>(nb), std::vector<uint8_t>(kf, 0));
        fp_live_out_.assign(static_cast<size_t>(nb), std::vector<uint8_t>(kf, 0));
        fp_live_off_ = std::getenv("PYADAMAS_NO_FPLIVE") != nullptr;
        if (kf == 0 || nb == 0 || fp_live_off_) return;

        std::vector<std::vector<uint8_t>> use(static_cast<size_t>(nb),
                                              std::vector<uint8_t>(kf, 0));
        std::vector<std::vector<uint8_t>> def(static_cast<size_t>(nb),
                                              std::vector<uint8_t>(kf, 0));
        for (int b = 0; b < nb; ++b) {
            // 只把「块内先读后写」的读算作 use：先 store 再 load 是本块
            // 自己定义的值，不构成对入口值的活跃需求。
            std::vector<uint8_t>& defined = def[static_cast<size_t>(b)];
            std::vector<uint8_t>& used = use[static_cast<size_t>(b)];
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                if (in.idx < 0 || in.idx >= static_cast<int>(slot_fpidx_.size())) continue;
                int fi = slot_fpidx_[static_cast<size_t>(in.idx)];
                if (fi < 0 || static_cast<size_t>(fi) >= kf) continue;
                if (in.op == IrOp::LoadSlot) {
                    if (!defined[static_cast<size_t>(fi)]) used[static_cast<size_t>(fi)] = 1;
                } else if (in.op == IrOp::StoreSlot) {
                    defined[static_cast<size_t>(fi)] = 1;
                }
            }
        }

        std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            auto add = [&](int t) {
                if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
            };
            if (insts.empty()) {
                add(b + 1);
                continue;
            }
            switch (insts.back().op) {
                case IrOp::Jump: add(insts.back().target); break;
                case IrOp::Branch:
                    add(insts.back().target);
                    add(insts.back().target2);
                    break;
                case IrOp::Return:
                case IrOp::GenYield:
                case IrOp::GenAwait:
                case IrOp::GenRethrow:
                case IrOp::GenDone:
                    break;
                default: add(b + 1); break;
            }
        }

        bool chg = true;
        while (chg) {
            chg = false;
            for (int b = nb - 1; b >= 0; --b) {
                std::vector<uint8_t> out(kf, 0);
                for (int s : succ[static_cast<size_t>(b)]) {
                    const auto& li = fp_live_in_[static_cast<size_t>(s)];
                    for (size_t j = 0; j < kf; ++j) out[j] = out[j] | li[j];
                }
                std::vector<uint8_t> in(kf, 0);
                const auto& ub = use[static_cast<size_t>(b)];
                const auto& db = def[static_cast<size_t>(b)];
                for (size_t j = 0; j < kf; ++j)
                    in[j] = ub[j] | (out[j] & static_cast<uint8_t>(~db[j]));
                if (out != fp_live_out_[static_cast<size_t>(b)] ||
                    in != fp_live_in_[static_cast<size_t>(b)]) {
                    fp_live_out_[static_cast<size_t>(b)] = out;
                    fp_live_in_[static_cast<size_t>(b)] = in;
                    chg = true;
                }
            }
        }
    }


// ── R1：整型槽的块级活跃性 ────────────────────────────────────────
//
// 目的：给 assign_slot_regs_linear 提供活跃区间。做法是标准的
// live-in/live-out 数据流（与上面的 build_fp_liveness 同形状，但跟踪
// 全部可提升的整型槽，且位图按槽号而不是按「提升过的下标」）。
//
// 排除条件与 assign_slot_regs 一致：堆槽（帧记录按地址读值，寄存器里
// 的值 GC 看不见）、取过地址的槽（SlotAddr）、handler hazard 槽（longjmp
// 会回滚 callee-saved）都不参与。
void AsmGen::build_slot_liveness() {
        const int nb = static_cast<int>(f_->blocks.size());
        const int ns = static_cast<int>(f_->slot_ty.size());
        slot_live_in_.assign(static_cast<size_t>(nb), {});
        slot_live_out_.assign(static_cast<size_t>(nb), {});
        slot_reg_cand_.clear();
        slot_cand_is_fp_.clear();
        slot_reg_cand_idx_.assign(static_cast<size_t>(ns), -1);
        if (nb == 0 || ns == 0) return;

        // 1) 排除：堆槽 / 取过地址 / handler hazard / 运行时直写槽。
        std::vector<bool> addr_taken(static_cast<size_t>(ns), false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::SlotAddr && in.idx >= 0 && in.idx < ns)
                    addr_taken[static_cast<size_t>(in.idx)] = true;
                // DictTryGet 由运行时把值写进栈槽（不是 StoreSlot）：提升后
                // 寄存器不会跟着更新，且无 StoreSlot 定义让块入口重载逻辑
                // 误以为前驱寄存器里已有值。一律留在内存（0.284）。
                if (in.op == IrOp::CallRt &&
                    in.imm == static_cast<int64_t>(RtFn::DictTryGet) &&
                    in.idx >= 0 && in.idx < ns)
                    addr_taken[static_cast<size_t>(in.idx)] = true;
            }

        std::vector<bool> hazard(static_cast<size_t>(ns), false);
        if (f_->has_handlers) {
            std::vector<bool> in_handler(static_cast<size_t>(nb), false);
            std::vector<int> open;
            for (int b = 0; b < nb; ++b) {
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    if (in.op == IrOp::CallRt &&
                        in.imm == static_cast<int64_t>(RtFn::HandlerPush))
                        open.push_back(b);
                    else if (in.op == IrOp::CallRt &&
                             (in.imm == static_cast<int64_t>(RtFn::HandlerLeave) ||
                              in.imm == static_cast<int64_t>(RtFn::HandlerDiscard))) {
                        if (!open.empty()) open.pop_back();
                    }
                }
                in_handler[static_cast<size_t>(b)] = !open.empty();
            }
            for (int s2 = 0; s2 < ns; ++s2) {
                bool wrote = false;
                bool read = false;
                for (int b = 0; b < nb; ++b) {
                    for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                        if (in.idx != s2) continue;
                        if (in.op == IrOp::StoreSlot && in_handler[static_cast<size_t>(b)])
                            wrote = true;
                        else if (in.op == IrOp::LoadSlot)
                            read = true;
                    }
                }
                if (wrote && read) hazard[static_cast<size_t>(s2)] = true;
            }
        }

        // 整型与浮点都纳入候选（R4）：两套池分别做线性扫描。
        // 浮点要求 opt_ >= 1（与旧逻辑一致）；整型无此限制。
        for (int s2 = 0; s2 < ns; ++s2) {
            if (ir_is_heap(f_->slot_ty[static_cast<size_t>(s2)])) continue;
            // **unknown 槽不能提升。** 它可能装着堆指针，GC 要靠帧记录里的
            // 栈槽地址找根。提升后栈槽不再更新，GC 读到序言清零的 0，
            // 对象被提前回收 —— ASan 实测 SEGV（x_min.py 里 u 是 unknown 槽）。
            TypePtr s2ty = f_->slot_ty[static_cast<size_t>(s2)];
            if (!s2ty || s2ty->kind == TyKind::Unknown) continue;
            // 被 GC 扫描 / 异常展开依赖的槽同样必须留在内存。
            if (s2 < static_cast<int>(gc_scan_slot_.size()) && gc_scan_slot_[s2])
                continue;
            if (addr_taken[static_cast<size_t>(s2)]) continue;
            if (hazard[static_cast<size_t>(s2)]) continue;
            bool isfp = ir_is_float(s2ty);
            if (isfp && opt_ < 1) continue;
            slot_reg_cand_idx_[static_cast<size_t>(s2)] =
                static_cast<int>(slot_reg_cand_.size());
            slot_reg_cand_.push_back(s2);
            slot_cand_is_fp_.push_back(isfp ? 1 : 0);
        }
        const int nc = static_cast<int>(slot_reg_cand_.size());
        if (nc == 0) return;
        const int words = (nc + 63) / 64;
        for (int b = 0; b < nb; ++b) {
            slot_live_in_[static_cast<size_t>(b)].assign(static_cast<size_t>(words), 0);
            slot_live_out_[static_cast<size_t>(b)].assign(static_cast<size_t>(words), 0);
        }

        // 2) 每块的 use / def（位图，按候选下标）。
        std::vector<std::vector<uint64_t>> use(static_cast<size_t>(nb)),
            def(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            use[static_cast<size_t>(b)].assign(static_cast<size_t>(words), 0);
            def[static_cast<size_t>(b)].assign(static_cast<size_t>(words), 0);
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                if (in.idx < 0 || in.idx >= ns) continue;
                int ci = slot_reg_cand_idx_[static_cast<size_t>(in.idx)];
                if (ci < 0) continue;
                uint64_t bit = 1ULL << (ci & 63);
                size_t w = static_cast<size_t>(ci >> 6);
                if (in.op == IrOp::LoadSlot) {
                    // 块内先写后读不算 use（读的是本块定义的值）。
                    if (!(def[static_cast<size_t>(b)][w] & bit))
                        use[static_cast<size_t>(b)][w] |= bit;
                } else if (in.op == IrOp::StoreSlot) {
                    def[static_cast<size_t>(b)][w] |= bit;
                }
            }
        }

        // 3) 后继边。
        std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            auto add = [&](int t) {
                if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
            };
            if (insts.empty()) {
                add(b + 1);
                continue;
            }
            switch (insts.back().op) {
                case IrOp::Jump: add(insts.back().target); break;
                case IrOp::Branch:
                    add(insts.back().target);
                    add(insts.back().target2);
                    break;
                case IrOp::Return:
                case IrOp::GenYield:
                case IrOp::GenAwait:
                case IrOp::GenRethrow:
                case IrOp::GenDone:
                    break;
                default: add(b + 1); break;
            }
        }

        // 4) 迭代到不动点：in = use | (out & ~def)，out = ∪ in(succ)。
        bool chg = true;
        while (chg) {
            chg = false;
            for (int b = nb - 1; b >= 0; --b) {
                std::vector<uint64_t> out(static_cast<size_t>(words), 0);
                for (int sc : succ[static_cast<size_t>(b)])
                    for (int w = 0; w < words; ++w)
                        out[static_cast<size_t>(w)] |=
                            slot_live_in_[static_cast<size_t>(sc)][static_cast<size_t>(w)];
                std::vector<uint64_t> in(static_cast<size_t>(words), 0);
                for (int w = 0; w < words; ++w)
                    in[static_cast<size_t>(w)] =
                        use[static_cast<size_t>(b)][static_cast<size_t>(w)] |
                        (out[static_cast<size_t>(w)] &
                         ~def[static_cast<size_t>(b)][static_cast<size_t>(w)]);
                if (out != slot_live_out_[static_cast<size_t>(b)] ||
                    in != slot_live_in_[static_cast<size_t>(b)]) {
                    slot_live_out_[static_cast<size_t>(b)] = std::move(out);
                    slot_live_in_[static_cast<size_t>(b)] = std::move(in);
                    chg = true;
                }
            }
        }

        if (std::getenv("PYADAMAS_RA_DEBUG")) {
            std::fprintf(stderr, "[ra] %s: %d blocks, %d cand slots, %d words\n",
                         f_->name.c_str(), nb, nc, words);
            for (int b = 0; b < nb; ++b) {
                std::fprintf(stderr, "  bb%d in:", b);
                for (int ci = 0; ci < nc; ++ci)
                    if (slot_live_in_[static_cast<size_t>(b)]
                            [static_cast<size_t>(ci >> 6)] >> (ci & 63) & 1)
                        std::fprintf(stderr, " s%d", slot_reg_cand_[static_cast<size_t>(ci)]);
                std::fprintf(stderr, "  out:");
                for (int ci = 0; ci < nc; ++ci)
                    if (slot_live_out_[static_cast<size_t>(b)]
                            [static_cast<size_t>(ci >> 6)] >> (ci & 63) & 1)
                        std::fprintf(stderr, " s%d", slot_reg_cand_[static_cast<size_t>(ci)]);
                std::fprintf(stderr, "\n");
            }
        }
    }

// ── R2：按活跃区间的线性扫描分配 ─────────────────────────────────
//
// 与旧的 assign_slot_regs（按访问频次挑前 N 个）的区别：旧办法一个槽
// 要么全程常驻、要么全程落栈；线性扫描允许**不同活跃区间的槽复用同一个
// 寄存器**，所以在变量数超过寄存器数时覆盖率明显更高。
//
// 区间计算：对每个候选槽，取「第一次出现（定义或使用）」到「最后一次
// 出现」之间的全部指令位置。这是保守的线性近似（真实的区间是活跃性
// 集合，可能不连续），但实现简单且不会错 —— 只会让复用变少，不会让
// 两个同时活跃的槽抢到同一个寄存器。
//
// 分配：区间按起点排序后线性扫描；寄存器用完时，把「当前终点最远」的
// 区间溢出到栈（它是最后才会空出来的那个，溢出的机会成本最大）。

// 判断一段线性位置区间内是否有调用类指令。caller-saved 寄存器只在
// 「不跨调用」的区间里可以安全承载变量。
//
// 位置编码与 assign_slot_regs_linear 一致：块号 * kBlockStride + 块内下标。
bool AsmGen::slot_range_has_call(int64_t beg, int64_t end) const {
        const int64_t kBlockStride = 1 << 20;
        const int64_t b0 = beg / kBlockStride;
        const int64_t b1 = end / kBlockStride;
        // 跨块一律保守判为「有调用」：块之间的跳转可能经过调用点。
        if (b0 != b1) return true;
        if (b0 < 0 || b0 >= static_cast<int64_t>(f_->blocks.size())) return true;
        const auto& insts = f_->blocks[static_cast<size_t>(b0)].insts;
        for (const auto& in : insts) {
            switch (in.op) {
                case IrOp::Call:
                case IrOp::CallRt:
                case IrOp::CallExtern:
                case IrOp::CallPtr:
                case IrOp::CallDirect:
                case IrOp::VCall:
                case IrOp::VCallStatic:
                case IrOp::SuperVCall:
                case IrOp::Setjmp:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

// ── 通用线性扫描（整型 / 浮点共用）──────────────────────────────
//
// 输入：候选槽号列表、寄存器池、池大小。输出：slot -> 寄存器名（空 = 落栈）。
//
// 与 assign_slot_regs_linear 的区别：那个函数把「区间计算 + 扫描 + 写回
// slot_reg_」揉在一起，只服务整型。这里抽出纯扫描部分，让浮点也能复用 ——
// 浮点原本是「按频次挑前 N 个整函数常驻」，在 nbody 这种同时活跃临时值
// 超过 8 个的函数里浪费严重（实测 79 次 movsd 写栈 + 70 次读栈）。
// ── 精确区间诊断（PYADAMAS_LI_STATS）────────────────────────────
//
// 对比「精确活跃区间」与「[first,last] 粗近似」的差距，用来验证
// 阶段 A（精确 LiveInterval）到底能省多少。
//
// 输出：每个函数的总精确长度 / 总包络长度 / 浪费比例。
// 浪费比例高 = 粗近似把大量不活跃位置算成了活跃 = 图着色/线性扫描
// 会误判冲突。
void AsmGen::report_interval_precision() {
        if (!std::getenv("PYADAMAS_LI_STATS")) return;
        auto ivs = build_precise_intervals(*f_);
        int n_used = 0;
        int64_t precise = 0, envelope = 0, wasted = 0;
        for (const auto& li : ivs) {
            if (li.empty()) continue;
            ++n_used;
            precise += li.totalLength();
            envelope += li.lastPos() - li.firstPos();
            wasted += li.wastedByEnvelope();
        }
        if (n_used == 0) return;
        // **峰值压力对比：精确区间 vs 包络。**
        //
        // 这是「区间级分配能带来多少收益」的上限判据：
        //   - 精确峰值 <= 6（GPR 数）   -> 洞感知分配能让所有槽常驻寄存器；
        //   - 包络峰值 > 6               -> 现在的整函数分配必然溢出。
        // 两者差距越大，重写的价值越大。
        auto peak = [](const std::vector<LiveInterval>& v, bool use_env) {
            std::vector<std::pair<int64_t, int>> ev;
            for (const auto& li : v) {
                if (li.empty()) continue;
                if (use_env) {
                    ev.emplace_back(li.firstPos(), 1);
                    ev.emplace_back(li.lastPos(), -1);
                } else {
                    for (const auto& r : li.ranges) {
                        ev.emplace_back(r.start, 1);
                        ev.emplace_back(r.end, -1);
                    }
                }
            }
            std::ranges::sort(ev, [](const auto& x, const auto& y) {
                return x.first != y.first ? x.first < y.first : x.second < y.second;
            });
            int cur = 0, mx = 0;
            for (const auto& e : ev) { cur += e.second; mx = std::max(mx, cur); }
            return mx;
        };
        std::fprintf(stderr,
                     "[li] %-20s slots=%2d precise=%6lld env=%6lld waste=%4.1f%%  peak: precise=%d env=%d\n",
                     f_->name.c_str(), n_used, static_cast<long long>(precise),
                     static_cast<long long>(envelope),
                     envelope > 0 ? 100.0 * static_cast<double>(wasted) / static_cast<double>(envelope) : 0.0,
                     peak(ivs, false), peak(ivs, true));
}

void AsmGen::linear_scan_pool(const std::vector<int>& cands, const char* const* regs,
                              int nreg, std::vector<std::string>& out) {
        const int nc = static_cast<int>(cands.size());
        if (nc == 0 || nreg <= 0) return;

        // 0) 建立「全局候选下标 -> 本函数局部下标」的映射。
        //    cands 可能是 slot_reg_cand_ 的子集（整型 / 浮点分开扫），
        //    而 slot_reg_cand_idx_ 存的是**全局**下标 —— 直接用会把区间
        //    算到别的槽上（实测：nbody 输出完全错乱）。
        std::vector<int> local_of(f_->slot_ty.size(), -1);
        for (int ci = 0; ci < nc; ++ci)
            local_of[static_cast<size_t>(cands[static_cast<size_t>(ci)])] = ci;

        // 1) 活跃区间：块内读写位置 + live-in/out 覆盖的整块。
        const int64_t kBlockStride = 1 << 20;
        std::vector<int64_t> first(static_cast<size_t>(nc), INT64_MAX);
        std::vector<int64_t> last(static_cast<size_t>(nc), -1);
        auto touch = [&](int ci, int64_t lo, int64_t hi) {
            if (lo < first[static_cast<size_t>(ci)])
                first[static_cast<size_t>(ci)] = lo;
            if (hi > last[static_cast<size_t>(ci)])
                last[static_cast<size_t>(ci)] = hi;
        };
        for (int b = 0; b < static_cast<int>(f_->blocks.size()); ++b) {
            const int64_t blk_lo = static_cast<int64_t>(b) * kBlockStride;
            const int64_t blk_hi =
                blk_lo + static_cast<int64_t>(f_->blocks[static_cast<size_t>(b)].insts.size());
            int64_t pos = blk_lo;
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                if (in.idx >= 0 && in.idx < static_cast<int>(local_of.size())) {
                    int ci = local_of[static_cast<size_t>(in.idx)];
                    if (ci >= 0 && (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot))
                        touch(ci, pos, pos);
                }
                ++pos;
            }
            if (b < static_cast<int>(slot_live_in_.size())) {
                // live 位图是按**全局**候选下标（slot_reg_cand_ 的位置）编码的，
                // 这里要通过 slot_reg_cand_idx_ 反查后再映射到局部下标。
                for (int ci = 0; ci < nc; ++ci) {
                    int gi = slot_reg_cand_idx_[static_cast<size_t>(cands[static_cast<size_t>(ci)])];
                    if (gi < 0) continue;
                    size_t w = static_cast<size_t>(gi >> 6);
                    uint64_t bit = 1ULL << (gi & 63);
                    if ((slot_live_in_[static_cast<size_t>(b)][w] & bit) ||
                        (slot_live_out_[static_cast<size_t>(b)][w] & bit))
                        touch(ci, blk_lo, blk_hi);
                }
            }
        }

        // 2) 按起点排序。
        struct Live {
            int64_t beg;
            int64_t end;
            int ci;
        };
        std::vector<Live> lives;
        for (int ci = 0; ci < nc; ++ci) {
            if (last[static_cast<size_t>(ci)] < 0) continue;
            lives.push_back({first[static_cast<size_t>(ci)],
                             last[static_cast<size_t>(ci)], ci});
        }
        if (lives.empty()) return;
        std::ranges::sort(lives, [](const Live& a, const Live& b) {
            return a.beg != b.beg ? a.beg < b.beg : a.ci < b.ci;
        });

        // 3) 扫描 + 溢出。
        std::vector<Live> active;
        std::vector<int> owner(static_cast<size_t>(nreg), -1);  // 寄存器 -> ci
        for (const Live& lv : lives) {
            for (size_t i2 = 0; i2 < active.size();) {
                if (active[i2].end < lv.beg) {
                    for (int r = 0; r < nreg; ++r)
                        if (owner[static_cast<size_t>(r)] == active[i2].ci)
                            owner[static_cast<size_t>(r)] = -1;
                    active.erase(active.begin() + static_cast<long>(i2));
                } else {
                    ++i2;
                }
            }
            int free_r = -1;
            for (int r = 0; r < nreg; ++r)
                if (owner[static_cast<size_t>(r)] < 0) {
                    free_r = r;
                    break;
                }
            if (free_r >= 0) {
                owner[static_cast<size_t>(free_r)] = lv.ci;
                out[static_cast<size_t>(cands[static_cast<size_t>(lv.ci)])] = regs[free_r];
                active.push_back(lv);
                continue;
            }
            // 寄存器耗尽：溢出「当前终点最远」的区间（它最晚空出来）。
            size_t worst = 0;
            for (size_t i2 = 1; i2 < active.size(); ++i2)
                if (active[i2].end > active[worst].end) worst = i2;
            if (active[worst].end > lv.end) {
                int freed = -1;
                for (int r = 0; r < nreg; ++r)
                    if (owner[static_cast<size_t>(r)] == active[worst].ci) {
                        freed = r;
                        break;
                    }
                if (freed >= 0) {
                    out[static_cast<size_t>(cands[static_cast<size_t>(active[worst].ci)])].clear();
                    owner[static_cast<size_t>(freed)] = lv.ci;
                    out[static_cast<size_t>(cands[static_cast<size_t>(lv.ci)])] = regs[freed];
                    active[worst] = lv;
                }
            }
        }
    }

}  // namespace pyadamas
