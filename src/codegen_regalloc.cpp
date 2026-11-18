// codegen_regalloc.cpp —— 寄存器/栈槽分配与函数帧
#include "codegen_impl.h"

#include "live_interval.h"
#include "spill_placement.h"

namespace pyadamas {

// 调用序列之外不会被指令生成器用到的临时寄存器（%r8–%r11）。

// 帧布局：出参区一次留够，局部区落在它上面，callee-saved 再往上压。
    // 这样 rsp 在函数体里恒定，槽位和帧记录都能相对 rsp 寻址，帧里不需要
    // 帧指针。SysV 要求 call 时 rsp 是 16 的倍数：入口 rsp ≡ 8 (mod 16)，
    // 所以「push 的字节数 + subq 的字节数」要 ≡ 8 (mod 16)。
void AsmGen::layout_frame(int total_slots) {
        int fixed = 8 * n_saved_regs_ + out_bytes_;
        int pad = 8 * total_slots;
        while (((fixed + pad) & 15) != 8) pad += 8;
        locals_ = pad;
        frame_pad_ = out_bytes_ + locals_;
        // frame_bytes_ 是 rsp 到返回地址的距离：push 的 + 下面这一整块。
        frame_bytes_ = 8 * n_saved_regs_ + frame_pad_;
    }

void AsmGen::frame_alloc() {
        if (frame_pad_ > 0) emit_imm_op("subq", frame_pad_, "%rsp");
        cfa_off_ = frame_bytes_ + 8;
        cfi(std::format(".cfi_def_cfa_offset {}", cfa_off_));
    }

// 尾声：先退局部区，再逐个弹回 callee-saved，最后 ret。
void AsmGen::emit_epilogue() {
        // CFI：尾声里 rsp 会一路退回去，先「记住」函数体的展开规则，
        // 把每一步都描述出来，ret 之后再还原——后面的块（异常落地、
        // 另一条 return）继续按函数体状态展开。
        cfi(".cfi_remember_state");
        const int body_cfa = cfa_off_;
        // 先弹帧记录再退栈：记录本身在局部区底部，%rcx 此时是自由的。
        if (frame_rec_) {
            line("movq " + rec_mem(0) + ", %rcx");
            line("movq %rcx, pa_frame_top(%rip)");
        }
        if (frame_pad_ > 0) {
            emit_imm_op("addq", frame_pad_, "%rsp");
            cfa_off_ -= frame_pad_;
            cfi(std::format(".cfi_def_cfa_offset {}", cfa_off_));
        }
        for (int i = n_saved_regs_ - 1; i >= 0; --i) {
            const char* reg = gen_ ? "%r15" : kSlotRegs()[i];
            line(std::format("popq {}", reg));
            cfi(std::format(".cfi_restore {}", reg));
            cfa_off_ -= 8;
            cfi(std::format(".cfi_def_cfa_offset {}", cfa_off_));
        }
        line("ret");
        cfi(".cfi_restore_state");
        cfa_off_ = body_cfa;
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
static bool vreg_is_rc_mark(const IRFunction& f, VReg v) {
    for (const auto& b : f.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::CallRt &&
                static_cast<RtFn>(in.imm) == RtFn::RcMark && in.dst == v)
                return true;
    return false;
}

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
        std::vector<std::vector<char>> blk_use(static_cast<size_t>(nb));
        std::vector<std::vector<char>> blk_def(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            blk_use[static_cast<size_t>(b)].assign(nv, 0);
            blk_def[static_cast<size_t>(b)].assign(nv, 0);
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
                    if (!bdef[r]) buse[r] = 1;  // 块内先使用后定义 → live-in
                };
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
                if (in.dst != kNoReg && in.dst < nv) {
                    def_pos[in.dst] = pos;
                    def_block_[in.dst] = b;
                    ++def_count[in.dst];
                    bdef[in.dst] = 1;
                }
                ++pos;
            }
        }
        const int total_pos = pos;
        // 跨块判定：定义块 ≠ 使用块。
        for (size_t v = 0; v < nv; ++v) {
            if (def_pos[v] < 0) { cross_block_[v] = false; continue; }
            bool cross = false;
            for (int b = 0; b < nb && !cross; ++b) {
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    auto hit = [&](VReg r) {
                        if (r == static_cast<VReg>(v) && b != def_block_[v]) cross = true;
                    };
                    hit(in.a); hit(in.b); hit(in.c);
                    for (VReg r : in.args) hit(r);
                }
            }
            cross_block_[v] = cross;
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
        std::vector<std::vector<char>> live_in(static_cast<size_t>(nb));
        std::vector<std::vector<char>> live_out(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            live_in[static_cast<size_t>(b)].assign(nv, 0);
            live_out[static_cast<size_t>(b)].assign(nv, 0);
        }
        bool changed = true;
        int guard = 0;
        while (changed && guard++ < 2000) {
            changed = false;
            for (int b = nb - 1; b >= 0; --b) {
                // out 每次都从空集重算：继承上一轮的值会让 live_out 单调
                // 增长、永不收缩 —— 区间被越拉越长，最后所有 vreg 都变成
                // 全函数活跃，复用彻底失效（且没有后继的块会残留脏值）。
                std::vector<char> out(static_cast<size_t>(nv), 0);
                for (int s : succ[static_cast<size_t>(b)]) {
                    const auto& si = live_in[static_cast<size_t>(s)];
                    for (size_t v = 0; v < nv; ++v)
                        if (si[v]) out[v] = 1;
                }
                std::vector<char> in(static_cast<size_t>(nv), 0);
                const auto& buse = blk_use[static_cast<size_t>(b)];
                const auto& bdef = blk_def[static_cast<size_t>(b)];
                for (size_t v = 0; v < nv; ++v)
                    if (buse[v] || (out[v] && !bdef[v])) in[v] = 1;
                if (in != live_in[static_cast<size_t>(b)] ||
                    out != live_out[static_cast<size_t>(b)]) {
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
            std::vector<char> cur(live_out[static_cast<size_t>(b)]);
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            const int bstart = block_start[static_cast<size_t>(b)];
            const int blen = static_cast<int>(insts.size());
            // live-out 的值一直活到块末尾
            if (blen > 0)
                for (size_t v = 0; v < nv; ++v)
                    if (cur[v]) mark_live(static_cast<VReg>(v), bstart + blen - 1);
            for (int k = blen - 1; k >= 0; --k) {
                const Inst& in = insts[static_cast<size_t>(k)];
                const int p = bstart + k;
                if (in.dst != kNoReg && in.dst < nv) cur[in.dst] = 0;
                auto use = [&](VReg v) {
                    if (v == kNoReg || v >= nv) return;
                    cur[v] = 1;
                    mark_live(v, p);
                };
                use(in.a);
                use(in.b);
                use(in.c);
                for (VReg r : in.args) use(r);
            }
            for (size_t v = 0; v < nv; ++v)
                if (cur[v]) mark_live(static_cast<VReg>(v), bstart);
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
            if (vreg_is_rc_mark(*f_, static_cast<VReg>(v)) || def_count[v] > 1 ||
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
            line(std::format("movsd {}, {}", slot_reg_[slot], smem(slot)));
        }
        // 跨块 vreg 同样要写回：SysV 下 xmm8–15 是 caller-saved。
        // 它们没有块级掩码（活跃性按整个函数算），一律写回。
        //
        // 栈副本必须用 `vreg_off(v)` 直接算，不能用 `mem(v)` —— 后者在
        // vreg 已被提升到寄存器时返回寄存器本身，`movsd %xmm8, %xmm8`
        // 是非法操作数组合（实测 spectral_norm 汇编失败）。
        for (int v : fp_promoted_vregs_) {
            const std::string& rg = vreg_reg_[static_cast<size_t>(v)];
            if (rg.empty()) continue;
            line(std::format("movsd {}, {}(%rsp)", rg, vreg_off(static_cast<VReg>(v))));
        }
    }

void AsmGen::reload_fp_slots() {
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            line(std::format("movsd {}, {}", smem(slot), slot_reg_[slot]));
        }
        for (int v : fp_promoted_vregs_) {
            const std::string& rg = vreg_reg_[static_cast<size_t>(v)];
            if (rg.empty()) continue;
            line(std::format("movsd {}(%rsp), {}", vreg_off(static_cast<VReg>(v)), rg));
        }
    }

// 浮点槽在调用点是否还需要：块级 live-in/live-out（只跟踪提升过的槽）。
void AsmGen::build_fp_liveness() {
        const int nb = static_cast<int>(f_->blocks.size());
        const size_t kf = fp_promoted_slots_.size();
        fp_live_in_.assign(static_cast<size_t>(nb), 0);
        fp_live_out_.assign(static_cast<size_t>(nb), 0);
        fp_live_off_ = std::getenv("PYADAMAS_NO_FPLIVE") != nullptr;
        if (kf == 0 || nb == 0 || fp_live_off_) return;

        std::vector<uint8_t> use(static_cast<size_t>(nb), 0);
        std::vector<uint8_t> def(static_cast<size_t>(nb), 0);
        for (int b = 0; b < nb; ++b) {
            // 只把「块内先读后写」的读算作 use：先 store 再 load 是本块
            // 自己定义的值，不构成对入口值的活跃需求。
            uint8_t defined = 0;
            uint8_t used = 0;
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                if (in.idx < 0 || in.idx >= static_cast<int>(slot_fpidx_.size())) continue;
                int fi = slot_fpidx_[static_cast<size_t>(in.idx)];
                if (fi < 0) continue;
                uint8_t bit = static_cast<uint8_t>(1u << fi);
                if (in.op == IrOp::LoadSlot) {
                    if (!(defined & bit)) used |= bit;
                } else if (in.op == IrOp::StoreSlot) {
                    defined |= bit;
                }
            }
            use[static_cast<size_t>(b)] = used;
            def[static_cast<size_t>(b)] = defined;
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
                uint8_t out = 0;
                for (int s : succ[static_cast<size_t>(b)])
                    out |= fp_live_in_[static_cast<size_t>(s)];
                uint8_t in = static_cast<uint8_t>(
                    use[static_cast<size_t>(b)] |
                    (out & ~def[static_cast<size_t>(b)]));
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
static std::vector<int> block_loop_depth(const IRFunction& f);

/* ── live range splitting：按块分配变量槽的寄存器 ──────────────
 *
 * **问题**：`slot_reg_[s]` 是「整函数在寄存器」的布尔标记，代码生成器
 * 在任意指令处看到非空就用寄存器，不做区间判断。所以只活跃一部分的
 * 变量只能二选一：整函数占寄存器（区间外读到过期值）或整函数在栈上
 * （热段落也走内存）。
 *
 * **做法**：每个基本块独立决定「本块用哪个寄存器」，块入口把值从栈
 * 装进寄存器，块出口写回。栈始终是权威副本。
 *
 * **2026-11 改进（阶段 B）：用 SpillPlacement 取代逐块独立判断。**
 *
 * 原来每块只看自己：「本块 reads - writes > 0 就提升」。这没有全局视野 ——
 * 相邻块都提升时本块跟着提升能省两次边界搬运；相邻块都不提升时本块单独
 * 提升反而多付一次 spill + reload。实测 0.262 的失败就是这个（nbody 慢
 * 56ms、spectral_norm 慢 56ms）。
 *
 * 现在用 LLVM 的 Hopfield 网络（src/spill_placement.cpp）先算「每个块的
 * 入口/出口边界上该不该放寄存器」，代价模型是**块执行频率**，自动做到
 * 「热块放寄存器、冷块放栈」。
 *
 * **只处理整型槽**（关键设计决定）：
 *
 *   GPR（%rbx/%r12-%r15/%rbp）是 callee-saved，跨函数调用天然安全。
 *   而 xmm8-15 是 caller-saved —— 浮点槽放进 xmm 后，每个调用点都要
 *   spill/reload，还得维护 fp_promoted_slots_ / fp_spill_mask_。
 *
 *   上一轮集成失败就是这个：让 split 也处理浮点槽，但没重建
 *   fp_promoted_slots_，调用点不保存 -> 值被 call 破坏 ->
 *   matmul 输出从 43198080 变 0、fannkuch 直接崩溃。
 *
 *   浮点池有 8 个 xmm，默认的整函数提升已经够用（实测 9 个基准全提升），
 *   不需要 split 介入。所以这里只做整型，复杂度大幅降低。 */
void AsmGen::assign_slot_regs_split() {
        const int nb = static_cast<int>(f_->blocks.size());
        const int ns = static_cast<int>(f_->slot_ty.size());
        slot_loc_.assign(static_cast<size_t>(nb), {});
        slot_reload_in_.assign(static_cast<size_t>(nb), {});
        if (nb == 0 || ns == 0) return;

        for (int b = 0; b < nb; ++b)
            slot_loc_[static_cast<size_t>(b)].assign(static_cast<size_t>(ns), std::string());

        std::vector<int> depth = block_loop_depth(*f_);

        // 块执行频率：10^depth 近似。LLVM 用 profile 数据，这里用循环深度 ——
        // 内层循环体的执行次数就是比外层多一个量级，足够做相对比较。
        std::vector<int64_t> freq(static_cast<size_t>(nb), 1);
        for (int b = 0; b < nb; ++b) {
            int64_t f = 1;
            for (int i = 0; i < depth[static_cast<size_t>(b)] && i < 4; ++i) f *= 10;
            freq[static_cast<size_t>(b)] = f;
        }

        // 后继表（SpillPlacement 需要）。
        std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            auto add = [&](int t) {
                if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
            };
            if (insts.empty()) { add(b + 1); continue; }
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

        SpillPlacement placement(succ, freq);

        // 逐槽求解。**只做整型槽**（见上面的设计说明）。
        for (int s = 0; s < ns; ++s) {
            if (!slot_reg_[static_cast<size_t>(s)].empty()) continue;
            TypePtr t = f_->slot_ty[static_cast<size_t>(s)];
            if (!t || t->kind == TyKind::Unknown) continue;
            if (ir_is_heap(t)) continue;
            if (ir_is_float(t)) continue;   // 浮点槽交给默认提升
            // **参数槽不能参与 split。**
            //
            // 序言的参数落位是 `move_to(slot_op(p.slot), arg_reg)`，而
            // split 的块入口 reload 是 `movq smem(slot), reg`。两者顺序
            // 是「先 reload（块入口）后落位（序言）」，于是：
            //   - reload 从**未初始化的栈槽**读垃圾；
            //   - 落位再把它覆盖成参数值。
            // 看似最终正确，但 reload 的那条 mov 已经把 %rbx 写脏，且
            // 若块入口在落位之前（entry 块本身就是 bb0），参数值直接丢。
            //
            // 实测 fib 输出 2178309 -> 0、binary_trees SIGSEGV。
            // 参数槽本来就整函数活跃（函数体内一直可能用到），
            // 交给默认的整函数提升即可，不需要 split。
            bool is_param = false;
            for (const auto& p : f_->params)
                if (p.slot == s) { is_param = true; break; }
            if (is_param) continue;
            if (s < static_cast<int>(gc_scan_slot_.size()) && gc_scan_slot_[s]) continue;

            std::vector<BlockConstraint> constraints;
            for (int b = 0; b < nb; ++b) {
                int64_t reads = 0, writes = 0;
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    if (in.idx != s) continue;
                    if (in.op == IrOp::LoadSlot) ++reads;
                    else if (in.op == IrOp::StoreSlot) ++writes;
                }
                if (reads == 0 && writes == 0) continue;
                BlockConstraint c;
                c.block = b;
                if (reads > writes) {
                    c.entry = BlockPref::PrefReg;
                    c.exit = BlockPref::PrefReg;
                } else if (writes > reads) {
                    c.entry = BlockPref::PrefSpill;
                    c.exit = BlockPref::PrefReg;
                } else {
                    c.entry = BlockPref::PrefReg;
                    c.exit = BlockPref::PrefReg;
                }
                constraints.push_back(c);
            }
            if (constraints.empty()) continue;
            if (!placement.solve(constraints)) continue;

            for (int b = 0; b < nb; ++b) {
                if (!placement.useRegAtEntry(b) && !placement.useRegAtExit(b)) continue;
                bool used = false;
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    if (in.idx != s) continue;
                    if (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot) {
                        used = true;
                        break;
                    }
                }
                if (!used) continue;
                slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)] = kSlotRegs()[0];
            }
        }

        // **寄存器分配：每个槽全局固定一个寄存器。**
        //
        // 曾经的错误做法是「每个块内按使用次数排序，依次分配不同寄存器」。
        // 那样同一个槽在不同块会拿到不同寄存器：
        //
        //     bb1: s7=%r12      bb3: s7=%rbx
        //
        // 而块出口 spill 用的是**本块**的寄存器（%r12），块入口 reload 也
        // 只装本块的。如果 bb1 直接跳 bb3、中间没有 spill 点，s7 的值就
        // 从 %r12 丢到了 %rbx（未初始化）—— 实测 nqueens 抛 IndexError、
        // matmul 输出垃圾值。
        //
        // 正确做法：一个槽在所有块里必须是同一个物理寄存器。这样跨块
        // 转移时即使中间没有 spill，值也一直在那个寄存器里（callee-saved，
        // 跨调用也安全）。spill/reload 只在「进出寄存器集合」的边界需要。
        {
            // 按「全函数使用次数」排序，决定谁先拿到寄存器。
            struct Use { int s; int64_t cnt; };
            std::vector<Use> cands;
            for (int s = 0; s < ns; ++s) {
                int64_t cnt = 0;
                bool any = false;
                for (int b = 0; b < nb; ++b) {
                    if (slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)].empty())
                        continue;
                    any = true;
                    for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts)
                        if (in.idx == s && (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot))
                            ++cnt;
                }
                if (any) cands.push_back(Use{s, cnt});
            }
            std::ranges::sort(cands, [](const Use& a, const Use& c) {
                return a.cnt != c.cnt ? a.cnt > c.cnt : a.s < c.s;
            });
            // 前 kMaxSlotRegs 个拿到固定寄存器，其余全部退回栈。
            std::vector<int> fixed(static_cast<size_t>(ns), -1);
            // **从 n_saved_regs_ 开始，避开参数槽已占用的寄存器。**
            // 参数槽先跑了线性扫描，用掉 kSlotRegs()[0..n_saved_regs_-1]。
            // split 若从 0 开始会抢同一批寄存器 —— 实测 fib 输出 -960、
            // mandelbrot 输出 0、nqueens 被 Killed。
            const int base = n_saved_regs_;
            // 实验开关：限制 split 能占用的寄存器数（0 = 完全不用 split 提升，
            // 只保留机制本身的开销），用来分离「机制成本」与「分配质量」。
            int split_cap = kMaxSlotRegs();
            if (const char* e = std::getenv("PYADAMAS_SPLIT_MAXREG")) split_cap = std::atoi(e);
            for (size_t i = 0; i < cands.size(); ++i) {
                const int idx = base + static_cast<int>(i);
                if (idx < kMaxSlotRegs() && static_cast<int>(i) < split_cap)
                    fixed[static_cast<size_t>(cands[i].s)] = idx;
            }
            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s) {
                    if (slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)].empty())
                        continue;
                    const int fi = fixed[static_cast<size_t>(s)];
                    slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)] =
                        fi >= 0 ? kSlotRegs()[fi] : std::string();
                }
        }
        // **跨块一致性（在全局固定映射之后再做一次）。**
        //
        // 只要槽 s 被提升（拿到固定寄存器），**所有使用它的块**都必须
        // 标记成同一位置。否则：
        //
        //     bb4: store.slot s5 -> %rbx（双写栈）
        //     bb8: load.slot s5  -> smem(s5)（栈）
        //
        // bb8 读栈是对的（双写保证了栈最新），但如果 bb8 的**前驱**没有
        // 写回栈，或者中间有别的块改了 s5 而没双写，就会读到旧值。
        //
        // 更重要的是：块入口 reload 只对 slot_loc_ 非空的槽做。bb8 没有
        // slot_loc_[bb8][s5]，于是它不 reload —— 但它内部的 slot_op(s5)
        // 又因为「本块无位置」回落到 slot_reg_ 或 smem，与 bb4 的 %rbx
        // 分裂成两个副本。
        //
        // 实测 nqueens 抛 IndexError。
        {
            std::vector<std::string> fixed(static_cast<size_t>(ns));
            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s) {
                    const std::string& rg =
                        slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)];
                    if (!rg.empty()) fixed[static_cast<size_t>(s)] = rg;
                }
            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s) {
                    if (fixed[static_cast<size_t>(s)].empty()) continue;
                    if (!slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)].empty())
                        continue;
                    bool uses = false;
                    for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                        if (in.idx != s) continue;
                        if (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot) {
                            uses = true;
                            break;
                        }
                    }
                    if (uses)
                        slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)] =
                            fixed[static_cast<size_t>(s)];
                }
        }
        // **精确 reload / spill 决策（取代「块入口无条件 reload + store 双写」）。**
        //
        // 旧方案：store.slot 双写栈 + 每个块入口无条件 reload。正确但昂贵 ——
        // 双写让每次 store 多一次内存写，还打断 store-to-load forwarding。
        // 实测 9 个基准 TOTAL +28.1%（matmul +42.9%、spectral_norm +37.9%）。
        //
        // 新方案：**只在寄存器集合的边界搬运**。
        //
        // 记 R(s) = { 块 b : slot_loc_[b][s] 非空 }（s 在 b 里住寄存器）。
        // 关键不变量：b ∈ R(s) 时，那个寄存器在 b 内任何位置都是权威值
        // （要么入口 reload 过，要么从全部在 R(s) 里的前驱带过来）。
        //
        //   reload(b,s)     ：b ∈ R(s) 且「b 无前驱且 b 用 s」或「∃前驱 p ∉ R(s)」。
        //                     全部前驱都在 R(s) 时寄存器直接带过来，不需要重装。
        //   need_stack(b,s) ：栈副本必须在 b 入口有效。
        //                     = b ∈ R(s) ? reload(b,s)
        //                                : (b 用 s || ∃后继 q: need_stack(q,s))
        //                     b ∉ R(s) 时不碰寄存器，栈没被写就原样传递，
        //                     所以「后继要栈」这个需求要穿过它往前传。
        //   spill(b,s)      ：b ∈ R(s) 且 ∃后继 q: need_stack(q,s)。
        //                     有后继要从栈读，才在 b 出口写回。
        //
        // 效果：循环体内相邻块之间零搬运；只有真正跨出寄存器集合的边界
        // 才有一条 mov。store.slot 不再双写（栈在块出口统一写回）。
        slot_spill_out_.assign(static_cast<size_t>(nb), {});
        {
            std::vector<std::vector<int>> preds(static_cast<size_t>(nb));
            for (int b = 0; b < nb; ++b)
                for (int q : succ[static_cast<size_t>(b)])
                    preds[static_cast<size_t>(q)].push_back(b);

            std::vector<std::vector<char>> uses(static_cast<size_t>(nb),
                                                std::vector<char>(static_cast<size_t>(ns), 0));
            for (int b = 0; b < nb; ++b)
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts)
                    if (in.idx >= 0 && in.idx < ns &&
                        (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot))
                        uses[static_cast<size_t>(b)][static_cast<size_t>(in.idx)] = 1;

            std::vector<std::vector<char>> in_r(static_cast<size_t>(nb),
                                               std::vector<char>(static_cast<size_t>(ns), 0));
            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s)
                    if (!slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)].empty())
                        in_r[static_cast<size_t>(b)][static_cast<size_t>(s)] = 1;

            std::vector<std::vector<char>> reload(static_cast<size_t>(nb),
                                                  std::vector<char>(static_cast<size_t>(ns), 0));
            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s) {
                    if (!in_r[static_cast<size_t>(b)][static_cast<size_t>(s)]) continue;
                    bool need;
                    if (preds[static_cast<size_t>(b)].empty()) {
                        need = uses[static_cast<size_t>(b)][static_cast<size_t>(s)] != 0;
                    } else {
                        need = false;
                        for (int p : preds[static_cast<size_t>(b)])
                            if (!in_r[static_cast<size_t>(p)][static_cast<size_t>(s)]) {
                                need = true;
                                break;
                            }
                    }
                    if (need) {
                        reload[static_cast<size_t>(b)][static_cast<size_t>(s)] = 1;
                        slot_reload_in_[static_cast<size_t>(b)].push_back(s);
                    }
                }

            std::vector<std::vector<char>> need(static_cast<size_t>(nb),
                                               std::vector<char>(static_cast<size_t>(ns), 0));
            bool changed = true;
            while (changed) {
                changed = false;
                for (int b = nb - 1; b >= 0; --b)
                    for (int s = 0; s < ns; ++s) {
                        if (need[static_cast<size_t>(b)][static_cast<size_t>(s)]) continue;
                        bool v;
                        if (in_r[static_cast<size_t>(b)][static_cast<size_t>(s)]) {
                            v = reload[static_cast<size_t>(b)][static_cast<size_t>(s)] != 0;
                        } else {
                            v = uses[static_cast<size_t>(b)][static_cast<size_t>(s)] != 0;
                            if (!v)
                                for (int q : succ[static_cast<size_t>(b)])
                                    if (need[static_cast<size_t>(q)][static_cast<size_t>(s)]) {
                                        v = true;
                                        break;
                                    }
                        }
                        if (v) {
                            need[static_cast<size_t>(b)][static_cast<size_t>(s)] = 1;
                            changed = true;
                        }
                    }
            }

            for (int b = 0; b < nb; ++b)
                for (int s = 0; s < ns; ++s) {
                    if (!in_r[static_cast<size_t>(b)][static_cast<size_t>(s)]) continue;
                    bool v = false;
                    for (int q : succ[static_cast<size_t>(b)])
                        if (need[static_cast<size_t>(q)][static_cast<size_t>(s)]) {
                            v = true;
                            break;
                        }
                    if (v) slot_spill_out_[static_cast<size_t>(b)].push_back(s);
                }
        }

        // **n_saved_regs_ 必须覆盖 split 实际用到的寄存器。**
        //
        // 序言按 n_saved_regs_ 压栈 callee-saved 寄存器，尾声按它弹栈。
        // split 用 kSlotRegs()[0..k-1]，所以 n_saved_regs_ 要设成
        // 「用到的最大序号 + 1」，否则用到 %r15/%rbp 却没压栈 ——
        // 调用者的值被破坏（实测 fannkuch 抛 IndexError）。
        {
            int used = 0;
            for (int r = 0; r < kMaxSlotRegs(); ++r) {
                bool any = false;
                for (int b = 0; b < nb && !any; ++b)
                    for (int s = 0; s < ns; ++s)
                        if (slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s)] ==
                            kSlotRegs()[r]) {
                            any = true;
                            break;
                        }
                if (any) used = r + 1;
            }
            // **必须直接赋值，不能取 max。**
            //
            // split 独占整型分配后，slot_reg_ 里的整型槽是空的，
            // n_saved_regs_ 在进入 split 前可能是线性扫描留下的残值。
            // 取 max 会让「残值小于实际用量」的情况漏压栈 ——
            // 实测 nqueens 用到 %rbp（第 6 个）却只压了 3 个，
            // 循环上界被调用破坏，抛 IndexError。
            // **取 max，不能直接赋值。**
            //            // 参数槽可能已经用掉 kSlotRegs()[0..]，这里如果直接赋值成
            // split 自己用到的数量，会把参数槽的压栈需求冲掉 ——
            // 实测 fib 序言没有 pushq %rbx，递归后参数被破坏，输出 -960。
            if (used > n_saved_regs_) n_saved_regs_ = used;
            if (std::getenv("PYADAMAS_SPLIT_DBG")) {
                std::fprintf(stderr, "[split] %s n_saved=%d\n", f_->name.c_str(), n_saved_regs_);
                for (int b = 0; b < nb; ++b) {
                    std::fprintf(stderr, "[split]   bb%d:", b);
                    for (int s2 = 0; s2 < ns; ++s2)
                        if (!slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s2)].empty())
                            std::fprintf(stderr, " s%d=%s", s2,
                                         slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s2)].c_str());
                    std::fprintf(stderr, "\n");
                }
            }
        }
}

