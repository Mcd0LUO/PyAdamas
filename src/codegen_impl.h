// codegen_impl.h —— AsmGen 的实现细节（只给 codegen_*.cpp 用）
//
// 代码生成按职责拆成：驱动/数据段（codegen.cpp）、寄存器分配
// （codegen_regalloc.cpp）、函数与基本块（codegen_block.cpp）、
// 指令选择（codegen_ops.cpp）、调用与对象（codegen_call.cpp）。
#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir.h"
#include "mach_instr.h"
#include "target_abi.h"
#include "module.h"

namespace pyadamas {

// 实参寄存器一律从 TargetABI 取：SysV 6 整型 / 8 浮点，Win64 各 4 个。
inline int kMaxIntArgs() { return target_abi().n_arg_int; }
inline int kMaxFpArgs() { return target_abi().n_arg_fp; }

// 可用来长期持有变量的寄存器。全部是 callee-saved：被调用方保证
// 返回时原样恢复，所以跨函数调用不需要把值挪回栈。
// rsp 是栈指针，不在其中；rbp 在——生成函数的帧相对 rsp 寻址，不需要
// 帧指针，回溯走 .eh_frame，所以它也能拿来存变量。
//
// 寄存器角色统一由 TargetABI 描述（见 target_abi.h）：这是换目标的唯一
// 接缝，取值在编译期定死，运行期零开销。
inline const char* const* kSlotRegs() { return target_abi().callee_pool; }
inline int kMaxSlotRegs() { return target_abi().n_callee_pool; }

// 浮点变量常驻寄存器。只用 xmm8–15：xmm0–7 是参数/临时寄存器，
// 所有指令生成器都只把它们当 scratch。提升仅发生在函数没有任何
// 「会正常返回的调用」时，所以 caller-saved 不影响正确性。
// 第 i 个整型/浮点实参寄存器。实参寄存器顺序是各 ABI 差异最大的地方
// （SysV 6 个整型 / 8 个浮点，Win64 各 4 个），所以一律走这里取。
inline const char* arg_reg_i(int i) { return target_abi().arg_int[i]; }
inline const char* arg_reg_f(int i) { return target_abi().arg_fp[i]; }
// 同一个实参寄存器的 32 位视图。xorl 清零、movl 存字段这类指令需要
// 32 位名字（%rsi -> %esi，Win64 的 %r8 -> %r8d），实参寄存器本身两种
// 目标都是 64 位名字，不能直接套。
// 64 位寄存器名的 32 位别名（%rdx -> %edx，%r8 -> %r8d）。
// 注意 %rdi/%rsi/%rbp/%rsp 的末位不是 x，只看第三个字符会把 %rdi
// 错认成 %rdx。
inline std::string reg32(const std::string& r) {
    if (r.size() == 4 && r[1] == 'r') {
        if (r[3] == 'x') {
            switch (r[2]) {
                case 'a': return "%eax";
                case 'b': return "%ebx";
                case 'c': return "%ecx";
                case 'd': return "%edx";
                default: break;
            }
        } else if (r[2] == 's' && r[3] == 'i') {
            return "%esi";
        } else if (r[2] == 'd' && r[3] == 'i') {
            return "%edi";
        } else if (r[2] == 'b' && r[3] == 'p') {
            return "%ebp";
        } else if (r[2] == 's' && r[3] == 'p') {
            return "%esp";
        }
    }
    if (r.size() == 3 && r[1] == 'r' && r[2] >= '8' && r[2] <= '9')
        return std::string(r) + "d";
    if (r.size() == 4 && r[1] == 'r' && r[2] == '1' && r[3] >= '0' && r[3] <= '5')
        return std::string(r) + "d";
    return r;
}

// 64 位寄存器名的 8 位别名（%rdx -> %dl，%r9 -> %r9b）。
inline std::string reg8(const std::string& r) {
    if (r.size() == 4 && r[1] == 'r') {
        if (r[3] == 'x') {
            switch (r[2]) {
                case 'a': return "%al";
                case 'b': return "%bl";
                case 'c': return "%cl";
                case 'd': return "%dl";
                default: break;
            }
        } else if (r[2] == 's' && r[3] == 'i') {
            return "%sil";
        } else if (r[2] == 'd' && r[3] == 'i') {
            return "%dil";
        } else if (r[2] == 'b' && r[3] == 'p') {
            return "%bpl";
        } else if (r[2] == 's' && r[3] == 'p') {
            return "%spl";
        }
    }
    if (r.size() == 3 && r[1] == 'r' && r[2] >= '8' && r[2] <= '9')
        return std::string(r) + "b";
    if (r.size() == 4 && r[1] == 'r' && r[2] == '1' && r[3] >= '0' && r[3] <= '5')
        return std::string(r) + "b";
    return r;
}

// 实参寄存器的 32 位 / 8 位视图。
inline std::string arg_reg_i32(int i) { return reg32(target_abi().arg_int[i]); }
inline std::string arg_reg_i8(int i) { return reg8(target_abi().arg_int[i]); }

inline const char* const* kFpSlotRegs() { return target_abi().fp_pool; }
inline int kMaxFpSlotRegs() { return target_abi().n_fp_pool; }

// 块内浮点临时寄存器。xmm0/xmm1 被各指令生成器当固定 scratch，
// xmm2 被浮点取模用来装除数，所以从 xmm3 开始。只在「整块没有
// 调用类指令」时借用，块结束即归还。
inline const char* const kFpTempRegs[] = {"%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7"};
constexpr int kMaxFpTempRegs = 5;

// 一条指令里临时用的寄存器。和 kSlotRegs 不相交，指令生成中途可以
// 随便写，不会踩到变量。

struct ArgLoc {
    bool fp = false;
    int reg = -1;       // 寄存器下标
    int stack_off = -1; // 相对 rsp 的偏移
};

inline std::string asm_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 32 || c >= 127) out += std::format("\\{:03o}", c);
                else out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

