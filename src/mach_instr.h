// mach_instr.h —— 汇编发射的结构化机器层
//
// 背景：codegen 过去把 AT&T 文本直接拼进 std::string（约 250 处 std::format
// + 250 处 out_ +=）。后果是没有能挂 pass 的地方、跨目标差异散落在各处的
// if (!gnu_cfi)、寄存器命名字符串到处重复。这一层把发射结果换成结构化
// 的 MItem 序列：标签 / 指示符 / 指令 / 注释 / 原始文本，指令再拆成
// 操作码 MOp 与操作数 MOperand。
//
// 铁律是 render() 的输出与改动前逐字节一致。因此：
//   * 寄存器按编号存（MReg），名字查编译期静态表（LLVM printRegName /
//     GCC reg_names[] 的做法），发射期零字符串构造；表外名字退化 text
//     原样输出，认不出的操作数退化 Raw，字节不变；
//   * MOperand 带 canonical text（与 render 结果逐字节一致），给历史
//     字符串接口（src_op/dst_op）当返回值用；Imm/Mem 的渲染走结构化字段；
//   * 操作数内联存储（MOpVec：2 个内联 + 溢出 vector），x86 指令操作数
//     ≤3，绝大多数指令零堆分配；
//   * Raw 只是迁移兜底，正常发射点应当落成 Label/Directive/Instr/Comment。
#pragma once

#include <cstdint>
#include <initializer_list>
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
    kMovsbq,  // P1 定宽 i8 正常化：低字节符号扩展（只在新路径发射）
    kLeaq,
    // ── 整数算术 ──
    kAddq, kSubq, kImulq, kIdivq, kDivq, kNegq, kNotq, kIncq, kDecq,
    kAndq, kOrq, kXorq, kCmpq, kTestq, kShlq, kSarq, kShrq, kSalq, kCqto, kCltq,
    kBtsq, kBtrq,
    kAddl, kSubl, kCmpl, kAndl, kOrl, kXorl, kTestl, kAndb, kOrb, kXorb,
    // ── 栈与控制流 ──
    kPushq, kPopq, kCall, kJmp, kRet, kNop, kUd2, kLeave,
    // ── 条件跳转 ──
    kJa, kJae, kJb, kJbe, kJe, kJne, kJg, kJge, kJl, kJle, kJs, kJns, kJp,
    // 注意 jo/jc/jz 故意不登记：历史 emit_jmp 对它们走文本兜底，push_line
    // 收到无 tab 文本落列 0 Raw。结构化 Instr 渲染带 tab，会破坏字节不变。
    // ── 置位（比较结果 → 0/1） ──
    kSeta, kSetae, kSetb, kSetbe, kSete, kSetg, kSetge, kSetl, kSetle, kSetne, kSetnp, kSetp,
    // ── 浮点 ──
    kMovsd, kMovapd, kMovss, kAddsd, kSubsd, kMulsd, kDivsd, kUcomisd, kComisd,
    kCvtsi2sdq, kCvtsi2sdl, kCvttsd2siq, kCvttsd2sil, kXorpd, kPxor,
    // ── packed SIMD（SSE2，0.333 向量化原型；门控 PYADAMAS_SIMD，默认关）──
    // 只登记助记符，现有发射点不引用 → flag-off 逐字节不变。
    kMovupd, kMulpd, kAddpd, kSubpd, kDivpd,
    kPaddq, kPsubq, kPmuludq, kPsrlq, kPsllq, kPshufd,
    kCvtdq2pd, kMovlhps, kMovhlps, kUnpcklpd,
    // 0.334 nbody pairwise：标量 sqrt 内联（与既有 sqrtsd fast path 同指令）。
    kSqrtsd,
    // 0.351 通用向量化深化：packed dword 算术右移（Convert i64→f64 折算
    // 序列取 lo 符号掩码用；SSE2 无 psraq，64 位算术右移不存在）。
    // 只登记助记符，现有发射点不引用 → flag-off 逐字节不变。
    kPsrad,
};

// 操作码 → 汇编助记符（kRaw 返回空串）。未登记的名字返回 MOp::kRaw。
const char* mop_name(MOp op);
MOp mop_from_name(std::string_view name);
// 裸符号操作数是代码标签（jmp/jcc 的目标）还是数据/函数符号（call 目标）。
bool mop_is_branch(MOp op);
bool mop_is_call(MOp op);

