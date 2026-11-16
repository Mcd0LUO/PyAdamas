// codegen_regalloc.cpp —— 寄存器/栈槽分配与函数帧
#include "codegen_impl.h"

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

void AsmGen::assign_vreg_slots() {
        size_t nv = f_->vreg_ty.size();
        vreg_slot_.assign(nv, 0);
        n_vreg_slots_ = 0;
        use_count_.assign(nv, 0);
        cross_block_.assign(nv, false);
        def_block_.assign(nv, -1);
        if (nv == 0) return;

        constexpr int kInf = INT32_MAX;
        std::vector<int> def_pos(nv, -1);
        // 同一个 vreg 可能被定义多次：挂起点恢复时用 load.slot 把值
        // 装回原来的 vreg（见 Lowerer::spill_across_suspends）。
        std::vector<int> def_count(nv, 0);
        std::vector<int> first_use(nv, kInf), last_use(nv, -1);

        auto touch = [&](VReg r, int bi) {
            if (r == kNoReg || r >= nv) return;
            ++use_count_[r];
            if (pos_ < first_use[r]) first_use[r] = pos_;
            last_use[r] = pos_;
            // 定义在别的块，或者干脆还没定义（前向引用）→ 跨块
            if (def_block_[r] != bi) cross_block_[r] = true;
        };

        pos_ = 0;
        int bi = 0;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts) {
                if (in.a != kNoReg) touch(in.a, bi);
                if (in.b != kNoReg) touch(in.b, bi);
                if (in.c != kNoReg) touch(in.c, bi);
                for (VReg r : in.args) touch(r, bi);
                if (in.dst != kNoReg && in.dst < nv) {
                    def_pos[in.dst] = pos_;
                    def_block_[in.dst] = bi;
                    ++def_count[in.dst];
                }
                ++pos_;
            }
            ++bi;
        }
        const int total_pos = pos_;

        struct Range {
            int start, end, vreg;
        };
        std::vector<Range> ranges;
        ranges.reserve(nv);
        for (size_t v = 0; v < nv; ++v) {
            int d = def_pos[v];
            int fu = first_use[v];
            int lu = last_use[v];

            if (d < 0 && fu == kInf) continue;  // 既没定义也没使用
            if (cross_block_[v] || def_count[v] > 1 ||
                vreg_is_rc_mark(*f_, static_cast<VReg>(v))) {
                // 独占：区间覆盖整个函数，不会与任何人重叠
                ranges.push_back({0, total_pos + 1, static_cast<int>(v)});
                continue;
            }
            int start = (d < 0) ? fu : (fu == kInf ? d : std::min(d, fu));
            int end = std::max(d, lu);
            ranges.push_back({start, end, static_cast<int>(v)});
        }
        std::ranges::sort(ranges, [](const Range& a, const Range& b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        });

        // 按结束位置维护活跃区间，释放已经过期的槽
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
        spill_int_caller_slots();
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            line(std::format("movsd {}, {}", slot_reg_[slot], smem(slot)));
        }
    }

void AsmGen::reload_fp_slots() {
        reload_int_caller_slots();
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            line(std::format("movsd {}, {}", smem(slot), slot_reg_[slot]));
        }
    }


// ── R3：caller-saved 池的调用点保存/恢复 ─────────────────────────
//
// 与浮点的 spill_fp_slots / reload_fp_slots 同构。区别：整型只在
// int_caller_slots_ 非空时才动作（大多数函数用不到 caller 池）。
void AsmGen::spill_int_caller_slots() {
        if (int_caller_slots_.empty()) return;
        for (size_t j = 0; j < int_caller_slots_.size(); ++j) {
            if (int_caller_mask_valid_ && j < int_caller_mask_.size() &&
                !int_caller_mask_[static_cast<size_t>(j)])
                continue;
            int slot = int_caller_slots_[j];
            line(std::format("movq {}, {}", slot_reg_[static_cast<size_t>(slot)], smem(slot)));
        }
    }

void AsmGen::reload_int_caller_slots() {
        if (int_caller_slots_.empty()) return;
        for (size_t j = 0; j < int_caller_slots_.size(); ++j) {
            if (int_caller_mask_valid_ && j < int_caller_mask_.size() &&
                !int_caller_mask_[static_cast<size_t>(j)])
                continue;
            int slot = int_caller_slots_[j];
            line(std::format("movq {}, {}", smem(slot), slot_reg_[static_cast<size_t>(slot)]));
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

        // 1) 排除：堆槽 / 取过地址 / handler hazard。
        std::vector<bool> addr_taken(static_cast<size_t>(ns), false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts)
                if (in.op == IrOp::SlotAddr && in.idx >= 0 && in.idx < ns)
                    addr_taken[static_cast<size_t>(in.idx)] = true;

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
            if (addr_taken[static_cast<size_t>(s2)]) continue;
            if (hazard[static_cast<size_t>(s2)]) continue;
            bool isfp = ir_is_float(f_->slot_ty[static_cast<size_t>(s2)]);
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
        if (std::getenv("PYADAMAS_RA_DEBUG")) {
            int assigned = 0;
            for (const auto& sr : slot_reg_)
                if (!sr.empty()) ++assigned;
            std::fprintf(stderr, "[ra] %s: %d/%d slots promoted (linear scan), %d regs\n",
                         f_->name.c_str(), assigned,
                         static_cast<int>(slot_reg_cand_.size()), n_used);
        }
    }

void AsmGen::assign_slot_regs() {
        int ns = static_cast<int>(f_->slot_ty.size());
        slot_reg_.assign(ns, std::string());
        // R1：先把整型槽的块级活跃性算出来（R2 的线性扫描要用）。
        build_slot_liveness();
        int_caller_slots_.clear();
        int_caller_mask_.clear();
        int_caller_mask_valid_ = false;
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
        static const bool ra_off = std::getenv("PYADAMAS_NO_RA") != nullptr;
        if (!ra_off) {
            assign_slot_regs_linear();
            // R4：浮点也走线性扫描。整型与浮点用各自的池分别扫描 ——
            // 两套池不相交（GPR vs xmm），互不干扰。
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
            bool safe = true;
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) {
                    safe = false;
                    break;
                }
            if (!safe) continue;

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

            // 浮点临时值单独用 xmm3–7：和整型池互不影响。只收块内产生、
            // 块内用完且确定是 float 的纯计算值；LoadSlot 有自己的别名
            // 机制，调用类指令所在块根本进不到这里。
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
    void AsmGen::assign_global_vreg_regs() {
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
        int next = n_saved_regs_;
        size_t ci = 0;
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

            // 无调用函数里 xmm8–15 也不会被破坏：空闲的拿走放跨块浮点值。
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
