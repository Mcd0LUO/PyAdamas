// frame.h —— 栈帧布局与帧记录（从 AsmGen 里拆出的第一个协作对象）
//
// 拆它的理由（见 docs/modularization.md 第 2 步）：AsmGen 原本把 161 个字段、
// 约 300 个方法塞在一个类里，其中「帧」是一块边界清晰的状态 —— 布局量（出参区 /
// 局部区 / CFA 偏移）和帧记录（GC 扫描、异常展开用的槽清单）。
//
// 这里只放**数据 + 不依赖发射器**的纯计算。需要发指令的部分（frame_alloc /
// emit_epilogue / outgoing_bytes / build_frame_scan_info）仍留在 AsmGen —— 它们
// 要读 target_abi()、call_ptag_words() 等上下文，硬搬会变成一堆反向依赖。
#pragma once

#include <cstdint>
#include <vector>

namespace pyadamas {

class Frame {
public:
    // ── 布局量 ────────────────────────────────────────────────────
    // 帧布局 = [出参区 out_bytes][局部区 locals][push 的 callee-saved][返回地址]。
    // rsp 在函数体里全程不动，槽位因此可以相对 rsp 寻址，帧里不需要 rbp。
    // SysV 要求 call 时 rsp 是 16 的倍数，布局阶段把 padding 补到最小。
    int out_bytes = 0;    // 出参区：整函数最大的那个调用点用量
    int locals = 0;       // 局部区（含对齐 padding）
    int pad = 0;          // subq 的字节数 = out_bytes + locals
    int bytes = 0;        // 返回地址到 rsp 的距离 = pad + 8 * n_saved_regs
    int cfa_off = 0;      // 当前 .cfi_def_cfa_offset（CFA = rsp + cfa_off）

    // ── 帧记录 ────────────────────────────────────────────────────
    // 持有堆引用的非借用槽，异常展开时按地址 dec。
    std::vector<int> owned;
    // 与 owned 一一对应：槽里可能是非堆值，展开时走注册表释放。
    // 记录里用地址最低位置 1 标记（槽地址 8 字节对齐）。
    std::vector<bool> owned_unknown;
    // 借来的 unknown 槽：只登记给 GC 扫描，异常展开不释放。
    std::vector<int> scan;
    // 需要保守扫描的临时区字数（0 表示这个帧的临时区一个字节都不扫）。
    int scan_words = 0;
    // 临时区位图：bit i = 1 表示第 i 个字可能需 GC 保守扫描。空表示没有位图
    // （运行时按整段扫）。位图必须是超集，宁多勿少。
    std::vector<uint64_t> scan_mask;

    // vreg 槽区里第 slot 个槽的偏移（vreg 槽紧挨着命名槽往上排）。
    int vreg_slot_off(int slot, int n_slots) const { return out_bytes + 8 * (n_slots + slot); }

    // 算局部区大小与两个总长；n_saved_regs 是 push 的 callee-saved 个数。
    void layout(int total_slots, int n_saved_regs) {
        const int fixed = 8 * n_saved_regs + out_bytes;
        int p = 8 * total_slots;
        while (((fixed + p) & 15) != 8) p += 8;
        locals = p;
        pad = out_bytes + locals;
        // bytes 是 rsp 到返回地址的距离：push 的 + 下面这一整块。
        bytes = 8 * n_saved_regs + pad;
    }

    void reset() { *this = Frame{}; }
};

}  // namespace pyadamas
