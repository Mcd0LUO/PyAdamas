// codegen_impl.h —— AsmGen 的实现细节（只给 codegen_*.cpp 用）
//
// 代码生成按职责拆成：驱动/数据段（codegen.cpp）、寄存器分配
// （codegen_regalloc.cpp）、函数与基本块（codegen_block.cpp）、
// 指令选择（codegen_ops.cpp）、调用与对象（codegen_call.cpp）。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir.h"
#include "module.h"

namespace py23 {

inline const char* const kIntArgRegs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
constexpr int kMaxIntArgs = 6;
constexpr int kMaxFpArgs = 8;

// 可用来长期持有变量的寄存器。全部是 callee-saved：被调用方保证
// 返回时原样恢复，所以跨函数调用不需要把值挪回栈。
// rbp 是帧指针，rsp 是栈指针，都不在其中。
inline const char* const kSlotRegs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
constexpr int kMaxSlotRegs = 5;

// 浮点变量常驻寄存器。只用 xmm8–15：xmm0–7 是参数/临时寄存器，
// 所有指令生成器都只把它们当 scratch。提升仅发生在函数没有任何
// 「会正常返回的调用」时，所以 caller-saved 不影响正确性。
inline const char* const kFpSlotRegs[] = {"%xmm8",  "%xmm9",  "%xmm10", "%xmm11",
                                          "%xmm12", "%xmm13", "%xmm14", "%xmm15"};
constexpr int kMaxFpSlotRegs = 8;

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

private:
    std::string global_label(int i) const {
        const auto& g = m_->globals[static_cast<size_t>(i)];
        return g.symbol.empty() ? std::format(".Lg{}", i) : g.symbol;
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
    int cur_block_ = -1;
    int n_slots_ = 0;
    int n_vreg_slots_ = 0;
    // 虚拟寄存器 → 栈槽下标。一个 vreg 的槽在它死后可以给别人复用，
    // 否则 2000 行的函数会生成 100KB 的栈帧（递归几层就爆）。
    std::vector<int> vreg_slot_;
    int pos_ = 0;
    int label_id_ = 0;
    std::string out_;

    // ── 位置状态（每次 emit_function 重建）──────────────────────
    //
    // slot_reg_[s] 非空表示变量 s 常驻在这个寄存器里，栈槽作废。
    // 分配是函数级的、固定的：不做活跃性分析，一个变量要么整函数
    // 在寄存器里，要么整函数在栈上，规则简单到不会出隐蔽 bug。
    std::vector<std::string> slot_reg_;
    int n_saved_regs_ = 0;  // 实际 push 了几个 callee-saved
    // 提升到 xmm8–15 的浮点槽。call 会破坏 caller-saved xmm，调用前后
    // 统一把它们写回/读出各自的栈槽。
    std::vector<int> fp_promoted_slots_;
    void spill_fp_slots();
    void reload_fp_slots();
    // 生成器/协程 resume：槽位在堆帧里，%r15 固定指向帧负载；
    // 帧头 10 个字：state/status/sent/out/resume/closing/kind/waiter/exc/cur。
    bool gen_ = false;
    static constexpr int kGenHeader = 10;

    // vreg → 它当前所在的物理位置（"%r13" 这种）。只在块内有效：
    // 块边界一律清空，所以跨块的值永远走栈槽，不会读到过期的寄存器。
    std::unordered_map<VReg, std::string> alias_;

    // 能被写成立即数的常量。同样只在块内有效。
    std::unordered_map<VReg, int64_t> const_val_;

    // 本指令的 dst 该写到哪里。为空表示按 mem(dst)；非空是融合目标
    // （通常是一个变量寄存器），此时 mem(dst) 完全不参与。
    std::string dst_loc_;

    std::vector<int> use_count_;
    std::vector<bool> cross_block_;
    std::vector<int> def_block_;

    // 当前块内的位置索引，gen_block 开头重建。
    std::unordered_map<int, std::vector<int>> block_stores_;
    std::unordered_map<VReg, int> block_first_use_;
    int cur_pos_ = 0;  // 正在生成的指令在块内的下标

    // ── 输出辅助 ────────────────────────────────────────────────

    void line(std::string_view s) {
        out_ += '\t';
        out_ += s;
        out_ += '\n';
    }

    void label(const std::string& s) {
        out_ += s;
        out_ += ":\n";
    }

    void comment(const std::string& s) { out_ += std::format("\t# {}\n", s); }

    std::string fresh_label(const char* tag) {
        return std::format(".L{}_{}", tag, label_id_++);
    }

    static std::string func_label(int i) { return std::format(".Lf{}", i); }

    std::string block_label(int bi) const { return std::format(".Lf{}_b{}", fi_, bi); }

    // 局部区必须从 push 下来的 callee-saved 寄存器**下面**开始：
    // rbp-8 起是 push 的内容，直接拿来当变量槽会把保存的寄存器覆盖掉，
    // 函数返回时 pop 回一个垃圾值，调用方就炸了。
    int slot_off(int slot) const { return -8 * (n_saved_regs_ + slot + 1); }

    int vreg_off(VReg r) const {
        int s = (r < vreg_slot_.size()) ? vreg_slot_[r] : 0;
        return -8 * (n_saved_regs_ + n_slots_ + s + 1);
    }

    std::string mem(VReg r) const { return std::format("{}(%rbp)", vreg_off(r)); }

    std::string smem(int slot) const { return std::format("{}(%rbp)", slot_off(slot)); }

    std::string gen_smem(int slot) const {
        return std::format("{}(%r15)", 8 * (kGenHeader + slot));
    }

    // 变量的现在位置：寄存器（提升过）或栈槽。
    std::string slot_op(int slot) const {
        if (gen_) return gen_smem(slot);
        if (slot >= 0 && slot < static_cast<int>(slot_reg_.size()) && !slot_reg_[slot].empty())
            return slot_reg_[slot];
        return smem(slot);
    }

    // 读一个 vreg 的操作数。块内别名优先（别名指向的就是它的当前值），
    // 其次是可以直接当立即数用的常量，最后退回它自己的栈槽。
    std::string src_op(VReg r) const {
        if (r == kNoReg) return "$0";
        auto it = alias_.find(r);
        if (it != alias_.end()) return it->second;
        auto ct = const_val_.find(r);
        if (ct != const_val_.end()) return std::format("${}", ct->second);
        return mem(r);
    }

    // 把 src 搬进 dst 操作数。两个都是内存时 x86 没有 mem-to-mem，
    // 中间过一下 rax。
    static bool is_mem_operand(const std::string& s) {
        return !s.empty() && s[0] != '%' && s[0] != '$';
    }

    void move_to(const std::string& dst, const std::string& src) {
        if (dst == src) return;
        if (is_mem_operand(dst) && is_mem_operand(src)) {
            line(std::format("movq {}, %rax", src));
            line(std::format("movq %rax, {}", dst));
            return;
        }
        line(std::format("movq {}, {}", src, dst));
    }

    // 本指令 dst 的落点。
    std::string dst_op(VReg r) const {
        if (!dst_loc_.empty()) return dst_loc_;
        return mem(r);
    }

    TypePtr ty_of(VReg r) const {
        if (r >= f_->vreg_ty.size()) return t_unknown();
        return f_->vreg_ty[r];
    }

    bool is_fp(VReg r) const { return ir_is_float(ty_of(r)); }

    // 栈帧 = push 下来的 callee-saved + 局部区。SysV 要求 call 时
    // rsp 是 16 的倍数，而 push 的个数会影响这个余数，所以把两者
    // 合起来算，只补最少的字节。
    void frame_size(int total_slots);

    // 尾声：先把 rsp 退回 push 的寄存器下面，逐个弹回，再弹 rbp。
    // 不能用 `movq %rbp, %rsp`——那会把 callee-saved 的槽一起丢掉。
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

    // ── 函数 ────────────────────────────────────────────────────
    std::vector<ArgLoc> classify(const std::vector<TypePtr>& types) const;

    void emit_function(const IRFunction& f, int fi);

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

        void gen_field_load(const Inst& in);

        void gen_field_store(const Inst& in);

    // 虚调用：self 固定占第一个整数参数（%rdi），其余实参照常分类。
    // 函数地址在装参数之前取到 %r11 —— %r11 不在参数序列里，
    // 装参数的过程不会碰它。
    void gen_vcall(const Inst& in);

    void gen_vcall_static(const Inst& in);

    void gen_super_vcall(const Inst& in);

    void gen_call_direct(const Inst& in);

        void gen_branch(const Inst& in);

        void gen_return(const Inst& in);

    // ── 调用 ────────────────────────────────────────────────────
    void gen_call(const Inst& in);

        void gen_setjmp(const Inst& in);

        void gen_call_rt(const Inst& in);
};

}  // namespace py23
