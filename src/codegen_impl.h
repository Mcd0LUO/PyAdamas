// codegen_impl.h —— AsmGen 的薄壳（只给 codegen_*.cpp 用）
//
// 代码生成按职责拆成：驱动/数据段（codegen.cpp）、寄存器分配
// （codegen_regalloc.cpp）、函数与基本块（codegen_block.cpp）、
// 指令选择（codegen_ops.cpp）、调用与对象（codegen_call.cpp）。
//
// 类体本身按职责切成 4 个**类体片段**，在本文件里于 class AsmGen 内
// #include（片段没有 include guard、不能写 namespace/#include，见各自文件头）：
//
//   codegen_core.h      位置状态 / 输出辅助 / 目标指示符 / 函数·基本块·指令·对象·调用
//   codegen_regalloc.h  全局寄存器分配 / live range splitting
//   codegen_frame.h     栈槽分配 / 帧记录
//   codegen_tag.h       P3 shadow tag / 直调 tag ABI / 零开销异常
//
// 内联函数体（原本 81 处）里 41 处已搬进 codegen_impl.cpp，头里只留声明：
// 改那些函数体只重编 1 个 TU，不再重编 29 个 include 本文件的 TU。
// 留在头里内联的是极热小工具（emit2 族 / op_* 操作数工厂 / line·label 发射
// 入口 / mem·smem·slot·src·dst 查表 / move_to·move_fp / fresh_label 等）——
// 没有 LTO，搬走就是真调用。
//
// 注意：**声明拆成 4 个头并不降低重编译代价**。任何实现 AsmGen 任一方法的
// TU 都需要完整的类定义，所以改这 4 个头里的一行仍然重编 29 个 TU。真正
// 省钱的是上面那条「函数体搬出类体」。按类拆对象是第 2 步的事。
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
#include "emit_sink.h"
#include "emitter.h"
#include "frame.h"
#include "reg_alloc.h"
#include "tag_lower.h"
#include "vect_rules.h"

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

class AsmGen : public EmitSink {
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
        void set_type_tags(bool v) { tag_.type_tags = v; }

private:
    const std::string& global_label(int i) const {
        return global_labels_[static_cast<size_t>(i)];
    }
    std::string roots_label() const;
    size_t local_global_count() const;
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
    // 输出侧状态（缓冲 / 标签号 / 行缓冲）已拆到 Emitter（src/emitter.h）。
    Emitter emit_;
    double alloc_ns_ = 0;
    double emit_ns_ = 0;

    // ── 类体按职责拆成 4 个片段（都是类体片段，只能在这里 #include）──
    #include "codegen_core.h"      // 位置状态 / 输出辅助 / 指示符 / 函数·块·指令·对象·调用
    #include "codegen_regalloc.h"  // 全局寄存器分配 / live range splitting
    #include "codegen_frame.h"     // 栈槽分配 / 帧记录
    #include "codegen_tag.h"       // shadow tag / 直调 tag ABI / 零开销异常
};

}  // namespace pyadamas