// ── 图着色分配器（PYADAMAS_RA_GRAPH）──────────────────────────────
//
// **为什么「在 [first,last] 区间上做图着色」是零收益，而这里不是**：
// 区间图是完美图，按左端点贪心着色即最优 —— 对同一组区间，Chaitin 与
// 现有线性扫描给出相同的寄存器数（实测 914 个函数逐一核对，0 例外）。
// 所以照抄「实现 Chaitin-Briggs」这个描述去做是白干。
//
// 真正的机会在**区间本身太粗**：线性扫描的区间取 [首次访问, 末次访问]，
// 而块级 live-in/live-out 会把跨块的槽整块算进区间，于是一个几十块的
// 大函数里十几个互不重叠的槽被判成「全部同时活跃」。实测
// （PYADAMAS_RA_LIVENESS_STATS）：
//     matmul.run_2          int 精确 5 / 区间 13
//     spectral_norm.main    int 精确 5 / 区间 12
//     nbody.advance_1       int 精确 4 / 区间 6，fp 精确 4 / 区间 5
// 池只有 6 GPR / 8 xmm：区间 13 必然大量溢出，精确 5 却全放得下。
// 这就是 nbody 比 Go 慢的直接原因（advance 帧 616 字节）。
//
// 所以这里用**逐指令精确活跃集**建冲突图，再做 Chaitin-Briggs 着色。
// 精确活跃集把「首末被夹住但中间有洞」的槽判为不冲突，边远少于区间重叠。
//
// **为什么着色结果仍能写进「整函数一个寄存器」的 slot_reg_**（关键正确性）：
// 若槽 A 在位置 p 写、q 读（p<q），则 A 在 [p,q] 全程活跃；任何在同一
// 寄存器上写的槽 B 必然在某个 r in (p,q) 活跃，于是与 A 冲突、拿到不同
// 寄存器。所以「整函数固定寄存器 + 只提升着色的槽」不会读到被覆盖的值，
// **不需要在洞边界插 spill/reload**（那正是 0.262 live range splitting
// 失败的地方：按块双写/回写，nbody 反而慢 56ms）。
//
// 整型与浮点用两套不相交的池，冲突图也按池切开 —— 一个 int 槽和一个
// fp 槽永远不会抢同一个物理寄存器，它们之间的边必须忽略，否则度数虚高、
// 着色会误判为溢出。
void AsmGen::assign_slot_regs_graph() {
        const int nb = static_cast<int>(f_->blocks.size());
        const int nc = static_cast<int>(slot_reg_cand_.size());
        if (nb == 0 || nc == 0) return;
        // 病态函数（几百个槽）直接交回线性扫描：O(nc^2) 的边集合会爆。
        if (nc > 512) return;

        const int words = (nc + 63) / 64;

        // 1) 冲突图。边 = 两个槽在**同一程序点**同时活跃。
        std::vector<std::vector<uint64_t>> adj(
            static_cast<size_t>(nc), std::vector<uint64_t>(static_cast<size_t>(words), 0));
        auto add_edge = [&](int i, int j) {
            if (i == j) return;
            adj[static_cast<size_t>(i)][static_cast<size_t>(j >> 6)] |= 1ULL << (j & 63);
            adj[static_cast<size_t>(j)][static_cast<size_t>(i >> 6)] |= 1ULL << (i & 63);
        };
        std::vector<int> live_list;
        auto clique = [&](const std::vector<uint64_t>& live) {
            live_list.clear();
            for (int w = 0; w < words; ++w) {
                uint64_t x = live[static_cast<size_t>(w)];
                while (x) {
                    int b = __builtin_ctzll(x);
                    x &= x - 1;
                    int i = w * 64 + b;
                    if (i < nc) live_list.push_back(i);
                }
            }
            for (size_t a = 0; a < live_list.size(); ++a)
                for (size_t c = a + 1; c < live_list.size(); ++c)
                    add_edge(live_list[a], live_list[c]);
        };

        // 2) 参数槽在序言里同时落位，必须两两建边。
        //
        // 序言把每个形参写进它的槽位置（slot_op）。若把多个参数槽着成
        // 同一个寄存器，序言里的 movq %r8, %rbx / movq %r9, %rbx 会互相
        // 覆盖 —— 实测 textwrap 的 wrap() 有 3 个参数全着色成 %rbx，
        // wrap 输出从 3 段变 4 段。
        //
        // 活跃性分析看不到这一点：参数槽若在块内先 store 再 load（或根本
        // 只被 store），use/def 分析里它就不是 live-in，度数算成 0。但它
        // 在序言里是同时存在的。
        {
            const int np = static_cast<int>(f_->params.size());
            for (int i = 0; i < np; ++i) {
                int ci = f_->params[static_cast<size_t>(i)].slot;
                if (ci < 0 || ci >= static_cast<int>(slot_reg_cand_idx_.size())) continue;
                int gi = slot_reg_cand_idx_[static_cast<size_t>(ci)];
                if (gi < 0) continue;
                for (int j = i + 1; j < np; ++j) {
                    int cj = f_->params[static_cast<size_t>(j)].slot;
                    if (cj < 0 || cj >= static_cast<int>(slot_reg_cand_idx_.size())) continue;
                    int gj = slot_reg_cand_idx_[static_cast<size_t>(cj)];
                    if (gj < 0) continue;
                    add_edge(gi, gj);
                }
            }
        }

        // 3) **只写不读的槽必须排除出候选，否则会冲掉别人的寄存器。**
        //
        // 教科书活跃性里 store 是 def，之后该变量就不活跃了。但这个后端的
        // 分配粒度是「整函数一个寄存器」（slot_reg_[s]），代码生成器在
        // store 处真的会写寄存器。若一个槽全函数只被 store、从不被 load，
        // 活跃性分析判它「全程不活跃」，于是它和真正活跃的槽共用寄存器 ——
        // 那条 store 的 movq 直接把别人的值冲掉。
        //
        // 实测 42_match_corners 的 cap()：match 模式绑定的 s4/s5/s6 只写不读，
        // 与返回值槽 s1 共用 %rbx；bb3 里
        //     store.slot s4, %10    ; movq %rax, %rbx
        //     store.slot s1, %10    ; movq %rax, %rbx   (s1 正确)
        //     store.slot s5, %12    ; movq %rdx, %rbx   (把 s1 冲掉了！)
        // 输出 cap([1,2]) 从 1 变成 2。
        //
        // 修法不是「store 也置位」（那会让所有跨块值都互相冲突，实测
        // spectral_norm 退化 31%），而是把这类死槽直接踢出候选：
        // 它们本来就不需要寄存器，写进栈槽即可，零成本且不影响别人。
        std::vector<char> dead_slot(static_cast<size_t>(nc), 0);
        {
            std::vector<char> ever_loaded(static_cast<size_t>(nc), 0);
            for (int b = 0; b < nb; ++b)
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    if (in.op != IrOp::LoadSlot) continue;
                    if (in.idx < 0 || in.idx >= static_cast<int>(slot_reg_cand_idx_.size()))
                        continue;
                    int ci = slot_reg_cand_idx_[static_cast<size_t>(in.idx)];
                    if (ci >= 0) ever_loaded[static_cast<size_t>(ci)] = 1;
                }
            for (int ci = 0; ci < nc; ++ci) {
                if (ever_loaded[static_cast<size_t>(ci)]) continue;
                dead_slot[static_cast<size_t>(ci)] = 1;
                for (int cj = 0; cj < nc; ++cj) {
                    if (cj == ci) continue;
                    adj[static_cast<size_t>(cj)][static_cast<size_t>(ci >> 6)] &=
                        ~(1ULL << (ci & 63));
                }
            }
        }

        // 4) 反向扫描每个块：live 初始化为块出口活跃集，逐条指令维护。
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            std::vector<uint64_t> live = slot_live_out_[static_cast<size_t>(b)];
            clique(live);
            for (int i = static_cast<int>(insts.size()) - 1; i >= 0; --i) {
                const auto& in = insts[static_cast<size_t>(i)];
                int ci = -1;
                if (in.idx >= 0 && in.idx < static_cast<int>(slot_reg_cand_idx_.size()))
                    ci = slot_reg_cand_idx_[static_cast<size_t>(in.idx)];
                if (ci >= 0) {
                    if (in.op == IrOp::StoreSlot)
                        live[static_cast<size_t>(ci >> 6)] &= ~(1ULL << (ci & 63));
                    else if (in.op == IrOp::LoadSlot)
                        live[static_cast<size_t>(ci >> 6)] |= 1ULL << (ci & 63);
                }
                clique(live);
            }
        }

        // 5) spill cost：使用次数 x 循环深度加权（每深一层 x10，最多 4 层）。
        std::vector<int64_t> cost(static_cast<size_t>(nc), 0);
        {
            std::vector<int> depth = block_loop_depth(*f_);
            for (int b = 0; b < nb; ++b) {
                int64_t w = 1;
                int d = b < static_cast<int>(depth.size()) ? depth[static_cast<size_t>(b)] : 0;
                for (int i = 0; i < d && i < 4; ++i) w *= 10;
                for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                    if (in.idx < 0 || in.idx >= static_cast<int>(slot_reg_cand_idx_.size()))
                        continue;
                    int ci = slot_reg_cand_idx_[static_cast<size_t>(in.idx)];
                    if (ci < 0) continue;
                    if (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot)
                        cost[static_cast<size_t>(ci)] += w;
                }
            }
        }

        // 6) 按池着色。返回「用了几个寄存器」（颜色已压实到 0..k-1）。
        auto color_pool = [&](bool isfp, const char* const* regs, int nreg) -> int {
            std::vector<int> nodes;
            for (int ci = 0; ci < nc; ++ci) {
                if (slot_cand_is_fp_[static_cast<size_t>(ci)] != (isfp ? 1 : 0)) continue;
                // 只写不读的槽不进图：它们不需要寄存器（留在栈上即可），
                // 参与着色只会白占一个颜色并冲掉别人。
                if (dead_slot[static_cast<size_t>(ci)]) continue;
                nodes.push_back(ci);
            }
            const int n = static_cast<int>(nodes.size());
            if (n == 0) return 0;

            // 池内局部邻接：只连同池的边。
            std::vector<int> local(static_cast<size_t>(nc), -1);
            for (int i = 0; i < n; ++i)
                local[static_cast<size_t>(nodes[static_cast<size_t>(i)])] = i;
            std::vector<std::vector<int>> nbr(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                int gi = nodes[static_cast<size_t>(i)];
                for (int w = 0; w < words; ++w) {
                    uint64_t x = adj[static_cast<size_t>(gi)][static_cast<size_t>(w)];
                    while (x) {
                        int b = __builtin_ctzll(x);
                        x &= x - 1;
                        int gj = w * 64 + b;
                        if (gj >= nc) continue;
                        int lj = local[static_cast<size_t>(gj)];
                        if (lj >= 0) nbr[static_cast<size_t>(i)].push_back(lj);
                    }
                }
            }

            // 简化：反复压栈度数 < K 的节点；卡住时按 Briggs 乐观压栈
            // cost 最低的那个（弹出时若真没颜色再判溢出）。
            std::vector<int> deg(static_cast<size_t>(n), 0);
            for (int i = 0; i < n; ++i)
                deg[static_cast<size_t>(i)] =
                    static_cast<int>(nbr[static_cast<size_t>(i)].size());
            std::vector<char> removed(static_cast<size_t>(n), 0);
            std::vector<int> stk;
            int remaining = n;
            while (remaining > 0) {
                int pick = -1;
                for (int i = 0; i < n; ++i) {
                    if (removed[static_cast<size_t>(i)]) continue;
                    if (deg[static_cast<size_t>(i)] < nreg) { pick = i; break; }
                }
                if (pick < 0) {
                    for (int i = 0; i < n; ++i) {
                        if (removed[static_cast<size_t>(i)]) continue;
                        if (pick < 0) { pick = i; continue; }
                        int64_t ca = cost[static_cast<size_t>(nodes[static_cast<size_t>(i)])];
                        int64_t cb = cost[static_cast<size_t>(nodes[static_cast<size_t>(pick)])];
                        if (ca < cb ||
                            (ca == cb && nodes[static_cast<size_t>(i)] <
                                             nodes[static_cast<size_t>(pick)]))
                            pick = i;
                    }
                }
                removed[static_cast<size_t>(pick)] = 1;
                stk.push_back(pick);
                --remaining;
                for (int j : nbr[static_cast<size_t>(pick)])
                    if (!removed[static_cast<size_t>(j)]) --deg[static_cast<size_t>(j)];
            }

            // 着色：弹出时取最小的、未被已着色邻居占用的颜色。
            std::vector<int> color(static_cast<size_t>(n), -1);
            while (!stk.empty()) {
                int i = stk.back();
                stk.pop_back();
                std::vector<char> used(static_cast<size_t>(nreg), 0);
                for (int j : nbr[static_cast<size_t>(i)]) {
                    int cj = color[static_cast<size_t>(j)];
                    if (cj >= 0 && cj < nreg) used[static_cast<size_t>(cj)] = 1;
                }
                int c = -1;
                for (int r = 0; r < nreg; ++r)
                    if (!used[static_cast<size_t>(r)]) { c = r; break; }
                color[static_cast<size_t>(i)] = c;  // -1 = 真溢出，留栈上
            }

            // 颜色压实：把用到的颜色重映射到 0..k-1。
            //
            // **必须压实**：assign_global_vreg_regs 用
            // fp_promoted_slots_.size() 当浮点基址（假设槽占了前 k 个
            // xmm），整型用 n_saved_regs_ 当起点。颜色不连续时这两处
            // 都会算出错的基址，跨块 vreg 会抢到已被槽占用的寄存器。
            std::vector<int> remap(static_cast<size_t>(nreg), -1);
            int nextc = 0;
            for (int r = 0; r < nreg; ++r) {
                bool used_r = false;
                for (int i = 0; i < n; ++i)
                    if (color[static_cast<size_t>(i)] == r) { used_r = true; break; }
                if (used_r) remap[static_cast<size_t>(r)] = nextc++;
            }
            for (int i = 0; i < n; ++i) {
                int c = color[static_cast<size_t>(i)];
                if (c < 0) continue;
                int rc = remap[static_cast<size_t>(c)];
                if (rc < 0) continue;
                slot_reg_[static_cast<size_t>(
                    slot_reg_cand_[static_cast<size_t>(nodes[static_cast<size_t>(i)])])] =
                    regs[rc];
            }
            if (std::getenv("PYADAMAS_RA_DEBUG")) {
                int spilled = 0;
                for (int i = 0; i < n; ++i)
                    if (color[static_cast<size_t>(i)] < 0) ++spilled;
                std::fprintf(stderr, "[graph] %s: %s %d/%d colored, %d spilled, %d regs\n",
                             f_->name.c_str(), isfp ? "fp" : "int", n - spilled, n, spilled,
                             nextc);
            }
            return nextc;
        };

        n_saved_regs_ = color_pool(false, kSlotRegs(), kMaxSlotRegs());
        (void)color_pool(true, kFpSlotRegs(), kMaxFpSlotRegs());

        // 重建 fp_promoted_slots_ / slot_fpidx_：调用点 spill/reload 依赖它们，
        // 必须与实际分配一致（顺序无所谓，只要 slot_fpidx_ 是逆映射）。
        fp_promoted_slots_.clear();
        for (int s = 0; s < static_cast<int>(slot_fpidx_.size()); ++s)
            slot_fpidx_[static_cast<size_t>(s)] = -1;
        for (int s = 0; s < static_cast<int>(slot_reg_.size()); ++s) {
            if (slot_reg_[static_cast<size_t>(s)].empty()) continue;
            TypePtr t = f_->slot_ty[static_cast<size_t>(s)];
            if (!t || !ir_is_float(t)) continue;
            slot_fpidx_[static_cast<size_t>(s)] = static_cast<int>(fp_promoted_slots_.size());
            fp_promoted_slots_.push_back(s);
        }
    }
