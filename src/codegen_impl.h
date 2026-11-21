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

#include "block_bitset.h"
#include "ir.h"
#include "mach_instr.h"
#include "target_abi.h"
#include "module.h"
#include "tag_defs.h"

namespace pyadamas {

// 实参寄存器一律从 TargetABI 取：SysV 6 整型 / 8 浮点，Win64 各 4 个。
inline int kMaxIntArgs() { return target_abi().n_arg_int; }
inline int kMaxFpArgs() { return target_abi().n_arg_fp; }

// P3 big：十进制串 → 基 2^32 小端 limb（codegen_big.cpp），返回带符号
// limb 数（PyAdamasBig.nd 口径）。静态 BigInt 字面量数据段发射用。
int64_t big_literal_limbs(const std::string& dec, std::vector<uint32_t>& out);

// 可用来长期持有变量的寄存器。全部是 callee-saved：被调用方保证
// 返回时原样恢复，所以跨函数调用不需要把值挪回栈。
// rsp 是栈指针，不在其中；rbp 在——生成函数的帧相对 rsp 寻址，不需要
// 帧指针，回溯走 .eh_frame，所以它也能拿来存变量。
//
// 寄存器角色统一由 TargetABI 描述（见 target_abi.h）：这是换目标的唯一
// 接缝，取值在编译期定死，运行期零开销。
inline const char* const* kSlotRegs() { return target_abi().callee_pool; }
// 实验开关：PYADAMAS_RA_POOLCAP=N 把**槽寄存器池**限制到 N 个（N < 6）。
//
// 用途：量化「每多一个寄存器值多少」—— 如果缩小池子性能急剧恶化，说明寄存器
// 容量是真正瓶颈（那么任何提升有效容量的方案都值得做）；如果曲线很平，说明
// 分配质量/容量都不是瓶颈，不值得再往寄存器分配上投入。
//
// 只影响槽分配（n_saved_regs_ 也随之变小，序言压栈数跟着变），不影响 vreg
// 路径。默认不设变量时行为完全不变。
inline int kMaxSlotRegs() {
    static const int capped = [] {
        const int full = target_abi().n_callee_pool;
        const char* e = std::getenv("PYADAMAS_RA_POOLCAP");
        if (!e) return full;
        const int v = std::atoi(e);
        if (v <= 0) return full;
        return v < full ? v : full;
    }();
    return capped;
}

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
        void set_index_readonly(bool v) { index_readonly_ = v; }
        // 零开销异常总开关（默认开启；--no-zero-cost-exc 可回退）。
        void set_zero_cost_exc(bool v) { zero_cost_exc_ = v; }
        // 分配器实验开关（驱动传入，参与缓存失效）：RA_SPLIT / RA_OPT（默认关）。
        void set_rc_split(bool v) { rc_split_ = v; }
        void set_ra_opt(bool v) { ra_opt_ = v; }
        // 浮点循环 SIMD 向量化开关（PYADAMAS_SIMD，默认关）：参与缓存失效。
        void set_simd(bool v) { simd_enabled_ = v; }
        void set_vect(bool v) { vect_enabled_ = v; }  // PYADAMAS_VECT，默认关，参与缓存失效
        void set_inline_opt(bool v) { inline_opt_ = v; }  // PYADAMAS_INLINE_OPT，默认关
        // 运行期类型标签（PYADAMAS_TYPE_TAGS，默认关，docs/numeric-types-design.md §2）：
        // 在 P3 shadow tag 之上追加 NONE/BOOL 与精确堆子类 tag，解锁
        // isinstance/repr/pprint。关闭时 tag 编号/判定逐字沿用 P3（asm 不变）。
        void set_type_tags(bool v) { type_tags_ = v; }

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
    bool index_readonly_ = false;
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
    std::vector<MOperand> slot_reg_;   // 结构化单元：empty()=未提升，text=寄存器名
    int n_saved_regs_ = 0;  // 实际 push 了几个 callee-saved
    // 提升到 xmm8–15 的浮点槽。call 会破坏 caller-saved xmm，调用前后
    // 只把「调用后仍存活」的槽写回/读出（块级活跃性在 build_fp_liveness
    // 里算好，按指令取 live-after 掩码）。
    std::vector<int> fp_promoted_slots_;
    // 提升到 xmm 的**跨块 vreg**（临时值，不是命名槽）。
    //
    // 与 fp_promoted_slots_ 平行：槽用 smem(slot) 做栈副本，vreg 用
    // vreg_mem_[v]（即 vreg_off(v)）。两者在调用点都要 spill/reload ——
    // SysV 下 xmm8–15 是 caller-saved，call 会破坏它们。
    std::vector<int> fp_promoted_vregs_;
    std::vector<int> slot_fpidx_;      // slot → fp_promoted_slots_ 下标
    // 块级活跃掩码：**每个提升槽一个字节**（不是位图）。
    //
    // 原来这里是 uint8_t 位图，隐含「提升槽 ≤ 8 个」的假设 —— 那是
    // 「整函数提升 + 寄存器不复用」时代的口径。R4 的线性扫描会按活跃
    // 区间复用寄存器（13 个浮点槽跑在 8 个 xmm 上是常态），提升槽数
    // 可以超过 8；位图第 8 位之后静默丢失，调用点不 spill 被调方要踩
    // 的槽（0.322 的 N19b 余半：amortize 的 pay 读成 0）。
    std::vector<std::vector<uint8_t>> fp_live_in_;   // [块][提升槽]
    std::vector<std::vector<uint8_t>> fp_live_out_;

