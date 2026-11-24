// backend/regalloc_scan.cpp —— 变量槽寄存器分配（线性扫描 + 按频次回退）
//
// 从 backend/regalloc.cpp 拆出（纯代码移动，函数体不变）：
// assign_slot_regs_linear / assign_slot_regs。块循环深度工具见
// backend/regalloc_loop_depth.h。live range splitting（PYADAMAS_RA_SPLIT）
// 与 SpillPlacement 已删除（见 docs/regalloc.md §已否决的方案）。
#include "backend/impl.h"

#include "backend/regalloc_loop_depth.h"

namespace pyadamas {

// live range splitting（PYADAMAS_RA_SPLIT）与 SpillPlacement 已删除：
// 三轮实现均 +27~28%（docs/regalloc.md §已否决的方案），入口在 main.cpp
// 直接报错退出。默认路径的分配器见下面的 assign_slot_regs_linear。

void AsmGen::assign_slot_regs_linear() {
        // 只把**整型**候选交给通用扫描：浮点由调用方用独立的池扫（R4）。
        std::vector<int> int_cands;
        for (size_t ci = 0; ci < ra_.slot_reg_cand.size(); ++ci)
            if (!ra_.slot_cand_is_fp[ci]) int_cands.push_back(ra_.slot_reg_cand[ci]);
        if (int_cands.empty()) return;
        std::vector<std::string> out(f_->slot_ty.size());
        linear_scan_pool(int_cands, kSlotRegs(), kMaxSlotRegs(), out);
        int n_used = 0;
        for (size_t i2 = 0; i2 < out.size(); ++i2) {
            if (out[i2].empty()) continue;
            ra_.slot_reg[i2] = out[i2];
        }
        for (int r = 0; r < kMaxSlotRegs(); ++r) {
            bool any = false;
            for (const auto& sr : ra_.slot_reg)
                if (sr == kSlotRegs()[r]) {
                    any = true;
                    break;
                }
            if (any) n_used = r + 1;
        }
        n_saved_regs_ = n_used;
        // 只统计**整型**槽。浮点槽在调用方（assign_slot_regs）随后才分配，
        // 此刻 ra_.slot_reg 里的浮点项还是空的。
        //
        // 这个统计以前写的是「ra_.slot_reg 里非空的个数」，把浮点算进去却
        // 又没分配，于是长期输出「7/14 promoted」这种误导数字 —— 实际
        // 浮点是 6/6 全提升，整型才是 7/14。排查时被它带偏过好几轮。
        if (std::getenv("PYADAMAS_RA_DEBUG")) {
            int int_assigned = 0, int_total = 0;
            for (size_t ci = 0; ci < ra_.slot_reg_cand.size(); ++ci) {
                if (ra_.slot_cand_is_fp[ci]) continue;
                ++int_total;
                if (!ra_.slot_reg[static_cast<size_t>(ra_.slot_reg_cand[ci])].empty())
                    ++int_assigned;
            }
            std::fprintf(stderr, "[ra] %s: int %d/%d promoted, %d regs\n",
                         f_->name.c_str(), int_assigned, int_total, n_used);
        }
    }

void AsmGen::assign_slot_regs() {
        report_interval_precision();
        int ns = static_cast<int>(f_->slot_ty.size());
        ra_.slot_reg.assign(ns, std::string());
        // R1：先把整型槽的块级活跃性算出来（R2 的线性扫描要用）。
        build_slot_liveness();
        ra_.slot_fpidx.assign(ns, -1);
        ra_.fp_promoted_slots.clear();
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
            //
            // **试过改成「槽与跨块 vreg 统一按 spill cost 竞争」，失败**：
            // `ra_.slot_reg[s]` 是「整函数在寄存器」的布尔标记，代码生成器
            // 不做区间判断。统一分配器让短区间槽（如 mandelbrot 的 s6，
            // 只活跃 7 个位置）也拿到寄存器，区间外读到过期值 → 输出错。
            // 而加「只收整函数活跃的槽」的限制后，覆盖的槽太少，
            // spectral_norm 从 28ms 退化到 94ms。
            //
            // 要真正统一，必须先让代码生成器支持「区间边界插 move」，
            // 即 live range splitting —— 那是独立的一轮工作。
            std::vector<int> fp_cands;
            for (size_t ci = 0; ci < ra_.slot_reg_cand.size(); ++ci)
                if (ra_.slot_cand_is_fp[ci]) fp_cands.push_back(ra_.slot_reg_cand[ci]);
            if (!fp_cands.empty()) {
                std::vector<std::string> fp_out(f_->slot_ty.size());
                linear_scan_pool(fp_cands, kFpSlotRegs(), kMaxFpSlotRegs(), fp_out);
                for (size_t i = 0; i < fp_out.size(); ++i) {
                    if (fp_out[i].empty()) continue;
                    ra_.slot_reg[i] = fp_out[i];
                }
                // 重建 ra_.fp_promoted_slots / ra_.slot_fpidx（调用点 spill 依赖它们）。
                ra_.fp_promoted_slots.clear();
                for (int i = 0; i < static_cast<int>(fp_out.size()); ++i) {
                    if (fp_out[static_cast<size_t>(i)].empty()) continue;
                    ra_.slot_fpidx[static_cast<size_t>(i)] =
                        static_cast<int>(ra_.fp_promoted_slots.size());
                    ra_.fp_promoted_slots.push_back(i);
                }
            }
            return;
        }

        int k = std::min<int>(kMaxSlotRegs(), static_cast<int>(hot_int.size()));
        for (int i = 0; i < k; ++i) ra_.slot_reg[hot_int[i].second] = kSlotRegs()[i];
        n_saved_regs_ = k;

        int kf = std::min<int>(kMaxFpSlotRegs(), static_cast<int>(hot_fp.size()));
        for (int i = 0; i < kf; ++i) {
            ra_.slot_reg[hot_fp[i].second] = kFpSlotRegs()[i];
            ra_.slot_fpidx[hot_fp[i].second] = i;
            ra_.fp_promoted_slots.push_back(hot_fp[i].second);
        }
    }

}  // namespace pyadamas