void AsmGen::assign_slot_regs_linear() {
        // 只把**整型**候选交给通用扫描：浮点由调用方用独立的池扫（R4）。
        std::vector<int> int_cands;
        for (size_t ci = 0; ci < slot_reg_cand_.size(); ++ci)
            if (!slot_cand_is_fp_[ci]) int_cands.push_back(slot_reg_cand_[ci]);
        if (int_cands.empty()) return;
        std::vector<std::string> out(f_->slot_ty.size());
        linear_scan_pool(int_cands, kSlotRegs(), kMaxSlotRegs(), out);
        int n_used = 0;
        for (size_t i2 = 0; i2 < out.size(); ++i2) {
            if (out[i2].empty()) continue;
            slot_reg_[i2] = out[i2];
        }
        for (int r = 0; r < kMaxSlotRegs(); ++r) {
            bool any = false;
            for (const auto& sr : slot_reg_)
                if (sr == kSlotRegs()[r]) {
                    any = true;
                    break;
                }
            if (any) n_used = r + 1;
        }
        n_saved_regs_ = n_used;
        // 只统计**整型**槽。浮点槽在调用方（assign_slot_regs）随后才分配，
        // 此刻 slot_reg_ 里的浮点项还是空的。
        //
        // 这个统计以前写的是「slot_reg_ 里非空的个数」，把浮点算进去却
        // 又没分配，于是长期输出「7/14 promoted」这种误导数字 —— 实际
        // 浮点是 6/6 全提升，整型才是 7/14。排查时被它带偏过好几轮。
        if (std::getenv("PYADAMAS_RA_DEBUG")) {
            int int_assigned = 0, int_total = 0;
            for (size_t ci = 0; ci < slot_reg_cand_.size(); ++ci) {
                if (slot_cand_is_fp_[ci]) continue;
                ++int_total;
                if (!slot_reg_[static_cast<size_t>(slot_reg_cand_[ci])].empty())
                    ++int_assigned;
            }
            std::fprintf(stderr, "[ra] %s: int %d/%d promoted, %d regs\n",
                         f_->name.c_str(), int_assigned, int_total, n_used);
        }
    }