// ── 静态寄存器名表 ─────────────────────────────────────────────
// 寄存器按编号存，名字查编译期静态表：发射/渲染零字符串构造。
// 覆盖 codegen 实际发射的全部 x86-64 寄存器（GPR 64/32/8 位 + xmm + rip）。
// 表外名字（is_reg_name 认得但表里没有）由 parse_operand 存进 text 兜底。
enum MReg : uint8_t {
    kMRegNone = 0,
    kRax, kRbx, kRcx, kRdx, kRsi, kRdi, kRbp, kRsp,
    kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15,
    kEax, kEbx, kEcx, kEdx, kEsi, kEdi, kEbp, kEsp,
    kR8d, kR9d, kR10d, kR11d, kR12d, kR13d, kR14d, kR15d,
    kAl, kBl, kCl, kDl, kSil, kDil, kBpl, kSpl,
    kR8b, kR9b, kR10b, kR11b, kR12b, kR13b, kR14b, kR15b,
    kXmm0, kXmm1, kXmm2, kXmm3, kXmm4, kXmm5, kXmm6, kXmm7,
    kXmm8, kXmm9, kXmm10, kXmm11, kXmm12, kXmm13, kXmm14, kXmm15,
    kRip,
    kMRegCount
};
// 编号 → 名字（带 %）。kMRegNone 返回空串。
std::string_view mreg_name(MReg r);
// 名字（带 %）→ 编号。表外返回 kMRegNone。
MReg mreg_from_name(std::string_view name);

enum class MOperandKind : uint8_t {
    Reg,    // 寄存器：reg 编号（表外名字 reg==kMRegNone，text 兜底）
    Imm,    // 立即数：imm + imm_hex，文本由值生成（原始拼写均为十进制或 0x 小写十六进制）
    Mem,    // 内存：reg(base)/index/scale/imm(disp)/sym
    Label,  // 代码标签：text = ".Lfoo" / "1f"
    Sym,    // 数据/函数符号：text = "pa_rc_dec"
    Raw,    // 兜底：text 原样输出
};

struct MOperand {
    MOperandKind kind = MOperandKind::Raw;
    // 间接前缀 '*'：call *%r11 / jmp *(%r10)
    bool indirect = false;
    MReg reg = kMRegNone;     // Reg：寄存器编号；Mem：基址寄存器编号
    MReg index = kMRegNone;   // Mem：变址寄存器编号
    // Imm：数值。十六进制按无符号位模式存（0x8000000000000000 会变成
    // 负的 int64），比较/打印统一走 uint64_t 视图。Mem：位移值。
    int64_t imm = 0;
    bool imm_hex = false;
    bool has_disp = false;    // 显式写了位移（含 "0(...)" 的 0）
    bool has_scale = false;   // 显式写了 ",scale"
    bool has_sym = false;     // Mem：符号位移（.Lstr0(%rip)），渲染走 text
    uint8_t scale = 1;

    // canonical 完整文本（不含 '*' 前缀），与 render 输出逐字节一致。
    // 给历史字符串接口（src_op/dst_op 等）当返回值；Reg/Label/Sym/Raw
    // 与符号位移 Mem 渲染直接用它，Imm/数值位移 Mem 走结构化字段。
    std::string text;

    // ── 缓存单元兼容接口 ─────────────────────────────────────────
    // 操作数缓存（vreg_mem_/slot_reg_/alias_ 等）从字符串换成 MOperand
    // 单元：定义点赋值时解析一次成结构化，使用点直发免回解析。历史字符
    // 串调用点经 implicit const string& 转换零改动读 canonical text。
    MOperand() = default;
    MOperand(const char* s) { set_text(s); }
    MOperand(std::string s) { set_text(s); }
    MOperand& operator=(const char* s) { set_text(s); return *this; }
    MOperand& operator=(std::string s) { set_text(s); return *this; }
    bool empty() const { return text.empty(); }
    void clear() { *this = MOperand{}; }
    operator const std::string&() const { return text; }
    // 比较按 canonical text（与历史字符串比较同口径）。字符串实参经
    // 转换构造参与比较（只这一个 == 重载，免与转换构造互相歧义）。
    friend bool operator==(const MOperand& a, const MOperand& b) { return a.text == b.text; }
    // 解析操作数字符串成结构化；认不出的文法退化 Raw（text 原样，字节不变）。
    void set_text(std::string_view s);
};