class AsmGen {
public:
        std::string gen(const IRModule& m);
        // 整程序没有任何 unknown 值、也不涉及 asyncio/生成器运行时：入口
        // 关闭存活堆索引维护（unknown 保守 retain 用不到）。
        void set_disable_unknown_index(bool v) { disable_unknown_index_ = v; }
        // 零开销异常总开关（--zero-cost-exc，默认关）。
        void set_zero_cost_exc(bool v) { zero_cost_exc_ = v; }

private:
    const std::string& global_label(int i) const {
        return global_labels_[static_cast<size_t>(i)];
    }
    std::string roots_label() const {
        return m_->module_symbol.empty() ? ".Lgroots"
                                         : module_roots_symbol(m_->module_symbol);
    }
    size_t local_global_count() const {
        size_t n = 0;
        for (auto& g : m_->globals)
            if (!g.external) ++n;
        return n;
    }
    const IRModule* m_ = nullptr;
    const IRFunction* f_ = nullptr;
    int fi_ = 0;
    // 生成代码优化等级：0 基础 / 1 常规 / 2 默认追加控制流与小函数优化。
    int opt_ = 2;
    bool disable_unknown_index_ = false;
    int cur_block_ = -1;
    int n_slots_ = 0;
    int n_vreg_slots_ = 0;
    // 虚拟寄存器 → 栈槽下标。一个 vreg 的槽在它死后可以给别人复用，
    // 否则 2000 行的函数会生成 100KB 的栈帧（递归几层就爆）。
    std::vector<int> vreg_slot_;
    int pos_ = 0;
    int label_id_ = 0;
    MachFunc out_;
    double alloc_ns_ = 0;
    double emit_ns_ = 0;
    // line() 的前导 tab 缓冲：复用容量，避免每行一次分配。
    std::string line_buf_;

    // ── 位置状态（每次 emit_function 重建）──────────────────────
    //
    // slot_reg_[s] 非空表示变量 s 常驻在这个寄存器里，栈槽作废。
    // 分配是函数级的、固定的：不做活跃性分析，一个变量要么整函数
    // 在寄存器里，要么整函数在栈上，规则简单到不会出隐蔽 bug。
    std::vector<std::string> slot_reg_;
    int n_saved_regs_ = 0;  // 实际 push 了几个 callee-saved
    // 提升到 xmm8–15 的浮点槽。call 会破坏 caller-saved xmm，调用前后
    // 只把「调用后仍存活」的槽写回/读出（块级活跃性在 build_fp_liveness
    // 里算好，按指令取 live-after 掩码）。
    std::vector<int> fp_promoted_slots_;
    std::vector<int> slot_fpidx_;      // slot → fp_promoted_slots_ 下标
    std::vector<uint8_t> fp_live_in_;  // nb 个块
    std::vector<uint8_t> fp_live_out_;
    std::vector<char> fp_spill_mask_;  // 当前指令需要保存的槽
    bool fp_mask_valid_ = false;
    bool fp_live_off_ = false;  // PYADAMAS_NO_FPLIVE：退回全量 spill（A/B 用）
    void build_fp_liveness();
    void spill_fp_slots();
    void reload_fp_slots();
    // 生成器/协程 resume：槽位在堆帧里，%r15 固定指向帧负载；
    // 帧头 10 个字：state/status/sent/out/resume/closing/kind/waiter/exc/cur。
    bool gen_ = false;
    static constexpr int kGenHeader = 10;