void AsmGen::assign_slot_regs() {
        report_interval_precision();
        int ns = static_cast<int>(f_->slot_ty.size());
        slot_reg_.assign(ns, std::string());
        // R1：先把整型槽的块级活跃性算出来（R2 的线性扫描要用）。
        build_slot_liveness();
        slot_fpidx_.assign(ns, -1);
        fp_promoted_slots_.clear();
        n_saved_regs_ = 0;
        if (ns == 0) return;
        // 含 setjmp 的函数：longjmp 会把 callee-saved 寄存器恢复到 setjmp
        // 时刻，寄存器里的变量会悄悄回退。但没必要整函数降级 —— 只有
        // 「在 handler 保护区内被写过、且出了保护区还要读」的槽才有这个
        // 风险。其余（保护区外写的、只在保护区内用的、循环计数器等）
        // 完全可以继续驻留寄存器。
        //
        // 判定方式：把每个 HandlerPush..HandlerLeave 覆盖的块标成
        // in_handler[b]；再对每个槽扫一遍，记录它在保护区内的最后一次
        // 写所在的块，以及该块之后是否还有读。有就是 hazard。
        std::vector<bool> handler_hazard(static_cast<size_t>(ns), false);
        if (f_->has_handlers) {
            const int nb = static_cast<int>(f_->blocks.size());
            std::vector<bool> in_handler(static_cast<size_t>(nb), false);
            // 用栈跟踪 handler 嵌套：按块顺序线性扫，遇到 Push 打开、
            // Leave/Discard 关闭。块内顺序足够（lower 保证 Push/Leave
            // 与块边界一致：异常体自成块）。
            std::vector<int> open;  // 嵌套深度
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
            // 每个槽：保护区内的写 + 任意读 => hazard。
            // 保守但可靠：不看块序（块序与 CFG 可达性无关，循环回边会让
            // 「读」出现在「写」之前的下标上），只要写过就可能被 longjmp
            // 回退，只要读过就可能读到回退值。
            for (int s = 0; s < ns; ++s) {
                bool wrote_in_handler = false;
                bool any_read = false;
                for (int b = 0; b < nb; ++b) {
                    for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                        if (in.idx != s) continue;
                        if (in.op == IrOp::StoreSlot && in_handler[static_cast<size_t>(b)])
                            wrote_in_handler = true;
                        else if (in.op == IrOp::LoadSlot)
                            any_read = true;
                    }
                }
                handler_hazard[static_cast<size_t>(s)] = wrote_in_handler && any_read;
            }
        }
        if (f_->is_generator) return;

        // 被取过地址的槽必须留在内存里：寄存器里的副本不会跟着地址写更新。
        std::vector<bool> addr_taken(ns, false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts)
                if (in.op == IrOp::SlotAddr && in.idx >= 0 && in.idx < ns) addr_taken[in.idx] = true;

        std::vector<int> score(ns, 0);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if ((in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot) && in.idx >= 0 &&
                    in.idx < ns && !addr_taken[in.idx])
                    ++score[in.idx];
            }

        // 槽类型可能被 unknown 吞掉（类型合并不上时），但实际流动的值
        // 仍是浮点。这类槽不能进 GPR——结果融合会把 movsd 直接写到
        // 整数寄存器上，汇编器都过不了。槽类型确定是 float 的才进 xmm，
        // 类型模糊但带浮点值的槽留在内存里，两种指令都能落。
        std::vector<bool> float_valued(ns, false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::StoreSlot && in.idx >= 0 && in.idx < ns &&
                    in.a != kNoReg && in.a < f_->vreg_ty.size() &&
                    ir_is_float(f_->vreg_ty[in.a]))
                    float_valued[in.idx] = true;
                if (in.op == IrOp::LoadSlot && in.idx >= 0 && in.idx < ns &&
                    in.dst != kNoReg && in.dst < f_->vreg_ty.size() &&
                    ir_is_float(f_->vreg_ty[in.dst]))
                    float_valued[in.idx] = true;
            }

        std::vector<std::pair<int, int>> hot_int, hot_fp;
        for (int i = 0; i < ns; ++i) {
            if (score[i] <= 0) continue;
            // 持有引用的堆槽必须留在内存：帧记录存的是栈槽地址，异常
            // 展开时直接从地址读值；提升到寄存器会让栈槽里留下旧值。
            if (ir_is_heap(f_->slot_ty[static_cast<size_t>(i)])) continue;
            // handler 保护区内的写 + 之后的读：longjmp 会把寄存器回退，
            // 这类槽必须留在内存。其余槽不受 setjmp 影响。
            if (handler_hazard[static_cast<size_t>(i)]) continue;
            if (ir_is_float(f_->slot_ty[i])) {
                if (opt_ >= 1) hot_fp.push_back({score[i], i});
            } else if (!float_valued[i]) {
                hot_int.push_back({score[i], i});
            }
        }
        auto hotter = [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        };
        std::ranges::sort(hot_int, hotter);
        std::ranges::sort(hot_fp, hotter);

        // R2：线性扫描分配。默认启用；PYADAMAS_NO_RA=1 回退到旧的按频次分配。
        //
        // 两者只在整型槽上不同（浮点池仍走下面的按频次逻辑）。线性扫描的
        // 优势是「不同活跃区间复用同一寄存器」，变量数超过寄存器数时覆盖率更高。
        // 用驱动传入的开关（参与模块缓存失效），不在这里重复读环境变量。
        //
        // 原来这里直接 getenv：切换开关后模块缓存键不变，仍命中旧产物 ——
        // 实测连续几轮 A/B 测试拿到完全相同的二进制。现在由 Program::rc_split
        // 统一管理，并写进 manifest 的 rcsplit 行。
        // 图着色分配器（PYADAMAS_RA_GRAPH）：默认关闭，由驱动传入开关。
        if (ra_graph_) {
            assign_slot_regs_graph();
            return;
        }
        static const bool use_split = rc_split_;
        if (use_split) {
            // **split 独占整型分配，不能先跑线性扫描。**
            //
            // 曾经的做法是「先 linear 填 slot_reg_，再让 split 覆盖」。
            // 那会抢寄存器：linear 用掉全部 6 个 GPR 给 slot_reg_ 的槽
            // （实测 matmul.run_2 是 9/16 提升、6 regs），而 split 只处理
            // **不在 slot_reg_ 里**的槽，却又去用 kSlotRegs()[0..] ——
            // 同一个 %rbx 被两个分配器同时分给不同的槽，读写互相覆盖。
            //
            // 实测后果：matmul 输出 43198080 -> 0、fannkuch 抛 IndexError、
            // 有的用例直接死循环 OOM。
            //
            // 正确做法：split 模式下整型槽**全部**由 split 决定，
            // slot_reg_ 里的整型槽留空，n_saved_regs_ 由 split 实际用的
            // 寄存器数决定。浮点槽仍走线性扫描（xmm 池不与 GPR 冲突，
            // 且需要 fp_promoted_slots_ 维护调用点 spill）。
            // **先给参数槽做整函数提升，再让 split 处理其余槽。**
            //
            // 参数槽是整函数活跃的（函数体内随时可能用到），本来就该
            // 整函数占一个寄存器。曾经把它们完全排除出 split 却没交给
            // 线性扫描，结果只能落栈：递归函数（fib）每次调用多两次
            // 内存访问，实测慢 47.3%。
            //
            // 这里只对**参数槽**跑线性扫描，其余整型槽仍由 split 独占，
            // 两者用不相交的寄存器区间（split 从 n_saved_regs_ 之后开始用）。
            {
                std::vector<int> param_cands;
                for (const auto& p : f_->params) {
                    if (p.slot < 0 || p.slot >= static_cast<int>(slot_reg_.size())) continue;
                    TypePtr pt = f_->slot_ty[static_cast<size_t>(p.slot)];
                    if (!pt || pt->kind == TyKind::Unknown || ir_is_heap(pt)) continue;
                    if (ir_is_float(pt)) continue;   // 浮点由下面的 fp 分支处理
                    if (p.slot < static_cast<int>(gc_scan_slot_.size()) && gc_scan_slot_[p.slot]) continue;
                    param_cands.push_back(p.slot);
                }
                if (!param_cands.empty()) {
                    std::vector<std::string> pout(f_->slot_ty.size());
                    linear_scan_pool(param_cands, kSlotRegs(), kMaxSlotRegs(), pout);
                    for (size_t i = 0; i < pout.size(); ++i) {
                        if (pout[i].empty()) continue;
                        slot_reg_[i] = pout[i];
                    }
                    // **n_saved_regs_ 必须按「用到的最大寄存器序号 + 1」设置。**
                    //                    // 不能简单数「有几个槽提升了」：linear_scan_pool 可能给                    // 不同槽分配 kSlotRegs()[0]、kSlotRegs()[2]，只提升 2 个                    // 却用到 3 号寄存器。按数量算会漏压栈。
                    //                    // 实测 fib：1 个参数槽拿到 %rbx，n_saved 却算成 0，序言
                    // 没 pushq %rbx，递归调用后参数被破坏，输出 -960。
                    int used = 0;
                    for (int r = 0; r < kMaxSlotRegs(); ++r) {
                        for (const auto& rg : pout)
                            if (!rg.empty() && rg == kSlotRegs()[r]) {
                                used = r + 1;
                                break;
                            }
                    }
                    if (used > n_saved_regs_) n_saved_regs_ = used;
                }
            }
            assign_slot_regs_split();
            // 浮点：线性扫描 + 重建 fp_promoted_slots_。
            std::vector<int> fp_cands;
            for (size_t ci = 0; ci < slot_reg_cand_.size(); ++ci)
                if (slot_cand_is_fp_[ci]) fp_cands.push_back(slot_reg_cand_[ci]);
            if (!fp_cands.empty()) {
                std::vector<std::string> fp_out(f_->slot_ty.size());
                linear_scan_pool(fp_cands, kFpSlotRegs(), kMaxFpSlotRegs(), fp_out);
                for (size_t i = 0; i < fp_out.size(); ++i) {
                    if (fp_out[i].empty()) continue;
                    slot_reg_[i] = fp_out[i];
                }
                fp_promoted_slots_.clear();
                for (int i = 0; i < static_cast<int>(slot_fpidx_.size()); ++i)
                    slot_fpidx_[static_cast<size_t>(i)] = -1;
                for (int i = 0; i < static_cast<int>(fp_out.size()); ++i) {
                    if (fp_out[static_cast<size_t>(i)].empty()) continue;
                    slot_fpidx_[static_cast<size_t>(i)] =
                        static_cast<int>(fp_promoted_slots_.size());
                    fp_promoted_slots_.push_back(i);
                }
            }
            return;
        }
        static const bool ra_off = std::getenv("PYADAMAS_NO_RA") != nullptr;
        if (!ra_off) {
            assign_slot_regs_linear();
            // R4：浮点也走线性扫描。整型与浮点用各自的池分别扫描 ——
            // 两套池不相交（GPR vs xmm），互不干扰。
            //
            // **试过改成「槽与跨块 vreg 统一按 spill cost 竞争」，失败**：
            // `slot_reg_[s]` 是「整函数在寄存器」的布尔标记，代码生成器
            // 不做区间判断。统一分配器让短区间槽（如 mandelbrot 的 s6，
            // 只活跃 7 个位置）也拿到寄存器，区间外读到过期值 → 输出错。
            // 而加「只收整函数活跃的槽」的限制后，覆盖的槽太少，
            // spectral_norm 从 28ms 退化到 94ms。
            //
            // 要真正统一，必须先让代码生成器支持「区间边界插 move」，
            // 即 live range splitting —— 那是独立的一轮工作。
            std::vector<int> fp_cands;
            for (size_t ci = 0; ci < slot_reg_cand_.size(); ++ci)
                if (slot_cand_is_fp_[ci]) fp_cands.push_back(slot_reg_cand_[ci]);
            if (!fp_cands.empty()) {
                std::vector<std::string> fp_out(f_->slot_ty.size());
                linear_scan_pool(fp_cands, kFpSlotRegs(), kMaxFpSlotRegs(), fp_out);
                for (size_t i = 0; i < fp_out.size(); ++i) {
                    if (fp_out[i].empty()) continue;
                    slot_reg_[i] = fp_out[i];
                }
                // 重建 fp_promoted_slots_ / slot_fpidx_（调用点 spill 依赖它们）。
                fp_promoted_slots_.clear();
                for (int i = 0; i < static_cast<int>(fp_out.size()); ++i) {
                    if (fp_out[static_cast<size_t>(i)].empty()) continue;
                    slot_fpidx_[static_cast<size_t>(i)] =
                        static_cast<int>(fp_promoted_slots_.size());
                    fp_promoted_slots_.push_back(i);
                }
            }
            return;
        }

        int k = std::min<int>(kMaxSlotRegs(), static_cast<int>(hot_int.size()));
        for (int i = 0; i < k; ++i) slot_reg_[hot_int[i].second] = kSlotRegs()[i];
        n_saved_regs_ = k;

        int kf = std::min<int>(kMaxFpSlotRegs(), static_cast<int>(hot_fp.size()));
        for (int i = 0; i < kf; ++i) {
            slot_reg_[hot_fp[i].second] = kFpSlotRegs()[i];
            slot_fpidx_[hot_fp[i].second] = i;
            fp_promoted_slots_.push_back(hot_fp[i].second);
        }
    }

