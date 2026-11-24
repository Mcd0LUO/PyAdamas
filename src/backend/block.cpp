// backend/block.cpp —— 函数与基本块、融合与分支
#include "common/block_bitset.h"
#include "backend/impl.h"

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

    // 槽级「入口是否已赋值」数据流用 64 位字位集（原来是每块一个
    // std::vector<bool>，每轮还要整块拷贝一次：块数 × 槽数 × 轮数的位操作
    // 加上分配，perf 里 slots_needing_zero 占 ~2%）。判定顺序与原实现
    // 逐块一致，不动点结果逐位相同。
    const size_t nsl = static_cast<size_t>(n_slots_);
    BlockBits entry_assigned(nsl, false);
    for (const auto& p : f_->params)
        if (p.slot >= 0 && p.slot < n_slots_) entry_assigned.set(static_cast<size_t>(p.slot));
    if (f_->env_slot >= 0 && f_->env_slot < n_slots_)
        entry_assigned.set(static_cast<size_t>(f_->env_slot));

    std::vector<BlockBits> blk_store(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        blk_store[static_cast<size_t>(b)].reset(nsl);
        for (const auto& inst : f_->blocks[b].insts)
            if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < n_slots_)
                blk_store[static_cast<size_t>(b)].set(static_cast<size_t>(inst.idx));
    }

    std::vector<BlockBits> in(static_cast<size_t>(nb), BlockBits(nsl, true));
    std::vector<BlockBits> out(static_cast<size_t>(nb), BlockBits(nsl, true));
    in[0].assign(entry_assigned);
    out[0].assign(entry_assigned);

    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 0; b < nb; ++b) {
            BlockBits o(nsl, false);
            o.assign(in[static_cast<size_t>(b)]);
            o.or_with(blk_store[static_cast<size_t>(b)]);
            out[static_cast<size_t>(b)].assign(o);
            for (int s : succ[b]) {
                if (s == 0 || s < 0 || s >= nb) continue;
                BlockBits& ins = in[static_cast<size_t>(s)];
                BlockBits want = ins;
                // in[s] &= out[b]，逐位与；有位置被清掉才算变化。
                uint64_t* w = want.words();
                const uint64_t* ob = o.words();
                for (size_t i = 0; i < want.word_count(); ++i) w[i] &= ob[i];
                if (!(want == ins)) {
                    ins.assign(want);
                    changed = true;
                }
            }
        }
    }

    for (int b = 0; b < nb; ++b) {
        BlockBits assigned(nsl, false);
        assigned.assign(in[static_cast<size_t>(b)]);
        for (const auto& inst : f_->blocks[b].insts) {
            if (inst.op == IrOp::SlotAddr && inst.idx >= 0 && inst.idx < n_slots_) {
                // 地址被取过：可能被间接读，保守清零。
                need[inst.idx] = true;
            } else if (inst.op == IrOp::LoadSlot && inst.idx >= 0 && inst.idx < n_slots_ &&
                       !assigned.test(static_cast<size_t>(inst.idx))) {
                need[inst.idx] = true;
            } else if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < n_slots_) {
                assigned.set(static_cast<size_t>(inst.idx));
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

// ── GC 保守扫描范围（临时区） ──────────────────────────────────
//
// 判据里只有一条算「证明」：静态类型是 unknown（或没有类型）的字可能装
// 着 unknown 值——unknown 的机器表示既可能是整数，也可能是借来的堆指针，
// 只有保守扫描能兜住。其余类型：
//
//   - int / bool / float / None / Optional[标量]：机器表示不可能是堆指针；
//     float 的位模式即使撞上堆地址也只能造成误报，不扫只会少留垃圾；
//   - 堆类型（str/list/dict/类实例……）：引用要么是 owned（在临时栈 /
//     槽 / 容器里，GC 从精确根可达），要么是 borrowed 而其持有者本身从根
//     可达，所以不需要按字扫——这也是「纯类型化帧整帧不扫」的既有前提；
//   - BoundMethod / 非 C 结构体指针：机器表示是指针但不归 RC 管，静态上
//     说不清谁持有它，一律标 1（宁可多扫）；
//   - vreg 槽复用：一个字被多个 vreg 用过时，只要任一 vreg 需要扫，这个
//     字就标 1（按字取并集）。
//
// 位图是超集，宁多勿少：少扫一个真正含指针的字 = 提前释放 = UAF，比多留
// 一个垃圾严重得多。收窄不到一半时直接不发位图，让运行时按整段扫。
void AsmGen::build_frame_scan_info(const IRFunction& f) {
        frame_.scan_words = 0;
        frame_.scan_mask.clear();
        if (gen_ || n_vreg_slots_ <= 0 || vreg_slot_.empty()) return;
        const size_t nwords = static_cast<size_t>(n_vreg_slots_);
        std::vector<uint64_t> mask((nwords + 63) / 64, 0);
        // 该类型在临时区里是否可能留下一个「只能靠保守扫描看见」的指针。
        auto maybe_pointer = [](const TypePtr& t, auto&& self) -> bool {
            if (!t) return true;
            switch (t->kind) {
                case TyKind::Unknown:
                    return true;
                // Optional 的机器表示是「内层值或 0」：内层是 unknown 时
                // 可能是借来的指针。
                case TyKind::Optional:
                    return self(t->sub.empty() ? TypePtr() : t->sub[0], self);
                case TyKind::BoundMethod:
                    return true;
                case TyKind::StructPtr:
                    return !(t->st && t->st->c_struct);
                default:
                    return false;
            }
        };
        int64_t nbits = 0;
        for (size_t v = 0; v < f.vreg_ty.size(); ++v) {
            if (!maybe_pointer(f.vreg_ty[v], maybe_pointer)) continue;
            // 槽号越界说明布局和 vreg 表对不上：这时不敢收窄，直接按整段扫。
            if (v >= vreg_slot_.size()) return;
            int s = vreg_slot_[v];
            if (s < 0 || s >= static_cast<int>(nwords)) return;
            uint64_t& w = mask[static_cast<size_t>(s) >> 6];
            uint64_t bit = static_cast<uint64_t>(1) << (s & 63);
            if (!(w & bit)) {
                w |= bit;
                ++nbits;
            }
        }
        if (nbits == 0) return;  // 这一帧的临时区零扫描
        frame_.scan_words = n_vreg_slots_;
        // 收窄不到一半就不发位图：整段扫的每字成本更低。
        if (nbits * 2 > static_cast<int64_t>(nwords)) return;
        frame_.scan_mask = std::move(mask);
}

void AsmGen::emit_function(const IRFunction& f, int fi) {
        f_ = &f;
        fi_ = fi;
        gen_ = f.is_generator;
        n_slots_ = static_cast<int>(f.slot_ty.size());
        frame_rec_ = false;
        frame_.owned.clear();
        frame_.owned_unknown.clear();
        frame_.scan.clear();
        frame_.scan_words = 0;
        frame_.scan_mask.clear();
        cur_loc_line_ = 0;
        auto t_alloc = std::chrono::steady_clock::now();
        assign_vreg_slots();
        // P3：平行 tag 字分配（设计 §3.2）。每个 unknown vreg / unknown 命名
        // 槽一个专属字（不复用，避免陈旧 tag）。生成器堆帧里的命名槽不加 tag
        // （装载得 UNK 回退探针）；vreg 在常规帧，生成器也分配（P3-1b：
        // 清零挪到 g.entry 块头，每次 resume 都走；GenSent 写 UNK）。
        tag_.vreg_tag.assign(f_->vreg_ty.size(), -1);
        tag_.slot_tag.assign(f_->slot_ty.size(), -1);
        tag_.n_tag_words = 0;
        if (tag_.tags_enabled) {
            if (!gen_)
                for (size_t s = 0; s < f_->slot_ty.size(); ++s)
                    if (is_unk_ty(f_->slot_ty[s]))
                        tag_.slot_tag[s] = tag_.n_tag_words++;
            for (size_t v = 0; v < f_->vreg_ty.size(); ++v)
                if (is_unk_ty(f_->vreg_ty[v]))
                    tag_.vreg_tag[static_cast<VReg>(v)] = tag_.n_tag_words++;
        }
        if (gen_) {
            ra_.slot_reg.assign(f.slot_ty.size(), std::string());
            // 生成器帧不提升槽位，但必须清掉上一个函数留下的浮点提升状态，
            // 否则调用点的 spill/reload 会去读写一个空寄存器。
            ra_.slot_fpidx.assign(f.slot_ty.size(), -1);
            ra_.fp_promoted_slots.clear();
            n_saved_regs_ = 1;  // %r15 帧指针
        } else {
            // **先算「哪些槽必须留在内存」，再分配寄存器。**
            //
            // 顺序不能反：GC 要靠帧记录里的栈槽地址找根，所以「会被 GC
            // 扫描的槽」绝不能提升。而一个槽是否被扫描取决于它的静态类型
            // （堆槽 / unknown 槽）和是否被取地址 —— 全是分配前就能确定的
            // 信息，不需要看 ra_.slot_reg。
            //
            // 之前的写法是「先分配寄存器，再把没提升的槽登记给 GC」，于是
            // unknown 槽被提升后既不在栈槽里、也不在扫描清单里，GC 读到
            // 序言清零的 0，对象被提前回收 —— ASan 实测 SEGV（x_min.py）。
            gc_scan_slot_.assign(f.slot_ty.size(), 0);
            {
                std::vector<bool> addr_taken(f.slot_ty.size(), false);
                for (const auto& bb : f.blocks)
                    for (const auto& in : bb.insts)
                        if (in.op == IrOp::SlotAddr && in.idx >= 0 &&
                            in.idx < static_cast<int>(f.slot_ty.size()))
                            addr_taken[static_cast<size_t>(in.idx)] = true;
                for (size_t s = 0; s < f.slot_ty.size(); ++s) {
                    TypePtr ty = f.slot_ty[s];
                    // 堆槽：异常展开要按地址读它来 dec。
                    // unknown 槽：可能装着堆指针，GC 要靠它找根。
                    const bool heap = ir_is_heap(ty);
                    const bool unk = !ty || ty->kind == TyKind::Unknown;
                    if (heap || unk) gc_scan_slot_[s] = 1;
                    (void)addr_taken;
                }
            }
            assign_slot_regs();

            // 持有引用的槽要按栈槽地址登记，异常 longjmp 跳过本帧时逐个
            // dec。生成器/协程的槽在堆帧里，由帧销毁负责，不走这条路径。
            gc_scan_slot_.assign(f.slot_ty.size(), 0);
            const size_t n_owned =
                std::min(f.slot_owned.size(), f.slot_ty.size());
            // **被提升到寄存器的槽不能进 owned 清单。**
            //
            // 帧记录登记的是栈槽地址，GC / 异常展开从那里读值。槽一旦
            // 提升到寄存器，栈槽里就是旧值（常常是序言清零留下的 0）：
            //   * GC 读到 0 → 不标记 → 对象被提前回收（use-after-free）；
            //   * 异常展开读到 0 → 不 dec → 泄漏。
            //
            // ASan 实测（x_min.py）：s1（u，200 元素列表）被提升到 %r12，
            // 栈槽 48(%rsp) 自序言清零后再没写过。major GC 时读到 0，
            // u 被回收，之后 `index.get` 从已释放的列表取元素得到 -26，
            // 传给 pa_rc_temp 当场 SEGV。
            //
            // 提升槽由 gc_scan_registers 兜底（见下面 frame_.scan 的注释）。
            for (size_t s = 0; s < n_owned; ++s) {
                TypePtr ty = f.slot_ty[s];
                bool unk_ty = !ty || ty->kind == TyKind::Unknown;
                // 0.323：unknown 槽也按 slot_owned 记账（写入点 retain、
                // 出口释放），所以「持有引用」的清单不再只限堆槽。异常
                // 展开时这些槽要按注册表 dec，和堆槽一样登记。
                if (f.slot_owned[s] && (ir_is_heap(ty) || unk_ty) &&
                    ra_.slot_reg[s].empty()) {
                    if (s < gc_scan_slot_.size()) gc_scan_slot_[s] = 1;
                    frame_.owned.push_back(static_cast<int>(s));
                    frame_.owned_unknown.push_back(
                        unk_ty || (s < f.slot_unknown.size() && f.slot_unknown[s]));
                }
            }
            // 借来的 unknown 引用（unknown 形参/未标记 owned 的槽）只登记
            // 给 GC 扫描，异常展开不能释放。提升到 callee-saved 寄存器的
            // 槽不在内存里，由 GC 的寄存器扫描兜住。
            for (size_t s = 0; s < n_owned; ++s) {
                TypePtr ty = f.slot_ty[s];
                bool unk = s < f.slot_unknown.size() && f.slot_unknown[s];
                bool unk_ty = !ty || ty->kind == TyKind::Unknown;
                bool owned = f.slot_owned[s] && (ir_is_heap(ty) || unk_ty);
                if (!owned && (unk || unk_ty) && ra_.slot_reg[s].empty()) {
                    frame_.scan.push_back(static_cast<int>(s));
                    if (s < gc_scan_slot_.size()) gc_scan_slot_[s] = 1;
                }
            }
            // 临时区里可能流过 unknown 值：只有「可能装指针」的帧才登记
            // 临时区，纯 int/float 帧和纯类型化帧一个字节都不扫。
            build_frame_scan_info(f);
        }
        const int rec_entries =
            static_cast<int>(frame_.owned.size() + frame_.scan.size());
        // 记录头 6 个字：prev、n、临时区起点、临时区字数、临时区位图、本帧 CFA。
        const int rec_words =
            (rec_entries == 0 && frame_.scan_words == 0) ? 0 : 6 + rec_entries;
        assign_global_vreg_regs();
        assign_block_temp_regs();
        build_fp_liveness();
        // 槽位操作数是缓存好的字符串，偏移依赖帧布局，所以先定型再建缓存。
        frame_.out_bytes = outgoing_bytes(f);
        if (std::getenv("PYADAMAS_FRAME_DBG"))
            std::fprintf(stderr, "[frame] %-32s gen=%d n_saved=%d out=%d slots=%d\n",
                         f.name.c_str(), static_cast<int>(gen_), n_saved_regs_, frame_.out_bytes,
                         n_slots_ + n_vreg_slots_ + rec_words);
        layout_frame(n_slots_ + n_vreg_slots_ + tag_.n_tag_words + rec_words);
        build_operand_caches();
        // 零开销异常（Z2-b2）：预扫本函数的 EH 标记、分配标签。fname 在这里
        // 就要算出来——LSDA 的偏移基准是 FDE 起点（.cfi_startproc 的地址），
        // 而 .LfN 与它之间隔着 .p2align 的填充，不能拿 .LfN 当基准。
        const std::string fname = debug_func_symbol(fi, f.name);
        prepare_eh_plan(f, fname);
        auto t_emit = std::chrono::steady_clock::now();
        alloc_ns_ += std::chrono::duration<double, std::nano>(t_emit - t_alloc).count();
        {
            size_t ninst = 0;
            for (const auto& b : f.blocks) ninst += b.insts.size();
            emit_.out.reserve(emit_.out.size() + ninst * 20 + 256);
        }

        emit_.out += std::format("\n# ---- {} ({}) ----\n", f.name, type_name(f.ret));
        label(func_label(fi));
        if (dbg_on_) {
            // 函数起点也登记一项：生成器/协程帧只保存 resume 代码地址，
            // 回溯时用它找回函数名。
            std::string start = num_label(".LdbgStart", fi);
            emit_.out += start + ":\n";
            dbg_entries_.push_back({start, fi, 0});
        }
        // 可读的函数别名，供 addr2line / nm / gdb 定位；.LfN 仍是内部跳转名。
        // （fname 在 prepare_eh_plan 之前就算好了，见上面的注释。）
        emit_.out += "\t.p2align 5\n";
        if (inline_opt_) label(func_label(fi) + "e");  // P0 对齐入口别名（docs/inline-devirt-design.md §2）
        emit_type_func(fname);
        emit_.out += fname + ":\n";
        // CFI（.eh_frame 里的栈展开规则）：从「rsp 指向返回地址」到「帧指针
        // 就位」的每一步都写下来。gdb 的 DWARF 展开器和 libgcc 的
        // _Unwind_Backtrace 都不解析 prologue，只认这张表——缺了它们就
        // 展不过生成出来的帧。运行时 traceback 同样走 CFI（rt_debug.c 用
        // _Unwind_Backtrace 按 .eh_frame 逐帧还原，不依赖帧指针）。
        cfi(".cfi_startproc");
        // 栈对齐自检（PYADAMAS_ALIGN_CHECK=1）。
        //
        // SysV 要求函数入口 rsp ≡ 8 (mod 16)。生成代码里若有间接调用/尾跳
        // 破坏了这个不变量，会在这里 ud2 当场暴露 —— 比等运行时函数里的
        // movaps 崩在别处好定位得多（见 docs/stack-alignment.md）。
        if (std::getenv("PYADAMAS_ALIGN_CHECK")) {
            const std::string aok = fresh_label("alignok");
            line("movq %rsp, %rax");
            line("andl $15, %eax");
            line("cmpl $8, %eax");
            emit_jmp("je", aok);
            line("ud2");
            label(aok);
        }
        // 零开销异常：personality / LSDA 必须落在 .cfi_startproc 与
        // .cfi_endproc 之间。空计划（老路径函数）什么也不发。
        emit_eh_directives_for_current();
        // P3-1b：直调 tag ABI 双入口（发射细节见 backend/tag.cpp）。
        if (func_ptag_words() > 0) emit_ptag_entry(fi);
        // 帧里没有帧指针：CFA 全程由 rsp 定义。prologue 每动一次 sp 就把
        // 偏移记下来，gdb / _Unwind_Backtrace 才能从任意 PC 还原上一层帧。
        // 被保存的寄存器相对 CFA 的偏移不随 push 变化（CFA 也跟着涨）。
        frame_.cfa_off = 8;
        if (gen_) {
            emit1(MOp::kPushq, kR15);
            frame_.cfa_off += 8;
            cfi_cfa_offset();
            cfi(".cfi_offset %r15, -16");
            // 生成器/协程 resume 的第一个实参是生成器对象，按 ABI 位置取。
            emit2(MOp::kMovq, arg_reg_i(0), kR15);
        } else {
            for (int i = 0; i < n_saved_regs_; ++i) {
                emit1(MOp::kPushq, mreg_from_name(kSlotRegs()[i]));
                frame_.cfa_off += 8;
                cfi_cfa_offset();
                cfi_offset(kSlotRegs()[i], frame_.cfa_off);
            }
        }
        frame_alloc();
        // P3：tag 区清零（UNK=0 探针回退）。非生成器在常规序言；生成器
        // 这里就是 g.entry 块头——每次 resume 都经过，垃圾 tag 不会跨
        // resume 残留（P3-1b，118/124/128/133 回归钉住）。
        for (int w = 0; w < tag_.n_tag_words; ++w)
            emit_imm(MOp::kMovq, 0, op_mem(kRsp, tag_off(w)));

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
                    const MOperand& loc = slot_mop(s);
                    if (loc.kind == MOperandKind::Reg && loc.reg >= kXmm0 && loc.reg <= kXmm15)
                        emit2(MOp::kXorpd, loc, loc);
                    else
                        emit_imm(MOp::kMovq, 0, loc);
                }
            // P3-1b：tag 入口装载形参 tag（%r11==0 → 保持 UNK 回退）。
            // P2（type_tags）：没有带 tag 形参的函数也要清一次侧信道 ——
            // 被调若不读（无 tag 形参），调用点的 stale 值可能被之后的
            // 运行时回调（vthread/生成器 resume）误读；每次进入生成代码
            // 函数都归零，异常展开另在 landing pad / setjmp 返回点清理。
            if (tag_.type_tags && func_ptag_words() == 0) {
                line("movq $0, pa_ptag_in(%rip)");
                line("movq $0, pa_ptag_len(%rip)");
            }
            if (func_ptag_words() > 0) emit_ptag_param_load();

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
                    emit2(MOp::kMovq, op_mem(kRsp, frame_.bytes + 8 + l.stack_off), kRax);
                    move_to(slot_mop(p.slot), mreg_operand(kRax));
                } else if (l.fp) {
                    emit2(MOp::kMovsd, static_cast<MReg>(kXmm0 + l.reg), slot_mop(p.slot));
                } else {
                    move_to(slot_mop(p.slot), mreg_operand(mreg_from_name(arg_reg_i(l.reg))));
                }
            }

            // 有捕获的函数从 %r10 拿闭包环境；调用方在 call 前设置。
            if (f.env_slot >= 0) emit2(MOp::kMovq, kR10, slot_mop(f.env_slot));

            // 压帧记录。槽地址在 %rsp 上是固定的（rsp 全程不动），异常
            // 展开时按地址读最新值即可；正常返回由 emit_epilogue 弹栈。
            if (rec_words > 0) {
                frame_rec_ = true;
                line("movq pa_frame_top(%rip), %rax");
                emit2(MOp::kMovq, kRax, rec_mem(0));
                emit_imm(MOp::kMovq, static_cast<int64_t>(rec_entries), rec_mem(1));
                if (frame_.scan_words > 0) {
                    // 临时区从第一个 vreg 槽开始，往高地址扫 scan_words 个字。
                    emit2(MOp::kLeaq, op_mem(kRsp, vreg_slot_off(0)), kRax);
                    emit2(MOp::kMovq, kRax, rec_mem(2));
                } else {
                    emit_imm(MOp::kMovq, static_cast<int64_t>(0), rec_mem(2));
                }
                emit_imm(MOp::kMovq, static_cast<int64_t>(frame_.scan_words), rec_mem(3));
                if (!frame_.scan_mask.empty()) {
                    // 位图是静态只读数据，地址编译期就能定。
                    emit2(MOp::kLeaq, op_mem_sym(num_label(".Lscanmask", fi), "%rip"), kRax);
                    emit2(MOp::kMovq, kRax, rec_mem(4));
                } else {
                    emit_imm(MOp::kMovq, static_cast<int64_t>(0), rec_mem(4));
                }
                int ri = 6;
                for (size_t i = 0; i < frame_.owned.size(); ++i, ++ri) {
                    emit2(MOp::kLeaq, smem_mop(frame_.owned[i]), kRax);
                    // 低位标记：该槽可能装着非堆值，展开时走注册表释放。
                    if (frame_.owned_unknown[i]) line("orq $1, %rax");
                    emit2(MOp::kMovq, kRax, rec_mem(ri));
                }
                for (int s : frame_.scan) {
                    emit2(MOp::kLeaq, smem_mop(s), kRax);
                    // bit1：只给 GC 扫描，异常展开不释放。
                    line("orq $2, %rax");
                    emit2(MOp::kMovq, kRax, rec_mem(ri++));
                }
                // 本帧 CFA（= rsp + frame_.bytes + 8，与 .cfi_def_cfa 同一口径）：
                // 表驱动展开在 phase 2 的 handler 帧人格调用里按它批量释放更深
                // 帧的记录，所以必须在进链之前写好。
                emit2(MOp::kLeaq, op_mem(kRsp, frame_.bytes + 8), kRax);
                emit2(MOp::kMovq, kRax, rec_mem(5));
                emit2(MOp::kLeaq, rec_mem(0), kRax);
                line("movq %rax, pa_frame_top(%rip)");
            }
        }

        // 被导入模块的 init：先把自己的全局根表交给 GC，再跑顶层代码。
        if (!m_->is_entry && fi == m_->entry) {
            // 根表指针也是实参：Win64 的最内层实参寄存器是 %rcx。
            emit2(MOp::kLeaq, op_mem_sym(roots_label(), "%rip"), arg_reg_i(0));
            emit_imm(MOp::kMovq, static_cast<int64_t>(local_global_count()), arg_reg_i(1));
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
            // 32 字节（DSB uop 缓存的窗口粒度）：16 字节对齐只稳定取指，
            // 上游代码尺寸变化仍会把热循环的 uop 挤出 DSB（obj_dispatch
            // 实测 MITE uops +176M、cycles +9%）；32 字节对齐后大部分恢复。
            // 64 字节无进一步收益（DSB 组冲突不由入口对齐 alone 解决）。
            if (loop_header[bi]) line(".p2align 5");
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
            std::string end = num_label(".LdbgEnd", fi);
            emit_.out += end + ":\n";
            dbg_entries_.push_back({end, -1, 0});
        }
        cfi(".cfi_endproc");
        emit_size_func(fname);
        if (!frame_.scan_mask.empty()) {
            // 临时区位图：只读静态数据，运行时按位决定扫哪些字。
            emit_.out += "\t.pushsection .rodata\n\t.align 8\n";
            emit_.out += std::format(".Lscanmask{}:\n", fi);
            for (uint64_t w : frame_.scan_mask)
                emit_.out += std::format("\t.quad 0x{:016x}\n", w);
            emit_.out += "\t.popsection\n";
        }
        emit_ns_ += std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - t_emit)
                        .count();
    }