    // vreg → 它当前所在的物理位置（"%r13" 这种）。只在块内有效：
    // 块边界一律清空，所以跨块的值永远走栈槽，不会读到过期的寄存器。
    std::unordered_map<VReg, std::string> alias_;

    // 能被写成立即数的常量。同样只在块内有效（保存 "$imm" 串）。
    std::unordered_map<VReg, std::string> const_val_;

    // 整函数范围的 const.int → "$imm"。跨块的常量不必占寄存器或栈槽，
    // 所有使用点直接用立即数；定义指令在 gen_block 里跳过。
    std::unordered_map<VReg, std::string> global_imm_;

    // 本指令的 dst 该写到哪里。为空表示按 mem(dst)；非空是融合目标
    // （通常是一个变量寄存器），此时 mem(dst) 完全不参与。
    std::string dst_loc_;

    std::vector<int> use_count_;
    std::vector<bool> cross_block_;

    // 调用的 dst 没人读：不必把返回值搬回栈槽（表达式语句里的调用）。
    bool dst_unused(const Inst& in) const {
        return in.dst == kNoReg || (in.dst < static_cast<VReg>(use_count_.size()) &&
                                    use_count_[static_cast<size_t>(in.dst)] == 0);
    }
    std::vector<int> def_block_;

    // 块内临时寄存器：整块没有会调用/占用 %r8–%r11 的指令时，块内定义、
    // 块内用完的临时值直接住在寄存器里，mem(v) 返回它。块边界清空。
    std::vector<std::string> vreg_reg_;
    // 块内临时寄存器可用池：无调用函数里被跨块提升占走的 %r8–%r11 会剔除。
    std::vector<std::string> block_temp_pool_;
    bool temp_reg_op_ok(const Inst& in) const;
    void assign_block_temp_regs();

    // 生成期缓存：块标签、vreg/槽的内存操作数串。这些字符串在函数内
    // 固定不变，生成每条指令时反复 format 会占掉相当一部分编译时间。
    std::vector<std::string> block_labels_;
    std::vector<std::string> vreg_mem_;
    std::vector<std::string> smem_cache_;
    std::vector<std::string> gen_smem_cache_;
    std::vector<std::string> global_labels_;
    std::vector<std::string> func_labels_;
    void build_operand_caches();

    // 当前块内的位置索引，gen_block 开头重建。
    std::unordered_map<int, std::vector<int>> block_stores_;
    std::unordered_map<VReg, int> block_first_use_;
    int cur_pos_ = 0;  // 正在生成的指令在块内的下标

    // ── 输出辅助（结构化 Machine 层）──────────────────────────────
    //
    // out_ 曾经是 std::string，逐段拼接 AT&T 文本；现在是 MachFunc：每条
    // 发射落成 MItem（Label/Directive/Instr/Comment/Raw）。指令结构化之后，
    // 后续的寄存器分配 / 指令选择 pass 可以直接改写 MInstr 的操作数，而
    // 不必只在发射前的字符串上做映射。
    //
    // 保留 line() 这个文本入口，是为了让历史发射点零改动地接进来：它把
    // 一行文本解析成结构化 item（认不出的文法退化 Raw，字节不变）。
    // 新写的发射点应优先用 instr()/directive()/label() 直接构造。

    // 兜底：原样文本进 Raw（与上一条 Raw 合并）。全部迁移完后应无调用点。
    void emit_raw(std::string_view s) { out_.append_raw(s); }

    // 一行汇编：指令 / 指示符 / 标签 / 注释都走这里。
    // 含换行的多行文本直接进 Raw，保证字节不变。
    void line(std::string_view s) {
        if (s.find('\n') != std::string_view::npos) {
            emit_raw("\t");
            emit_raw(s);
            emit_raw("\n");
            return;
        }
        // 旧实现是 "\t" + s + "\n"；这里复用缓冲区补上前导 tab，避免每行一次分配。
        line_buf_.clear();
        line_buf_ += '\t';
        line_buf_.append(s);
        out_.push_line(line_buf_);
    }

    // 列 0 的标签（函数标签、数据段符号）。
    void label(const std::string& s) { out_.push_label(s, false); }
    // 带前导 tab 的块内局部标签（"\t1:" 这种）。
    void label_block(const std::string& s) { out_.push_label(s, true); }
    // "\t# s"
    void comment(const std::string& s) { out_.push(make_comment("\t# " + s)); }
    // 列 0 的注释行（"# ---- ... ----"）。
    void comment0(const std::string& s) { out_.push(make_comment("# " + s)); }
    // "\t" + body + "\n"：.align/.quad/.section 这类汇编指示符。
    void directive(std::string_view body) { out_.push_directive(body); }
    // 空行。
    void blank_line() { out_.push(make_raw("\n")); }

