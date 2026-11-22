// codegen_regalloc_scan.cpp —— 变量槽寄存器分配（split/图着色/线性扫描）
//
// 从 codegen_regalloc.cpp 拆出（纯代码移动，函数体不变）：
// assign_slot_regs_split / assign_slot_regs_linear /
// assign_slot_regs。块循环深度工具见 regalloc_loop_depth.h。
#include "codegen_impl.h"

#include "regalloc_loop_depth.h"
#include "spill_placement.h"

namespace pyadamas {

/* ── live range splitting：按块分配变量槽的寄存器 ──────────────
 *
 * **问题**：`ra_.slot_reg[s]` 是「整函数在寄存器」的布尔标记，代码生成器
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
 *   spill/reload，还得维护 ra_.fp_promoted_slots / ra_.fp_spill_mask。
 *
 *   上一轮集成失败就是这个：让 split 也处理浮点槽，但没重建
 *   ra_.fp_promoted_slots，调用点不保存 -> 值被 call 破坏 ->
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
            if (!ra_.slot_reg[static_cast<size_t>(s)].empty()) continue;
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
        // 又因为「本块无位置」回落到 ra_.slot_reg 或 smem，与 bb4 的 %rbx
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
            // split 独占整型分配后，ra_.slot_reg 里的整型槽是空的，
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
                                         slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s2)].text.c_str());
                    std::fprintf(stderr, "\n");
                }
            }
        }
}

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
        // 用驱动传入的开关（参与模块缓存失效），不在这里重复读环境变量。
        //
        // 原来这里直接 getenv：切换开关后模块缓存键不变，仍命中旧产物 ——
        // 实测连续几轮 A/B 测试拿到完全相同的二进制。现在由 Program::rc_split
        // 统一管理，并写进 manifest 的 rcsplit 行。
        // split 分配器已废弃：main.cpp 在解析 PYADAMAS_RA_SPLIT 时直接报错
        // 退出，所以 rc_split 恒 false、这里不可达。代码先留着 —— 它和下面
        // codegen_block 的槽位置发射（块入口 reload / 融合存储双写 / fp 别名
        // 掩码）是一套，删除要重排默认路径的发射代码，另开一轮再做。
        static const bool use_split = rc_split_;
        if (use_split) {
            // **split 独占整型分配，不能先跑线性扫描。**
            //
            // 曾经的做法是「先 linear 填 ra_.slot_reg，再让 split 覆盖」。
            // 那会抢寄存器：linear 用掉全部 6 个 GPR 给 ra_.slot_reg 的槽
            // （实测 matmul.run_2 是 9/16 提升、6 regs），而 split 只处理
            // **不在 ra_.slot_reg 里**的槽，却又去用 kSlotRegs()[0..] ——
            // 同一个 %rbx 被两个分配器同时分给不同的槽，读写互相覆盖。
            //
            // 实测后果：matmul 输出 43198080 -> 0、fannkuch 抛 IndexError、
            // 有的用例直接死循环 OOM。
            //
            // 正确做法：split 模式下整型槽**全部**由 split 决定，
            // ra_.slot_reg 里的整型槽留空，n_saved_regs_ 由 split 实际用的
            // 寄存器数决定。浮点槽仍走线性扫描（xmm 池不与 GPR 冲突，
            // 且需要 ra_.fp_promoted_slots 维护调用点 spill）。
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
                    if (p.slot < 0 || p.slot >= static_cast<int>(ra_.slot_reg.size())) continue;
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
                        ra_.slot_reg[i] = pout[i];
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
            // 浮点：线性扫描 + 重建 ra_.fp_promoted_slots。
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
                ra_.fp_promoted_slots.clear();
                for (int i = 0; i < static_cast<int>(ra_.slot_fpidx.size()); ++i)
                    ra_.slot_fpidx[static_cast<size_t>(i)] = -1;
                for (int i = 0; i < static_cast<int>(fp_out.size()); ++i) {
                    if (fp_out[static_cast<size_t>(i)].empty()) continue;
                    ra_.slot_fpidx[static_cast<size_t>(i)] =
                        static_cast<int>(ra_.fp_promoted_slots.size());
                    ra_.fp_promoted_slots.push_back(i);
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
