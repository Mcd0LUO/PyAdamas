// codegen_block.cpp —— 函数与基本块、融合与分支
#include "codegen_impl.h"

namespace py23 {

// O2：只清零「可能读到未赋值值」的槽。对每个块做 must 数据流：
// in = 所有前驱 out 的交集；out = in ∪ 块内 store。LoadSlot 遇到
// 不在 assigned 集合里的槽就需要序言清零；StoreSlot 标记已赋值。
// 入口块的 in 固定为形参集合（加上 env 槽），不被回边影响。
std::vector<bool> AsmGen::slots_needing_zero() const {
    std::vector<bool> need(static_cast<size_t>(n_slots_), false);
    int nb = static_cast<int>(f_->blocks.size());
    if (n_slots_ <= 0 || nb == 0) return need;

    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const auto& insts = f_->blocks[b].insts;
        if (insts.empty()) {
            if (b + 1 < nb) succ[b].push_back(b + 1);
            continue;
        }
        const Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            succ[b].push_back(last.target);
        } else if (last.op == IrOp::Branch) {
            succ[b].push_back(last.target);
            succ[b].push_back(last.target2);
        } else if (last.op != IrOp::Return && last.op != IrOp::GenYield &&
                   last.op != IrOp::GenAwait && last.op != IrOp::GenRethrow &&
                   last.op != IrOp::GenDone) {
            if (b + 1 < nb) succ[b].push_back(b + 1);
        }
    }

    std::vector<bool> entry_assigned(static_cast<size_t>(n_slots_), false);
    for (const auto& p : f_->params)
        if (p.slot >= 0 && p.slot < n_slots_) entry_assigned[p.slot] = true;
    if (f_->env_slot >= 0 && f_->env_slot < n_slots_) entry_assigned[f_->env_slot] = true;

    std::vector<std::vector<bool>> in(static_cast<size_t>(nb),
                                      std::vector<bool>(static_cast<size_t>(n_slots_), true));
    std::vector<std::vector<bool>> out(static_cast<size_t>(nb),
                                       std::vector<bool>(static_cast<size_t>(n_slots_), true));
    in[0] = entry_assigned;
    out[0] = entry_assigned;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 0; b < nb; ++b) {
            std::vector<bool> o = in[b];
            for (const auto& inst : f_->blocks[b].insts)
                if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < n_slots_)
                    o[inst.idx] = true;
            out[b] = std::move(o);
            for (int s : succ[b]) {
                if (s == 0 || s < 0 || s >= nb) continue;
                for (int i = 0; i < n_slots_; ++i) {
                    bool v = in[s][i] && out[b][i];
                    if (v != in[s][i]) {
                        in[s][i] = v;
                        changed = true;
                    }
                }
            }
        }
    }

    for (int b = 0; b < nb; ++b) {
        std::vector<bool> assigned = in[b];
        for (const auto& inst : f_->blocks[b].insts) {
            if (inst.op == IrOp::SlotAddr && inst.idx >= 0 && inst.idx < n_slots_) {
                // 地址被取过：可能被间接读，保守清零。
                need[inst.idx] = true;
            } else if (inst.op == IrOp::LoadSlot && inst.idx >= 0 && inst.idx < n_slots_ &&
                       !assigned[inst.idx]) {
                need[inst.idx] = true;
            } else if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < n_slots_) {
                assigned[inst.idx] = true;
            }
        }
    }
    return need;
}