// ── 基本块 ──────────────────────────────────────────────────
    //
    // 别名只在块内有效，块边界一律清空——跨块的值永远走栈槽，所以
    // 不需要在块之间传播任何状态，也就不可能读到过期的寄存器。
void AsmGen::gen_block(const Block& b) {
        alias_.clear();
        const_val_.clear();

        // 向量化前奏（PYADAMAS_VECT 门控，默认开；0.480 起只剩通用路径）：
        // 匹配到可数循环的 map/reduction → 发「守卫 + 向量循环 + 跳标量尾」，
        // 随后照常发射原标量指令作尾循环/fallback；masked divergence 形状由
        // try_vect_loop 内部转到 backend/masked.cpp。不匹配零字节。
        try_vect_loop(b);

        // 预扫描：每个变量槽在本块内被 store 的位置，以及每个 vreg
        // 第一次被读的位置。LoadSlot 的别名只在「读之后、第一次赋值之前」
        // 成立；第一次赋值之后别名会读到新值。
        block_stores_.clear();
        block_use_.reset_first_use();
        for (int i = 0; i < static_cast<int>(b.insts.size()); ++i) {
            const Inst& in = b.insts[i];
            if (in.op == IrOp::StoreSlot) block_stores_[in.idx].push_back(i);
            VReg ops[3] = {in.a, in.b, in.c};
            for (VReg r : ops) block_use_.mark_first_use(r, i);
            for (VReg r : in.args) block_use_.mark_first_use(r, i);
        }

        // 每条指令执行后仍存活的浮点提升槽（live-after），调用点只保存
        // 这些槽。从块末尾倒推，遇到 store 清位、load 置位。
        //
        // **别名 load 的 use 点也是槽寄存器的一次读。**
        //
        // gen_load_slot 对「单次使用 + 块内 + 读后无赋值」的 load 不发
        // 指令，直接把 vreg 别名到槽寄存器。于是这个值在寄存器里一直活到
        // vreg 的**使用点**，而槽级活跃性只看见 load 那一点：s = s + f(x)
        // 的 load 在调用前、真正 readsd 在调用后，调用点掩码因此漏掉累加器，
        // 被调用者（自己也用 xmm8-15 提升）踩掉 —— 0.314 的 N19a：
        // det4(单位阵) 静默算成 0.0。这里把别名 vreg 的使用点补成一次读，
        // 位在「使用点之前、最近一次 store 之后」整段置位（store 仍清位，
        // 因为 store 会重写寄存器）。块内没有别名的 load 不置位。
        std::vector<uint8_t> fp_after;
        if (!ra_.fp_promoted_slots.empty() && !ra_.fp_live_off &&
            cur_block_ >= 0 && static_cast<size_t>(cur_block_) < ra_.fp_live_out.size()) {
            const size_t kf = ra_.fp_promoted_slots.size();
            fp_after.assign(b.insts.size() * kf, 0);
            std::vector<uint8_t> fp_alias_use(b.insts.size() * kf, 0);
            for (int k = 0; k < static_cast<int>(b.insts.size()); ++k) {
                const Inst& x = b.insts[static_cast<size_t>(k)];
                if (x.op != IrOp::LoadSlot || x.dst == kNoReg) continue;
                if (x.idx < 0 || x.idx >= static_cast<int>(ra_.slot_fpidx.size()))
                    continue;
                const int fi = ra_.slot_fpidx[static_cast<size_t>(x.idx)];
                if (fi < 0 || static_cast<size_t>(fi) >= kf) continue;
                // 别名只发生在单次使用上；跨块值/多次使用的值，load
                // 会真的写寄存器或内存，寄存器此后无人读。
                if (x.dst >= use_count_.size() || use_count_[x.dst] != 1 ||
                    cross_block_[x.dst])
                    continue;
                const int us = block_use_.first_use(x.dst);
                if (us < 0 || us <= k) continue;
                // 使用点前有对该槽的赋值 → gen_load_slot 判定 clobbered，
                // 不会别名（真的发了一次 mov），寄存器此后无人读。
                auto stv = block_stores_.find(x.idx);
                if (stv != block_stores_.end()) {
                    auto it = std::upper_bound(stv->second.begin(),
                                               stv->second.end(), k);
                    if (it != stv->second.end() && *it < us) continue;
                }
                fp_alias_use[static_cast<size_t>(us) * kf +
                             static_cast<size_t>(fi)] = 1;
            }
            std::vector<uint8_t> cur = ra_.fp_live_out[static_cast<size_t>(cur_block_)];
            for (int k = static_cast<int>(b.insts.size()) - 1; k >= 0; --k) {
                const size_t row = static_cast<size_t>(k) * kf;
                std::memcpy(&fp_after[row], cur.data(), kf);
                for (size_t j = 0; j < kf; ++j)
                    if (fp_alias_use[row + j]) cur[j] = 1;
                const Inst& x = b.insts[static_cast<size_t>(k)];
                if (x.idx >= 0 && x.idx < static_cast<int>(ra_.slot_fpidx.size())) {
                    int fi = ra_.slot_fpidx[static_cast<size_t>(x.idx)];
                    if (fi >= 0 && static_cast<size_t>(fi) < kf) {
                        if (x.op == IrOp::StoreSlot)
                            cur[static_cast<size_t>(fi)] = 0;
                        else if (x.op == IrOp::LoadSlot)
                            cur[static_cast<size_t>(fi)] = 1;
                    }
                }
            }
        }

        size_t forwarded_load = b.insts.size();
        bool forward_to_receiver = false;
        size_t i = 0;
        while (i < b.insts.size()) {
            const Inst& in = b.insts[i];
            dst_loc_.clear();

            cur_pos_ = static_cast<int>(i);
            if (i == forwarded_load) {
                set_loc(in.line);
                comment_inst(in);
                if (forward_to_receiver) alias_[in.dst] = arg_reg_i(0);
                ++i;
                continue;
            }

            // 比较 + 分支连在一起时不需要把布尔值真的算出来
            if (i + 1 < b.insts.size() && can_fuse_branch(in, b.insts[i + 1])) {
                dst_loc_.clear();
                gen_cmp_branch(in, b.insts[i + 1]);
                i += 2;
                continue;
            }

            int fused_slot = -1;
            int fused_global = -1;
            bool fp_temp = false;
            bool bin_cmp_branch = false;
            bool int_bin_temp = false;
            // 拷贝合并：目标值和左操作数被分到同一个临时寄存器时，直接原地
            // 算就行，不必再借 %xmm1 中转（否则还要多一条 movsd）。
            bool dst_coalesced =
                in.dst != kNoReg && in.a != kNoReg &&
                in.dst < static_cast<VReg>(vreg_reg_.size()) &&
                in.a < static_cast<VReg>(vreg_reg_.size()) &&
                !vreg_reg_[in.dst].empty() && vreg_reg_[in.dst] == vreg_reg_[in.a];
            if (i + 2 < b.insts.size() &&
                can_fuse_bin_cmp_branch(in, b.insts[i + 1], b.insts[i + 2])) {
                // Bin 结果留给紧随的 Cmp+Branch 直接用 %rax 比较。
                dst_loc_ = "%rax";
                bin_cmp_branch = true;
            } else if (i + 1 < b.insts.size() &&
                       can_fuse_int_bin_temp(in, b.insts[i + 1])) {
                // 整数 Bin 结果只被下一条 Bin 用：留在 %rax。
                dst_loc_ = "%rax";
                int_bin_temp = true;
            } else if (i + 1 < b.insts.size() && can_fuse_global(in, b.insts[i + 1])) {
                fused_global = b.insts[i + 1].idx;
                dst_loc_ = "%rax";
            } else if (i + 1 < b.insts.size() && can_fuse(in, b.insts[i + 1])) {
                fused_slot = b.insts[i + 1].idx;
                // 提升到寄存器的槽直接写寄存器；没提升的槽也可以把
                // 结果直接落到变量内存，省掉「临时 vreg 槽 → 变量槽」。
                dst_loc_ = ra_.slot_reg[fused_slot].empty() ? slot_mop(fused_slot)
                                                         : ra_.slot_reg[fused_slot];
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
                const_val_[in.dst] = op_imm(in.imm);
                ++i;
                continue;
            }

            if (!fp_after.empty()) {
                ra_.fp_spill_mask.assign(ra_.fp_promoted_slots.size(), 0);
                const size_t row = i * ra_.fp_promoted_slots.size();
                for (size_t j = 0; j < ra_.fp_spill_mask.size(); ++j)
                    ra_.fp_spill_mask[j] = static_cast<char>(fp_after[row + j]);
                ra_.fp_mask_valid = true;
            }
            const bool fused_vcall_add = try_fuse_guarded_vcall_add(b, i);
            if (!fused_vcall_add &&
                !try_forward_global_across_rc(b, i, forwarded_load, forward_to_receiver))
                gen_inst(in);
            if (forward_to_receiver && i == forwarded_load + 1) {
                alias_.erase(b.insts[forwarded_load].dst);
                forward_to_receiver = false;
            }
            ra_.fp_mask_valid = false;
            dst_loc_.clear();
            if (fused_vcall_add) {
                i += 3;
                continue;
            }
            if (fp_temp) alias_[in.dst] = "%xmm1";
            if (bin_cmp_branch) {
                // Bin 结果留在 %rax：让紧随的 Cmp（与 Branch 融合）直接
                // 从 %rax 取操作数，跳过临时槽的 store/load。
                alias_[in.dst] = "%rax";
            }
            if (int_bin_temp) {
                // 整数 Bin 结果留在 %rax 供下一条 Bin 直接使用。
                alias_[in.dst] = "%rax";
            }

            if (fused_slot >= 0) {
                // 融合掉的 store.slot 仍然改了变量，指向它的别名要失效
                invalidate_slot(fused_slot);
                ++i;
            }
            if (fused_global >= 0) {
                emit2(MOp::kMovq, kRax, global_mop(fused_global));
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
        // P3：unknown dst 的常量不折叠 —— 折叠会跳过 tag 字写入。
        if (tag_.tags_enabled && is_unk_ty(ty_of(in.dst))) return false;
        if (in.op == IrOp::ConstNone) return true;
        if (in.op != IrOp::ConstInt) return false;
        return in.imm >= INT32_MIN && in.imm <= INT32_MAX;
    }

void AsmGen::invalidate_slot(int slot) {
        if (slot < 0 || slot >= static_cast<int>(ra_.slot_reg.size())) return;
        const MOperand& r = ra_.slot_reg[slot];
        if (r.empty()) return;
        for (auto it = alias_.begin(); it != alias_.end();)
            it = (it->second.text == r.text) ? alias_.erase(it) : std::next(it);
    }

void AsmGen::emit_closure_shim(const IRFunction& f, int fi) {
        // 统一调用约定：第 i 个实参放第 i 个整型实参寄存器，返回值放 %rax。
        // 间接调用点常常不知道真实签名（实参静态类型是 unknown），只能按统一
        // 约定装填；由这一层按真实签名重排，返回时再把真实返回值搬回 %rax。
        //
        line(std::format("{}:", shim_label(fi)));
        if (gnu_cfi()) line(".cfi_startproc");

        const int n = static_cast<int>(f.params.size());
        std::vector<TypePtr> ptypes;
        ptypes.reserve(f.params.size());
        for (auto& p : f.params) ptypes.push_back(p.ty);

        // 快路径：签名里没有浮点。此时真实入口本来就按「第 i 个实参进第 i 个
        // 整型寄存器、返回值在 %rax」收，与统一约定逐位相同 —— 直接跳过去即可，
        // 省掉一次 call/ret。这是绝大多数函数的情形（202 个用例里 2180 个 shim
        // 中绝大多数属于此类）。
        bool any_fp = ir_is_float(f.ret);
        for (const auto& t : ptypes)
            if (ir_is_float(t)) any_fp = true;
        if (!any_fp) {
            // 跳转前不需要动栈：真实入口看到的栈状态与直接调用它时完全一致。
            line(std::format("jmp {}", entry_ref(fi)));
            if (gnu_cfi()) line(".cfi_endproc");
            return;
        }

        // 慢路径：需要真正的重排（签名含浮点，或参数超过 6 个有栈上实参）。
        //
        // 统一约定把第 i 个实参放在第 i 个整型寄存器（前 6 个）或溢出区
        // out_base + (i-6)*8（其余）。真实 ABI 则是「整型/浮点各自计数、
        // 超出各自寄存器数的走栈」。两者在含浮点时不兼容，所以这里要重排。
        //
        // 临时区布局：
        //   [0, 8*n)          统一约定传来的 n 个实参（前 6 个从寄存器落栈，
        //                     其余本来就在调用者的溢出区，这里一并搬进来）
        //   [8*n, 8*n+8*sp)   真实 ABI 的栈传参区（sp = 走栈的实参个数）
        // 重排完成后 rsp 调整到让 [8*n] 正好落在被调方期望的栈传参起点。
        auto locs = classify(ptypes);
        int nstack = 0;
        for (const auto& l : locs)
            if (l.stack_off >= 0) ++nstack;
        const int spill = 8 * n;
        const int stack_bytes = 8 * nstack;
        // 让 **call 那一刻** 的 rsp ≡ 0 mod 16。
        //
        // 这里的序列是：subq tmp → addq spill → call。入口 rsp ≡ 8，
        // 所以要求 (8 - tmp + spill) ≡ 0 (mod 16)，即 tmp ≡ spill+8 (mod 16)。
        //
        // 原来写的是 tmp = align16(spill + stack_bytes) + 8，只保证了
        // 「subq 之后、addq spill 之前」是 16 对齐；一旦 spill ≡ 8（n 为奇数），
        // addq spill 就把 rsp 又推成 ≡ 8，call 时被调方看到 ≡ 0 —— 违反 ABI。
        //
        // 后果：gcc 15 的运行时函数在对齐溢出 xmm 时用 movaps，直接 #GP。
        // 实测 24_async 里 float_one_32（1 个 float 参数 → spill=8）就是这样崩的。
        // n 为偶数时 spill 是 16 的倍数，恰好没暴露，所以这个 bug 藏了很久。
        int tmp = ((spill + stack_bytes + 15) & ~15);
        while (((8 - tmp + spill) & 15) != 0) tmp += 8;
        line(std::format("subq ${}, %rsp", tmp));
        if (gnu_cfi()) line(std::format(".cfi_def_cfa_offset {}", 8 + tmp));
        // 1) 把所有实参搬到临时区 [0, 8*n)。
        for (int i = 0; i < n; ++i) {
            if (i < kMaxIntArgs()) {
                emit2("movq", arg_reg_i(i), std::format("{}(%rsp)", 8 * i));
            } else {
                // 溢出实参：调用者放在 out_base + (i-6)*8（相对它执行 call 时
                // 的 rsp）。**必须 +8**：call 压入了返回地址，所以进入 shim 时
                // 调用者的溢出区整体后移 8 字节。
                //
                // 漏掉这个 +8 的后果：读到返回地址当第 1 个溢出实参，真正的
                // 溢出区整体错位一格，最后一个实参永远读不到 —— 实测 10 个
                // float 参数时 10.1 丢失（输出 49.5 而非 59.6）。
                line(std::format("movq {}(%rsp), %rax",
                                 tmp + 8 + out_base() + (i - kMaxIntArgs()) * 8));
                line(std::format("movq %rax, {}(%rsp)", 8 * i));
            }
        }
        // 2) 按真实 ABI 重排到寄存器与栈传参区。
        int sslot = 0;
        for (int i = 0; i < n; ++i) {
            const auto& l = locs[static_cast<size_t>(i)];
            if (l.stack_off >= 0) {
                line(std::format("movq {}(%rsp), %rax", 8 * i));
                line(std::format("movq %rax, {}(%rsp)", spill + sslot * 8));
                ++sslot;
                continue;
            }
            if (l.fp) {
                line(std::format("movsd {}(%rsp), %xmm{}", 8 * i, l.reg));
            } else {
                emit2("movq", std::format("{}(%rsp)", 8 * i), arg_reg_i(l.reg));
            }
        }
        // 闭包环境指针：调用方已放在 %r10，临时区搬运不碰它，直接沿用。

        // 栈传参区必须紧贴 call 的返回地址：把 rsp 抬到 spill 处。
        // （重排后的栈实参在 [spill, spill+stack_bytes)，正好是 call 前
        //  被调方看到的 (rsp+8) 起。）
        if (spill > 0) {
            line(std::format("addq ${}, %rsp", spill));
            if (gnu_cfi()) line(std::format(".cfi_def_cfa_offset {}", 8 + tmp - spill));
        }

        line(std::format("call {}", entry_ref(fi)));

        // 真实返回值 -> 统一约定：浮点在 %xmm0，搬到 %rax。
        if (ir_is_float(f.ret)) line("movq %xmm0, %rax");

        // 恢复 rsp：call 前抬了 spill，这里补回。
        if (spill > 0) {
            line(std::format("subq ${}, %rsp", spill));
            if (gnu_cfi()) line(std::format(".cfi_def_cfa_offset {}", 8 + tmp));
        }
        line(std::format("addq ${}, %rsp", tmp));
        if (gnu_cfi()) line(std::format(".cfi_def_cfa_offset {}", 8));
        line("ret");
        if (gnu_cfi()) line(".cfi_endproc");
    }
}  // namespace pyadamas
