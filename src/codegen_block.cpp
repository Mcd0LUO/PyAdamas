// codegen_block.cpp —— 函数与基本块、融合与分支
#include "codegen_impl.h"

#include <cctype>

namespace pyadamas {

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
    // 堆槽一律从 0 起步：赋值前会先 load 旧值再 dec，内联入口也可能直接清零，
    // 这两处都要求槽初值是可安全 dec 的空指针。
    for (int i = 0; i < n_slots_; ++i)
        if (ir_is_heap(f_->slot_ty[static_cast<size_t>(i)])) need[static_cast<size_t>(i)] = true;
    return need;
}

static std::string debug_func_symbol(int fi, const std::string& name) {
    std::string s = "pyadamas_fn_" + std::to_string(fi);
    for (char c : name) s.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return s;
}

// 出参区：调用点把装不下的实参写在 rsp 下面那块地方。以前每个调用点
// 自己 subq/addq，栈指针一动，槽位地址（相对 rsp）就跟着漂；现在整函数
// 按最大用量一次性留在帧里，rsp 全程不动。这里只按「每个实参至多一个
// 八字节、结构体按 ABI 尺寸、再加隐藏指针/暂存」估上界，宁大勿小。
int AsmGen::outgoing_bytes(const IRFunction& f) const {
    int worst = 16 + target_abi().shadow_space;  // 至少留两个槽：sret 隐藏指针 + 对齐；再加影子空间
    for (const auto& b : f.blocks) {
        for (const auto& in : b.insts) {
            int need = 0;
            switch (in.op) {
                case IrOp::Call:
                case IrOp::CallPtr:
                case IrOp::CallDirect:
                case IrOp::CallRt:
                case IrOp::VCall:
                case IrOp::VCallStatic:
                case IrOp::SuperVCall:
                    // 寄存器用完的实参逐个占 8 字节；+16 兜住隐藏指针和对齐。
                    need = 8 * (static_cast<int>(in.args.size()) + 2);
                    break;
                case IrOp::CallExtern: {
                    // C 调用还多一块暂存：返回值分块暂存 + sret 指针共 48 字节。
                    const IRExtern& ex = m_->externs[static_cast<size_t>(in.idx)];
                    need = 48 + 16;
                    for (size_t i = 0; i < in.args.size(); ++i) {
                        int64_t sz = 8;
                        if (i < ex.param_abi.size() && ex.param_abi[i].size > sz)
                            sz = ex.param_abi[i].size;
                        need += static_cast<int>((sz + 7) & ~static_cast<int64_t>(7));
                    }
                    break;
                }
                default:
                    continue;
            }
            if (need > worst) worst = need;
        }
    }
    return (worst + 15) & ~15;
}

