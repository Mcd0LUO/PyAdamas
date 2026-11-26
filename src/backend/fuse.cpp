// backend/fuse.cpp —— 代码生成的指令融合判据（纯 IR 结构）
//
// can_fuse / can_fuse_global / can_fuse_fp_temp / can_fuse_branch：相邻指令融合的准入判据；
// can_fuse_bin_cmp_branch / can_fuse_int_bin_temp：整数结果留在 %rax 的相邻融合。
// 判据全部只看 IR 结构（相邻指令、单次使用、不跨块、类型类别），不含任何程序/名字/常量特判。
// 从 backend/block.cpp 拆出（该文件超 1200 行硬限），行为零变化。
#include "backend/impl.h"

namespace pyadamas {

bool AsmGen::can_fuse(const Inst& in, const Inst& nx) const {
        if (in.dst == kNoReg) return false;
        if (in.op == IrOp::StoreSlot || in.op == IrOp::StoreGlobal) return false;
        if (nx.op != IrOp::StoreSlot || nx.a != in.dst) return false;
        // P3：unknown 结果不融合 —— 融合绕过 StoreSlot 的 tag 搬运。
        if (tag_.tags_enabled && is_unk_ty(ty_of(in.dst))) return false;
        // **GC 要扫描的槽不能融合。**
        //
        // 融合会把 `store.slot s` 的目标替换成 dst_loc_（某个 vreg 寄存器或
        // 临时槽），**栈槽 smem(s) 从此不再被写**。而帧记录登记的是
        // smem(s) —— GC 和异常展开都从那里读值。结果是：
        //   * GC 读到序言清零留下的 0 → 不标记 → 对象被回收；
        //   * 之后从已释放对象取元素得到垃圾指针，当场 SEGV。
        //
        // ASan 实测（x_min.py）：s1（u，200 元素列表）的 store 被融合成
        // `movq 224(%rsp), %r12`，48(%rsp) 自序言清零后再没写过；major GC
        // 回收 u 后 `index.get` 取出 -26，传给 pa_rc_temp 崩溃。
        if (nx.idx >= 0 && nx.idx < static_cast<int>(gc_scan_slot_.size()) &&
            gc_scan_slot_[static_cast<size_t>(nx.idx)])
            return false;
        if (nx.idx < 0 || nx.idx >= static_cast<int>(ra_.slot_reg.size())) return false;
        // 值与目标槽的寄存器类不同（典型：入口把 const.int 0 写进提升到
        // xmm 的 float 槽）时不能融合：融合会绕过 store_slot_checked 对
        // 「xmm 槽 + 立即数」的 %rax 中转，直接发出非法的
        // `movq $imm, %xmmN`（实测 `def f(): for y in [1.5, 2.5]: pass`
        // 的入口零初始化，as 报 operand type mismatch）。留给普通
        // StoreSlot 走 store_slot_checked。
        if (f_ && nx.idx < static_cast<int>(f_->slot_ty.size()) &&
            ir_is_float(ty_of(in.dst)) != ir_is_float(f_->slot_ty[static_cast<size_t>(nx.idx)]))
            return false;
        // O0 只融合到提升过的寄存器；O1 起允许直接写变量栈槽。
        if (opt_ < 1 && ra_.slot_reg[nx.idx].empty()) return false;
        if (in.dst >= use_count_.size()) return false;
        // 只有「定义后在本块里正好被用一次」才能就地写变量槽。
        // 用两次就不行了：第二次读到的会是新值。
        return use_count_[in.dst] == 1 && !cross_block_[in.dst];
    }

// 0.289：Bin(int) + StoreGlobal 融合。obj_dispatch 的 total += s.area()
// 每轮把 add 结果写进 vreg 栈槽再读回来写全局（mov %rax,0xf8 +
// mov 0xf8,%rax 的 STLF 往返，64M 轮）。结果只被这条 store 用时，
// 让 bin 直接留在 %rax，紧跟一条 movq %rax, global 即可。
// StoreGlobal 是纯 store（RC 由降低器单独发 inc/dec），无 GC 帧扫描
// 问题（全局根本身就是那个符号），任意类型安全；这里只接整数 Bin。
bool AsmGen::can_fuse_global(const Inst& in, const Inst& nx) const {
        if (opt_ < 1) return false;
        if (in.dst == kNoReg || in.op != IrOp::Bin) return false;
        if (ir_is_float(in.ty)) return false;
        switch (in.bop) {
            case BinKind::Add:
            case BinKind::Sub:
            case BinKind::Mul:
            case BinKind::BitAnd:
            case BinKind::BitOr:
            case BinKind::BitXor:
            case BinKind::Shl:
            case BinKind::Shr:
                break;
            default:
                return false;
        }
        if (nx.op != IrOp::StoreGlobal || nx.a != in.dst) return false;
        // P3：unknown 全局不融合 —— 融合绕过 StoreGlobal 的 tag 搬运。
        if (tag_.tags_enabled && nx.idx >= 0 &&
            nx.idx < static_cast<int>(m_->globals.size()) &&
            is_unk_ty(m_->globals[static_cast<size_t>(nx.idx)].ty))
            return false;
        if (in.dst >= use_count_.size()) return false;
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
        // str/bytes 走内容比较（pa_str_eq 慢路径），融合成指针 cmpq 会静默判不等
        // （zipfile magic 校验，基线 0.327 已存在）；浮点交给 gen_cmp_branch_fp（ja/jae 归 NaN 假分支，== 补 jp）。
        TypePtr t = ty_of(in.a);
        if (t && (t->kind == TyKind::Str || t->kind == TyKind::Bytes)) return false;
        return true;
    }

// 比较结果只用来做分支时，直接发条件跳转：  `setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
void AsmGen::gen_cmp_branch(const Inst& in, const Inst& br) {
        if (ir_is_float(ty_of(in.a))) {
            gen_cmp_branch_fp(in, br);
            return;
        }
        comment(std::format("{} -> {}", in.text(), br.text()));
        const MOperand& sa = src_mop(in.a);
        const MOperand& sb = src_mop(in.b);
        bool sa_mem = is_mem_mop(sa);
        bool sb_mem = is_mem_mop(sb);
        if (sa_mem && sb_mem) {
            emit2(MOp::kMovq, sa, kRax);
            emit2(MOp::kCmpq, sb, kRax);
        } else if (sa_mem || sa.kind == MOperandKind::Reg) {
            emit2(MOp::kCmpq, sb, sa);
        } else {
            emit2(MOp::kMovq, sa, kRax);
            emit2(MOp::kCmpq, sb, kRax);
        }
        MOp jcc = MOp::kJe;
        MOp inv = MOp::kJne;
        // 无符号 64 位（P1 定宽 u64）用 ja/jb 族。宽度 < 64 的无符号槽内
        // 值恒非负，jl/jg 与 jb/ja 结论相同（见 norm_int_rax 的不变量）。
        const bool uns = ty_of(in.a) && ty_of(in.a)->kind == TyKind::U64;
        switch (in.cop) {
            case CmpKind::Lt:
                jcc = uns ? MOp::kJb : MOp::kJl;
                inv = uns ? MOp::kJae : MOp::kJge;
                break;
            case CmpKind::Le:
                jcc = uns ? MOp::kJbe : MOp::kJle;
                inv = uns ? MOp::kJa : MOp::kJg;
                break;
            case CmpKind::Gt:
                jcc = uns ? MOp::kJa : MOp::kJg;
                inv = uns ? MOp::kJbe : MOp::kJle;
                break;
            case CmpKind::Ge:
                jcc = uns ? MOp::kJae : MOp::kJge;
                inv = uns ? MOp::kJb : MOp::kJl;
                break;
            case CmpKind::Eq: jcc = MOp::kJe; inv = MOp::kJne; break;
            case CmpKind::Ne: jcc = MOp::kJne; inv = MOp::kJe; break;
        }
        // O2：一边就是下一个块时，用反向条件跳走另一边，省掉无条件跳。
        if (opt_ >= 2 && br.target == cur_block_ + 1) {
            emit_jmp(inv, block_label(br.target2));
        } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
            emit_jmp(jcc, block_label(br.target));
        } else {
            emit_jmp(jcc, block_label(br.target));
            emit_jmp(MOp::kJmp, block_label(br.target2));
        }
    }