// 预建寄存器操作数静态表：发射期按编号取，零构造（LLVM 静态 reg 表思路）。
const MOperand& mreg_operand(MReg r);

// 操作数小向量：2 个内联 + 溢出 vector。x86 指令操作数 ≤3，
// 两操作数指令（绝大多数）整条零堆分配；三操作数 imulq 才溢出。
// 接口与 std::vector 对齐需要的子集，迁移期调用点零改动。
class MOpVec {
public:
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    void reserve(size_t n) { if (n > 2) ext_.reserve(n - 2); }
    void clear() { n_ = 0; ext_.clear(); }
    void push_back(MOperand o) {
        if (n_ < 2) inl_[n_] = std::move(o);
        else ext_.push_back(std::move(o));
        ++n_;
    }
    void assign(std::initializer_list<MOperand> il) {
        clear();
        for (const MOperand& o : il) push_back(o);
    }
    MOperand& operator[](size_t i) { return i < 2 ? inl_[i] : ext_[i - 2]; }
    const MOperand& operator[](size_t i) const { return i < 2 ? inl_[i] : ext_[i - 2]; }
    class const_iterator {
    public:
        const_iterator(const MOpVec* v, size_t i) : v_(v), i_(i) {}
        const MOperand& operator*() const { return (*v_)[i_]; }
        const_iterator& operator++() { ++i_; return *this; }
        bool operator!=(const const_iterator& o) const { return i_ != o.i_; }
    private:
        const MOpVec* v_;
        size_t i_;
    };
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, n_); }
private:
    MOperand inl_[2];
    std::vector<MOperand> ext_;
    uint8_t n_ = 0;
};

struct MInstr {
    MOp op = MOp::kRaw;
    MOpVec ops;
    // kRaw 助记符不进结构化层：parse_instr 认不出的助记符整行退化 Raw，
    // 所以这里不需要 mnemonic 字符串（省 32B + 每次拷贝）。
};

struct MItem {
    enum Kind : uint8_t { Label, Directive, Instr, Raw, Comment } kind = Raw;
    bool indented = false;  // Label：是否带前导 tab（块内数字局部标签 "\t1:"）
    // Instr：指令在 MachFunc::insts 池里的下标（其余 kind 为 -1）。
    // 指令单独存池：MItem 保持 40B 轻量，标签/注释/指示符不背操作数存储，
    // items 向量 push/移动/render 遍历的流量比内嵌 MInstr 小一个量级。
    int32_t inst = -1;
    // Label：标签名（不含冒号）；Directive：正文（不含前导 tab）；
    // Comment：整行文本（含缩进）；Raw：原样文本（含换行）
    std::string text;
};

// 一个可渲染的机器函数：顺序敏感的 MItem 列表 + 指令池。
// bytes 是已累计文本长度的估算，只给 reserve 当提示用。
struct MachFunc {
    std::vector<MItem> items;
    std::vector<MInstr> insts;
    size_t bytes = 0;

    void clear();
    void reserve(size_t n) { items.reserve(n / 24 + 16); insts.reserve(n / 40 + 8); }
    size_t size() const { return bytes; }
    bool empty() const { return items.empty(); }

    void push(MItem it);
    void push_instr(MInstr ins);
    // 静态行缓存命中：复制缓存的 item + 指令进本函数（指令下标重定位）。
    // ins 按值收：调用方从缓存拷贝一次，这里 move 进池，免二次拷贝。
    void push_cached(const MItem& it, MInstr ins);
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

// 行/指令/操作数解析。解析器只认 codegen 实际会生成的文法；认不出的
// 一律返回失败，由调用方退化成 Raw，保证字节不变。
// 完整行（含缩进，不含换行）解析进 f：指令落 insts 池，item 记下标。
void parse_line_into(std::string_view line, MachFunc& f);
MInstr parse_instr(std::string_view body, bool* ok);           // body = "mnemonic op, op"
bool parse_operand(std::string_view text, bool as_label, MOperand* out);

// 渲染成汇编文本。必须与改动前的字符串拼接结果逐字节一致。
std::string render(const MachFunc& f);
void render_item(const MItem& it, const MachFunc& f, std::string& out);

}  // namespace pyadamas