void AsmGen::emit_function(const IRFunction& f, int fi) {
        f_ = &f;
        fi_ = fi;
        gen_ = f.is_generator;
        n_slots_ = static_cast<int>(f.slot_ty.size());
        frame_rec_ = false;
        frame_owned_.clear();
        frame_owned_unknown_.clear();
        frame_scan_.clear();
        frame_scan_words_ = 0;
        cur_loc_line_ = 0;
        auto t_alloc = std::chrono::steady_clock::now();
        assign_vreg_slots();
        if (gen_) {
            slot_reg_.assign(f.slot_ty.size(), std::string());
            // 生成器帧不提升槽位，但必须清掉上一个函数留下的浮点提升状态，
            // 否则调用点的 spill/reload 会去读写一个空寄存器。
            slot_fpidx_.assign(f.slot_ty.size(), -1);
            fp_promoted_slots_.clear();
            n_saved_regs_ = 1;  // %r15 帧指针
        } else {
            assign_slot_regs();
            // 持有引用的槽要按栈槽地址登记，异常 longjmp 跳过本帧时逐个
            // dec。生成器/协程的槽在堆帧里，由帧销毁负责，不走这条路径。
            const size_t n_owned =
                std::min(f.slot_owned.size(), f.slot_ty.size());
            for (size_t s = 0; s < n_owned; ++s)
                if (f.slot_owned[s] && ir_is_heap(f.slot_ty[s])) {
                    frame_owned_.push_back(static_cast<int>(s));
                    frame_owned_unknown_.push_back(
                        s < f.slot_unknown.size() && f.slot_unknown[s]);
                }
            // 借来的 unknown 引用（unknown 局部/形参）只登记给 GC 扫描，
            // 异常展开不能释放。提升到 callee-saved 寄存器的槽不在内存里，
            // 由 GC 的寄存器扫描兜住。
            for (size_t s = 0; s < n_owned; ++s) {
                TypePtr ty = f.slot_ty[s];
                bool owned_heap = f.slot_owned[s] && ir_is_heap(ty);
                bool unk = s < f.slot_unknown.size() && f.slot_unknown[s];
                bool unk_ty = !ty || ty->kind == TyKind::Unknown;
                if (!owned_heap && (unk || unk_ty) && slot_reg_[s].empty())
                    frame_scan_.push_back(static_cast<int>(s));
            }
            // 临时区里可能流过 unknown 值：含 unknown vreg 的帧把整段
            // vreg 槽登记给 GC，别的帧一个字节都不扫。
            for (auto& vt : f.vreg_ty)
                if (!vt || vt->kind == TyKind::Unknown) {
                    frame_scan_words_ = n_vreg_slots_;
                    break;
                }
        }
        const int rec_entries =
            static_cast<int>(frame_owned_.size() + frame_scan_.size());
        const int rec_words =
            (rec_entries == 0 && frame_scan_words_ == 0) ? 0 : 4 + rec_entries;
        assign_global_vreg_regs();
        assign_block_temp_regs();
        build_fp_liveness();
        // 槽位操作数是缓存好的字符串，偏移依赖帧布局，所以先定型再建缓存。
        out_bytes_ = outgoing_bytes(f);
        layout_frame(n_slots_ + n_vreg_slots_ + rec_words);
        build_operand_caches();
        auto t_emit = std::chrono::steady_clock::now();
        alloc_ns_ += std::chrono::duration<double, std::nano>(t_emit - t_alloc).count();
        {
            size_t ninst = 0;
            for (const auto& b : f.blocks) ninst += b.insts.size();
            out_.reserve(out_.size() + ninst * 20 + 256);
        }

        out_ += std::format("\n# ---- {} ({}) ----\n", f.name, type_name(f.ret));
        label(func_label(fi));
        if (dbg_on_) {
            // 函数起点也登记一项：生成器/协程帧只保存 resume 代码地址，
            // 回溯时用它找回函数名。
            std::string start = std::format(".LdbgStart{}", fi);
            out_ += start + ":\n";
            dbg_entries_.push_back({start, fi, 0});
        }
        // 可读的函数别名，供 addr2line / nm / gdb 定位；.LfN 仍是内部跳转名。
        const std::string fname = debug_func_symbol(fi, f.name);
        out_ += "\t.p2align 4\n";
        emit_type_func(fname);
        out_ += fname + ":\n";
        // CFI（.eh_frame 里的栈展开规则）：从「rsp 指向返回地址」到「帧指针
        // 就位」的每一步都写下来。gdb 的 DWARF 展开器和 libgcc 的
        // _Unwind_Backtrace 都不解析 prologue，只认这张表——缺了它们就
        // 展不过生成出来的帧。运行时的 traceback 仍走 rbp 链，不依赖 CFI。
        cfi(".cfi_startproc");
        // 帧里没有帧指针：CFA 全程由 rsp 定义。prologue 每动一次 sp 就把
        // 偏移记下来，gdb / _Unwind_Backtrace 才能从任意 PC 还原上一层帧。
        // 被保存的寄存器相对 CFA 的偏移不随 push 变化（CFA 也跟着涨）。
        cfa_off_ = 8;
        if (gen_) {
            line("pushq %r15");
            cfa_off_ += 8;
            cfi(std::format(".cfi_def_cfa_offset {}", cfa_off_));
            cfi(".cfi_offset %r15, -16");
            line("movq %rdi, %r15");
        } else {
            for (int i = 0; i < n_saved_regs_; ++i) {
                line(std::format("pushq {}", kSlotRegs()[i]));
                cfa_off_ += 8;
                cfi(std::format(".cfi_def_cfa_offset {}", cfa_off_));
                cfi(std::format(".cfi_offset {}, -{}", kSlotRegs()[i], cfa_off_));
            }
        }
        frame_alloc();

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
                        emit2("xorpd", loc, loc);
                    else
                        emit2("movq", "$0", loc);
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
                    // 栈上实参在返回地址上面：rsp + 帧长 + 8 + 偏移。
                    line(std::format("movq {}(%rsp), %rax", frame_bytes_ + 8 + l.stack_off));
                    move_to(slot_op(p.slot), "%rax");
                } else if (l.fp) {
                    line(std::format("movsd %xmm{}, {}", l.reg, slot_op(p.slot)));
                } else {
                    move_to(slot_op(p.slot), arg_reg_i(l.reg));
                }
            }

            // 有捕获的函数从 %r10 拿闭包环境；调用方在 call 前设置。
            if (f.env_slot >= 0) emit2("movq", "%r10", slot_op(f.env_slot));

            // 压帧记录。槽地址在 %rsp 上是固定的（rsp 全程不动），异常
            // 展开时按地址读最新值即可；正常返回由 emit_epilogue 弹栈。
            if (rec_words > 0) {
                frame_rec_ = true;
                line("movq pa_frame_top(%rip), %rax");
                emit2("movq", "%rax", rec_mem(0));
                emit_imm_op("movq", static_cast<int64_t>(rec_entries), rec_mem(1));
                if (frame_scan_words_ > 0) {
                    // 临时区从第一个 vreg 槽开始，往高地址扫 scan_words 个字。
                    line(std::format("leaq {}(%rsp), %rax", vreg_slot_off(0)));
                    emit2("movq", "%rax", rec_mem(2));
                } else {
                    emit_imm_op("movq", static_cast<int64_t>(0), rec_mem(2));
                }
                emit_imm_op("movq", static_cast<int64_t>(frame_scan_words_), rec_mem(3));
                int ri = 4;
                for (size_t i = 0; i < frame_owned_.size(); ++i, ++ri) {
                    line(std::format("leaq {}, %rax", smem(frame_owned_[i])));
                    // 低位标记：该槽可能装着非堆值，展开时走注册表释放。
                    if (frame_owned_unknown_[i]) line("orq $1, %rax");
                    emit2("movq", "%rax", rec_mem(ri));
                }
                for (int s : frame_scan_) {
                    line(std::format("leaq {}, %rax", smem(s)));
                    // bit1：只给 GC 扫描，异常展开不释放。
                    line("orq $2, %rax");
                    emit2("movq", "%rax", rec_mem(ri++));
                }
                line(std::format("leaq {}, %rax", rec_mem(0)));
                line("movq %rax, pa_frame_top(%rip)");
            }
        }

        // 被导入模块的 init：先把自己的全局根表交给 GC，再跑顶层代码。
        if (!m_->is_entry && fi == m_->entry) {
            line(std::format("leaq {}(%rip), %rdi", roots_label()));
            emit_imm_op("movq", local_global_count(), arg_reg_i(1));
            spill_fp_slots();
            line("call pa_gc_add_roots");
            reload_fp_slots();
        }

        // 循环头对齐：热循环的入口落在 16 字节边界上，取指和分支预测
        // 都更稳定。回边目标就是循环头，不需要额外的支配分析。
        std::vector<bool> loop_header(f.blocks.size(), false);
        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            const auto& blk = f.blocks[bi];
            if (blk.insts.empty()) continue;
            const Inst& term = blk.insts.back();
            auto mark = [&](int t) {
                if (t >= 0 && t <= static_cast<int>(bi) && t < static_cast<int>(f.blocks.size()))
                    loop_header[static_cast<size_t>(t)] = true;
            };
            if (term.op == IrOp::Jump) mark(term.target);
            else if (term.op == IrOp::Branch) {
                mark(term.target);
                mark(term.target2);
            }
        }
        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            cur_block_ = static_cast<int>(bi);
            if (loop_header[bi]) line(".p2align 4");
            label(block_label(static_cast<int>(bi)));
            gen_block(f.blocks[bi]);
        }

        bool empty_func = f.blocks.empty() || f.blocks.back().insts.empty();
        if (empty_func || !f.blocks.back().terminated()) {
            // 正常路径不会走到这里，除非 lower 漏了终止指令
            line("xorl %eax, %eax");
            emit_epilogue();
        }
        if (dbg_on_) {
            std::string end = std::format(".LdbgEnd{}", fi);
            out_ += end + ":\n";
            dbg_entries_.push_back({end, -1, 0});
        }
        cfi(".cfi_endproc");
        emit_size_func(fname);
        emit_ns_ += std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - t_emit)
                        .count();
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
        // 字符串走三路比较，没有能直接用的条件跳转；浮点交给
        // gen_cmp_branch_fp（有序比较用 ja/jae 把 NaN 归到假分支，== 再补 jp）。
        TypePtr t = ty_of(in.a);
        if (t && t->kind == TyKind::Str) return false;
        return true;
    }