// 块内临时寄存器：%r8–%r11 只在调用序列里被用到（参数/函数地址），
// 所以「整块没有任何调用类指令」时可以把它们借给块内临时值。区间用
// 线性扫描分配，定义/使用不重叠的临时值复用同一个寄存器；块结束时
// 全部归还（值本身也不会跨块使用，候选要求 !cross_block_）。

bool AsmGen::temp_reg_op_ok(const Inst& in) const {
        switch (in.op) {
            case IrOp::ConstInt:
            case IrOp::ConstFloat:
            case IrOp::ConstStr:
            case IrOp::ConstNone:
            case IrOp::FuncAddr:
            case IrOp::FuncCode:
            case IrOp::Mov:
            case IrOp::Un:
            case IrOp::Cmp:
            case IrOp::LoadSlot:
            case IrOp::StoreSlot:
            case IrOp::LoadGlobal:
            case IrOp::LoadExtern:
            case IrOp::StoreGlobal:
            case IrOp::Len:
            case IrOp::IndexLoadUnchecked:
            case IrOp::FieldLoad:
            case IrOp::PtrLoad:
            case IrOp::FieldStore:
            case IrOp::SlotAddr:
            case IrOp::Jump:
            case IrOp::Branch:
            case IrOp::Return:
                return true;
            case IrOp::IndexLoad: {
                // 字符串下标会真的调 pa_str_char_at；列表下标只有不会
                // 返回的越界慢路径，借用寄存器是安全的。
                TypePtr bt = ty_of(in.a);
                return !(bt && bt->kind == TyKind::Str);
            }
            case IrOp::Bin: {
                TypePtr t = in.ty;
                if (t && t->kind == TyKind::Str) return false;
                switch (in.bop) {
                    case BinKind::Add:
                    case BinKind::Sub:
                    case BinKind::Mul:
                    case BinKind::BitAnd:
                    case BinKind::BitOr:
                    case BinKind::BitXor:
                    case BinKind::Shl:
                    case BinKind::Shr:
                        return true;
                    case BinKind::Div:
                        // 浮点除法只用 xmm0，不碰 %r8–%r11；整数除法要
                        // %rax/%rdx，块内临时寄存器方案不适用。
                        return in.ty && in.ty->kind == TyKind::Float;
                    default:
                        return false;  // mod/floordiv/pow 用 %r8 或调运行时
                }
            }
            case IrOp::Convert: {
                TypePtr t = in.ty;
                return t && (t->kind == TyKind::Bool || t->kind == TyKind::Int ||
                             t->kind == TyKind::Float);
            }
            default:
                return false;
        }
    }

