// codegen_ops.cpp —— 指令选择：算术/比较/转换
#include "codegen_impl.h"

namespace pyadamas {

namespace {
// 测上界用：PYADAMAS_NO_BOUNDS=1 时所有索引访问都走无检查版本。
// 只用于测量「边界检查全消掉能快多少」，不检查下标是否真的在范围内，
// 因此绝不能用在正常编译里。
bool no_bounds_probe() {
    static const bool v = std::getenv("PYADAMAS_NO_BOUNDS") != nullptr;
    return v;
}
}  // namespace

// idiv 的除数必须在 %rcx。SysV 直接用它；Win64 下 %rcx 是第 1 个实参
// 寄存器，而除零检查里的 panic 是调用、会破坏它，所以统一用 %r10 中转。
static const char* div_reg() { return target_abi().gnu_cfi ? "%rcx" : "%r10"; }

namespace {

// gen_div_const 的模式：0 = 向零取整商（/），1 = 向下取整商（//），2 = 余数（%）
constexpr int kDivModeTrunc = 0;
constexpr int kDivModeFloor = 1;
constexpr int kDivModeMod = 2;

// 操作数文本是不是一个立即数常量（"$123" / "$-4"）。src_op 对「能直接当
// 立即数用」的 vreg 返回这种形态；寄存器/栈槽则以 '%' / 数字开头。
bool operand_const_imm(const std::string& s, int64_t* out) {
    if (s.size() < 2 || s[0] != '$') return false;
    size_t i = 1;
    bool neg = false;
    if (s[i] == '-') { neg = true; ++i; }
    if (i >= s.size()) return false;
    unsigned long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + static_cast<unsigned long long>(s[i] - '0');
        if (v > 0x8000000000000000ULL) return false;
    }
    if (neg) *out = v == 0x8000000000000000ULL ? INT64_MIN : -static_cast<int64_t>(v);
    else      *out = static_cast<int64_t>(v);
    return true;
}

// 有符号 64 位「除以常数」：把 |x| 当无符号做魔数除法，最后按符号还原。
//
// 为什么值得做：x86-64 的 64 位 idivq 在 Cascade Lake 上吞吐约 24 周期，
// 而下面这段是十来条一周期指令加一次 3 周期的 imul。实测 idivq 在
// queue_sim 的 simulate 里占该函数 47.6% 的采样（t % 3、t % 7 各一处），
// 在 str_build 的 build_line 里占 71.9%（i % 10）—— 全是循环变量对常数取模。
//
// 算法（Granlund-Montgomery 的 round-up 魔数）：
//   对 d > 1（非 2 的幂），取 L = ceil(log2 d)，M = floor(2^(64+L)/d) + 1。
//   M 一定 > 2^64（因为 d < 2^L），写成 M = 2^64 + m'，于是
//       floor(x*M / 2^(64+L)) == (mulhu(x, m') + x) >> L
//   —— 这正是把 65 位魔数拆成「高位那 1 加上 m'」的结果，x*2^64 那部分
//   恰好整块进位到高 64 位。
//
//   有符号情形不必另找魔数：trunc(x/d) = sign(x) * floor(|x|/d)，
//   而 |x| 用 (x ^ sign) - sign 在无符号域里算（INT64_MIN 得到 2^63，正好）。
//   这一步顺便把 INT64_MIN/d 的溢出问题一起解决了。
//
//   2 的幂另有更便宜的路径：trunc(x/2^L) = (x + (x>>63 & (2^L-1))) >> L，
//   不用乘法。这一条必须单独写 —— 魔数法在 2 的幂上会退化（M 变成 2^63+1，
//   代入上面的式子不成立）。
//
// 正确性验证：30,583,433 个 (x, d) 组合 0 失败，覆盖 d ∈ [-4000,4000] ∪
// {2^k±1, 2^k} ∪ 500 个随机 31 位除数，x 含 0/±1/±d/±(d±1)/±2d/
// INT64_MIN/INT64_MAX，小除数上再密集跑满 [0, 2^18)。

// d >= 2，返回 L 使 d == 2^L；不是 2 的幂返回 -1。
int pow2_log2(uint64_t d) {
    if (d < 2 || (d & (d - 1)) != 0) return -1;
    int L = 0;
    while ((d >> L) != 1) ++L;
    return L;
}

// d > 1 且非 2 的幂。返回 m' 与 L 使 (mulhu(x,m') + x) >> L == x / d。
bool udiv_magic(uint64_t d, uint64_t* mp_out, int* L_out) {
    if (pow2_log2(d) >= 0) return false;
    using u128 = unsigned __int128;
    int L = 0;
    while ((static_cast<uint64_t>(1) << L) < d) ++L;
    const u128 two64 = static_cast<u128>(1) << 64;
    const u128 m = (static_cast<u128>(1) << (64 + L)) / d + 1;
    if (m <= two64) return false;
    const u128 mp = m - two64;
    if (mp >= two64) return false;
    *mp_out = static_cast<uint64_t>(mp);
    *L_out = L;
    return true;
}

}  // namespace


// ── 指令 ────────────────────────────────────────────────────
void AsmGen::set_loc(int line) {
    if (line <= 0 || line == cur_loc_line_ || !m_ || m_->source_path.empty()) return;
    cur_loc_line_ = line;
    // 行变化处放一个本地标签，运行时表把 PC 映射回 (函数, 行)。
    // to_chars 直写：每行两次 std::format 临时串免掉。
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, dbg_seq_++);
    std::string lab = ".Ldbg";
    lab.append(buf, r.ptr);
    out_ += lab;
    out_ += ":\n";
    line_buf_ = "\t.loc 1 ";
    auto r2 = std::to_chars(buf, buf + sizeof buf, line);
    line_buf_.append(buf, r2.ptr);
    line_buf_ += '\n';
    emit_raw(line_buf_);
    dbg_entries_.push_back({lab, fi_, line});
}

