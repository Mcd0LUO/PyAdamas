// eh_emit.cpp —— 零开销异常的 CFI / LSDA 文本发射器实现
//
// 字节布局权威口径：tools/lsda_ref/FINDINGS.md §0.1 / §6（gcc 14.2 实测）。
//   20 字节 catch-all 基准（handler 区间 + lp=0 覆盖区间）：
//     ff 9b 11 01 08 | 04 05 13 01 | 1b 05 00 00 | 01 00 | 00 | 00 00 00 00
//   12 字节 cleanup 基准：
//     ff ff 01 08 | 05 05 0c 00 | 1c 05 00 00
//
// 这里刻意把每条硬约束放在对应代码旁边：LPStart 用 omit ⇒ 偏移全相对函数
// 起点；action 记录是 (filter,next) 两个 SLEB128；TType 偏移指向表尾且表项
// 无重定位；call-site 表长度是编码后的字节总数。
// 纯函数式：不读 IR、不碰全局状态，失败时抛 std::invalid_argument。

#include "eh_emit.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace pyadamas {
namespace {

// DWARF 异常表编码字节（完整常量表见 runtime/rt_personality.c）。
constexpr unsigned char kEhPeOmit = 0xff;                // 省略字段 / LPStart 用 FDE region start
constexpr unsigned char kEhPeIndirectPcrelSdata4 = 0x9b; // TType 项：间接 + pcrel + sdata4
constexpr unsigned char kEhPePcrelSdata4 = 0x1b;         // LSDA 指针：pcrel + sdata4
constexpr unsigned char kEhPeUleb128 = 0x01;             // call-site 字段编码

// 一个 uleb128 值编码后的字节数（size_of_encoded_value 只对定宽编码成立，
// uleb128 必须真算）。
int uleb_size(uint64_t v) {
    int n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

void line(std::string& out, const std::string& text) {
    out += '\t';
    out += text;
    out += '\n';
}

void label(std::string& out, const std::string& name) {
    out += name;
    out += ":\n";
}

// .LLSDA<N> / .LLSDATT<N> … 的标签名。
std::string indexed(const char* base, int index) {
    return std::string(base) + std::to_string(index);
}

void require_valid(const EhFuncPlan& plan, const char* who) {
    std::string error;
    if (!eh_plan_check(plan, &error)) throw std::invalid_argument(std::string(who) + ": " + error);
}

// action 表布局：每个 range 的 action 字段值 + action 记录条数。
struct ActionPlan {
    std::vector<int> offsets;  // 每个 range 的 action 字段值；0 = 无 action 记录
    int record_count = 0;      // action 表里的记录条数
};

// ABI 语义：call-site 的 action 字段是 action 表内 **1 起的字节偏移**，不是
// 记录序号 —— libsupc++/libc++abi 都按 action_table + (cs_action - 1) 定位。
// 我们每条 action 记录固定是 (filter=1,next=0) 两个 SLEB128 = 2 字节，所以
// 第 k 条（0 起）记录的偏移值是 2k+1（1, 3, 5, …）。只有「handler 且有
// landing pad」的区间才占一条记录。
ActionPlan assign_actions(const EhFuncPlan& plan, bool labels) {
    ActionPlan out;
    out.offsets.assign(plan.ranges.size(), 0);
    for (size_t i = 0; i < plan.ranges.size(); ++i) {
        const EhRange& r = plan.ranges[i];
        const bool has_lp = labels ? (r.lp_label != nullptr) : (r.lp_off != 0);
        if (r.is_handler && has_lp) out.offsets[i] = 2 * out.record_count++ + 1;
    }
    return out;
}

// 数值模式：call-site 表的字节长度（**不是条目数**，硬约束 5）。
uint64_t call_site_table_len(const EhFuncPlan& plan, const std::vector<int>& actions) {
    uint64_t bytes = 0;
    for (size_t i = 0; i < plan.ranges.size(); ++i) {
        const EhRange& r = plan.ranges[i];
        bytes += static_cast<uint64_t>(uleb_size(r.begin_off));
        bytes += uleb_size(static_cast<uint64_t>(r.end_off) - r.begin_off);
        bytes += uleb_size(r.lp_off);
        bytes += uleb_size(static_cast<uint64_t>(actions[i]));
    }
    return bytes;
}

// 数值模式：TType 表偏移（uleb128）——从「该字段之后」指向 TType 表**末尾**，
// 反向索引要求第 i 项在 ttype_end - i*4（硬约束 3）。偏移字段自身的 uleb
// 宽度会反过来影响总长，所以迭代到不动点；实际取值恒 < 128，一次就收敛。
uint64_t ttype_table_offset(uint64_t call_site_len, int action_count) {
    const uint64_t action_len = static_cast<uint64_t>(action_count) * 2;  // (filter,next) 各 1 字节
    const uint64_t cs_len_uleb = static_cast<uint64_t>(uleb_size(call_site_len));
    int off_len = 1;
    for (int iter = 0; iter < 8; ++iter) {
        // base = 偏移字段之后的位置：lpstart_encoding + ttype_encoding + 偏移字段
        const uint64_t base = 2 + static_cast<uint64_t>(off_len);
        // pre = action 表之后的位置（call-site 编码字节 + 表长 uleb + 表体 + action 表）
        const uint64_t pre = base + 1 + cs_len_uleb + call_site_len + action_len;
        const uint64_t pad = (4 - pre % 4) % 4;   // .align 4（不属于任何表）
        const uint64_t total = pre + pad + 4;     // 末尾 4 字节 TType[1]
        const uint64_t off = total - base;
        const int next = uleb_size(off);
        if (next == off_len) return off;
        off_len = next;
    }
    throw std::logic_error("eh_emit: TType 表偏移的 uleb 宽度迭代不收敛");
}

}  // namespace

bool eh_plan_check(const EhFuncPlan& plan, std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };
    if (plan.ranges.empty()) return true;  // 空计划：三段都不发射
    if (plan.lsda_index < 0) return fail("lsda_index 不能为负");