void AsmGen::assign_block_temp_regs() {
        if (opt_ < 1 || gen_) return;

        for (const auto& b : f_->blocks) {
            // GPR 池（r8-r11）与浮点池（xmm3-7）独立门控：整数除法类
            // 指令踢出 GPR 池但不踢浮点池（见 fp_temp_op_ok）。
            bool safe = true, fp_safe = true;
            for (const auto& in : b.insts) {
                if (safe && !temp_reg_op_ok(in)) safe = false;
                if (fp_safe && !fp_temp_op_ok(in)) fp_safe = false;
                if (!safe && !fp_safe) break;
            }
            if (!safe && !fp_safe) continue;

            std::unordered_map<VReg, int> def_pos;
            std::unordered_map<VReg, const Inst*> def_inst;
            std::unordered_map<VReg, int> last_use;
            int pos = 0;
            for (const auto& in : b.insts) {
                auto touch = [&](VReg r) {
                    if (r != kNoReg) last_use[r] = pos;
                };
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
                if (in.dst != kNoReg) {
                    def_pos[in.dst] = pos;
                    def_inst[in.dst] = &in;
                }
                ++pos;
            }

            struct Range {
                int start;
                int end;
                VReg v;
            };
            // 分配一个寄存器池。除了线性扫描，还带拷贝合并提示：`v = a op b`
            // 且 a 的最后一个使用就是这条指令时，把 a 的寄存器直接分给 v。
            // 指令生成器看到目标寄存器就是左操作数时会原地算（`subsd b, a`），
            // 省掉一条 movsd / movq。
            //
            // 提示只接受本块临时池里的寄存器：跨块值和变量槽的寄存器在定义
            // 块之外还有读者，覆盖掉就错了。in_place_ok 再筛掉后端不能原地
            // 算的算子，避免白白占住寄存器。
            auto allocate = [&](std::vector<Range>& ranges, const std::vector<std::string>& pool,
                                auto in_place_ok) {
                std::ranges::sort(ranges, [](const Range& x, const Range& y) {
                    return x.start != y.start ? x.start < y.start : x.end < y.end;
                });
                // until[i]：池里第 i 个寄存器当前占用区间的最大结束位置。
                std::vector<int> until(pool.size(), -1);
                for (const auto& r : ranges) {
                    int pick = -1;
                    auto di = def_inst.find(r.v);
                    if (di != def_inst.end() && in_place_ok(*di->second)) {
                        VReg a = di->second->a;
                        auto lu = last_use.find(a);
                        if (a != kNoReg && lu != last_use.end() && lu->second == r.start &&
                            a < vreg_reg_.size() && !vreg_reg_[a].empty() && !cross_block_[a]) {
                            for (size_t i = 0; i < pool.size(); ++i)
                                if (pool[i] == vreg_reg_[a] && until[i] <= r.start) {
                                    pick = static_cast<int>(i);
                                    break;
                                }
                        }
                    }
                    if (pick < 0) {
                        // 最近释放的优先，保持和原来 LIFO 自由表一致的行为。
                        for (size_t i = 0; i < pool.size(); ++i)
                            if (until[i] < r.start && (pick < 0 || until[i] >= until[pick]))
                                pick = static_cast<int>(i);
                    }
                    if (pick < 0) continue;  // 后面的区间更晚开始，仍可能排上
                    vreg_reg_[r.v] = pool[static_cast<size_t>(pick)];
                    until[static_cast<size_t>(pick)] =
                        std::max(until[static_cast<size_t>(pick)], r.end);
                }
            };

            if (safe) {
            std::vector<Range> ranges;
            for (const auto& [v, d] : def_pos) {
                if (v >= static_cast<VReg>(f_->vreg_ty.size())) continue;
                if (cross_block_[v]) continue;
                auto lu = last_use.find(v);
                if (lu == last_use.end()) continue;
                // unknown 的机器表示可能是浮点位模式（元素类型丢失的下标装载），
                // 浮点/可选类型同理：只给「确定是整数/指针」的值分配 GPR。
                TypePtr vt = f_->vreg_ty[v];
                if (!vt) continue;
                if (vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                    vt->kind == TyKind::Optional)
                    continue;
                IrOp op = def_inst[v]->op;
                // LoadSlot 有自己的别名机制；常量可以延迟成立即数，不用占寄存器。
                if (op == IrOp::LoadSlot || op == IrOp::ConstInt || op == IrOp::ConstNone)
                    continue;
                ranges.push_back(Range{d, lu->second, v});
            }
            allocate(ranges, block_temp_pool_, [](const Inst& in) {
                if (in.op != IrOp::Bin) return false;
                switch (in.bop) {
                    case BinKind::Add:
                    case BinKind::Sub:
                    case BinKind::Mul:
                    case BinKind::BitAnd:
                    case BinKind::BitOr:
                    case BinKind::BitXor:
                    case BinKind::Shl:
                    case BinKind::Shr:
                        return true;
                    default:
                        return false;  // 除法/取模/幂走 rax/rdx 或运行时调用
                }
            });

            }
            if (fp_safe) {
            // 浮点临时值单独用 xmm3–7：和整型池互不影响。只收块内产生、
            // 块内用完且确定是 float 的纯计算值；LoadSlot 有自己的别名
            // 机制，会正常返回的调用类指令所在块进不到这里。
            std::vector<Range> fp_ranges;
            for (const auto& [v, d] : def_pos) {
                if (v >= static_cast<VReg>(f_->vreg_ty.size())) continue;
                if (cross_block_[v]) continue;
                auto lu = last_use.find(v);
                if (lu == last_use.end()) continue;
                TypePtr vt = f_->vreg_ty[v];
                if (!vt || vt->kind != TyKind::Float) continue;
                IrOp op = def_inst[v]->op;
                if (op != IrOp::Bin && op != IrOp::Un && op != IrOp::Convert &&
                    op != IrOp::ConstFloat && op != IrOp::Mov)
                    continue;
                fp_ranges.push_back(Range{d, lu->second, v});
            }
            allocate(fp_ranges, std::vector<std::string>(kFpTempRegs, kFpTempRegs + kMaxFpTempRegs),
                     [](const Inst& in) {
                         if (in.op != IrOp::Bin || !ir_is_float(in.ty)) return false;
                         return in.bop == BinKind::Add || in.bop == BinKind::Sub ||
                                in.bop == BinKind::Mul || in.bop == BinKind::Div;
                     });
            }
        }
    }