void AsmGen::gen_cmp_branch_fp(const Inst& in, const Inst& br) {
        comment(std::format("{} -> {}", in.text(), br.text()));
        const MOperand* sa = &src_mop(in.a);
        const MOperand* sb = &src_mop(in.b);
        MOp jcc = MOp::kJe;
        MOp inv = MOp::kJne;
        bool ordered = true;  // 无序（NaN）时 Python 的比较结果为假
        bool swap = false;
        // 四种有序比较换个操作数顺序就能把 NaN 归到「不满足」那一侧：
        // ucomisd 对无序置 CF=1，ja/jae 都不会跳，正好落到假分支，于是
        // 不用再补一条 jp（内层循环里省一条分支）。
        switch (in.cop) {
            case CmpKind::Lt: swap = true; jcc = MOp::kJa; inv = MOp::kJbe; ordered = false; break;
            case CmpKind::Le: swap = true; jcc = MOp::kJae; inv = MOp::kJb; ordered = false; break;
            case CmpKind::Gt: jcc = MOp::kJa; inv = MOp::kJbe; ordered = false; break;
            case CmpKind::Ge: jcc = MOp::kJae; inv = MOp::kJb; ordered = false; break;
            case CmpKind::Eq: jcc = MOp::kJe; inv = MOp::kJne; break;
            case CmpKind::Ne: jcc = MOp::kJne; inv = MOp::kJe; ordered = false; break;  // NaN 由下面的特例先跳真分支
        }
        if (swap) std::swap(sa, sb);
        // ucomisd 的第二个操作数是 xmm；左操作数不在 xmm 里就先搬进 %xmm0。
        if (sa->kind == MOperandKind::Reg && sa->reg >= kXmm0 && sa->reg <= kXmm15) {
            emit2(MOp::kUcomisd, *sb, *sa);
        } else {
            emit2(MOp::kMovsd, *sa, kXmm0);
            emit2(MOp::kUcomisd, *sb, kXmm0);
        }
        // NaN 时 PF=1：!= 的结果应为真，不能直接 je。单独把无序跳到真分支，
        // 再按 ZF 分真/假；其它比较的 jp 在下面按 ordered 处理。
        if (in.cop == CmpKind::Ne) {
            if (opt_ >= 2 && br.target == cur_block_ + 1) {
                // 真块就是下一块：NaN 和不等都落到 fallthrough，相等跳假。
                emit_jmp(MOp::kJp, block_label(br.target));
                emit_jmp(MOp::kJe, block_label(br.target2));
            } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
                // 假块就是下一块：NaN 和不等跳真，相等 fallthrough 到假。
                emit_jmp(MOp::kJp, block_label(br.target));
                emit_jmp(MOp::kJne, block_label(br.target));
            } else {
                emit_jmp(MOp::kJp, block_label(br.target));
                emit_jmp(MOp::kJne, block_label(br.target));
                emit_jmp(MOp::kJmp, block_label(br.target2));
            }
            return;
        }
        // 有序比较的 ja/jae 本身已排除无序，ordered=false 时不需要 jp。
        // 之后的落点选择与整型版本一致，利用「目标块就是下一块」省掉无条件跳。
        if (opt_ >= 2 && br.target == cur_block_ + 1) {
            if (ordered) emit_jmp(MOp::kJp, block_label(br.target2));
            emit_jmp(inv, block_label(br.target2));
        } else if (opt_ >= 2 && br.target2 == cur_block_ + 1) {
            if (ordered) emit_jmp(MOp::kJp, block_label(br.target2));
            emit_jmp(jcc, block_label(br.target));
        } else {
            if (ordered) emit_jmp(MOp::kJp, block_label(br.target2));
            emit_jmp(jcc, block_label(br.target));
            emit_jmp(MOp::kJmp, block_label(br.target2));
        }
    }