// 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
void AsmGen::gen_cmp_branch(const Inst& in, const Inst& br) {
        if (ir_is_float(ty_of(in.a))) {
            gen_cmp_branch_fp(in, br);
            return;
        }
        comment(std::format("{} -> {}", in.text(), br.text()));
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        bool sa_mem = is_mem_operand(sa);
        bool sb_mem = is_mem_operand(sb);
        if (sa_mem && sb_mem) {
            emit2("movq", sa, "%rax");
            emit2("cmpq", sb, "%rax");
        } else if (sa_mem || sa[0] == '%') {
            emit2("cmpq", sb, sa);
        } else {
            emit2("movq", sa, "%rax");
            emit2("cmpq", sb, "%rax");
        }
        const char* jcc = "je";
        const char* inv = "jne";
        switch (in.cop) {
            case CmpKind::Lt: jcc = "jl"; inv = "jge"; break;
            case CmpKind::Le: jcc = "jle"; inv = "jg"; break;
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
            emit_jmp("jmp", block_label(br.target2));
        }
    }

void AsmGen::gen_cmp_branch_fp(const Inst& in, const Inst& br) {
        comment(std::format("{} -> {}", in.text(), br.text()));
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        const char* jcc = "je";
        const char* inv = "jne";
        bool ordered = true;  // 无序（NaN）时 Python 的比较结果为假
        bool swap = false;
        // 四种有序比较换个操作数顺序就能把 NaN 归到「不满足」那一侧：
        // ucomisd 对无序置 CF=1，ja/jae 都不会跳，正好落到假分支，于是
        // 不用再补一条 jp（内层循环里省一条分支）。
        switch (in.cop) {
            case CmpKind::Lt: swap = true; jcc = "ja"; inv = "jbe"; ordered = false; break;
            case CmpKind::Le: swap = true; jcc = "jae"; inv = "jb"; ordered = false; break;
            case CmpKind::Gt: jcc = "ja"; inv = "jbe"; ordered = false; break;
            case CmpKind::Ge: jcc = "jae"; inv = "jb"; ordered = false; break;
            case CmpKind::Eq: jcc = "je"; inv = "jne"; break;
            case CmpKind::Ne: jcc = "jne"; inv = "je"; ordered = false; break;  // NaN 由下面的特例先跳真分支
        }
        if (swap) {
            std::string t = sa;
            sa = sb;
            sb = t;
        }
        // ucomisd 的第二个操作数是 xmm；左操作数不在 xmm 里就先搬进 %xmm0。
        if (sa.starts_with("%xmm")) {
            emit2("ucomisd", sb, sa);
        } else {
            emit2("movsd", sa, "%xmm0");
            emit2("ucomisd", sb, "%xmm0");
        }
        // NaN 时 PF=1：!= 的结果应为真，不能直接 je。单独把无序跳到真分支，
        // 再按 ZF 分真/假；其它比较的 jp 在下面按 ordered 处理。
        if (in.cop == CmpKind::Ne) {
            if (opt_ >= 2 && br.target == cur_block_ + 1) {
                // 真块就是下一块：NaN 和不等都落到 fallthrough，相等跳假。
                line(std::format("jp {}", block_label(br.target)));
                line(std::format("je {}", block_label(br.target2)));
            } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
                // 假块就是下一块：NaN 和不等跳真，相等 fallthrough 到假。
                line(std::format("jp {}", block_label(br.target)));
                line(std::format("jne {}", block_label(br.target)));
            } else {
                line(std::format("jp {}", block_label(br.target)));
                line(std::format("jne {}", block_label(br.target)));
                emit_jmp("jmp", block_label(br.target2));
            }
            return;
        }
        // 有序比较的 ja/jae 本身已排除无序，ordered=false 时不需要 jp。
        // 之后的落点选择与整型版本一致，利用「目标块就是下一块」省掉无条件跳。
        if (opt_ >= 2 && br.target == cur_block_ + 1) {
            if (ordered) emit_jmp("jp", block_label(br.target2));
            line(std::format("{} {}", inv, block_label(br.target2)));
        } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
            if (ordered) emit_jmp("jp", block_label(br.target2));
            line(std::format("{} {}", jcc, block_label(br.target)));
        } else {
            if (ordered) emit_jmp("jp", block_label(br.target2));
            line(std::format("{} {}", jcc, block_label(br.target)));
            emit_jmp("jmp", block_label(br.target2));
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

        // 每条指令执行后仍存活的浮点提升槽（live-after），调用点只保存
        // 这些槽。从块末尾倒推，遇到 store 清位、load 置位。
        std::vector<uint8_t> fp_after;
        if (!fp_promoted_slots_.empty() && !fp_live_off_ &&
            cur_block_ >= 0 && static_cast<size_t>(cur_block_) < fp_live_out_.size()) {
            fp_after.assign(b.insts.size(), 0);
            uint8_t cur = fp_live_out_[static_cast<size_t>(cur_block_)];
            for (int k = static_cast<int>(b.insts.size()) - 1; k >= 0; --k) {
                fp_after[static_cast<size_t>(k)] = cur;
                const Inst& x = b.insts[static_cast<size_t>(k)];
                if (x.idx >= 0 && x.idx < static_cast<int>(slot_fpidx_.size())) {
                    int fi = slot_fpidx_[static_cast<size_t>(x.idx)];
                    if (fi >= 0) {
                        if (x.op == IrOp::StoreSlot)
                            cur = static_cast<uint8_t>(cur & ~(1u << fi));
                        else if (x.op == IrOp::LoadSlot)
                            cur = static_cast<uint8_t>(cur | (1u << fi));
                    }
                }
            }
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
            // 拷贝合并：目标值和左操作数被分到同一个临时寄存器时，直接原地
            // 算就行，不必再借 %xmm1 中转（否则还要多一条 movsd）。
            bool dst_coalesced =
                in.dst != kNoReg && in.a != kNoReg &&
                in.dst < static_cast<VReg>(vreg_reg_.size()) &&
                in.a < static_cast<VReg>(vreg_reg_.size()) &&
                !vreg_reg_[in.dst].empty() && vreg_reg_[in.dst] == vreg_reg_[in.a];
            if (i + 1 < b.insts.size() && can_fuse(in, b.insts[i + 1])) {
                fused_slot = b.insts[i + 1].idx;
                // 提升到寄存器的槽直接写寄存器；没提升的槽也可以把
                // 结果直接落到变量内存，省掉「临时 vreg 槽 → 变量槽」。
                dst_loc_ = slot_reg_[fused_slot].empty() ? slot_op(fused_slot)
                                                         : slot_reg_[fused_slot];
            } else if (!dst_coalesced && i + 2 < b.insts.size() &&
                       can_fuse_fp_temp(in, b.insts[i + 1]) &&
                       !can_fuse_fp_temp(b.insts[i + 1], b.insts[i + 2])) {
                // 下一条自己也要当临时结果时不能共用 %xmm1。
                dst_loc_ = "%xmm1";
                fp_temp = true;
            }

            // 常量干脆不生成指令，让使用者把它当立即数。`s += 1` 因此
            // 是 `addq $1, %rbx`，而不是「先 movq $1 到栈上再读回来」。
            // 只处理 32 位范围内的整数：x86 的 movq 立即数只有
            // 符号扩展的 32 位，更大的必须走 movabsq。跨块常量由
            // assign_global_vreg_regs 记进 global_imm_，整函数范围生效。
            if (fused_slot < 0 && is_deferable_const(in) &&
                (single_use_in_block(in.dst) || global_imm_.contains(in.dst))) {
                const_val_[in.dst] = std::format("${}", in.imm);
                ++i;
                continue;
            }

            if (!fp_after.empty()) {
                fp_spill_mask_.assign(fp_promoted_slots_.size(), 0);
                uint8_t m = fp_after[i];
                for (size_t j = 0; j < fp_spill_mask_.size(); ++j)
                    fp_spill_mask_[j] = static_cast<char>((m >> j) & 1);
                fp_mask_valid_ = true;
            }
            gen_inst(in);
            fp_mask_valid_ = false;
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

}  // namespace pyadamas
