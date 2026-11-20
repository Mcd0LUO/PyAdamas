// codegen_regalloc_scan.cpp —— 变量槽寄存器分配（split/图着色/线性扫描）
//
// 从 codegen_regalloc.cpp 拆出（纯代码移动，函数体不变）：
// assign_slot_regs_split / assign_slot_regs_graph / assign_slot_regs_linear /
// assign_slot_regs。块循环深度工具见 regalloc_loop_depth.h。
#include "codegen_impl.h"

#include "regalloc_loop_depth.h"
#include "spill_placement.h"

namespace pyadamas {

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
                                         slot_loc_[static_cast<size_t>(b)][static_cast<size_t>(s2)].text.c_str());
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

}  // namespace pyadamas