// 保留原 IR 和活跃性。只融合相邻的「读旧全局、发布、释放、再读」，
// 中间至多跨过一个不同全局的整数读，且它不能覆盖融合的结果位置。
bool AsmGen::try_forward_global_across_rc(const Block& b, size_t at, size_t& load_pos,
                                         bool& forward_to_receiver) {
    if (opt_ < 2 || gen_ || at < 2 || at + 1 >= b.insts.size()) return false;
    const auto& old = b.insts[at - 2];
    const auto& store = b.insts[at - 1];
    const auto& dec = b.insts[at];
    if (old.op != IrOp::LoadGlobal || store.op != IrOp::StoreGlobal || old.idx != store.idx ||
        dec.op != IrOp::CallRt || dec.imm != static_cast<int64_t>(RtFn::RcDec) ||
        dec.dst != kNoReg || dec.args.size() != 1 || dec.args[0] != old.dst ||
        !ir_is_heap(old.ty) || old.dst == kNoReg || store.a == kNoReg || store.a == old.dst)
        return false;
    const auto& next = b.insts[at + 1];
    // 不能跨越 EH 标记、其他释放、写入或可能重入的操作。
    // 中间可以夹一条标量读：全局（别的全局）或帧槽（入口模块私有标量
    // 全局提升后，同一累加形态里的定值是 load.slot）。两者都是纯读，
    // 读不到也改不了 store 的全局。
    const bool across_scalar =
        (next.op == IrOp::LoadGlobal && next.idx != store.idx &&
         next.ty && next.ty->kind == TyKind::Int) ||
        (next.op == IrOp::LoadSlot && next.ty && next.ty->kind == TyKind::Int);
    const size_t pos = at + 1 + across_scalar;
    if (pos + 1 >= b.insts.size()) return false; // 原块末的 spill 不能跳过。
    const auto& load = b.insts[pos];
    if (load.op != IrOp::LoadGlobal || load.idx != store.idx ||
        !single_use_in_block(load.dst) || !ir_is_heap(load.ty) ||
        !type_eq(ty_of(store.a), load.ty)) return false;
    const auto& use = b.insts[pos + 1];
    const bool receiver = use.op == IrOp::VCall && use.a == load.dst && use.args.empty();
    const bool gen_next = use.op == IrOp::CallRt &&
                          use.imm == static_cast<int64_t>(RtFn::GenNext) &&
                          use.args.size() == 1 && use.args[0] == load.dst;
    if (!receiver && (!gen_next || across_scalar)) return false;

    // 从已发布值的栈位置读，只在没有运行时调用的那一支使用；慢路
    // 可能释放该对象，返回后不得再读 src，必须按当前全局内容写回。
    const auto src = src_op(store.a);
    if (!is_mem_operand(src) || src != mem(store.a) || alias_.contains(store.a)) return false;
    const std::string dst = receiver ? arg_reg_i(0) : mem(load.dst);
    if (!receiver && !is_mem_operand(dst)) return false;
    set_loc(dec.line);
    comment(dec.text() + " -> conditional global forwarding");
    gen_rc_dec(dec, src, dst, load.idx);
    load_pos = pos;
    // 只有紧邻且不带其他实参的 VCall 可以直接消费 arg0；别名到调用后失效。
    forward_to_receiver = receiver;
    return true;
}

