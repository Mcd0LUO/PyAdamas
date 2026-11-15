// mach_instr.h —— 汇编发射的结构化机器层
//
// 背景：codegen 过去把 AT&T 文本直接拼进 std::string（约 250 处 std::format
// + 250 处 out_ +=）。后果是没有能挂 pass 的地方、跨目标差异散落在各处的
// if (!gnu_cfi)、寄存器命名字符串到处重复。这一层把发射结果换成结构化
// 的 MItem 序列：标签 / 指示符 / 指令 / 注释 / 原始文本，指令再拆成
// 操作码 MOp 与操作数 MOperand。
//
// 这一轮是纯重构，铁律是 render() 的输出与改动前逐字节一致。因此：
//   * 第一阶段寄存器仍按名字存（MOperand::Reg.text 形如 "%rax"），
//     等指令层稳定后再换成物理寄存器编号 / 虚拟寄存器编号；
//   * 任何解析不出来的助记符或操作数都退化到 kRaw / MOperandKind::Raw /
//     MItem::Raw，原样保留字节，不做「猜测式重写」；
//   * Raw 只是迁移兜底，正常发射点应当落成 Label/Directive/Instr/Comment。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pyadamas {

// 指令操作码。只覆盖 codegen 当前实际发射的指令（按语料统计 + 源码扫描
// 的并集）；新指令先加枚举，来不及加的走 kRaw 保留原始助记符。
enum class MOp : uint8_t {
    kRaw = 0,
    // ── 数据搬运 ──
    kMovq, kMovl, kMovw, kMovb, kMovabsq, kMovzbq, kMovzbl, kMovzwl, kMovslq, kMovswq,
    kLeaq,
    // ── 整数算术 ──
    kAddq, kSubq, kImulq, kIdivq, kDivq, kNegq, kNotq, kIncq, kDecq,
    kAndq, kOrq, kXorq, kCmpq, kTestq, kShlq, kSarq, kShrq, kSalq, kCqto, kCltq,
    kAddl, kSubl, kCmpl, kAndl, kOrl, kXorl, kTestl, kAndb, kOrb, kXorb,
    // ── 栈与控制流 ──
    kPushq, kPopq, kCall, kJmp, kRet, kNop, kUd2, kLeave,
    // ── 条件跳转 ──
    kJa, kJae, kJb, kJbe, kJe, kJne, kJg, kJge, kJl, kJle, kJs, kJns, kJp,
    // ── 置位（比较结果 → 0/1） ──
    kSeta, kSetae, kSetb, kSetbe, kSete, kSetg, kSetge, kSetl, kSetle, kSetne, kSetnp, kSetp,
    // ── 浮点 ──
    kMovsd, kMovapd, kMovss, kAddsd, kSubsd, kMulsd, kDivsd, kUcomisd, kComisd,
    kCvtsi2sdq, kCvtsi2sdl, kCvttsd2siq, kCvttsd2sil, kXorpd, kPxor,
};

// 操作码 → 汇编助记符（kRaw 返回空串）。未登记的名字返回 MOp::kRaw。
const char* mop_name(MOp op);
MOp mop_from_name(std::string_view name);
// 裸符号操作数是代码标签（jmp/jcc 的目标）还是数据/函数符号（call 目标）。
bool mop_is_branch(MOp op);
bool mop_is_call(MOp op);

enum class MOperandKind : uint8_t {
    Reg,    // 寄存器：text = "%rax"
    Imm,    // 立即数：imm + imm_hex，文本由值生成（原始拼写均为十进制或 0x 小写十六进制）
    Mem,    // 内存：base/index/scale/disp/sym
    Label,  // 代码标签：text = ".Lfoo" / "1f"
    Sym,    // 数据/函数符号：text = "pa_rc_dec"
    Raw,    // 兜底：text 原样输出
};

struct MOperand {
    MOperandKind kind = MOperandKind::Raw;
    // 间接前缀 '*'：call *%r11 / jmp *(%r10)
    bool indirect = false;

    // Reg / Label / Sym / Raw：原样文本（Reg 带 %）
    std::string text;
    // Imm：数值。十六进制按无符号位模式存（0x8000000000000000 会变成
    // 负的 int64），比较/打印统一走 uint64_t 视图。
    int64_t imm = 0;
    bool imm_hex = false;

    // Mem：基址与变址寄存器名（带 %，可为空）、比例、符号位移
    std::string base;
    std::string index;
    std::string sym;
    int64_t disp = 0;
    bool has_disp = false;    // 显式写了位移（含 "0(...)" 的 0）
    int scale = 1;
    bool has_scale = false;   // 显式写了 ",scale"
};

struct MInstr {
    MOp op = MOp::kRaw;
    std::string mnemonic;  // op == kRaw 时的原始助记符
    std::vector<MOperand> ops;
};

struct MItem {
    enum Kind : uint8_t { Label, Directive, Instr, Raw, Comment } kind = Raw;
    bool indented = false;  // Label：是否带前导 tab（块内数字局部标签 "\t1:"）
    // Label：标签名（不含冒号）；Directive：正文（不含前导 tab）；
    // Comment：整行文本（含缩进）；Raw：原样文本（含换行）
    std::string text;
    MInstr inst;
};

// 一个可渲染的机器函数：顺序敏感的 MItem 列表。
// bytes 是已累计文本长度的估算，只给 reserve 当提示用。
struct MachFunc {
    std::vector<MItem> items;
    size_t bytes = 0;

    void clear();
    void reserve(size_t n) { items.reserve(n / 24 + 16); }
    size_t size() const { return bytes; }
    bool empty() const { return items.empty(); }

    void push(MItem it);
    void push_instr(MInstr ins);
    void push_line(std::string_view line);       // 解析完整一行（含 tab，不含换行）
    void push_body(std::string_view body);       // 解析 tab 之后的正文（line() 的入口）
    void push_directive(std::string_view body);  // "\t" + body + "\n"
    void push_label(std::string_view name, bool indented = false);

    // 兼容旧发射点：原样文本进 Raw（与上一条 Raw 合并，避免碎片化）。
    void append_raw(std::string_view s);
    void operator+=(std::string_view s) { append_raw(s); }
    void operator+=(char c) { append_raw(std::string_view(&c, 1)); }
    void append(const char* first, const char* last);
};

// 便利构造（迁移后的发射点直接 push 这些）。
MItem make_label(std::string name, bool indented = false);
MItem make_directive(std::string body);
MItem make_comment(std::string line);  // line 是整行文本（含 "\t# " 之类前缀）
MItem make_raw(std::string text);
MItem make_instr_item(MInstr ins);

// 行/指令/操作数解析。解析器只认 codegen 实际会生成的文法；认不出的
// 一律返回失败，由调用方退化成 Raw，保证字节不变。
MItem parse_line(std::string_view line);  // 完整行（含缩进，不含换行），失败退化 Raw
MItem parse_body(std::string_view body);  // tab 之后的正文，失败退化 Raw（带 tab 前缀）
MInstr parse_instr(std::string_view body, bool* ok);           // body = "mnemonic op, op"
bool parse_operand(std::string_view text, bool as_label, MOperand* out);

// 渲染成汇编文本。必须与改动前的字符串拼接结果逐字节一致。
std::string render(const MachFunc& f);
void render_item(const MItem& it, std::string& out);

}  // namespace pyadamas