    // ── 全局寄存器分配（R1：整型槽的块级活跃性）──────────────────
    // slot_live_in_[b] / slot_live_out_[b] 是第 b 块入口/出口仍活跃的
    // 整型槽集合（按槽号直接索引的位图，每槽一位）。R2 的线性扫描
    // 分配器靠它算活跃区间。
    //
    // 只跟踪**可提升**的槽：堆槽（帧记录按地址读值）、取过地址的槽
    // （SlotAddr）、handler hazard 槽都不参与 —— 与 assign_slot_regs
    // 的排除条件保持一致，否则会分到一个永远不能用的寄存器。
    std::vector<std::vector<uint64_t>> slot_live_in_;
    std::vector<std::vector<uint64_t>> slot_live_out_;
    std::vector<int> slot_reg_cand_;   // 参与分配的槽号（升序）
    std::vector<char> slot_cand_is_fp_; // 与 slot_reg_cand_ 对齐：是否浮点槽
    std::vector<int> slot_reg_cand_idx_;  // 槽号 -> slot_reg_cand_ 下标，-1 = 不参与
    void build_slot_liveness();
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
    std::unordered_map<VReg, MOperand> alias_;

    // 能被写成立即数的常量。同样只在块内有效（保存 "$imm" 串）。
    std::unordered_map<VReg, MOperand> const_val_;

    // 整函数范围的 const.int → "$imm"。跨块的常量不必占寄存器或栈槽，
    // 所有使用点直接用立即数；定义指令在 gen_block 里跳过。
    std::unordered_map<VReg, MOperand> global_imm_;

    // 本指令的 dst 该写到哪里。为空表示按 mem(dst)；非空是融合目标
    // （通常是一个变量寄存器），此时 mem(dst) 完全不参与。
    MOperand dst_loc_;

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
    std::vector<MOperand> vreg_reg_;
    // 块内临时寄存器可用池：无调用函数里被跨块提升占走的 %r8–%r11 会剔除。
    std::vector<std::string> block_temp_pool_;
    bool temp_reg_op_ok(const Inst& in) const;
    // xmm3-7 浮点临时值的安全性（定义在 codegen_ops.cpp）：与 GPR 池
    // 独立 —— 整数 div/mod/floordiv 只用 GPR scratch，不碰 xmm3-7。
    bool fp_temp_op_ok(const Inst& in) const;
    void assign_block_temp_regs();

    // 生成期缓存：块标签、vreg/槽的内存操作数串。这些字符串在函数内
    // 固定不变，生成每条指令时反复 format 会占掉相当一部分编译时间。
    std::vector<std::string> block_labels_;
    // 操作数缓存是结构化 MOperand 单元：build_operand_caches 赋值时解析
    // 一次，发射点直取免回解析；.text 仍是 canonical 操作数字符串。
    std::vector<MOperand> vreg_mem_;
    std::vector<MOperand> smem_cache_;
    std::vector<MOperand> gen_smem_cache_;
    std::vector<std::string> global_labels_;
    // rip 相对全局操作数按模块缓存：长 mangled 符号（pyadamas$...）每次
    // 访问都建 canonical text 会堆分配，缓存后全局访问零分配。
    std::vector<MOperand> global_mops_;
    std::vector<MOperand> global_tag_mops_;
    std::vector<std::string> func_labels_;
    void build_operand_caches();