    bool any_labels = false;
    bool all_labels = true;
    for (size_t i = 0; i < plan.ranges.size(); ++i) {
        const EhRange& r = plan.ranges[i];
        const std::string at = "ranges[" + std::to_string(i) + "]";
        const bool begin_l = r.begin_label != nullptr;
        const bool end_l = r.end_label != nullptr;
        const bool lp_l = r.lp_label != nullptr;
        if (begin_l != end_l) return fail(at + "：begin_label 与 end_label 必须成对给出");
        const bool labels = begin_l && end_l;
        any_labels = any_labels || labels;
        all_labels = all_labels && labels;
        if (labels) {
            if (plan.func_label == nullptr || plan.func_label[0] == '\0')
                return fail("标签模式需要 EhFuncPlan::func_label（函数起点标签）");
            if (!lp_l && r.lp_off != 0)
                return fail(at + "：标签模式只认 lp_label；没有 landing pad 时 lp_off 必须为 0");
        } else {
            if (lp_l) return fail(at + "：没有 begin/end 标签时不能单独给 lp_label");
            if (r.begin_off >= r.end_off) return fail(at + "：begin_off 必须小于 end_off");
        }
        if (r.is_handler) {
            const bool has_lp = labels ? lp_l : (r.lp_off != 0);
            if (!has_lp) return fail(at + "：handler 区间必须有 landing pad（lp_off / lp_label）");
        }
    }
    if (any_labels && !all_labels) return fail("同一个 plan 不能混用标签模式与数值模式");
    return true;
}

bool eh_plan_uses_labels(const EhFuncPlan& plan) {
    return !plan.ranges.empty() && plan.ranges.front().begin_label != nullptr &&
           plan.ranges.front().end_label != nullptr;
}

void emit_eh_func_directives(std::string& out, const EhFuncPlan& plan) {
    require_valid(plan, "emit_eh_func_directives");
    if (plan.ranges.empty()) return;
    // 0x9b = indirect|pcrel|sdata4：操作数必须是 DW.ref 指针槽，不能是函数本体
    //（直接写 pa_personality 汇编和链接都不报错，运行期 SIGSEGV）。
    line(out, std::format(".cfi_personality {:#x}, {}",
                          static_cast<unsigned>(kEhPeIndirectPcrelSdata4), kEhDwrefLabel));
    // 0x1b = pcrel|sdata4：指到本文件 .gcc_except_table 里的 .LLSDA<N>。
    line(out, std::format(".cfi_lsda {:#x}, {}", static_cast<unsigned>(kEhPePcrelSdata4),
                          indexed(".LLSDA", plan.lsda_index)));
}

