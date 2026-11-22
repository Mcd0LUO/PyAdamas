// tag_lower.h —— tag（影子类型）与零开销异常状态（从 AsmGen 拆出的第三个协作对象）
//
// 拆它的理由同 Frame / Emitter：这 14 个字段是「每个函数重建一次的附属状态」，
// 与指令选择无关。边界取法与前面一致 —— 只搬状态，发射方法留在 AsmGen。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mach_instr.h"

namespace pyadamas {

class TagLower {
public:
    // ── tag（影子类型，PYADAMAS_TYPE_TAGS，默认关）────────────────────
    bool tags_enabled = false;
    bool type_tags = false;
    int n_tag_words = 0;              // 帧内平行 tag 区字数
    std::vector<int> vreg_tag;        // vreg -> tag 字号（-1 无）
    std::vector<int> slot_tag;        // 命名槽 -> tag 字号（-1 无）
    std::vector<MOperand> vreg_tag_mem;
    std::vector<MOperand> slot_tag_mem;

    // ── 零开销异常（Z2-b2，--zero-cost-exc）──────────────────────────
    // 每个函数的 EH 计划在 emit_function 开头按 IR 里的 EhBegin/EhEnd/
    // EhLandingPad 标记预扫描出来（标签名也在那时按发射顺序分配好，发射标记
    // 指令时按同一顺序取用）。计划为空 = 老 setjmp 路径，一个字节不发。
    // 一个函数的所有保护区合成一份 LSDA（多条 call-site 记录）。
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
    std::vector<EhPlanRec> eh_plans;
    // 当前函数预扫描出来的标签（发射标记指令时按顺序取用）。
    std::vector<std::string> eh_begin_labels;
    std::vector<std::string> eh_end_labels;
    std::vector<std::string> eh_lp_labels;
    std::size_t eh_begin_seq = 0;
    std::size_t eh_end_seq = 0;
    std::size_t eh_lp_seq = 0;

    void reset() { *this = TagLower{}; }
};

}  // namespace pyadamas