    // ── P3 shadow tag（0.327，docs/p3-tagged-union-design.md）─────────
    //
    // unknown 值仍是 8B i64（float 仍装箱），平行 tag 字跟值走：tag 是
    // RC/分派快路径的判据，UNK(0) 一律回退探针 —— 正确性不依赖传播完备。
    // 门控 tags_enabled_ = 模块含 AnyArithT/AnyCmpT/AnyNegT（封闭活跃域）；
    // 门控关的模块（含 26 个热基准）不进任何 tag 分支，asm 逐字节不变。
    // tag 编码与静态类型映射见 tag_defs.h（含 P2 的 NONE/BOOL/堆子类扩展，
    // 以及堆 ⇔ 3..5 或 >=8 的判定口径）。
    bool tags_enabled_ = false;
    bool type_tags_ = false;              // PYADAMAS_TYPE_TAGS（默认关）
    int n_tag_words_ = 0;                 // 帧内平行 tag 区字数
    std::vector<int> vreg_tag_;           // vreg -> tag 字号（-1 无）
    std::vector<int> slot_tag_;           // 命名槽 -> tag 字号（-1 无）
    std::vector<MOperand> vreg_tag_mem_;
    std::vector<MOperand> slot_tag_mem_;
    // tag 区在 vreg 栈槽区之上、帧记录之下（rec_off 按 locals_ 自动让位）。
    int tag_off(int w) const {
        return out_bytes_ + 8 * (n_slots_ + n_vreg_slots_ + w);
    }
    static bool is_unk_ty(const TypePtr& t) {
        return !t || t->kind == TyKind::Unknown;
    }
    bool unk_vreg(VReg r) const {
        return r != kNoReg && r < f_->vreg_ty.size() && is_unk_ty(f_->vreg_ty[r]);
    }
    bool unk_slot(int s) const {
        return s >= 0 && s < static_cast<int>(f_->slot_ty.size()) &&
               is_unk_ty(f_->slot_ty[static_cast<size_t>(s)]);
    }
    // 全局 tag 健全性：只有「别的模块写不到」的 unknown 全局才配 tag 字。
    // 外部全局（存储属对方）与非入口模块的导出全局（可能被 import 方写入，
    // 对方不维护本模块 tag）一律 UNK 回退。入口模块 __entry__ 不可被 import。
    bool global_tag_eligible(int gi) const {
        if (gi < 0 || gi >= static_cast<int>(m_->globals.size())) return false;
        const IRGlobal& g = m_->globals[static_cast<size_t>(gi)];
        return !g.external && is_unk_ty(g.ty) &&
               (g.symbol.empty() || m_->is_entry);
    }
    std::string global_tag_label(int gi) const {
        return global_label(gi) + "$t";
    }
    // 静态类型导出的 tag（定义在 tag_defs.h；type_tags_ 决定 P2 扩展）。
    int64_t static_tag_of(const TypePtr& t) const {
        return pyadamas::static_tag_of(t, type_tags_);
    }
    bool vreg_tagged(VReg r) const {
        return tags_enabled_ && r != kNoReg &&
               r < vreg_tag_.size() && vreg_tag_[r] >= 0;
    }
    bool slot_tagged(int s) const {
        return tags_enabled_ && s >= 0 &&
               s < static_cast<int>(slot_tag_.size()) && slot_tag_[s] >= 0;
    }
    // tag 操作数：结构化单元优先，字符串接口委托 .text。
    const MOperand& tag_mop(VReg r) const {
        if (r < vreg_tag_mem_.size() && !vreg_tag_mem_[r].empty())
            return vreg_tag_mem_[r];
        static const MOperand kZero = op_imm(0);
        return kZero;
    }
    const MOperand& slot_tag_mop(int s) const {
        if (s >= 0 && s < static_cast<int>(slot_tag_mem_.size()) &&
            !slot_tag_mem_[static_cast<size_t>(s)].empty())
            return slot_tag_mem_[static_cast<size_t>(s)];
        static const MOperand kZero = op_imm(0);
        return kZero;
    }
    const std::string& tag_mem(VReg r) const { return tag_mop(r).text; }
    const std::string& slot_tag_op(int s) const { return slot_tag_mop(s).text; }
    // vreg 的 tag 源操作数：有 tag 字读字，没有按静态类型导常量。
    std::string tag_src(VReg r) const {
        if (vreg_tagged(r)) return tag_mem(r);
        return "$" + std::to_string(static_tag_of(ty_of(r)));
    }
    // 写 dst 的 tag（常量）。
    void emit_tag_imm(VReg r, int64_t t) {
        if (vreg_tagged(r)) emit_imm_op("movq", t, tag_mem(r));
    }
    // 调用返回的 unknown dst 落 tag：除已知生产者外一律 UNK（探针回退）。
    void emit_call_dst_tag(const Inst& in);
    // AnyArithT/AnyNegT 的复用匹配（设计 §4）：结果被存回 S 且某实参恰是
    // 同块对 S 的装载、装载到存储之间无对 S 的其它写入 → 1，否则 0。
    int64_t any_reuse_ok(const Inst& in);
    void gen_any_tagged(const Inst& in, RtFn fn);
    void gen_rc_unknown_tagged(const Inst& in, RtFn fn);
    void gen_append_tagged(const Inst& in);
    void gen_index_store_tagged(const Inst& in, bool unchecked);
    void gen_index_load_tag(const Inst& in, bool have_regs);
    void emit_heap_from_tag(VReg v, const std::string& dst_reg);