void AsmGen::gen_inst(const Inst& in) {
        set_loc(in.line);
        comment(in.text());
        switch (in.op) {
            case IrOp::ConstInt: gen_const_int(in); break;
            case IrOp::ConstFloat: gen_const_float(in); break;
            case IrOp::ConstStr:
                // 单字节 ASCII 的 str 字面量直接用驻留表里的对象。
                //
                // 字符串不可变，内容相同的单字节 str 在语义上无法区分，所以
                // 共享对象是安全的；而一旦共享，`s[i] == "0"` 这种最常见的
                // 逐字符比较就变成「两个指针相等」，pa_str_eq 的第一行直接
                // 命中，连长度检查和 memcmp 都省了。CPython 对 latin1 也有
                // 同样的驻留。bytes 字面量不参与（pub 语义不同）。
                if (char_tab_literal(in.idx) >= 0) {
                    emit2(MOp::kMovq,
                          op_mem_sym("pa_char_tab+" + std::to_string(char_tab_literal(in.idx) * 8), "%rip"),
                          kRax);
                } else {
                    char b[24];
                    auto rr = std::to_chars(b, b + sizeof b, in.idx);
                    std::string sym = ".Lstr";
                    sym.append(b, rr.ptr);
                    emit2(MOp::kLeaq, op_mem_sym(std::move(sym), "%rip"), kRax);
                }
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                emit_tag_imm(in.dst, kTagStr);
                break;
            case IrOp::ConstNone:
                move_to(dst_op(in.dst), "$0");
                // None 在 unknown 域仍是裸 0（0.324 边界），tag 按 INT。
                emit_tag_imm(in.dst, kTagInt);
                break;
            case IrOp::FuncAddr: {
                char b[24];
                auto rr = std::to_chars(b, b + sizeof b, in.imm);
                std::string sym = ".Lclos";
                sym.append(b, rr.ptr);
                emit2(MOp::kLeaq, op_mem_sym(std::move(sym), "%rip"), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            }
            case IrOp::FuncCode:
                emit2(MOp::kLeaq, op_mem_sym(entry_ref(static_cast<int>(in.imm)), "%rip"), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            case IrOp::Mov:
                move_to(dst_mop(in.dst), src_mop(in.a));
                // P3：unknown dst 的搬运同时搬 tag（tag 跟值走，设计 §3.3）。
                if (vreg_tagged(in.dst)) move_to(tag_mop(in.dst), MOperand(tag_src(in.a)));
                break;
            case IrOp::Bin: gen_bin(in); break;
            case IrOp::Un: gen_un(in); break;
            case IrOp::Cmp: gen_cmp(in); break;
            case IrOp::Convert: gen_convert(in); break;
            case IrOp::LoadSlot: gen_load_slot(in); break;
            case IrOp::StoreSlot: {
                // **split 模式：写寄存器后同步写回栈。**
                //
                // 为什么必须双写：split 的块入口无条件 reload（见
                // codegen_block.cpp），前提是**栈里永远是最新值**。
                // 如果只写寄存器、靠块出口批量 spill，那么：
                //   - 块中间有分支跳出时，出口 spill 走不到；
                //   - 部分块提升时，未提升块的读写直接看栈，读到旧值。
                //
                // 0.262 曾给**所有**槽双写，spectral_norm 从 573ms 涨到
                // 1006ms（+76%），所以回退了。但那次是所有槽都双写；
                // 这里只对**本块确实用 split 寄存器**的槽双写 —— 那些槽
                // 本来就已经有 reload 开销，多一条 mov 换正确性是划算的。
                const MOperand& loc = slot_mop(in.idx);
                move_to(loc, src_mop(in.a));
                // P3：unknown 槽写入同时写 tag 字。
                if (slot_tagged(in.idx)) move_to(slot_tag_mop(in.idx), MOperand(tag_src(in.a)));
                invalidate_slot(in.idx);
                break;
            }            case IrOp::LoadGlobal:
                emit2(MOp::kMovq, global_mop(in.idx), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                // P3：unknown 全局装载同时装 tag；无 tag 字的全局（外部/
                // 非入口导出）落 UNK 回退探针。
                if (vreg_tagged(in.dst)) {
                    if (global_tag_eligible(in.idx)) {
                        emit2(MOp::kMovq, global_tag_mop(in.idx), kRax);
                        move_to(tag_mop(in.dst), mreg_operand(kRax));
                    } else {
                        emit_tag_imm(in.dst, kTagUnk);
                    }
                }
                break;
            case IrOp::StoreGlobal:
                emit2(MOp::kMovq, src_mop(in.a), kRax);
                emit2(MOp::kMovq, kRax, global_mop(in.idx));
                // P3：unknown 全局写入同时写 tag 字（仅 eligible 全局）。
                if (tags_enabled_ && in.idx >= 0 &&
                    in.idx < static_cast<int>(m_->globals.size()) &&
                    is_unk_ty(m_->globals[static_cast<size_t>(in.idx)].ty) &&
                    global_tag_eligible(in.idx)) {
                    // 历史路径：含 '$' 的 mangled tag 符号 parse_operand 拒绝 →
                    // emit2 文本兜底列 0 Raw。转换构造复刻该兜底，不能用缓存单元。
                    move_to(MOperand(global_tag_label(in.idx) + "(%rip)"), MOperand(tag_src(in.a)));
                }
                break;
            case IrOp::GlobalAddr:
                emit2(MOp::kLeaq, global_mop(in.idx), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            case IrOp::ClassAddr: {
                char b[24];
                auto rr = std::to_chars(b, b + sizeof b, in.imm);
                std::string sym = ".Lvt";
                sym.append(b, rr.ptr);
                emit2(MOp::kLeaq, op_mem_sym(std::move(sym), "%rip"), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            }
            case IrOp::MakeList: gen_make_list(in); break;
            case IrOp::MakeTuple: gen_make_list(in); break;
            case IrOp::IndexLoad:
                if (no_bounds_probe()) gen_index_unchecked_load(in);
                else gen_index_load(in);
                break;
            case IrOp::IndexLoadUnchecked: gen_index_unchecked_load(in); break;
            case IrOp::IndexStore:
                if (no_bounds_probe()) gen_index_store_unchecked(in);
                else gen_index_store(in);
                break;
            case IrOp::IndexStoreUnchecked: gen_index_store_unchecked(in); break;
            case IrOp::Len:
                emit2(MOp::kMovq, src_mop(in.a), kRax);
                emit2(MOp::kMovq, op_mem(kRax), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            case IrOp::ObjNew: gen_obj_new(in); break;
            case IrOp::StructNew: gen_struct_new(in); break;
            case IrOp::StructCopy: gen_struct_copy(in); break;
            case IrOp::FieldLoad: gen_field_load(in); break;
            case IrOp::FieldStore: gen_field_store(in); break;
            case IrOp::PtrLoad: gen_ptr_load(in); break;
            case IrOp::PtrStore: gen_ptr_store(in); break;
            case IrOp::VCall:
                if (!gen_guarded_vcall(in)) gen_vcall(in);
                break;
            case IrOp::VCallStatic: gen_vcall_static(in); break;
            case IrOp::SuperVCall: gen_super_vcall(in); break;
            case IrOp::CallDirect: gen_call_direct(in); break;
            case IrOp::CallPtr: gen_call_ptr(in); break;
            case IrOp::Call: gen_call(in); break;
            case IrOp::CallRt: gen_call_rt(in); break;
            case IrOp::CallExtern: gen_call_extern(in); break;
            case IrOp::LoadExtern: gen_load_extern(in); break;
            case IrOp::StoreExtern: gen_store_extern(in); break;
            case IrOp::Setjmp: gen_setjmp(in); break;
            case IrOp::EhBegin: gen_eh_begin(); break;
            case IrOp::EhEnd: gen_eh_end(); break;
            case IrOp::EhLandingPad: gen_eh_landing_pad(); break;
            case IrOp::ImportModule: {
                const std::string& name = m_->imports[static_cast<size_t>(in.imm)];
                line(std::format("leaq {}(%rip), {}", module_ready_symbol(name), arg_reg_i(0)));
                line(std::format("leaq {}(%rip), {}", module_init_symbol(name), arg_reg_i(1)));
                spill_fp_slots();
                line("call pa_import_module");
                reload_fp_slots();
                break;
            }
            case IrOp::Jump:
                if (!(opt_ >= 2 && in.target == cur_block_ + 1))
                    emit_jmp("jmp", block_label(in.target));
                break;
            case IrOp::Branch: gen_branch(in); break;
            case IrOp::Return: gen_return(in); break;
            case IrOp::SlotAddr: {
                int slot = in.idx;
                emit2(MOp::kLeaq, gen_ ? gen_smem_mop(slot) : smem_mop(slot), kRax);
                move_to(dst_mop(in.dst), mreg_operand(kRax));
                break;
            }
            case IrOp::GenNew: {
                const auto& g = m_->generators[static_cast<size_t>(in.imm)];
                // 四个实参按 ABI 位置走：n_slots / resume / gcrel / frame_kind。
                // Win64 只有 4 个整型实参寄存器，这里正好用满；第 4 个在
                // SysV 是 %rcx，在 Win64 是 %r9。
                emit_imm_op("movq", g.n_slots, arg_reg_i(0));
                line(std::format("leaq {}(%rip), {}", entry_ref(g.resume), arg_reg_i(1)));
                line(std::format("leaq .Lgengcrel{}(%rip), {}", in.imm, arg_reg_i(2)));
                emit_imm_op("movq", g.frame_kind, arg_reg_i(3));
                line("call pa_gen_new");
                move_to(dst_op(in.dst), "%rax");
                break;
            }
            case IrOp::GenSetSlot:
                // 基址 %rax + 值暂存：值不能借实参寄存器（Win64 下 arg_reg_i(3)
                // 是 %r9，而这里根本没有调用），走 ABI 的 scratch_field。
                emit2(MOp::kMovq, src_mop(in.args[0]), kRax);
                emit2(MOp::kMovq, src_mop(in.args[1]), target_abi().scratch_field);
                emit2(MOp::kMovq, target_abi().scratch_field, op_mem(kRax, 8 * (10 + in.idx)));
                break;
            case IrOp::GenGetState:
                line("movq 0(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenClosing:
                line("movq 40(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenGetExc:
                line("movq 64(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenTakeExc:
                line("movq 64(%r15), %rax");
                line("movq $0, 64(%r15)");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenSent:
                line("movq 16(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                // P3-1b：send 边界无 tag 来源 → UNK 探针回退（不写会残留
                // 上一轮的陈旧 tag）。
                emit_tag_imm(in.dst, kTagUnk);
                break;
            case IrOp::GenYield:
                if (in.a != kNoReg) emit2(MOp::kMovq, src_mop(in.a), kRax);
                else emit2(MOp::kXorl, kEax, kEax);
                emit2(MOp::kMovq, kRax, op_mem(kR15, 24));
                emit_imm(MOp::kMovq, in.imm, op_mem(kR15, 0));
                emit2(MOp::kXorl, kEax, kEax);
                emit_epilogue();
                break;
            case IrOp::GenAwait:
                if (in.a != kNoReg) emit2(MOp::kMovq, src_mop(in.a), kRax);
                else emit2(MOp::kXorl, kEax, kEax);
                emit2(MOp::kMovq, kRax, op_mem(kR15, 24));
                emit_imm(MOp::kMovq, in.imm, op_mem(kR15, 0));
                line("movq $3, 8(%r15)");
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
            case IrOp::GenRethrow:
                emit2(MOp::kMovq, kR15, arg_reg_i(0));
                line("call pa_coro_rethrow");
                line("ud2");
                break;
            case IrOp::GenDone:
                if (in.a != kNoReg) {
                    emit2(MOp::kMovq, src_mop(in.a), kRax);
                    emit2(MOp::kMovq, kRax, op_mem(kR15, 24));
                }
                line("movq $1, 8(%r15)");
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
        }
    }

// x = y 这种加载能不能省掉，看两件事：
    //   1. 变量 x 在寄存器里（否则读出来就得写回一个栈槽，省不了）
    //   2. 这个临时值只被读一次，且读的时候中间没有对 x 的赋值
    // 都满足时，临时值直接指向 x 的寄存器，整条指令消失。
void AsmGen::gen_load_slot(const Inst& in) {
        // P3：unknown 槽不走别名优化（别名只搬 payload 会丢 tag），
        // 老老实实搬 payload + tag 两个字。冷路径，正确性优先。
        if (slot_tagged(in.idx)) {
            move_to(dst_mop(in.dst), slot_mop(in.idx));
            if (vreg_tagged(in.dst))
                move_to(tag_mop(in.dst), slot_tag_mop(in.idx));
            else
                emit_tag_imm(in.dst, kTagUnk);
            return;
        }
        // split 模式下位置按块查表；默认模式保持原行为。
        const MOperand& sop = slot_mop(in.idx);
        // 融合目标非空时**必须**真的写过去，不能走别名——别名只是
        // 让 %v 指向变量的位置，而 `store.slot s, %v` 已经被吞掉，
        // 变量的位置不会有任何变化，值就悄悄丢了。
        // split 模式下禁用别名优化：别名让 vreg 直接指向槽的位置，
        // 而 split 的位置是**按块**的，跨块或块内 store 之后会失效。
        if (slot_loc_.empty() && dst_loc_.empty() && in.dst != kNoReg &&
            in.dst < use_count_.size() &&
            use_count_[in.dst] == 1 && !cross_block_[in.dst] &&
            (opt_ >= 1 || sop.kind == MOperandKind::Reg)) {
            // 读之后、第一次真正使用之前不能有对该变量的赋值。
            bool clobbered = false;
            auto stv = block_stores_.find(in.idx);
            auto us = block_first_use_.find(in.dst);
            if (stv != block_stores_.end() && us != block_first_use_.end()) {
                auto it = std::upper_bound(stv->second.begin(), stv->second.end(), cur_pos_);
                if (it != stv->second.end() && *it < us->second) clobbered = true;
            }
            if (!clobbered) {
                alias_[in.dst] = sop;
                return;
            }
        }
        move_to(dst_mop(in.dst), sop);
    }

void AsmGen::gen_const_int(const Inst& in) {
        const MOperand& d = dst_mop(in.dst);
        emit_tag_imm(in.dst, kTagInt);
        if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) {
            emit_imm(MOp::kMovq, in.imm, d);
        } else {
            emit_imm_hex(MOp::kMovabsq, static_cast<uint64_t>(in.imm), kRax);
            emit2(MOp::kMovq, kRax, d);
        }
    }

void AsmGen::gen_const_float(const Inst& in) {
        uint64_t bits = 0;
        std::memcpy(&bits, &in.fimm, sizeof(bits));
        emit_imm_hex(MOp::kMovabsq, bits, kRax);
        emit2(MOp::kMovq, kRax, kXmm0);
        move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
    }

void AsmGen::gen_bin(const Inst& in) {
        TypePtr ty = in.ty;
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::Bytes)) {
            if (in.bop != BinKind::Add) {
                line("# unsupported string operator");
                return;
            }
            emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
            emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(1));
            spill_fp_slots();
            // str 的 pub 相加、bytes 的 pub = 字节数，两个入口分开
            line(ty->kind == TyKind::Bytes ? "call pa_bytes_concat" : "call pa_str_concat");
            reload_fp_slots();
            move_to(dst_mop(in.dst), mreg_operand(kRax));
            return;
        }
        if (ir_is_float(ty)) {
            gen_bin_fp(in);
        } else {
            gen_bin_int(in);
        }
    }

void AsmGen::gen_bin_int(const Inst& in) {
        // 结构化操作数直取（缓存单元引用，免字符串拷贝与回解析）。
        const MOperand& sa = src_mop(in.a);
        const MOperand& sb = src_mop(in.b);
        const MOperand& d = dst_mop(in.dst);

        // 目标位置恰好装着左操作数时，可以就地算：`x = x + y` 变成
        // 一条 `addq y, x`。这是变量提升之后最常见的形状，省掉
        // 「搬到 rax、算、搬回去」两条指令。
        if (d.kind == MOperandKind::Reg && sa.text == d.text) {
            switch (in.bop) {
                case BinKind::Add: emit2(MOp::kAddq, sb, d); return;
                case BinKind::Sub: emit2(MOp::kSubq, sb, d); return;
                case BinKind::Mul: emit2(MOp::kImulq, sb, d); return;
                case BinKind::BitAnd: emit2(MOp::kAndq, sb, d); return;
                case BinKind::BitOr: emit2(MOp::kOrq, sb, d); return;
                case BinKind::BitXor: emit2(MOp::kXorq, sb, d); return;
                // 移位量必须落在 %cl（x86 硬性要求），不能按实参位置装填：
                // Win64 的 arg_reg_i(3) 是 %r9。SysV 下两者同为 %rcx。
                case BinKind::Shl:
                    emit2(MOp::kMovq, sb, kRcx);
                    emit2(MOp::kShlq, kCl, d);
                    return;
                case BinKind::Shr:
                    emit2(MOp::kMovq, sb, kRcx);
                    emit2(MOp::kSarq, kCl, d);
                    return;
                default: break;  // 除法/取模/幂要用 rax/rdx，走通用路径
            }
        }

        emit2(MOp::kMovq, sa, kRax);
        switch (in.bop) {
            case BinKind::Add: emit2(MOp::kAddq, sb, kRax); break;
            case BinKind::Sub: emit2(MOp::kSubq, sb, kRax); break;
            case BinKind::Mul: emit2(MOp::kImulq, sb, kRax); break;
            case BinKind::BitAnd: emit2(MOp::kAndq, sb, kRax); break;
            case BinKind::BitOr: emit2(MOp::kOrq, sb, kRax); break;
            case BinKind::BitXor: emit2(MOp::kXorq, sb, kRax); break;
            case BinKind::Shl:
                emit2(MOp::kMovq, sb, kRcx);
                emit2(MOp::kShlq, kCl, kRax);
                break;
            case BinKind::Shr:
                emit2(MOp::kMovq, sb, kRcx);
                emit2(MOp::kSarq, kCl, kRax);
                break;
            case BinKind::Div:
                if (gen_div_const(in, kDivModeTrunc)) break;
                emit2(MOp::kMovq, sb, div_reg());
                gen_div_guard();
                emit0(MOp::kCqto);
                if (!target_abi().gnu_cfi) emit2(MOp::kMovq, kR10, kRcx);
                emit1(MOp::kIdivq, kRcx);
                break;
            case BinKind::Mod: gen_mod(in); break;
            case BinKind::FloorDiv: gen_floordiv(in); break;
            case BinKind::Pow:
                emit2(MOp::kMovq, sa, arg_reg_i(0));
                emit2(MOp::kMovq, sb, arg_reg_i(1));
                spill_fp_slots();
                line("call pa_pow_i64");
                reload_fp_slots();
                break;
        }
        move_to(d, mreg_operand(kRax));
    }

// idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。

void AsmGen::gen_div_guard() {
        std::string ok = fresh_label("divok");
        std::string ok2 = fresh_label("divok2");
        emit2(MOp::kTestq, div_reg(), div_reg());
        emit_jmp(MOp::kJne, ok);
        line("call pa_panic_div0");
        label(ok);
        emit_imm(MOp::kCmpq, -1, div_reg());
        emit_jmp(MOp::kJne, ok2);
        line("movabsq $0x8000000000000000, %r8");
        line("cmpq %r8, %rax");
        emit_jmp("jne", ok2);
        line("call pa_panic_overflow");
        label(ok2);
    }

// ── 常数除数的除法：魔数乘法替代 idivq ─────────────────────────
//
// 生成后 %rax = 向零取整商（与 idivq 的约定一致）。mode 1/2 再据此算余数
// 并按 Python 的语义修正符号。%r9 全程保存被除数。
void AsmGen::gen_div_const_emit(int64_t d, int mode, bool a_nonneg) {
        const uint64_t ad = d < 0 ? static_cast<uint64_t>(-(d + 1)) + 1
                                  : static_cast<uint64_t>(d);
        const int pl = pow2_log2(ad);
        uint64_t mp = 0;
        int L = 0;
        bool ok = false;
        if (pl >= 0) {
            L = pl;
            ok = true;
        } else {
            ok = udiv_magic(ad, &mp, &L);
        }
        if (!ok) { line("# internal: no magic for constant divisor"); }

        // 被除数可证非负且除数为正：|x| == x，商非负，余数天然落在
        // [0, d) —— 省掉 abs/符号还原（4 条）与余数符号修正（test+jns+
        // add，3 条）。2 的幂：商是逻辑右移，mod 退化成一条 AND。
        // 魔数本来就按无符号域算（udiv_magic），x>=0 时直接复用。
        if (a_nonneg && d > 0) {
            if (pl >= 0) {
                line("movq %r9, %rax");
                if (mode == kDivModeMod) {
                    if (pl > 0) emit_imm(MOp::kAndq, static_cast<int64_t>(ad - 1), kRax);
                } else if (pl > 0) {
                    emit_imm(MOp::kShrq, pl, kRax);
                }
                return;
            }
            emit2(MOp::kMovq, kR9, kR11);
            emit_imm_hex(MOp::kMovabsq, mp, kRcx);
            line("mulq %rcx");            // rdx = mulhu(x, m')
            emit2(MOp::kAddq, kR11, kRdx);
            emit_imm(MOp::kShrq, L, kRdx);
            if (mode != kDivModeMod) {
                line("movq %rdx, %rax");  // trunc == floor（x>=0, d>0）
                return;
            }
            emit2(MOp::kMovq, kRdx, kRcx);
            emit_imm(MOp::kImulq, d, kRcx);
            emit2(MOp::kMovq, kR9, kRax);
            emit2(MOp::kSubq, kRcx, kRax);      // r = x - q*d ∈ [0, d)
            return;
        }

        if (pl >= 0) {
            // 2 的幂：trunc(x / 2^L) = (x + (x>>63 & (2^L-1))) >> L
            line("movq %r9, %rax");
            if (pl > 0) {
                emit2(MOp::kMovq, kRax, kRcx);
                emit_imm(MOp::kSarq, 63, kRcx);
                emit_imm(MOp::kAndq, static_cast<int64_t>(ad - 1), kRcx);
                emit2(MOp::kAddq, kRcx, kRax);
                emit_imm(MOp::kSarq, pl, kRax);
            }
        } else {
            // ax = |x|（无符号域），q_abs = (mulhu(ax, m') + ax) >> L
            line("movq %r9, %r8");
            line("sarq $63, %r8");        // r8 = 0 或 -1
            line("movq %r9, %rax");
            line("xorq %r8, %rax");
            line("subq %r8, %rax");       // rax = ax
            emit2(MOp::kMovq, kRax, kR11);
            emit_imm_hex(MOp::kMovabsq, mp, kRcx);
            line("mulq %rcx");            // rdx:rax = ax * m', rdx = mulhu
            emit2(MOp::kAddq, kR11, kRdx);
            emit_imm(MOp::kShrq, L, kRdx);
            line("movq %rdx, %rax");
            line("xorq %r8, %rax");
            line("subq %r8, %rax");       // 按符号还原成有符号商
        }
        // 除数为负：trunc(x/d) = -trunc(x/|d|)。
        if (d < 0) line("negq %rax");
        if (mode == kDivModeTrunc) return;

        // 余数 r = x - q*d。q 先挪到 %rcx，|d| <= 2^31 所以能用 32 位立即数。
        line("movq %rax, %rcx");
        emit_imm(MOp::kImulq, d, kRcx);
        line("movq %r9, %rdx");
        line("subq %rcx, %rdx");
        std::string done = fresh_label("dcdiv");
        line("testq %rdx, %rdx");
        // r 与除数异号时才要修正。除数的符号编译期已知，省掉一次 xor。
        emit_jmp(d > 0 ? "jns" : "jle", done);
        if (mode == kDivModeFloor) {
            line("decq %rax");
            label(done);
            return;
        }
        // Python 的 % 结果与除数同号：r 异号时补一个 d。
        emit_imm(MOp::kAddq, d, kRdx);
        label(done);
        line("movq %rdx, %rax");
}

bool AsmGen::gen_div_const(const Inst& in, int mode) {
        if (in.a == kNoReg || in.b == kNoReg) return false;
        int64_t d = 0;
        if (!operand_const_imm(src_op(in.b), &d)) return false;
        // 除数 0 交给通用路径（它调 pa_panic_div0 给可读报错）；除数 -1 也要
        // 走通用路径：x * -1 的 INT64_MIN 溢出保护省不得，而魔数法算不出那个
        // 上溢（它会安静地给出 INT64_MIN）。
        if (d == 0 || d == -1 || d == INT64_MIN) return false;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        if (mode == kDivModeMod && d == 1) { line("xorq %rax, %rax"); return true; }
        if (d == 1) { emit2(MOp::kMovq, src_mop(in.a), kRax); return true; }

        // %r9 暂存被除数：算完商之后还原余数还要用它。%r9 是块内临时寄存器，
        // 不受槽分配影响（vreg 只用 callee pool：rbx/r12-r15/rbp）。
        emit2(MOp::kMovq, src_mop(in.a), kR9);
        // 被除数可证非负（降级期打标）→ 无符号魔数路径。优化遍新建的
        // vreg 没有标（越界按 false），保守走有符号路径。
        bool a_nn = static_cast<size_t>(in.a) < f_->vreg_nonneg.size() &&
                    f_->vreg_nonneg[static_cast<size_t>(in.a)] != 0;
        gen_div_const_emit(d, mode, a_nn);
        return true;
}

// x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
void AsmGen::gen_floordiv(const Inst& in) {
        if (gen_div_const(in, kDivModeFloor)) return;
        std::string done = fresh_label("fdiv");
        emit2(MOp::kMovq, src_mop(in.b), mreg_from_name(div_reg()));
        gen_div_guard();
        line("cqto");
        if (!target_abi().gnu_cfi) line("movq %r10, %rcx");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        emit_jmp("je", done);
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        emit_jmp("jns", done);
        line("decq %rax");
        label(done);
    }

// Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
void AsmGen::gen_mod(const Inst& in) {
        if (gen_div_const(in, kDivModeMod)) return;
        std::string done = fresh_label("mod");
        emit2(MOp::kMovq, src_mop(in.b), mreg_from_name(div_reg()));
        gen_div_guard();
        line("cqto");
        if (!target_abi().gnu_cfi) line("movq %r10, %rcx");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        emit_jmp("je", done);
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        emit_jmp("jns", done);
        line("addq %rcx, %rdx");
        label(done);
        line("movq %rdx, %rax");
    }

void AsmGen::gen_bin_fp(const Inst& in) {
        const MOperand& d = dst_mop(in.dst);
        bool simple = in.bop == BinKind::Add || in.bop == BinKind::Sub ||
                      in.bop == BinKind::Mul || in.bop == BinKind::Div;
        // 结果直接落在提升过的浮点寄存器时，把左操作数装进目标寄存器
        // 就地算。累加 `s = s + ...` 因此只剩一条 addsd。
        if (opt_ >= 1 && simple && d.kind == MOperandKind::Reg &&
            d.reg >= kXmm0 && d.reg <= kXmm15) {
            const MOperand& sa = src_mop(in.a);
            if (sa.text != d.text) move_fp(d, sa);
            switch (in.bop) {
                case BinKind::Add: emit2(MOp::kAddsd, src_mop(in.b), d); break;
                case BinKind::Sub: emit2(MOp::kSubsd, src_mop(in.b), d); break;
                case BinKind::Mul: emit2(MOp::kMulsd, src_mop(in.b), d); break;
                case BinKind::Div: emit2(MOp::kDivsd, src_mop(in.b), d); break;
                default: break;
            }
            return;
        }
        move_fp(mreg_operand(kXmm0), src_mop(in.a));
        switch (in.bop) {
            case BinKind::Add: emit2(MOp::kAddsd, src_mop(in.b), kXmm0); break;
            case BinKind::Sub: emit2(MOp::kSubsd, src_mop(in.b), kXmm0); break;
            case BinKind::Mul: emit2(MOp::kMulsd, src_mop(in.b), kXmm0); break;
            case BinKind::Div: emit2(MOp::kDivsd, src_mop(in.b), kXmm0); break;
            case BinKind::FloorDiv:
                // floor(a / b)：先除再向下取整。roundsd 要 SSE4.1，
                // 为了兼容性走 libm。
                emit2(MOp::kDivsd, src_mop(in.b), kXmm0);
                spill_fp_slots();
                line("call floor");
                reload_fp_slots();
                break;
            case BinKind::Mod: {
                // C 的 fmod 余数与被除数同号，Python 的 % 与除数同号，
                // 两者在异号时差一个 |b|，这里补上。xmm1 是调用者保存的，
                // fmod 回来必须重新装 b。余数为 0 时 Python 取除数的符号，
                // 所以 -0.0 % 3.0 是 0.0 而不是 -0.0。
                std::string done = fresh_label("fmod");
                std::string nz = fresh_label("fmodnz");
                emit2(MOp::kMovsd, src_mop(in.b), kXmm1);
                spill_fp_slots();
                line("call fmod");
                reload_fp_slots();
                emit2(MOp::kMovsd, src_mop(in.b), kXmm1);
                line("xorpd %xmm2, %xmm2");
                line("ucomisd %xmm2, %xmm0");
                emit_jmp(MOp::kJp, done);  // NaN 原样返回
                emit_jmp(MOp::kJne, nz);
                line("movq %xmm1, %rdx");
                line("movabsq $0x8000000000000000, %rax");
                line("andq %rdx, %rax");
                line("movq %rax, %xmm0");  // ±0.0，符号跟除数
                emit_jmp("jmp", done);
                label(nz);
                line("movq %xmm0, %rax");
                line("movq %xmm1, %rdx");
                line("xorq %rdx, %rax");
                emit_jmp(MOp::kJns, done);  // 同号，无需修正
                line("addsd %xmm1, %xmm0");
                label(done);
                break;
            }
            case BinKind::Pow:
                emit2(MOp::kMovsd, src_mop(in.b), kXmm1);
                spill_fp_slots();
                line("call pow");
                reload_fp_slots();
                break;
            default:
                line("# unsupported float operator");
                break;
        }
        move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
    }

// xmm3-7 浮点临时值只被「会正常返回的调用」和「使用 xmm3-7 的指令」
// 破坏。与 GPR 临时池（r8-r11）的判定独立：整数 div/mod/floordiv 的
// 魔数/idiv 序列只用 GPR scratch（panic 调用不返回），不碰 xmm ——
// spectral_norm 的 eval_a 内联后含 //2，旧判定把整块踢出临时分配，
// 所有浮点中间值被迫走栈往返。浮点 FloorDiv/Mod/Pow 调 floor/fmod/pow
// 会正常返回 → 不安全；列表 IndexLoad 的越界慢路径不返回 → 安全，
// 字符串 IndexLoad 调 pa_str_char_at 会返回 → 不安全。
bool AsmGen::fp_temp_op_ok(const Inst& in) const {
        switch (in.op) {
            case IrOp::Bin: {
                TypePtr t = in.ty;
                if (t && t->kind == TyKind::Str) return false;
                bool fp = t && ir_is_float(t);
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
                        return true;   // 浮点只用 xmm0；整数 GPR + 不返回的 panic
                    case BinKind::Mod:
                    case BinKind::FloorDiv:
                        return !fp;    // 浮点调 fmod/floor（会返回）
                    default:
                        return false;  // Pow 调 pow/pa_pow_i64
                }
            }
            case IrOp::IndexLoad: {
                TypePtr bt = ty_of(in.a);
                return !(bt && bt->kind == TyKind::Str);
            }
            case IrOp::Un:
            case IrOp::Cmp:
            case IrOp::Convert:
                return true;   // 只用 xmm0/xmm1 或 GPR
            default:
                return temp_reg_op_ok(in);
        }
    }

void AsmGen::gen_un(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (in.uop == UnKind::Not) {
            emit_truth(in.a, "%al");
            emit_imm(MOp::kXorb, 1, kAl);
            emit2(MOp::kMovzbq, kAl, kRax);
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
        if (ir_is_float(ty)) {
            if (in.uop == UnKind::Neg) {
                move_fp(mreg_operand(kXmm0), src_mop(in.a));
                line("movabsq $0x8000000000000000, %rax");
                line("movq %rax, %xmm1");
                line("xorpd %xmm1, %xmm0");
                move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            }
            return;
        }
        emit2(MOp::kMovq, src_mop(in.a), kRax);
        switch (in.uop) {
            case UnKind::Neg: emit1(MOp::kNegq, kRax); break;
            case UnKind::BitNot: emit1(MOp::kNotq, kRax); break;
            default: break;
        }
        emit2(MOp::kMovq, kRax, dst_mop(in.dst));
    }

// 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
void AsmGen::emit_truth(VReg r, const char* byte_reg) {
        (void)byte_reg;
        TypePtr ty = ty_of(r);
        if (ir_is_float(ty)) {
            move_fp(mreg_operand(kXmm0), src_mop(r));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            line("setne %al");
            line("setp %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            emit2(MOp::kMovq, src_mop(r), kRax);
            emit_imm(MOp::kCmpq, 0, op_mem(kRax));
            emit1(MOp::kSetne, kAl);
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2(MOp::kMovq, src_mop(r), kRax);
            emit2(MOp::kXorpd, kXmm1, kXmm1);
            emit2(MOp::kUcomisd, op_mem(kRax, 8), kXmm1);
            emit1(MOp::kSetne, kAl);
            emit2(MOp::kUcomisd, op_mem(kRax, 16), kXmm1);
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2(MOp::kMovq, src_mop(r), kRax);
            emit2(MOp::kXorpd, kXmm1, kXmm1);
            emit2(MOp::kUcomisd, op_mem(kRax, 8), kXmm1);
            emit1(MOp::kSetne, kAl);
            emit2(MOp::kUcomisd, op_mem(kRax, 16), kXmm1);
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        // 静态类型 unknown：可能是整数、空串、空容器、None —— 必须按
        // Python 真值判断（0/0.0/空串/空容器为假）。按整数比较会把
        // 空串（非空指针）当成真，filter(None, 异构列表) 因此保留空串。
        if (!ty || ty->kind == TyKind::Unknown) {
            emit2(MOp::kMovq, src_mop(r), arg_reg_i(0));
            spill_fp_slots();
            line("call pa_truth_unknown");
            reload_fp_slots();
            line("testq %rax, %rax");
            line("setne %al");
            return;
        }
        const MOperand* s = &src_mop(r);
        // 立即数不能直接进 cmpq 的源位置：先落 %rax。常量条件一般会被
        // 分支折叠吃掉，这里只兜底（如 bool(x) 的 x 是常量）。
        if (s->kind == MOperandKind::Imm) {
            emit2(MOp::kMovq, *s, kRax);
            s = &mreg_operand(kRax);
        }
        emit_imm(MOp::kCmpq, 0, *s);
        emit1(MOp::kSetne, kAl);
    }

void AsmGen::gen_cmp(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::Bytes)) {
            if (in.cop == CmpKind::Eq || in.cop == CmpKind::Ne) {
                // 同一对象的快路径：字符串不可变，指针相等必然内容相等。
                // 单字节字面量已经被接到运行时的驻留表上，所以
                // `s[i] == "0"` 这类逐字符比较直接命中这里，完全不必进
                // pa_str_eq（那种比较在 str_build 里每轮内层循环 60 次）。
                const MReg da = mreg_from_name(arg_reg_i(0));
                const MReg db = mreg_from_name(arg_reg_i(1));
                const std::string same = fresh_label("streq");
                const std::string done = fresh_label("streqd");
                emit2(MOp::kMovq, src_mop(in.a), da);
                emit2(MOp::kMovq, src_mop(in.b), db);
                emit2(MOp::kCmpq, db, da);
                emit_jmp(MOp::kJe, same);
                // 0.288：单字节内容快路径。指针不等之后，双方 len==1 就
                // 直接比 data[0]。str_build 的 c=="0" 有 9/10 是不等的
                // 数字字符，原来每次都要为一个字节比较走完整 pa_str_eq
                // 调用（占该用例 8.4%）。len 在 +8、data 在 +24，str/bytes
                // 同布局；scratch 用 r10/r11 避开 Win64 实参寄存器。
                const std::string slow = fresh_label("strslow");
                emit_imm(MOp::kCmpq, 1, op_mem(da, 8));
                emit_jmp(MOp::kJne, slow);
                emit_imm(MOp::kCmpq, 1, op_mem(db, 8));
                emit_jmp(MOp::kJne, slow);
                emit2(MOp::kMovq, op_mem(da, 24), kR10);
                emit2(MOp::kMovq, op_mem(db, 24), kR11);
                line("movzbq (%r10), %rax");
                line("cmpb (%r11), %al");
                emit_jmp(MOp::kJe, same);
                line(in.cop == CmpKind::Eq ? "movq $0, %rax" : "movq $1, %rax");
                emit_jmp(MOp::kJmp, done);
                label(slow);
                spill_fp_slots();
                line("call pa_str_eq");
                reload_fp_slots();
                // != 是 == 的取反。**慢路径也要取反** —— 曾经只改了「指针
                // 相等」那条快路径，非等指针的 != 直接返回了 == 的结果。
                if (in.cop == CmpKind::Ne) emit_imm(MOp::kXorq, 1, kRax);
                emit_jmp(MOp::kJmp, done);
                label(same);
                line(in.cop == CmpKind::Eq ? "movq $1, %rax" : "movq $0, %rax");
                label(done);
                emit2(MOp::kMovq, kRax, dst_mop(in.dst));
                return;
            }
            // 大小比较走三路比较。早先这里只留了一句注释、什么都不生成，
            // 于是 a < b 静默拿到槽里的残留值——恒为 False。
            emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
            emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(1));
            spill_fp_slots();
            line("call pa_str_cmp");
            reload_fp_slots();
            line("cmpq $0, %rax");
            switch (in.cop) {
                case CmpKind::Lt: line("setl %al"); break;
                case CmpKind::Le: line("setle %al"); break;
                case CmpKind::Gt: line("setg %al"); break;
                case CmpKind::Ge: line("setge %al"); break;
                default: break;
            }
            emit2(MOp::kMovzbq, kAl, kRax);
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
        if (ir_is_float(ty)) {
            gen_cmp_fp(in);
            return;
        }
        // cmp 不写坏操作数，所以左操作数已经在寄存器/栈上时不必先搬进
        // rax。但有两个限制：cmp 不能两个操作数都在内存；立即数只能
        // 放在第一操作数，直接写会把减法方向搞反。
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
        switch (in.cop) {
            case CmpKind::Lt: line("setl %al"); break;
            case CmpKind::Le: line("setle %al"); break;
            case CmpKind::Gt: line("setg %al"); break;
            case CmpKind::Ge: line("setge %al"); break;
            case CmpKind::Eq: line("sete %al"); break;
            case CmpKind::Ne: line("setne %al"); break;
        }
        emit2(MOp::kMovzbq, kAl, kRax);
        move_to(dst_mop(in.dst), mreg_operand(kRax));
    }

void AsmGen::gen_cmp_fp(const Inst& in) {
        move_fp(mreg_operand(kXmm0), src_mop(in.a));
        emit2(MOp::kUcomisd, src_mop(in.b), kXmm0);
        switch (in.cop) {
            case CmpKind::Lt:
                line("setb %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Le:
                line("setbe %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Gt:
                line("seta %al");
                break;
            case CmpKind::Ge:
                line("setae %al");
                break;
            case CmpKind::Eq:
                line("sete %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Ne:
                line("setne %al");
                line("setp %cl");
                line("orb %cl, %al");
                break;
        }
        emit2(MOp::kMovzbq, kAl, kRax);
        emit2(MOp::kMovq, kRax, dst_mop(in.dst));
    }

void AsmGen::gen_convert(const Inst& in) {
        TypePtr from = ty_of(in.a);
        TypePtr to = in.ty;
        if (!to) return;
        if (to->kind == TyKind::Bool) {
            emit_truth(in.a, "%al");
            emit2(MOp::kMovzbq, kAl, kRax);
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
        if (to->kind == TyKind::Float) {
            if (ir_is_float(from)) {
                // 已经是 double，只搬一次
                move_fp(dst_mop(in.dst), src_mop(in.a));
                return;
            }
            // cvtsi2sd 的源必须是寄存器或内存，立即数不能直接转
            const MOperand* sa = &src_mop(in.a);
            if (sa->kind == MOperandKind::Imm) {
                emit2(MOp::kMovq, *sa, kRax);
                sa = &mreg_operand(kRax);
            }
            emit2(MOp::kCvtsi2sdq, *sa, kXmm0);
            move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            return;
        }
        if (to->kind == TyKind::Int) {
            if (ir_is_float(from)) {
                emit2(MOp::kCvttsd2siq, src_mop(in.a), kRax);
                emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            } else {
                emit2(MOp::kMovq, src_mop(in.a), kRax);
                emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            }
            return;
        }
        if (to->kind == TyKind::Str) {
            // 源类型已经是 str：什么都不用做，只补一条搬运
            if (from && from->kind == TyKind::Str) {
                emit2(MOp::kMovq, src_mop(in.a), kRax);
                emit2(MOp::kMovq, kRax, dst_mop(in.dst));
                return;
            }
            if (ir_is_float(from)) {
                move_fp(mreg_operand(kXmm0), src_mop(in.a));
                spill_fp_slots();
                line("call pa_str_from_f64");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::Bool) {
                emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
                spill_fp_slots();
                line("call pa_str_from_bool");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::None) {
                spill_fp_slots();
                line("call pa_str_from_none");
                reload_fp_slots();
            } else {
                emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
                spill_fp_slots();
                line("call pa_str_from_i64");
                reload_fp_slots();
            }
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
    }


// ── 零开销异常（Z2-b2）：保护区 / landing pad 标记的发射 ─────────────
void AsmGen::gen_eh_begin() {
    // 保护区起点：标签地址 = 紧随其后的第一条指令。
    //
    // 同时标记「进入表驱动展开区」：C 运行时抛的异常（IndexError /
    // ZeroDivisionError 等）走的是 exc_longjmp，它据此改走 _Unwind，
    // 否则 setjmp 路径的 handler 栈抓不到这些异常。
    if (eh_begin_seq_ < eh_begin_labels_.size())
        label(eh_begin_labels_[eh_begin_seq_++]);
    line("call pa_eh_enter");
}

void AsmGen::gen_eh_end() {
    // 保护区终点。先退标记再落标签：标签之后的代码已不在保护区里。
    line("call pa_eh_leave");
    if (eh_end_seq_ < eh_end_labels_.size())
        label(eh_end_labels_[eh_end_seq_++]);
}

void AsmGen::gen_eh_landing_pad() {
    // landing pad 入口：%rax 是展开包装指针（personality 的 GR0）；紧随其后的
    // CallRt EhLand 会把它作为第一个实参传给 pa_eh_land。
    if (eh_lp_seq_ < eh_lp_labels_.size()) label(eh_lp_labels_[eh_lp_seq_++]);
}

}  // namespace pyadamas