// 每个基本块所在的循环嵌套深度：块被多少个自然循环体包含。
//
// 跨块 vreg 提升只看「使用次数」时，循环外只执行一次的常量（循环不变量
// 的装载、2.0/4.0 这类外提常量）会和每轮迭代都要读的内层循环值打成平手，
// 然后靠 vreg 编号小抢走寄存器，把内层循环的热值挤回栈上。按使用点所在
// 块的深度加权后，内层循环的热值优先留在寄存器。
static std::vector<int> block_loop_depth(const IRFunction& f) {
    const int nb = static_cast<int>(f.blocks.size());
    std::vector<int> depth(static_cast<size_t>(nb), 0);
    if (nb == 0) return depth;

    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const auto& insts = f.blocks[static_cast<size_t>(b)].insts;
        auto add = [&](int t) {
            if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
        };
        if (insts.empty()) {
            if (b + 1 < nb) add(b + 1);
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
            default:
                if (b + 1 < nb) add(b + 1);
                break;
        }
    }

    // reach[b][c]：b 沿 CFG 走至少一条边能否到达 c。
    std::vector<std::vector<uint8_t>> reach(
        static_cast<size_t>(nb), std::vector<uint8_t>(static_cast<size_t>(nb), 0));
    for (int b = 0; b < nb; ++b) {
        std::vector<int> work(succ[static_cast<size_t>(b)]);
        while (!work.empty()) {
            int c = work.back();
            work.pop_back();
            if (reach[static_cast<size_t>(b)][static_cast<size_t>(c)]) continue;
            reach[static_cast<size_t>(b)][static_cast<size_t>(c)] = 1;
            for (int s : succ[static_cast<size_t>(c)])
                if (!reach[static_cast<size_t>(b)][static_cast<size_t>(s)]) work.push_back(s);
        }
    }

    // 支配集（迭代到不动点），用来识别回边：u→v 且 v 支配 u。
    // 只考虑从入口可达的块，不可达块的支配集没有意义。
    std::vector<char> live(static_cast<size_t>(nb), 0);
    live[0] = 1;
    for (int b = 1; b < nb; ++b) live[static_cast<size_t>(b)] = reach[0][static_cast<size_t>(b)];
    std::vector<std::vector<char>> dom(static_cast<size_t>(nb),
                                       std::vector<char>(static_cast<size_t>(nb), 1));
    dom[0].assign(static_cast<size_t>(nb), 0);
    dom[0][0] = 1;
    std::vector<std::vector<int>> pred(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b)
        for (int s : succ[static_cast<size_t>(b)]) pred[static_cast<size_t>(s)].push_back(b);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < nb; ++b) {
            if (!live[static_cast<size_t>(b)] || pred[static_cast<size_t>(b)].empty()) continue;
            std::vector<char> nd;
            for (int p : pred[static_cast<size_t>(b)]) {
                if (nd.empty()) {
                    nd = dom[static_cast<size_t>(p)];
                    continue;
                }
                for (size_t i = 0; i < nd.size(); ++i)
                    nd[i] = nd[i] && dom[static_cast<size_t>(p)][i];
            }
            nd[static_cast<size_t>(b)] = 1;
            if (nd != dom[static_cast<size_t>(b)]) {
                dom[static_cast<size_t>(b)] = std::move(nd);
                changed = true;
            }
        }
    }
    std::vector<char> header(static_cast<size_t>(nb), 0);
    for (int u = 0; u < nb; ++u) {
        if (!live[static_cast<size_t>(u)]) continue;
        for (int v : succ[static_cast<size_t>(u)])
            if (live[static_cast<size_t>(v)] && dom[static_cast<size_t>(u)][static_cast<size_t>(v)])
                header[static_cast<size_t>(v)] = 1;
    }

    // 循环头 v 的自然循环体：v 支配、且能回到 v 的块。只用「双向可达」
    // 会误判：内层循环的块能退出到外层、再从外层绕回来，看着和内层循环
    // 头互相可达，其实不在同一个自然循环里。
    for (int v = 0; v < nb; ++v) {
        if (!header[static_cast<size_t>(v)]) continue;
        for (int b = 0; b < nb; ++b)
            if (b == v || (dom[static_cast<size_t>(b)][static_cast<size_t>(v)] &&
                           reach[static_cast<size_t>(b)][static_cast<size_t>(v)]))
                ++depth[static_cast<size_t>(b)];
    }
    return depth;
}

// ── 跨块 vreg 提升 ──────────────────────────────────────────
    //
    // 块内临时寄存器只给「定义和使用同块」的值；跨块的值（循环不变量、
    // 合并结果、循环边界等）一直住栈槽。这里把局部变量没用到的
    // callee-saved 寄存器按「使用次数 × 循环深度」分给最热的跨块整型 vreg：
    //   - 寄存器整函数独占，不与其他值复用，所以不需要活跃性分析；
    //   - callee-saved 跨函数调用不会被破坏，only has_handlers 的函数
    //     例外（longjmp 会回滚），整函数跳过；
    //   - 不做支配性校验：IR 的 vreg 定义先于使用，整函数独占寄存器
    //     天然满足「定义后一直有效」。
    /* ── 失败记录：统一浮点寄存器分配 ────────────────────────────────
 *
 * 试过把「命名变量槽」和「跨块 vreg」放进同一个池、按 spill cost 竞争
 * xmm8–15（取代原来 assign_slot_regs 先占满、assign_global_vreg_regs 捡剩
 * 的两趟分配）。**失败**，两个原因：
 *
 * 1. `slot_reg_[s]` 是「整函数在寄存器」的布尔标记，代码生成器（slot_op）
 *    在任意指令处看到非空就直接用寄存器，**不做区间判断、不在边界插 move**。
 *    所以给「只活跃一小段」的槽分配寄存器是错的：区间外的读写会拿到过期
 *    寄存器值。实测 mandelbrot 的 s6 只活跃 7 个位置，分到 %xmm12 后
 *    输出从 2990360 变成 1。
 *
 * 2. 加「只收整函数活跃的槽」的限制后覆盖的槽太少 —— 实测 spectral_norm
 *    从 28ms 退化到 94ms（3.4x）。而让不活跃的槽「参与竞争但不占位」
 *    同样退化（93ms）。
 *
 * 结论：要真正统一，必须先让代码生成器支持「区间边界插 move」，即
 * live range splitting。那是独立的一轮工作，不是分配器内部能解决的。
 *
 * 现状保留原版的两趟分配，跨块浮点 vreg 只在「无调用」函数里提升
 * （那里没有调用点 spill 开销，纯赚）。 */