    // ── P3-1b：直调 tag ABI（0.328，设计 §7 P3-1b）─────────────────
    // 值 ABI 不变（payload 仍按 classify 落位）：unknown 形参的 tag 经
    // 平行 tag 数组传递，调用方把数组指针放 %r11、call 被调的 $pt 入口；
    // 旧入口（闭包/间接/跨模块/vtable）先把 %r11 清零再进同一函数体，
    // 形参 tag 全 UNK 回退探针。unknown 返回经 %rdx 带 tag（旧调用方
    // 忽略 rdx，无兼容问题）。门控关/生成器一律不进。
    static std::string ptag_label(int i) {
        std::string s = ".Lf";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, i);
        s.append(buf, r.ptr);
        s += "$pt";
        return s;
    }
    // 形参是否在 tag 数组里占一个字（type_tags_ 时按声明类型，否则按槽类型）。
    bool ptag_param(const IRFunction& f, const IRParam& p) const;
    // 本函数 unknown 形参个数（>0 才发双入口；生成器/门控关恒 0）。
    int func_ptag_words() const;
    // CallDirect 的 tag 数组字数（0 = 不走 tag ABI：门控关/跨模块/生成器/
    // 实参数与形参数不一致）。
    int call_ptag_words(const Inst& in) const;
    // 函数是否经 %rdx 返回 tag（unknown 返回 + 门控 + 非生成器）。
    bool func_ret_tag(const IRFunction& f) const;
    // 被调双入口 gadget：旧入口清零 %r11 跳函数体，$pt 入口直通。
    // gadget 不动 rsp（CFI CFA=rsp+8 不变）；LSDA 按标签差解析自动让位。
    void emit_ptag_entry(int fi);
    // 被调序言：从 %r11 tag 数组装载 unknown 形参 tag（0 → 保持 UNK）。
    // %r11 从入口到此只经 push/subq/movq $0（都不碰它）。
    void emit_ptag_param_load();
    // Return 的 %rdx tag（unknown 返回 + 门控 + 非生成器；否则空操作）。
    void emit_ret_tag(const Inst& in);
    // P3-1b：无 tag 来源的 unknown 装载（dict 值/字段）一次探针导 tag。
    void emit_tag_probe(const std::string& payload_op, const std::string& tag_op);
    // 调用方：在出参区栈实参之后写 tag 数组，返回数组的 rsp 偏移。
    int emit_ptag_array(const Inst& in, const std::vector<ArgLoc>& locs);

    // 当前块内的位置索引，gen_block 开头重建。
    std::unordered_map<int, std::vector<int>> block_stores_;
    // 块内首次读位置 + RcMark 打标（见 block_bitset.h 里的 BlockUseIndex）
    BlockUseIndex block_use_;
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

    // 静态字面量行（line("movq %rax, %rbx") 这类）：进程内只解析一次，
    // 之后直接复制结构化结果——RC 内联序列等固定指令模板每次发射都重新
    // parse_instr/parse_operand 是纯浪费。按内容做透明哈希键（字面量地址
    // 可能被链接器合并/变化，内容才是稳定标识）。编译器单线程，静态缓存
    // 无锁；键只来自字面量与 ABI 静态串，规模有界。
    void line(const char* s) {
        struct SvHash {
            using is_transparent = void;
            size_t operator()(std::string_view v) const {
                return std::hash<std::string_view>{}(v);
            }
        };
        struct Entry { MItem item; int32_t inst = -1; };
        struct Cache {
            std::unordered_map<std::string, Entry, SvHash, std::equal_to<>> map;
            std::vector<MInstr> insts;  // 缓存指令池（Entry.inst 下标）
        };
        static Cache cache;
        const std::string_view sv(s);
        auto it = cache.map.find(sv);
        if (it == cache.map.end()) {
            Entry e;
            if (sv.find('\n') != std::string_view::npos) {
                e.item = make_raw("\t" + std::string(sv) + "\n");
            } else {
                line_buf_.clear();
                line_buf_ += '\t';
                line_buf_.append(sv);
                MachFunc tmp;
                parse_line_into(line_buf_, tmp);
                e.item = std::move(tmp.items[0]);
                if (e.item.kind == MItem::Instr) {
                    e.inst = static_cast<int32_t>(cache.insts.size());
                    cache.insts.push_back(std::move(tmp.insts[0]));
                    e.item.inst = -1;  // 下标按目标函数重定位，缓存里不存
                }
            }
            it = cache.map.emplace(std::string(sv), std::move(e)).first;
        }
        const Entry& e = it->second;
        if (e.inst >= 0)
            out_.push_cached(e.item, cache.insts[static_cast<size_t>(e.inst)]);
        else
            out_.push(e.item);
    }

    // 列 0 的标签（函数标签、数据段符号）。
    void label(const std::string& s) { out_.push_label(s, false); }
    // 带前导 tab 的块内局部标签（"\t1:" 这种）。
    void label_block(const std::string& s) { out_.push_label(s, true); }
    // "\t# s" / 指令注释 / 列 0 注释行（"# ..."）。实现见 codegen_comment.cpp。
    void comment(std::string_view s); void comment_inst(const Inst& in); void comment0(std::string_view s);
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
    // 操作数工厂 + 迷你发射器（emit2/emit_imm_op/emit_jmp）拆在 codegen_emit.h，
    // 类体片段，就地展开（文件规模闸门）。