// 整数 Bin 结果只被紧随的 Cmp 使用、且该 Cmp 与 Branch 融合：让 Bin 把结果
// 留在 %rax，Cmp 直接拿 %rax 当操作数，省掉「Bin 结果 → 临时槽 → cmpq $0,槽」
// 的 store + 内存读（Collatz 内层循环每轮一次，占生成指令 ~2.5%）。
//
// 纯 IR 结构判据，不含任何程序/名字/常量特判：
//   * in 是整数 ALU Bin（除法/取模除外：它们已自己管 %rax）；
//   * nx 是 Cmp，且 nx.a == in.dst；nx2 是 Branch 且消费 nx.dst；
//   * in.dst 只在本块用这一次、不跨块。
bool AsmGen::can_fuse_bin_cmp_branch(const Inst& in, const Inst& nx,
                                     const Inst& nx2) const {
    if (opt_ < 2) return false;
    if (in.op != IrOp::Bin || in.dst == kNoReg) return false;
    if (ir_is_float(in.ty) || in.ty == nullptr) return false;
    switch (in.bop) {
        case BinKind::Add: case BinKind::Sub: case BinKind::Mul:
        case BinKind::BitAnd: case BinKind::BitOr: case BinKind::BitXor:
        case BinKind::Shl: case BinKind::Shr:
        // 除/取模/地板除的所有路径（gen_div_const 快路径 + 通用 idiv 尾巴）
        // 最终都把结果放进 %rax，与 ALU 运算一样可留在寄存器里。
        case BinKind::Div: case BinKind::Mod: case BinKind::FloorDiv:
            break;
        default: return false;
    }
    if (nx.op != IrOp::Cmp || nx.a != in.dst) return false;
    if (nx2.op != IrOp::Branch || nx2.a != nx.dst) return false;
    if (in.dst >= use_count_.size() || use_count_[in.dst] != 1) return false;
    if (cross_block_[in.dst]) return false;
    if (tag_.tags_enabled && is_unk_ty(ty_of(in.dst))) return false;
    return true;
}