void AsmGen::assign_global_vreg_regs() {
        // **每个函数都要清空**：这些是 AsmGen 的成员，跨函数累积。
        //
        // 原来只清 fp_promoted_slots_（在 assign_slot_regs 里），漏了
        // fp_promoted_vregs_ —— 于是上一个函数提升的 vreg 编号会带到下一个
        // 函数，而 vreg 编号是函数内局部的：同一个编号在下一个函数里可能
        // 是整型，spill 就生成 `movsd %r9, 80(%rsp)`（%r9 不是 xmm）。
        // 实测 spectral_norm 的 mul_ata_2 把 v=39（int）当浮点写回。
        fp_promoted_vregs_.clear();
        const size_t nv = f_->vreg_ty.size();
        vreg_reg_.assign(nv, std::string());

        block_temp_pool_.assign(target_abi().block_temps,
                                 target_abi().block_temps + target_abi().n_block_temps);
        global_imm_.clear();
        if (nv == 0) return;
        if (opt_ < 2 || gen_ || f_->has_handlers) return;

        // 跨块的 const.int 整函数当立即数：寄存器、栈槽都不占。所有使用
        // 点生成的都是 movq $imm / addq $imm / cmpq $imm 形态（比较和
        // 分支对立即数位置有专门处理，转换会先搬进 %rax）。
        std::vector<int64_t> const_imm(nv, 0);
        std::vector<uint8_t> is_const(nv, 0);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if (in.op != IrOp::ConstInt || in.dst == kNoReg || in.dst >= nv) continue;
                if (in.imm < INT32_MIN || in.imm > INT32_MAX) continue;
                const_imm[static_cast<size_t>(in.dst)] = in.imm;
                is_const[static_cast<size_t>(in.dst)] = 1;
            }
        for (size_t v = 0; v < nv; ++v) {
            if (is_const[v])
                global_imm_[static_cast<VReg>(v)] =
                    std::format("${}", const_imm[v]);
        }

        struct Cand {
            int v;
            int uses;
        };
        // 使用点按所在块的循环深度加权：内层循环里读一次，实际执行次数
        // 是循环外读一次的几十倍，寄存器应该给它。
        std::vector<int> depth = block_loop_depth(*f_);
        std::vector<int64_t> weight(nv, 0);
        {
            int bi = 0;
            for (const auto& b : f_->blocks) {
                int d = depth[static_cast<size_t>(bi)];
                int64_t w = 1;
                for (int i = 0; i < d && i < 4; ++i) w *= 10;
                auto add = [&](VReg r) {
                    if (r != kNoReg && r < nv) weight[r] += w;
                };
                for (const auto& in : b.insts) {
                    add(in.a);
                    add(in.b);
                    add(in.c);
                    for (VReg r : in.args) add(r);
                }
                ++bi;
            }
        }
        std::vector<Cand> cands;
        for (size_t v = 0; v < nv; ++v) {
            if (!cross_block_[v]) continue;
            if (global_imm_.contains(static_cast<VReg>(v))) continue;
            TypePtr vt = f_->vreg_ty[v];
            if (!vt || vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                vt->kind == TyKind::Optional)
                continue;
            if (use_count_[v] == 0 || def_block_[v] < 0) continue;
            // RcMark 的值必须留在栈槽里：它是 RC 临时栈水位，跨整个函数
            // 存活。放进 callee 寄存器看似安全，但它会和槽寄存器/其它
            // vreg 的复用产生冲突，cleanup 拿到错水位 —— 实测 fasta 报
            // invalid temporary stack mark、binary_trees 报 refcount 错误。
            if (vreg_is_rc_mark(*f_, static_cast<VReg>(v))) continue;
            cands.push_back(Cand{static_cast<int>(v), static_cast<int>(weight[v])});
        }
        if (cands.empty()) return;
        std::ranges::sort(cands, [](const Cand& a, const Cand& b) {
            return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
        });
        // **split 模式下 vreg 不得使用 kSlotRegs() 池。**
        //
        // split 已经把这些寄存器独占分配给变量槽（slot_loc_），而且
        // slot_reg_ 是空的（split 独占整型分配）—— 所以下面这段
        // 「从 n_saved_regs_ 继续往 kSlotRegs() 分配」会把同一个物理
        // 寄存器同时分给「槽」和「跨块 vreg」，两边互相覆盖。
        //
        // 实测 nqueens：循环上界槽拿到 %rbp，同一函数的跨块 vreg 又
        // 拿到 %rbp，循环条件被写坏 -> IndexError。
        //
        // 正确做法：split 模式下跳过这一段，vreg 只走下面的
        // %r8-%r11（block_temps，调用点会保存）等不与槽冲突的路径。
        // split 模式下跳过 kSlotRegs() 分配（那些寄存器归槽所有），
        // 但**不能直接 return** —— 后面还有 block_temps（%r8-%r11）
        // 的分配路径，跨块 vreg 需要它。
        //
        // 曾经在这里 return，结果跨块 vreg（如 LICM 提升的循环上界）
        // 拿不到任何寄存器，只能留在栈上被反复重载；更糟的是
        // %r8-%r11 在调用点会被破坏，而那些 vreg 的栈副本没人维护 ——
        // 实测 nqueens N=1 就抛 IndexError。
        int next = n_saved_regs_;
        size_t ci = 0;
        // **vreg 从 n_saved_regs_ 开始用，正好是 split 之后剩下的那部分。**
        //
        // split 的槽占 kSlotRegs()[base .. base+k)，而 n_saved_regs_ 已被
        // 设成 base+k（见 assign_slot_regs_split 末尾的 max 逻辑）。所以从
        // n_saved_regs_ 起分配不会与槽冲突。
        //
        // 曾经在 split 模式下整段跳过，结果跨块 vreg（LICM 提升的循环上界、
        // 函数返回值等）只能落栈，每次使用要「load 到临时 + 再 store 临时」
        // 两条指令 —— 这才是 split 慢 27% 的主因，不是双写。
        for (; ci < cands.size() && next < kMaxSlotRegs(); ++ci)
            vreg_reg_[static_cast<size_t>(cands[ci].v)] = kSlotRegs()[next++];
        n_saved_regs_ = next;

        // 无调用函数里 %r8–%r11 也不会被任何指令破坏：再拿至多两个
        // 承载最热的跨块值，剩下的留给块内临时寄存器。
        bool call_free = true;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) {
                    call_free = false;
                    break;
                }
            if (!call_free) break;
        }
        if (call_free) {
            int used = 0;
            for (; ci < cands.size() && used < 2 && used < target_abi().n_block_temps;
                 ++ci, ++used)
                vreg_reg_[static_cast<size_t>(cands[ci].v)] = target_abi().block_temps[used];
            if (used > 0)
                block_temp_pool_.assign(target_abi().block_temps + used,
                                        target_abi().block_temps + target_abi().n_block_temps);

        }
        // 跨块浮点 vreg 提升：只在「无调用」函数里做。
        //
        // SysV 下 xmm8–15 是 caller-saved，有调用的函数需要每个调用点",
        // spill/reload。这套机制存在（fp_promoted_vregs_），但实测收益为
        // 负：nbody 的 advance 有 7 个浮点变量 + 4 个跨块临时值竞争 8 个
        // xmm，变量优先的分配已经是最优；把临时值也塞进来反而挤掉变量。
        //
        // 保留无调用函数里的提升（那里没有调用点开销，纯赚）。
        bool call_free_fp = true;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) { call_free_fp = false; break; }
            if (!call_free_fp) break;
        }
        if (call_free_fp) {
            size_t fp_base = fp_promoted_slots_.size();
            if (fp_base < static_cast<size_t>(kMaxFpSlotRegs())) {
                std::vector<Cand> fcands;
                for (size_t v = 0; v < nv; ++v) {
                    if (!cross_block_[v] || !vreg_reg_[v].empty()) continue;
                    TypePtr vt = f_->vreg_ty[v];
                    if (!vt || vt->kind != TyKind::Float) continue;
                    if (use_count_[v] == 0 || def_block_[v] < 0) continue;
                    fcands.push_back(Cand{static_cast<int>(v), static_cast<int>(weight[v])});
                }
                std::ranges::sort(fcands, [](const Cand& a, const Cand& b) {
                    return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
                });
                size_t used_fp = 0;
                for (const auto& cd : fcands) {
                    if (fp_base + used_fp >= static_cast<size_t>(kMaxFpSlotRegs())) break;
                    vreg_reg_[static_cast<size_t>(cd.v)] = kFpSlotRegs()[fp_base + used_fp];
                    fp_promoted_vregs_.push_back(cd.v);
                    ++used_fp;
                }
            }
        }
    }
// ── 生成期字符串缓存 ────────────────────────────────────────
void AsmGen::build_operand_caches() {
        block_labels_.resize(f_->blocks.size());
        for (size_t i = 0; i < block_labels_.size(); ++i)
            block_labels_[i] = std::format(".Lf{}_b{}", fi_, i);
        vreg_mem_.resize(f_->vreg_ty.size());
        for (size_t r = 0; r < vreg_mem_.size(); ++r)
            vreg_mem_[r] = std::format("{}(%rsp)", vreg_off(static_cast<VReg>(r)));
        smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < smem_cache_.size(); ++s)
            smem_cache_[s] = std::format("{}(%rsp)", slot_off(static_cast<int>(s)));
        gen_smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < gen_smem_cache_.size(); ++s)
            gen_smem_cache_[s] =
                std::format("{}(%r15)", 8 * (kGenHeader + static_cast<int>(s)));
    }

// ── 函数 ────────────────────────────────────────────────────
std::vector<ArgLoc> AsmGen::classify(const std::vector<TypePtr>& types) const {
        std::vector<ArgLoc> locs(types.size());
        int gi = 0, fi = 0, sp = 0;
        // Win64 按位置分配：第 i 个实参固定占第 i 个槽，与类型无关，
        // 浮点实参在第 i 个位置就用 xmmI；SysV 按类型分计数，两套独立序列。
        const bool pos = target_abi().positional_args;
        for (size_t i = 0; i < types.size(); ++i) {
            bool fp = ir_is_float(types[i]);
            if (pos) {
                if (i < static_cast<size_t>(kMaxIntArgs()) &&
                    i < static_cast<size_t>(kMaxFpArgs())) {
                    locs[i].fp = fp;
                    locs[i].reg = static_cast<int>(i);
                } else {
                    locs[i].fp = fp;
                    locs[i].stack_off = out_base() + sp++ * 8;
                }
                continue;
            }
            if (fp && fi < kMaxFpArgs()) {
                locs[i].fp = true;
                locs[i].reg = fi++;
            } else if (!fp && gi < kMaxIntArgs()) {
                locs[i].fp = false;
                locs[i].reg = gi++;
            } else {
                locs[i].fp = fp;
                locs[i].stack_off = out_base() + sp++ * 8;
            }
        }
        return locs;
    }

}  // namespace pyadamas