void emit_eh_lsda(std::string& out, const EhFuncPlan& plan) {
    require_valid(plan, "emit_eh_lsda");
    if (plan.ranges.empty()) return;

    const int n = plan.lsda_index;
    const bool labels = eh_plan_uses_labels(plan);
    const ActionPlan action_plan = assign_actions(plan, labels);
    const std::vector<int>& actions = action_plan.offsets;
    const int action_count = action_plan.record_count;

    // 有 handler ⇒ 形态 A（ttype=0x9b + action 表 + TType 表）；否则形态 B
    // （ttype=0xff，偏移字段/action 表/TType 表全都没有）。
    const bool handler_form = action_count > 0;

    uint64_t cs_len = 0;
    uint64_t ttype_off = 0;
    if (!labels) {
        cs_len = call_site_table_len(plan, actions);
        if (handler_form) ttype_off = ttype_table_offset(cs_len, action_count);
    }

    out += std::format("\n# ---- LSDA {}：{} ----\n", n,
                       handler_form
                           ? "catch-all 形态（ttype=0x9b + TType null 项 + filter=1）"
                           : "cleanup 形态（ttype=0xff，无 action/TType 表）");
    line(out, ".section .gcc_except_table,\"a\",@progbits");
    line(out, ".align 4");
    label(out, indexed(".LLSDA", n));

    // ── header：lpstart_enc | ttype_enc | [ttype_off] | call_site_enc | call_site_len ──
    line(out, std::format(".byte {:#x}", static_cast<unsigned>(kEhPeOmit)));  // LPStart = 函数起点
    if (handler_form) {
        line(out, std::format(".byte {:#x}",
                              static_cast<unsigned>(kEhPeIndirectPcrelSdata4)));  // TType enc
        if (labels) {
            line(out, std::format(".uleb128 {}-{}", indexed(".LLSDATT", n), indexed(".LLSDATTD", n)));
            label(out, indexed(".LLSDATTD", n));
        } else {
            line(out, std::format(".uleb128 {}", ttype_off));  // 指向 TType 表末尾
        }
    } else {
        // ttype_encoding = 0xff ⇒ TType 偏移字段整段消失（header 少一个 uleb）。
        line(out, std::format(".byte {:#x}", static_cast<unsigned>(kEhPeOmit)));
    }
    line(out, std::format(".byte {:#x}", static_cast<unsigned>(kEhPeUleb128)));  // call-site enc
    if (labels) {
        line(out, std::format(".uleb128 {}-{}", indexed(".LLSDACSE", n), indexed(".LLSDACSB", n)));
        label(out, indexed(".LLSDACSB", n));
    } else {
        line(out, std::format(".uleb128 {}", cs_len));  // 字节数，不是条目数
    }

    // ── call-site 表：每条 range 四个字段（start, length, landing_pad, action）──
    for (size_t i = 0; i < plan.ranges.size(); ++i) {
        const EhRange& r = plan.ranges[i];
        if (labels) {
            line(out, std::format(".uleb128 {}-{}", r.begin_label, plan.func_label));
            line(out, std::format(".uleb128 {}-{}", r.end_label, r.begin_label));
            if (r.lp_label != nullptr)
                line(out, std::format(".uleb128 {}-{}", r.lp_label, plan.func_label));
            else
                line(out, ".uleb128 0");  // 0 = 该区间没有 landing pad
        } else {
            line(out, std::format(".uleb128 {}", r.begin_off));
            line(out, std::format(".uleb128 {}",
                                  static_cast<uint32_t>(r.end_off - r.begin_off)));
            line(out, std::format(".uleb128 {}", r.lp_off));
        }
        line(out, std::format(".uleb128 {}", actions[i]));
    }
    label(out, indexed(".LLSDACSE", n));  // action 表起点（也是标签模式长度的右端）

    // ── action 表 + TType 表（仅形态 A）──
    if (handler_form) {
        for (int a = 0; a < action_count; ++a) {
            line(out, ".byte 0x01");  // filter = 1（SLEB128）：handler，取 TType[1]
            line(out, ".byte 0x00");  // next   = 0（SLEB128）：链尾
        }
        // 硬约束 4：TType 项前 .align 4，padding 不属于任何表。
        line(out, ".align 4");
        // 硬约束 3：TType[1] = 4 字节 0 且**不能有重定位** ⇒ null type_info*
        // ⇒ catch(...)。filter=0 才是 cleanup，filter>0 + null 才是 catch-all。
        line(out, ".long 0");
        label(out, indexed(".LLSDATT", n));  // = TType 表末尾（反向索引的基准）
    }
}

void emit_eh_dwref(std::string& out) {
    out += "\n# ---- personality 间接指针槽（0x9b 要求；每个 TU 只发一次）----\n";
    line(out, ".section .data.rel.local,\"aw\",@progbits");
    line(out, ".align 8");
    label(out, kEhDwrefLabel);
    line(out, std::format(".quad {}", kEhPersonalitySymbol));
}

}  // namespace pyadamas