void AsmGen::emit_function(const IRFunction& f, int fi) {
        f_ = &f;
        fi_ = fi;
        gen_ = f.is_generator;
        n_slots_ = static_cast<int>(f.slot_ty.size());
        assign_vreg_slots();
        if (gen_) {
            slot_reg_.assign(f.slot_ty.size(), std::string());
            n_saved_regs_ = 1;  // %r15 帧指针
        } else {
            assign_slot_regs();
        }

        out_ += std::format("\n# ---- {} ({}) ----\n", f.name, type_name(f.ret));
        label(func_label(fi));
        line("pushq %rbp");
        line("movq %rsp, %rbp");
        if (gen_) {
            line("pushq %r15");
            line("movq %rdi, %r15");
        } else {
            for (int i = 0; i < n_saved_regs_; ++i) line(std::format("pushq {}", kSlotRegs[i]));
        }
        frame_size(n_slots_ + n_vreg_slots_);

        if (!gen_) {
            // 非参数变量清零。`x: int` 这种纯声明在 lower 里不生成任何指令，
            // 靠的是「槽初值为 0」，而栈帧里的内存是脏的——不显式清掉就会
            // 读到上一个函数留下的垃圾。参数槽下面马上被实参覆盖，跳过。
            std::vector<bool> is_param(static_cast<size_t>(n_slots_), false);
            for (const auto& p : f.params)
                if (p.slot >= 0 && p.slot < n_slots_) is_param[p.slot] = true;
            std::vector<bool> need_zero;
            const std::vector<bool>* need = nullptr;
            if (opt_ >= 2 && !f.has_handlers) {
                need_zero = slots_needing_zero();
                need = &need_zero;
            }
            for (int s = 0; s < n_slots_; ++s)
                if (!is_param[s] && (!need || (*need)[static_cast<size_t>(s)])) {
                    std::string loc = slot_op(s);
                    if (loc.starts_with("%xmm"))
                        line(std::format("xorpd {}, {}", loc, loc));
                    else
                        line(std::format("movq $0, {}", loc));
                }

            // 形参落位
            std::vector<TypePtr> ptypes;
            ptypes.reserve(f.params.size());
            for (auto& p : f.params) ptypes.push_back(p.ty);
            auto locs = classify(ptypes);
            for (size_t i = 0; i < f.params.size(); ++i) {
                const auto& p = f.params[i];
                const auto& l = locs[i];
                if (l.stack_off >= 0) {
                    line(std::format("movq {}(%rbp), %rax", 16 + l.stack_off));
                    move_to(slot_op(p.slot), "%rax");
                } else if (l.fp) {
                    line(std::format("movsd %xmm{}, {}", l.reg, slot_op(p.slot)));
                } else {
                    move_to(slot_op(p.slot), kIntArgRegs[l.reg]);
                }
            }

            // 有捕获的函数从 %r10 拿闭包环境；调用方在 call 前设置。
            if (f.env_slot >= 0) line(std::format("movq %r10, {}", slot_op(f.env_slot)));
        }

        // 被导入模块的 init：先把自己的全局根表交给 GC，再跑顶层代码。
        if (!m_->is_entry && fi == m_->entry) {
            line(std::format("leaq {}(%rip), %rdi", roots_label()));
            line(std::format("movq ${}, %rsi", local_global_count()));
            spill_fp_slots();
            line("call py23_gc_add_roots");
            reload_fp_slots();
        }

        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            cur_block_ = static_cast<int>(bi);
            label(block_label(static_cast<int>(bi)));
            gen_block(f.blocks[bi]);
        }

        bool empty_func = f.blocks.empty() || f.blocks.back().insts.empty();
        if (empty_func || !f.blocks.back().terminated()) {
            // 正常路径不会走到这里，除非 lower 漏了终止指令
            line("xorl %eax, %eax");
            emit_epilogue();
        }
    }

// ── 基本块 ──────────────────────────────────────────────────
    //
    // 别名只在块内有效，块边界一律清空——跨块的值永远走栈槽，所以
    // 不需要在块之间传播任何状态，也就不可能读到过期的寄存器。
bool AsmGen::can_fuse(const Inst& in, const Inst& nx) const {
        if (in.dst == kNoReg) return false;
        if (in.op == IrOp::StoreSlot || in.op == IrOp::StoreGlobal) return false;
        if (nx.op != IrOp::StoreSlot || nx.a != in.dst) return false;
        if (nx.idx < 0 || nx.idx >= static_cast<int>(slot_reg_.size())) return false;
        // O0 只融合到提升过的寄存器；O1 起允许直接写变量栈槽。
        if (opt_ < 1 && slot_reg_[nx.idx].empty()) return false;
        if (in.dst >= use_count_.size()) return false;
        // 只有「定义后在本块里正好被用一次」才能就地写变量槽。
        // 用两次就不行了：第二次读到的会是新值。
        return use_count_[in.dst] == 1 && !cross_block_[in.dst];
    }

bool AsmGen::can_fuse_fp_temp(const Inst& in, const Inst& nx) const {
        if (opt_ < 1) return false;
        auto simple_bin = [](const Inst& x) {
            if (x.op != IrOp::Bin || !ir_is_float(x.ty)) return false;
            return x.bop == BinKind::Add || x.bop == BinKind::Sub ||
                   x.bop == BinKind::Mul || x.bop == BinKind::Div;
        };
        if (!simple_bin(in) || !simple_bin(nx)) return false;
        if (in.dst == kNoReg || (nx.a != in.dst && nx.b != in.dst)) return false;
        if (in.dst >= use_count_.size()) return false;
        return use_count_[in.dst] == 1 && !cross_block_[in.dst];
    }

bool AsmGen::can_fuse_branch(const Inst& in, const Inst& nx) const {
        if (in.op != IrOp::Cmp || nx.op != IrOp::Branch) return false;
        if (in.dst == kNoReg || nx.a != in.dst) return false;
        if (!single_use_in_block(in.dst)) return false;
        // 浮点要走 setnp 处理 NaN，字符串走三路比较，都直接吃掉标志位，
        // 没有能直接用的条件跳转。
        TypePtr t = ty_of(in.a);
        if (ir_is_float(t)) return false;
        if (t && t->kind == TyKind::Str) return false;
        return true;
    }