    // 结构化指令发射：操作码 + 操作数。
    void instr(MOp op, std::initializer_list<MOperand> ops) {
        MInstr ins;
        ins.op = op;
        ins.ops.assign(ops);
        out_.push_instr(std::move(ins));
    }

    // 操作数工厂。寄存器第一阶段仍按名字存（op_reg("%rax")）。
    static MOperand op_reg(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Reg;
        o.text = std::move(name);
        return o;
    }
    static MOperand op_imm(int64_t v) {
        MOperand o;
        o.kind = MOperandKind::Imm;
        o.imm = v;
        return o;
    }
    static MOperand op_imm_hex(uint64_t bits) {
        MOperand o;
        o.kind = MOperandKind::Imm;
        o.imm = static_cast<int64_t>(bits);
        o.imm_hex = true;
        return o;
    }
    static MOperand op_label(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Label;
        o.text = std::move(name);
        return o;
    }
    static MOperand op_sym(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Sym;
        o.text = std::move(name);
        return o;
    }
    // "(%base)"
    static MOperand op_mem(std::string base) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.base = std::move(base);
        return o;
    }
    // "disp(%base)"
    static MOperand op_mem(std::string base, int64_t disp) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.base = std::move(base);
        o.disp = disp;
        o.has_disp = true;
        return o;
    }
    // "sym(%base)"
    static MOperand op_mem_sym(std::string sym, std::string base) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.sym = std::move(sym);
        o.base = std::move(base);
        return o;
    }

    // ── 目标相关的汇编指示符 ────────────────────────────────────
    // ELF/GNU as 的 .cfi_*、.type、.size、.note.GNU-stack 在 COFF 目标上
    // 要么非法要么被忽略，所以按 TargetABI::gnu_cfi 关掉。关掉后 Windows
    // 侧没有 .eh_frame 展开信息——那是 SEH 的地盘，需要单独实现。
    bool gnu_cfi() const { return target_abi().gnu_cfi; }

    // 出参区的基址。Win64 要求调用点在栈顶留 32 字节影子空间给被调方写
    // 寄存器实参，所以栈传实参和所有暂存都从 shadow_space 之上开始。
    // SysV 的 shadow_space 是 0，偏移与原来完全一致。
    int out_base() const { return target_abi().shadow_space; }

    // 搬运第 pos 个（0 基）实参到 ABI 规定的位置。is_fp 表示浮点类；
    // cls_idx 是该类内的序号（SysV 按类型分计数时用）。
    void emit_arg(int pos, bool is_fp, int cls_idx, const std::string& v);

    void cfi(const std::string& s) {
        if (target_abi().gnu_cfi) line(s);
    }
    // relro 段：ELF 下是 .data.rel.ro，COFF 没有这个概念，退到 .rdata。
    void section_relro() {
        directive(target_abi().gnu_cfi ? ".section .data.rel.ro,\"aw\",@progbits"
                                        : ".section .rdata,\"dr\"");
    }
    void section_rodata() {
        directive(target_abi().gnu_cfi ? ".section .rodata" : ".section .rdata,\"dr\"");
    }
    void emit_type_func(const std::string& n) {
        if (target_abi().gnu_cfi) directive(".type " + n + ", @function");
    }
    void emit_size_func(const std::string& n) {
        if (target_abi().gnu_cfi) directive(std::format(".size {}, .-{}", n, n));
    }
    void emit_gnu_stack_note() {
        if (target_abi().gnu_cfi) directive(".section .note.GNU-stack,\"\",@progbits");
    }

    // 热路径的迷你发射器：直接构造结构化指令，绕开 std::format。
    // 助记符没进 MOp 枚举、或操作数文法不认识时退回文本路径（Raw，字节不变）。
    void emit2(const char* mn, const std::string& a, const std::string& b) {
        MOp op = mop_from_name(mn);
        MOperand oa;
        MOperand ob;
        if (op != MOp::kRaw && parse_operand(a, false, &oa) && parse_operand(b, false, &ob)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(std::move(oa));
            ins.ops.push_back(std::move(ob));
            out_.push_instr(std::move(ins));
            return;
        }
        std::string text = mn;
        text += ' ';
        text += a;
        text += ", ";
        text += b;
        out_.push_line(text);
    }

    void emit_imm_op(const char* mn, int64_t v, const std::string& b) {
        MOp op = mop_from_name(mn);
        MOperand ob;
        if (op != MOp::kRaw && parse_operand(b, false, &ob)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(op_imm(v));
            ins.ops.push_back(std::move(ob));
            out_.push_instr(std::move(ins));
            return;
        }
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, v);
        std::string text = mn;
        text += " $";
        text.append(buf, r.ptr);
        text += ", ";
        text += b;
        out_.push_line(text);
    }

    void emit_jmp(const char* mn, const std::string& target) {
        MOp op = mop_from_name(mn);
        MOperand ot;
        if (op != MOp::kRaw && parse_operand(target, mop_is_branch(op), &ot)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(std::move(ot));
            out_.push_instr(std::move(ins));
            return;
        }
        std::string text = mn;
        text += ' ';
        text += target;
        out_.push_line(text);
    }

    void emit_imm_mem(const std::string& mem, int64_t v) { emit_imm_op("movq", v, mem); }


    std::string fresh_label(const char* tag) {
        return std::format(".L{}_{}", tag, label_id_++);
    }

    static std::string func_label(int i) { return std::format(".Lf{}", i); }
    // 统一调用约定的 shim 入口标签（见 ir_types.h 的 closure_needs_shim）。
    static std::string shim_label(int i) { return std::format(".Lshim{}", i); }

    std::string block_label(int bi) const { return block_labels_[static_cast<size_t>(bi)]; }

    // 槽位一律相对 rsp：帧的布局是
    //   [出参区 out_bytes_][槽位区 slot0…][帧记录][push 的 callee-saved][返回地址]
    // rsp 在函数体里从头到尾不动（出参区一次性预留，调用点不再 subq，
    // setjmp/longjmp 也只会回到同一个 rsp），所以偏移是常量。
    // 槽号小的（形参、最常用的局部）排在**低地址**，偏移能落在 disp8 里；
    // 帧记录这种每函数只碰一两次的放到最上面。
    int slot_off(int slot) const { return out_bytes_ + 8 * slot; }

    int vreg_off(VReg r) const {
        int s = (r < vreg_slot_.size()) ? vreg_slot_[r] : 0;
        return out_bytes_ + 8 * (n_slots_ + s);
    }

    // 帧记录（PyAdamasFrame）在槽位区上面，内存里从低地址往高地址依次是
    // prev、n、临时区起点、临时区字数、临时区位图、本帧 CFA、槽地址×n，
    // 与两边共用的结构体布局一致。
    int rec_off(int i) const {
        int words = 6 + static_cast<int>(frame_owned_.size() + frame_scan_.size());
        return out_bytes_ + locals_ - 8 * (words - i);
    }
    std::string rec_mem(int i) const { return std::format("{}(%rsp)", rec_off(i)); }

    // 本次代码生成是否压了帧记录（弹栈只在这个函数里发）。
    bool frame_rec_ = false;
    // DWARF 行号：当前已发出的 .loc 行，切函数/切行时更新。
    int cur_loc_line_ = 0;
    // 生成指令前按需要发出 .loc；line<=0 或未变化时不动。
    void set_loc(int line);
    // 运行时 traceback 用的 PC → (函数, 行号) 表的一项。
    struct DbgEntry {
        std::string label;  // .LdbgN / .LdbgEndN
        int fi = -1;        // 函数下标；-1 表示函数结束哨兵
        int line = 0;
    };
    std::vector<DbgEntry> dbg_entries_;
    int dbg_seq_ = 0;
    bool dbg_on_ = false;
    std::string dbg_name_label(int fi) const { return std::format(".Lfnname{}", fi); }
    void emit_debug_sections(const IRModule& m);

    // ── 零开销异常（Z2-b2，--zero-cost-exc）────────────────────────
    //
    // 每个函数的 EH 计划在 emit_function 开头按 IR 里的 EhBegin/EhEnd/
    // EhLandingPad 标记**预扫描**出来（标签名也在那时按发射顺序分配好，
    // 发射标记指令时按同一顺序取用）。.cfi_personality / .cfi_lsda 紧跟
    // .cfi_startproc 发射；.gcc_except_table 与 DW.ref 指针槽等所有函数都
    // 发完后在 gen() 尾部统一发。计划为空 = 老 setjmp 路径，一个字节不发。
    struct EhPlanRec {
        std::string func_label;  // FDE 起点（= .cfi_startproc 地址，不是 .LfN）
        std::string begin;       // 保护区起点标签
        std::string end;         // 保护区终点标签（开区间）
        std::string lp;          // landing pad 标签
        int lsda_index = 0;
    };
    std::vector<EhPlanRec> eh_plans_;
    // 当前函数预扫描出来的标签（发射标记指令时按顺序取用）。
    std::vector<std::string> eh_begin_labels_;
    std::vector<std::string> eh_end_labels_;
    std::vector<std::string> eh_lp_labels_;
    size_t eh_begin_seq_ = 0;
    size_t eh_end_seq_ = 0;
    size_t eh_lp_seq_ = 0;
    bool eh_plan_active_ = false;
    // 总开关（--zero-cost-exc）。关闭时 prepare_eh_plan 直接返回，保证老
    // 路径的汇编逐字节不变。
    bool zero_cost_exc_ = false;
    // 预扫描本函数的 EH 标记（fname 是 FDE 起点标签）；没有标记则不动作。
    void prepare_eh_plan(const IRFunction& f, const std::string& fname);
    // 发 .cfi_personality / .cfi_lsda（必须在 .cfi_startproc 之后调用）。
    void emit_eh_directives_for_current();
    // 发所有 LSDA + DW.ref（在 gen() 尾部、text 都发完之后）。
    void emit_eh_tables();
    void gen_eh_begin();
    void gen_eh_end();
    void gen_eh_landing_pad();

    // vreg 槽区里第 slot 个槽的偏移（vreg 槽紧挨着命名槽往上排）。
    int vreg_slot_off(int slot) const { return out_bytes_ + 8 * (n_slots_ + slot); }
    // 帧记录里的槽：持有堆引用的非借用槽，异常展开时按地址 dec。
    std::vector<int> frame_owned_;
    // 与 frame_owned_ 一一对应：槽里可能是非堆值，展开时走注册表释放。
    // 记录里用地址最低位置 1 标记（槽地址 8 字节对齐）。
    std::vector<bool> frame_owned_unknown_;
    // 借来的 unknown 槽：只登记给 GC 扫描，异常展开不释放。
    std::vector<int> frame_scan_;
    // 需要保守扫描的临时区字数（0 表示这个帧的临时区一个字节都不扫）。
    int frame_scan_words_ = 0;
    // 临时区位图：bit i = 1 表示第 i 个字可能需 GC 保守扫描。空表示没有
    // 位图（运行时按整段扫）。位图必须是超集，宁多勿少。
    std::vector<uint64_t> frame_scan_mask_;
    // 按 vreg 静态类型算出临时区的保守扫描范围 / 位图（见 codegen_block.cpp）。
    void build_frame_scan_info(const IRFunction& f);

    const std::string& mem(VReg r) const {
        if (r < static_cast<VReg>(vreg_reg_.size()) && !vreg_reg_[r].empty())
            return vreg_reg_[r];
        if (r < static_cast<VReg>(vreg_mem_.size())) return vreg_mem_[r];
        static const std::string kFallback = "0(%rsp)";
        return kFallback;
    }

    const std::string& smem(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(smem_cache_.size()))
            return smem_cache_[static_cast<size_t>(slot)];
        static const std::string kFallback = "0(%rsp)";
        return kFallback;
    }

    const std::string& gen_smem(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(gen_smem_cache_.size()))
            return gen_smem_cache_[static_cast<size_t>(slot)];
        static const std::string kFallback = "0(%r15)";
        return kFallback;
    }

    // 变量的现在位置：寄存器（提升过）或栈槽。
    const std::string& slot_op(int slot) const {
        if (gen_) return gen_smem(slot);
        if (slot >= 0 && slot < static_cast<int>(slot_reg_.size()) && !slot_reg_[slot].empty())
            return slot_reg_[slot];
        return smem(slot);
    }

    // 读一个 vreg 的操作数。块内别名优先（别名指向的就是它的当前值），
    // 其次是可以直接当立即数用的常量，最后退回它自己的栈槽。
    const std::string& src_op(VReg r) const {
        static const std::string kZero = "$0";
        if (r == kNoReg) return kZero;
        auto it = alias_.find(r);
        if (it != alias_.end()) return it->second;
        auto ct = const_val_.find(r);
        if (ct != const_val_.end()) return ct->second;
        auto gi = global_imm_.find(r);
        if (gi != global_imm_.end()) return gi->second;
        return mem(r);
    }

    // 把 src 搬进 dst 操作数。两个都是内存时 x86 没有 mem-to-mem，
    // 中间过一下 rax。
    static bool is_mem_operand(const std::string& s) {
        return !s.empty() && s[0] != '%' && s[0] != '$';
    }

    void move_to(const std::string& dst, const std::string& src) {
        if (dst == src) return;
        // mem-to-mem：x86 没有这种编码，中间过一下 %rax。
        if (is_mem_operand(dst) && is_mem_operand(src)) {
            emit2("movq", src, "%rax");
            emit2("movq", "%rax", dst);
            return;
        }
        emit2("movq", src, dst);
    }

    // 浮点搬运。寄存器之间用 movapd 做整 128 位拷贝：movsd 只写低 64 位，
    // 硬件得把它和目的寄存器的旧值合并，于是凭空多出一条对旧值的依赖
    // （紧挨着复用一个临时寄存器时可能明显变慢）。访存仍用 movsd。
    void move_fp(const std::string& dst, const std::string& src) {
        if (dst == src) return;
        bool dreg = dst.size() > 4 && dst.compare(0, 4, "%xmm") == 0;
        bool sreg = src.size() > 4 && src.compare(0, 4, "%xmm") == 0;
        // SSE 不支持内存到内存的 movsd：两侧都在栈槽时必须经寄存器中转。
        // 走到这里的调用点都是「把已知值搬到某个槽/寄存器」，%xmm0 是
        // 各指令生成器的固定 scratch，不持有活值。（不中转的话汇编器直接
        // 报 operand size mismatch，整文件汇编失败。）
        if (!dreg && !sreg) {
            emit2("movsd", src, "%xmm0");
            emit2("movsd", "%xmm0", dst);
            return;
        }
        emit2(dreg && sreg ? "movapd" : "movsd", src, dst);
    }

    // 本指令 dst 的落点。
    const std::string& dst_op(VReg r) const {
        if (!dst_loc_.empty()) return dst_loc_;
        return mem(r);
    }

    TypePtr ty_of(VReg r) const {
        if (r >= f_->vreg_ty.size()) return t_unknown();
        return f_->vreg_ty[r];
    }

    bool is_fp(VReg r) const { return ir_is_float(ty_of(r)); }

    // 帧布局 = [出参区 out_bytes_][局部区 locals_][push 的 callee-saved][返回地址]。
    // rsp 在函数体里全程不动，槽位因此可以相对 rsp 寻址，帧里不需要 rbp。
    // SysV 要求 call 时 rsp 是 16 的倍数，布局阶段把 padding 补到最小。
    int out_bytes_ = 0;    // 出参区：整函数最大的那个调用点用量
    int locals_ = 0;       // 局部区（含对齐 padding）
    int frame_pad_ = 0;    // subq 的字节数 = out_bytes_ + locals_
    int frame_bytes_ = 0;  // 返回地址到 rsp 的距离 = frame_pad_ + 8 * n_saved_regs_
    int cfa_off_ = 0;      // 当前 .cfi_def_cfa_offset（CFA = rsp + cfa_off_）
    // 扫描全部调用点，算出整函数要预留多少出参区。
    int outgoing_bytes(const IRFunction& f) const;
    // 计算 out_bytes_ 之外的几个布局量；必须在 build_operand_caches 之前
    // 调用——槽位操作数是在那时格式化好的。
    void layout_frame(int total_slots);
    // 发 subq 并把 CFA 偏移推到函数体状态。
    void frame_alloc();

    // 尾声：先退局部区，再逐个弹回 callee-saved，然后 ret。
    void emit_epilogue();

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
    void assign_vreg_slots();

    // 变量提升：按被读写次数排序，把最热的几个局部变量放进 callee-saved
    // 寄存器。没提升的变量继续住栈槽，语义完全一样，所以这一步错了
    // 最多是慢，不会算错。
    //
    // 不做活跃性分析——一个变量要么整函数在寄存器里，要么整函数在栈上。
    // 分支汇合、循环回边都因此天然正确，不需要任何 spill 逻辑。代价是
    // 寄存器被长期占着，但 Python 的局部变量本来就不多，这个代价基本
    // 不存在。
    //
    // 浮点变量不参与：xmm 全是 caller-saved，跨调用要自己 save/restore，
    // 收益抵不上复杂度。
    void assign_slot_regs();

    // 跨块 vreg 提升：局部变量没占满的 callee-saved 寄存器分给跨块
    // 整型临时值。用 CFG 活跃性算区间（覆盖循环回边），定义支配所有
    // 使用的值才参与；调用不会破坏 callee-saved，所以跨调用也安全。
    void assign_global_vreg_regs();

    // ── 函数 ────────────────────────────────────────────────────
    std::vector<ArgLoc> classify(const std::vector<TypePtr>& types) const;

    void emit_function(const IRFunction& f, int fi);
    // 统一调用约定的 shim 入口（见 ir_types.h 的 closure_needs_shim）。
    void emit_closure_shim(const IRFunction& f, int fi);

    // O2：只给可能「读在写之前」的槽做序言清零。
    std::vector<bool> slots_needing_zero() const;

    // ── 基本块 ──────────────────────────────────────────────────
    //
    // 别名只在块内有效，块边界一律清空——跨块的值永远走栈槽，所以
    // 不需要在块之间传播任何状态，也就不可能读到过期的寄存器。
    bool can_fuse(const Inst& in, const Inst& nx) const;

    // 浮点二元结果只被紧邻的下一条浮点二元指令使用时，先留在 %xmm1，
    // 省掉「结果 → 临时槽 → 再读回」的一条存储和一条装载。
    bool can_fuse_fp_temp(const Inst& in, const Inst& nx) const;

        bool can_fuse_branch(const Inst& in, const Inst& nx) const;

    // 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
    void gen_cmp_branch(const Inst& in, const Inst& br);

    // 浮点比较 + 分支：ucomisd 直接吃标志位，NaN 走 jp 排除。
    void gen_cmp_branch_fp(const Inst& in, const Inst& br);

        void gen_block(const Block& b);

        bool single_use_in_block(VReg v) const;

        bool is_deferable_const(const Inst& in) const;

        void invalidate_slot(int slot);

    // ── 指令 ────────────────────────────────────────────────────
    void gen_inst(const Inst& in);

    // x = y 这种加载能不能省掉，看两件事：
    //   1. 变量 x 在寄存器里（否则读出来就得写回一个栈槽，省不了）
    //   2. 这个临时值只被读一次，且读的时候中间没有对 x 的赋值
    // 都满足时，临时值直接指向 x 的寄存器，整条指令消失。
    void gen_load_slot(const Inst& in);

        void gen_const_int(const Inst& in);

        void gen_const_float(const Inst& in);

        void gen_bin(const Inst& in);

        void gen_bin_int(const Inst& in);

    // idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。
    void gen_div_guard();

    // x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
    void gen_floordiv(const Inst& in);

    // Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
    void gen_mod(const Inst& in);

        void gen_bin_fp(const Inst& in);

        void gen_un(const Inst& in);

    // 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
    void emit_truth(VReg r, const char* byte_reg);

        void gen_cmp(const Inst& in);

        void gen_cmp_fp(const Inst& in);

        void gen_convert(const Inst& in);

        void gen_make_list(const Inst& in);

        void gen_index_load(const Inst& in);

    void gen_index_unchecked_load(const Inst& in);

    void gen_index_store(const Inst& in);

    // ── 对象 ────────────────────────────────────────────────────

    // 对象布局：+0 是 vtable 指针，字段从 +8 开始，每个 8 字节。
    // 属性访问因此是一条固定偏移的 movq，没有查表、没有哈希。
    void gen_obj_new(const Inst& in);
    void gen_struct_new(const Inst& in);
    void gen_struct_copy(const Inst& in);

        void gen_field_load(const Inst& in);

        void gen_field_store(const Inst& in);

    // 虚调用：self 固定占第一个整数参数（%rdi），其余实参照常分类。
    // 函数地址在装参数之前取到 %r11 —— %r11 不在参数序列里，
    // 装参数的过程不会碰它。
    void gen_vcall(const Inst& in);

    void gen_vcall_static(const Inst& in);

    void gen_super_vcall(const Inst& in);

    void gen_call_direct(const Inst& in);
    void gen_call_ptr(const Inst& in);

        void gen_branch(const Inst& in);

        void gen_return(const Inst& in);

    // ── 调用 ────────────────────────────────────────────────────
    void gen_call(const Inst& in);

    void gen_call_extern(const Inst& in);
    void gen_load_extern(const Inst& in);
    void gen_store_extern(const Inst& in);
    void gen_ptr_load(const Inst& in);
    void gen_ptr_store(const Inst& in);

        void gen_setjmp(const Inst& in);

        void gen_call_rt(const Inst& in);

        // 运行期调用的实参装填：按实参形态分组，实现在 codegen_call_rt.cpp。
        void gen_rt_args(const Inst& in, RtFn fn);
        void gen_rt_none(const Inst& in);
        void gen_rt_args_n(const Inst& in, int n);
        void gen_rt_i1(const Inst& in);
        void gen_rt_i2(const Inst& in);
        void gen_rt_i3(const Inst& in);
        void gen_rt_i4(const Inst& in);
        void gen_rt_i5(const Inst& in);
        void gen_rt_i6(const Inst& in);
        void gen_rt_f1(const Inst& in);
        void gen_rt_f2(const Inst& in);
        void gen_rt_f3(const Inst& in);
        void gen_rt_i1f1(const Inst& in);
        void gen_rt_i1f2(const Inst& in);
        void gen_rt_i1f3(const Inst& in);
        void gen_rt_f1i1(const Inst& in);
        void gen_rt_i4opt5(const Inst& in);
        void gen_rt_reduce_f(const Inst& in);
        void gen_rt_subprocess_run(const Inst& in);
};

}  // namespace pyadamas