#include "codegen_emit.h"
    // SIMD 向量化成员/声明拆在 codegen_simd.h（类体片段，文件规模闸门）。
#include "codegen_simd.h"
#include "codegen_vect.h"
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
    void emit_arg(int pos, bool is_fp, int cls_idx, const MOperand& v);

    void cfi(const std::string& s) {
        if (target_abi().gnu_cfi) line(s);
    }
    // CFI 数字行直拼（to_chars）：序言/尾声每函数十几条，免 std::format。
    // 走 directive() 直发，不复用 line()（避免 line_buf_ 自引用）。
    void cfi_cfa_offset() {
        if (!target_abi().gnu_cfi) return;
        line_buf_ = ".cfi_def_cfa_offset ";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, cfa_off_);
        line_buf_.append(buf, r.ptr);
        directive(line_buf_);
    }
    void cfi_restore(const char* reg) {
        if (!target_abi().gnu_cfi) return;
        line_buf_ = ".cfi_restore ";
        line_buf_ += reg;
        directive(line_buf_);
    }
    void cfi_offset(const char* reg, int off) {
        if (!target_abi().gnu_cfi) return;
        line_buf_ = ".cfi_offset ";
        line_buf_ += reg;
        line_buf_ += ", -";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, off);
        line_buf_.append(buf, r.ptr);
        directive(line_buf_);
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


    // 数字后缀标签（.Lvt3 / .Ldtor2 这类）：to_chars 直拼。
    static std::string num_label(const char* prefix, int64_t n) {
        std::string s = prefix;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, n);
        s.append(buf, r.ptr);
        return s;
    }

    // 标签工厂：to_chars 直拼，免 std::format（RC 序列每条 2-3 个标签，
    // 是 vformat 热点的大头）。
    std::string fresh_label(const char* tag) {
        std::string s = ".L";
        s += tag;
        s += '_';
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, label_id_++);
        s.append(buf, r.ptr);
        return s;
    }

    static std::string func_label(int i) {
        std::string s = ".Lf";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, i);
        s.append(buf, r.ptr);
        return s;
    }
    std::string entry_ref(int i) const { return inline_opt_ ? func_label(i) + "e" : func_label(i); }
    // 统一调用约定的 shim 入口标签（见 ir_types.h 的 closure_needs_shim）。
    static std::string shim_label(int i) {
        std::string s = ".Lshim";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, i);
        s.append(buf, r.ptr);
        return s;
    }

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
    MOperand rec_mem(int i) const { return op_mem(kRsp, rec_off(i)); }

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
    std::string dbg_name_label(int fi) const {
        std::string s = ".Lfnname";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, fi);
        s.append(buf, r.ptr);
        return s;
    }
    void emit_debug_sections(const IRModule& m);

    // ── 零开销异常（Z2-b2，--zero-cost-exc）────────────────────────
    //
    // 每个函数的 EH 计划在 emit_function 开头按 IR 里的 EhBegin/EhEnd/
    // EhLandingPad 标记**预扫描**出来（标签名也在那时按发射顺序分配好，
    // 发射标记指令时按同一顺序取用）。.cfi_personality / .cfi_lsda 紧跟
    // .cfi_startproc 发射；.gcc_except_table 与 DW.ref 指针槽等所有函数都
    // 发完后在 gen() 尾部统一发。计划为空 = 老 setjmp 路径，一个字节不发。
    // 一个函数的所有保护区合成一份 LSDA（多条 call-site 记录）。
    // 嵌套 try 时每个 region 各占一条记录，共用同一个 landing pad 分发入口
    // （分发链按 handler 顺序逐个匹配，与老路径一致）。
    struct EhPlanRec {
        std::string func_label;  // FDE 起点（= .cfi_startproc 地址，不是 .LfN）
        struct Range {
            std::string begin;   // 保护区起点标签
            std::string end;     // 保护区终点标签（开区间）
            std::string lp;      // 该区间的 landing pad 标签
        };
        std::vector<Range> ranges;
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
    // 总开关（--zero-cost-exc）：关闭时 prepare_eh_plan 直接返回，老路径 asm 不变。
    bool zero_cost_exc_ = true;
    bool rc_split_ = false;
    bool ra_opt_ = false;    // RA_OPT：P1 跨调用提升 + P2 只读别名（ra_opt.cpp），默认关
    bool inline_opt_ = false;  // INLINE_OPT：内联/devirt 深化 + 入口对齐修复，默认关
    void prepare_eh_plan(const IRFunction& f, const std::string& fname);  // 预扫描 EH 标记
    void emit_eh_directives_for_current();  // 发 .cfi_personality / .cfi_lsda
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
    // 按槽号索引的「这个槽要给 GC 扫描」布尔表。
    // 融合/别名优化据此判断能否绕过栈槽 —— 被扫描的槽必须让 smem(s)
    // 保持权威副本，否则 GC 从帧记录读到旧值/0。
    std::vector<char> gc_scan_slot_;
    // 需要保守扫描的临时区字数（0 表示这个帧的临时区一个字节都不扫）。
    int frame_scan_words_ = 0;
    // 临时区位图：bit i = 1 表示第 i 个字可能需 GC 保守扫描。空表示没有
    // 位图（运行时按整段扫）。位图必须是超集，宁多勿少。
    std::vector<uint64_t> frame_scan_mask_;
    // 按 vreg 静态类型算出临时区的保守扫描范围 / 位图（见 codegen_block.cpp）。
    void build_frame_scan_info(const IRFunction& f);

    // vreg/槽的内存操作数：结构化单元优先（发射点直取免回解析），
    // 字符串接口委托 .text（历史调用点零改动）。
    const MOperand& mem_mop(VReg r) const {
        if (r < static_cast<VReg>(vreg_reg_.size()) && !vreg_reg_[r].empty())
            return vreg_reg_[r];
        if (r < static_cast<VReg>(vreg_mem_.size())) return vreg_mem_[r];
        static const MOperand kFallback = op_mem("%rsp", 0);
        return kFallback;
    }
    const MOperand& smem_mop(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(smem_cache_.size()))
            return smem_cache_[static_cast<size_t>(slot)];
        static const MOperand kFallback = op_mem("%rsp", 0);
        return kFallback;
    }
    const MOperand& gen_smem_mop(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(gen_smem_cache_.size()))
            return gen_smem_cache_[static_cast<size_t>(slot)];
        static const MOperand kFallback = op_mem("%r15", 0);
        return kFallback;
    }
    const std::string& mem(VReg r) const { return mem_mop(r).text; }
    const std::string& smem(int slot) const { return smem_mop(slot).text; }
    const std::string& gen_smem(int slot) const { return gen_smem_mop(slot).text; }

    // 变量的现在位置：寄存器（提升过）或栈槽。结构化版本。
    const MOperand& slot_mop(int slot) const {
        if (gen_) return gen_smem_mop(slot);
        // **split 模式：按当前块查表。**
        //
        // 放在 slot_reg_ 之前：split 模式下 slot_reg_ 不用（整函数固定位置
        // 与按块分裂互斥）。`slot_loc_` 为空时（默认模式）整段跳过，
        // 行为与原来完全一致。
        if (!slot_loc_.empty() && split_block_ >= 0 &&
            split_block_ < static_cast<int>(slot_loc_.size()) &&
            slot >= 0 &&
            slot < static_cast<int>(slot_loc_[static_cast<size_t>(split_block_)].size())) {
            const MOperand& r =
                slot_loc_[static_cast<size_t>(split_block_)][static_cast<size_t>(slot)];
            if (!r.empty()) return r;
            // **本块没有 split 位置时，回落到默认分配的整函数寄存器。**
            //
            // 安全性来自一个不变量：assign_slot_regs_split 只处理「不在
            // slot_reg_ 里」的槽（if (!slot_reg_[s].empty()) continue;），
            // 所以 slot_loc_ 与 slot_reg_ 的槽集合**天然互斥**。
            //
            // 回落只对「默认分配已整函数提升」的槽生效 —— 那些槽不走 split
            // 的 reload/spill 编排，整函数一个位置，本来就自洽。
            //
            // （曾经错误地让 split 模式一律走栈：结果默认提升的浮点槽也被
            // 拉回栈，而 spill_fp_slots 仍把从未写过的寄存器写回栈，覆盖了
            // 真实值 —— matmul 浮点值被冲掉、生成程序死循环 OOM。）
            if (slot < static_cast<int>(slot_reg_.size()) && !slot_reg_[slot].empty())
                return slot_reg_[slot];
            return smem_mop(slot);
        }
        if (slot >= 0 && slot < static_cast<int>(slot_reg_.size()) && !slot_reg_[slot].empty())
            return slot_reg_[slot];
        return smem_mop(slot);
    }
    const std::string& slot_op(int slot) const { return slot_mop(slot).text; }

    // ── live range splitting ───────────────────────────────────
    //
    // `slot_loc_[b][s]` 非空表示「槽 s 在块 b 里住在那个寄存器」。
    // 与 `slot_reg_[s]`（整函数固定位置）互斥：split 模式用这张表，
    // 默认模式用 slot_reg_。两者都为空时走栈（smem）。
    //
    // slot_op 里加了一个分支：slot_loc_ 非空时按块查表，否则走 slot_reg_
    // （默认路径行为完全不变）。split 模式只在 PYADAMAS_RA_SPLIT=1 时启用。
    std::vector<std::vector<MOperand>> slot_loc_;
    // 块 b 入口需要从栈装进寄存器的槽（前驱可能在栈上）。
    std::vector<std::vector<int>> slot_reload_in_;
    // 块 b 出口要写回栈的槽（存在后继要从栈读）。
    // split 模式不再双写 store，这里是栈副本唯一的更新点。
    std::vector<std::vector<int>> slot_spill_out_;
    // 当前生成到哪个块（split 模式用）。
    int split_block_ = -1;

    // 读一个 vreg 的操作数。块内别名优先（别名指向的就是它的当前值），
    // 其次是可以直接当立即数用的常量，最后退回它自己的栈槽。
    const MOperand& src_mop(VReg r) const {
        static const MOperand kZero = op_imm(0);
        if (r == kNoReg) return kZero;
        auto it = alias_.find(r);
        if (it != alias_.end()) return it->second;
        auto ct = const_val_.find(r);
        if (ct != const_val_.end()) return ct->second;
        auto gi = global_imm_.find(r);
        if (gi != global_imm_.end()) return gi->second;
        return mem_mop(r);
    }
    const std::string& src_op(VReg r) const { return src_mop(r).text; }

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

    // 结构化搬运：操作数单元直发。mem-to-mem 判定与字符串版同口径
    // （非 Reg 非 Imm 即「内存类」，含符号/标签兜底）。
    static bool is_mem_mop(const MOperand& o) {
        return o.kind != MOperandKind::Reg && o.kind != MOperandKind::Imm;
    }
    void move_to(const MOperand& dst, const MOperand& src) {
        if (dst.text == src.text) return;
        if (is_mem_mop(dst) && is_mem_mop(src)) {
            emit2(MOp::kMovq, src, kRax);
            emit2(MOp::kMovq, kRax, dst);
            return;
        }
        emit2(MOp::kMovq, src, dst);
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

    // 结构化浮点搬运（与字符串版同口径：%xmm 寄存器判据）。
    void move_fp(const MOperand& dst, const MOperand& src) {
        if (dst.text == src.text) return;
        bool dreg = dst.kind == MOperandKind::Reg && dst.reg >= kXmm0 && dst.reg <= kXmm15;
        bool sreg = src.kind == MOperandKind::Reg && src.reg >= kXmm0 && src.reg <= kXmm15;
        if (!dreg && !sreg) {
            emit2(MOp::kMovsd, src, kXmm0);
            emit2(MOp::kMovsd, kXmm0, dst);
            return;
        }
        emit2(dreg && sreg ? MOp::kMovapd : MOp::kMovsd, src, dst);
    }

    // 本指令 dst 的落点。
    const MOperand& dst_mop(VReg r) const {
        if (!dst_loc_.empty()) return dst_loc_;
        return mem_mop(r);
    }
    const std::string& dst_op(VReg r) const { return dst_mop(r).text; }

    // 字符串字面量 idx 如果是「单字节 ASCII 的 str」，返回该字节值（0..127），
    // 否则 -1。这类字面量在生成代码里直接取运行时驻留表 pa_char_tab 的对象，
    // 于是和 s[i] 的结果是同一个指针（见 gen_inst 的 ConstStr 分支）。
    int char_tab_literal(int idx) const {
        if (!m_ || idx < 0 || idx >= static_cast<int>(m_->strings.size())) return -1;
        const IRStringLit& lit = m_->strings[static_cast<size_t>(idx)];
        if (lit.is_bytes || lit.text.size() != 1) return -1;
        const unsigned char b = static_cast<unsigned char>(lit.text[0]);
        return b < 0x80 ? static_cast<int>(b) : -1;
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

    // 变量提升（旧路径，PYADAMAS_NO_RA=1 回退用）：按被读写次数排序，把
    // 最热的几个整型槽放进 callee-saved；浮点槽按 xmm8–15 处理。
    // 默认路径是下面的活跃区间的线性扫描（assign_slot_regs_linear /
    // linear_scan_pool），这里保留旧实现只为可回退对比。
    void assign_slot_regs();
    // 精确区间诊断（PYADAMAS_LI_STATS）：对比精确区间 vs 粗近似。
    void report_interval_precision();
    void assign_slot_regs_linear();
    // live range splitting：按块给变量槽分配寄存器。
    void assign_slot_regs_split();
    // 单个池的着色：cands 是槽号列表（同属一个池），结果写进 out[槽号]。
    void color_slot_pool(const std::vector<int>& cands, const char* const* regs,
                         int nreg, std::vector<std::string>& out);
    // 通用线性扫描：给定活跃区间与寄存器池，返回 slot -> 寄存器名。
    // 整型与浮点共用（R4：浮点也走线性扫描，不再按频次挑前 N 个）。
    void linear_scan_pool(const std::vector<int>& cands, const char* const* regs,
                          int nreg, std::vector<std::string>& out);
    bool slot_range_has_call(int64_t beg, int64_t end) const;

    // 跨块 vreg 提升：槽没占满的 callee-saved 分给跨块整型临时值，
    // 整函数独占；callee-saved 跨调用安全（CFG 活跃性见函数体注释）。
    void assign_global_vreg_regs();

    std::vector<char> ra_opt_readonly_alias();
    std::vector<std::pair<int, int>> ra_opt_call_block_cands(const std::vector<int64_t>& weight);

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
    bool can_fuse_global(const Inst& in, const Inst& nx) const;
    bool can_fuse_fp_temp(const Inst& in, const Inst& nx) const;

        bool can_fuse_branch(const Inst& in, const Inst& nx) const;

    // 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
    void gen_cmp_branch(const Inst& in, const Inst& br);

    // 浮点比较 + 分支：ucomisd 直接吃标志位，NaN 走 jp 排除。
    void gen_cmp_branch_fp(const Inst& in, const Inst& br);

        void gen_block(const Block& b);
    bool try_forward_global_across_rc(const Block& b, size_t at, size_t& load_pos,
                                      bool& forward_to_receiver);

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

        // P3 big：永生静态 BigInt 字面量（docs/numeric-types-design.md §1/§4）。
        void gen_const_big(const Inst& in);

        void gen_const_float(const Inst& in);

        void gen_bin(const Inst& in);

        void gen_bin_int(const Inst& in);

        // P1 定宽整数（codegen_int_fixed.cpp）：按宽度回绕、u64 用无符号指令。
        void gen_bin_int_fixed(const Inst& in);
        void norm_int_rax(const TypePtr& t);  // 把 %rax 正常化到 t 的规范值
        void gen_udiv64(const Inst& in);      // 商在 %rax、余数在 %rdx

    // idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。
    void gen_div_guard(bool check_overflow = true);
    // RC 记账（inc/dec/mark/cleanup）的内联快路径。返回 true 表示指令已发完。
    // 慢路径落回 pa_rc_*_slow，且会调运行时 —— 必须落盘提升过的浮点槽。
    bool gen_rc_inline(const Inst& in, RtFn fn);
    // 带全局读的版本：冷路永远重新装载，热路从已发布的操作数转发。
    void gen_rc_dec(const Inst& in, const std::string& src = {},
                    const std::string& dst = {}, int global = -1);
    // 常数除数的除法：能化简就生成「魔数乘法 + 符号修正」，结果留在 %rax
    // （mode 1/2 还会在 %rdx 留下余数），返回 true 表示已生成。
    // 除数为 0 / -1 / INT64_MIN 或不是编译期常量时返回 false，走 idivq。
    bool gen_div_const(const Inst& in, int mode);
    void gen_div_const_emit(int64_t d, int mode, bool a_nonneg);

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
    void gen_tuple_inline(const Inst& in);
    void gen_append_inline(const Inst& in);
    void gen_str_index_inline(const Inst& in);

        void gen_index_load(const Inst& in);

    void gen_index_unchecked_load(const Inst& in);

    void gen_index_store(const Inst& in);
    void gen_index_store_unchecked(const Inst& in);

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
    void gen_vcall(const Inst& in, bool keep_int_result = false, int direct_fn = -1);
    bool gen_guarded_vcall(const Inst& in, bool keep_int_result = false);
    bool gen_guarded_vcall_direct(const Inst& in, bool keep_int_result, int base, size_t slot);
    bool try_fuse_guarded_vcall_add(const Block& b, size_t pos);
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