// 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
void AsmGen::gen_cmp_branch(const Inst& in, const Inst& br) {
        comment(std::format("{} -> {}", in.text(), br.text()));
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        bool sa_mem = is_mem_operand(sa);
        bool sb_mem = is_mem_operand(sb);
        if (sa_mem && sb_mem) {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        } else if (sa_mem || sa[0] == '%') {
            line(std::format("cmpq {}, {}", sb, sa));
        } else {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        }
        const char* jcc = "je";
        const char* inv = "jne";
        switch (in.cop) {
            case CmpKind::Lt: jcc = "jl"; inv = "jge"; break;
            case CmpKind::Le: jcc = "jle"; inv = "jgt"; break;
            case CmpKind::Gt: jcc = "jg"; inv = "jle"; break;
            case CmpKind::Ge: jcc = "jge"; inv = "jl"; break;
            case CmpKind::Eq: jcc = "je"; inv = "jne"; break;
            case CmpKind::Ne: jcc = "jne"; inv = "je"; break;
        }
        // O2：一边就是下一个块时，用反向条件跳走另一边，省掉无条件跳。
        if (opt_ >= 2 && br.target == cur_block_ + 1) {
            line(std::format("{} {}", inv, block_label(br.target2)));
        } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
            line(std::format("{} {}", jcc, block_label(br.target)));
        } else {
            line(std::format("{} {}", jcc, block_label(br.target)));
            line(std::format("jmp {}", block_label(br.target2)));
        }
    }

void AsmGen::gen_block(const Block& b) {
        alias_.clear();
        const_val_.clear();

        // 预扫描：每个变量槽在本块内被 store 的位置，以及每个 vreg
        // 第一次被读的位置。LoadSlot 的别名只在「读之后、第一次赋值之前」
        // 成立；第一次赋值之后别名会读到新值。
        block_stores_.clear();
        block_first_use_.clear();
        for (int i = 0; i < static_cast<int>(b.insts.size()); ++i) {
            const Inst& in = b.insts[i];
            if (in.op == IrOp::StoreSlot) block_stores_[in.idx].push_back(i);
            VReg ops[3] = {in.a, in.b, in.c};
            for (VReg r : ops)
                if (r != kNoReg && !block_first_use_.contains(r)) block_first_use_[r] = i;
            for (VReg r : in.args)
                if (!block_first_use_.contains(r)) block_first_use_[r] = i;
        }

        size_t i = 0;
        while (i < b.insts.size()) {
            const Inst& in = b.insts[i];
            dst_loc_.clear();

            cur_pos_ = static_cast<int>(i);

            // 比较 + 分支连在一起时不需要把布尔值真的算出来
            if (i + 1 < b.insts.size() && can_fuse_branch(in, b.insts[i + 1])) {
                dst_loc_.clear();
                gen_cmp_branch(in, b.insts[i + 1]);
                i += 2;
                continue;
            }

            int fused_slot = -1;
            bool fp_temp = false;
            if (i + 1 < b.insts.size() && can_fuse(in, b.insts[i + 1])) {
                fused_slot = b.insts[i + 1].idx;
                // 提升到寄存器的槽直接写寄存器；没提升的槽也可以把
                // 结果直接落到变量内存，省掉「临时 vreg 槽 → 变量槽」。
                dst_loc_ = slot_reg_[fused_slot].empty() ? slot_op(fused_slot)
                                                         : slot_reg_[fused_slot];
            } else if (i + 2 < b.insts.size() &&
                       can_fuse_fp_temp(in, b.insts[i + 1]) &&
                       !can_fuse_fp_temp(b.insts[i + 1], b.insts[i + 2])) {
                // 下一条自己也要当临时结果时不能共用 %xmm1。
                dst_loc_ = "%xmm1";
                fp_temp = true;
            }

            // 常量干脆不生成指令，让使用者把它当立即数。`s += 1` 因此
            // 是 `addq $1, %rbx`，而不是「先 movq $1 到栈上再读回来」。
            // 只处理 32 位范围内的整数：x86 的 movq 立即数只有
            // 符号扩展的 32 位，更大的必须走 movabsq。
            if (fused_slot < 0 && is_deferable_const(in) && single_use_in_block(in.dst)) {
                const_val_[in.dst] = in.imm;
                ++i;
                continue;
            }

            gen_inst(in);
            dst_loc_.clear();
            if (fp_temp) alias_[in.dst] = "%xmm1";

            if (fused_slot >= 0) {
                // 融合掉的 store.slot 仍然改了变量，指向它的别名要失效
                invalidate_slot(fused_slot);
                ++i;
            }
            ++i;
        }
    }

bool AsmGen::single_use_in_block(VReg v) const {
        return v != kNoReg && v < use_count_.size() && use_count_[v] == 1 && !cross_block_[v];
    }

bool AsmGen::is_deferable_const(const Inst& in) const {
        if (in.dst == kNoReg) return false;
        if (in.op == IrOp::ConstNone) return true;
        if (in.op != IrOp::ConstInt) return false;
        return in.imm >= INT32_MIN && in.imm <= INT32_MAX;
    }

void AsmGen::invalidate_slot(int slot) {
        if (slot < 0 || slot >= static_cast<int>(slot_reg_.size())) return;
        const std::string& r = slot_reg_[slot];
        if (r.empty()) return;
        for (auto it = alias_.begin(); it != alias_.end();)
            it = (it->second == r) ? alias_.erase(it) : std::next(it);
    }

}  // namespace py23