// 整数 Bin 结果只被紧邻的下一条整数 Bin 使用时留在 %rax。
// 判据纯 IR 结构：相邻两 Bin、结果单次使用、不跨块、非浮点/非 unknown。
bool AsmGen::can_fuse_int_bin_temp(const Inst& in, const Inst& nx) const {
    if (opt_ < 2) return false;
    if (in.op != IrOp::Bin || in.dst == kNoReg) return false;
    if (nx.op != IrOp::Bin || nx.dst == kNoReg) return false;
    if (ir_is_float(in.ty) || in.ty == nullptr) return false;
    if (ir_is_float(nx.ty) || nx.ty == nullptr) return false;
    // 只融合结果当 nx.a 的情形：gen_bin_int 先把 nx.a 装进 %rax，若结果
    // 在 nx.b 则这次装载会先破坏 %rax 里的上一条结果。
    if (nx.a != in.dst) return false;
    if (in.dst >= use_count_.size() || use_count_[in.dst] != 1) return false;
    if (cross_block_[in.dst]) return false;
    if (tag_.tags_enabled && is_unk_ty(ty_of(in.dst))) return false;
    return true;
}

// 同一条「结果留在 %rax」的判据，把消费者从 Bin 扩到两种「紧邻即用」的
// 情形：①直接调用（CallDirect）的实参，②本块 return 的返回值。
//
// 为什么安全（不碰 ABI）：
//  * 值只在相邻两条指令之间活；调用实参装载的第一条就是
//    `movq %rax, <arg 寄存器>`，之前没有任何指令碰 %rax（栈实参用 %r10
//    中转，ptag 编组会用 %rax 中转 → 那条路径被 call_ptag_words==0 关掉）；
//  * return 的 gen_return 就是 `movq src, %rax`，源已经是 %rax 时直接跳过；
//  * 只放行非浮点、非堆类型（int/bool）：没有 RC/GC 所有权转移，也不涉及
//    多字返回结构；单次使用 + 不跨块保证没有第二个读者。
//  * 被调函数仍然按原 ABI 收实参；改动只发生在调用点把临时值留在哪个寄存器。
bool AsmGen::can_fuse_bin_value_use(const Inst& in, const Inst& nx) const {
    if (opt_ < 2) return false;
    if (in.op != IrOp::Bin || in.dst == kNoReg) return false;
    if (!in.ty) return false;
    if (ir_is_float(in.ty) || ir_is_heap(in.ty)) return false;
    if (in.ty->kind != TyKind::Int && in.ty->kind != TyKind::Bool) return false;
    if (in.dst >= use_count_.size() || use_count_[in.dst] != 1) return false;
    if (cross_block_[in.dst]) return false;
    if (tag_.tags_enabled && is_unk_ty(ty_of(in.dst))) return false;
    if (nx.op == IrOp::Return) return nx.a == in.dst;
    if (nx.op != IrOp::CallDirect) return false;
    bool arg = false;
    for (VReg v : nx.args)
        if (v == in.dst) { arg = true; break; }
    if (!arg) return false;
    // tag 数组编组（tag.cpp 的 move_to）在装实参之前会用 %rax 当内存中转，
    // 会把刚留在 %rax 的值冲掉；有平行 tag 数组的调用点不走这条融合。
    if (call_ptag_words(nx) > 0) return false;
    return true;
}

}  // namespace pyadamas
