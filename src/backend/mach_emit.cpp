// backend/mach_emit.cpp —— Machine 层的解析与渲染
//
// 解析：把 codegen 原有发射辅助函数给出的文本行拆成 MItem / MInstr /
// MOperand；认不出的文法一律退化 Raw（原字节保留），这样迁移期可以先
// 换底层容器、再逐文件清理直写点，而生成的汇编一个字节都不变。
// 渲染：render() 重新拼出与原先完全相同的文本；用过的语料（175 个测试
// 逐个 asm）做 md5 全量比对，见任务报告。
#include "backend/mach_instr.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace pyadamas {
namespace {

struct NamedOp {
    const char* name;
    MOp op;
};

// 常见指令排在前面：查找是线性扫描，每条指令一次。
constexpr NamedOp kOpNames[] = {
    {"movq", MOp::kMovq},
    {"call", MOp::kCall},
    {"leaq", MOp::kLeaq},
    {"addq", MOp::kAddq},
    {"subq", MOp::kSubq},
    {"cmpq", MOp::kCmpq},
    {"testq", MOp::kTestq},
    {"jmp", MOp::kJmp},
    {"je", MOp::kJe},
    {"jne", MOp::kJne},
    {"jb", MOp::kJb},
    {"jbe", MOp::kJbe},
    {"ja", MOp::kJa},
    {"jae", MOp::kJae},
    {"jg", MOp::kJg},
    {"jge", MOp::kJge},
    {"jl", MOp::kJl},
    {"jle", MOp::kJle},
    {"js", MOp::kJs},
    {"jns", MOp::kJns},
    {"jp", MOp::kJp},
    {"ret", MOp::kRet},
    {"pushq", MOp::kPushq},
    {"popq", MOp::kPopq},
    {"xorl", MOp::kXorl},
    {"xorq", MOp::kXorq},
    {"xorpd", MOp::kXorpd},
    {"xorb", MOp::kXorb},
    {"orq", MOp::kOrq},
    {"orb", MOp::kOrb},
    {"andq", MOp::kAndq},
    {"andb", MOp::kAndb},
    {"imulq", MOp::kImulq},
    {"idivq", MOp::kIdivq},
    {"divq", MOp::kDivq},
    {"negq", MOp::kNegq},
    {"notq", MOp::kNotq},
    {"incq", MOp::kIncq},
    {"decq", MOp::kDecq},
    {"shlq", MOp::kShlq},
    {"sarq", MOp::kSarq},
    {"shrq", MOp::kShrq},
    {"salq", MOp::kSalq},
    {"cqto", MOp::kCqto},
    {"cltq", MOp::kCltq},
    {"btsq", MOp::kBtsq},
    {"btrq", MOp::kBtrq},
    {"ud2", MOp::kUd2},
    {"nop", MOp::kNop},
    {"leave", MOp::kLeave},
    {"movl", MOp::kMovl},
    {"movb", MOp::kMovb},
    {"movw", MOp::kMovw},
    {"movabsq", MOp::kMovabsq},
    {"movzbq", MOp::kMovzbq},
    {"movzbl", MOp::kMovzbl},
    {"movzwl", MOp::kMovzwl},
    {"movslq", MOp::kMovslq},
    {"movswq", MOp::kMovswq},
    {"movsbq", MOp::kMovsbq},
    {"movsd", MOp::kMovsd},
    {"movapd", MOp::kMovapd},
    {"movss", MOp::kMovss},
    {"addsd", MOp::kAddsd},
    {"subsd", MOp::kSubsd},
    {"mulsd", MOp::kMulsd},
    {"divsd", MOp::kDivsd},
    {"ucomisd", MOp::kUcomisd},
    {"comisd", MOp::kComisd},
    {"cvtsi2sdq", MOp::kCvtsi2sdq},
    {"cvtsi2sdl", MOp::kCvtsi2sdl},
    {"cvttsd2siq", MOp::kCvttsd2siq},
    {"cvttsd2sil", MOp::kCvttsd2sil},
    {"pxor", MOp::kPxor},
    // packed SIMD（SSE2，0.333 向量化原型）
    {"movupd", MOp::kMovupd},
    {"mulpd", MOp::kMulpd},
    {"addpd", MOp::kAddpd},
    {"subpd", MOp::kSubpd},
    {"divpd", MOp::kDivpd},
    {"paddq", MOp::kPaddq},
    {"pcmpeqd", MOp::kPcmpeqd},
    {"pand", MOp::kPand},
    {"psubq", MOp::kPsubq},
    {"pmuludq", MOp::kPmuludq},
    {"psrlq", MOp::kPsrlq},
    {"psllq", MOp::kPsllq},
    {"psrad", MOp::kPsrad},
    {"pshufd", MOp::kPshufd},
    {"cvtdq2pd", MOp::kCvtdq2pd},
    {"movlhps", MOp::kMovlhps},
    {"movhlps", MOp::kMovhlps},
    {"unpcklpd", MOp::kUnpcklpd},
    {"sqrtsd", MOp::kSqrtsd},
    {"seta", MOp::kSeta},
    {"setae", MOp::kSetae},
    {"setb", MOp::kSetb},
    {"setbe", MOp::kSetbe},
    {"sete", MOp::kSete},
    {"setg", MOp::kSetg},
    {"setge", MOp::kSetge},
    {"setl", MOp::kSetl},
    {"setle", MOp::kSetle},
    {"setne", MOp::kSetne},
    {"setnp", MOp::kSetnp},
    {"setp", MOp::kSetp},
};

// 静态寄存器名表：下标 = MReg 编号。与 backend/mach_instr.h 的枚举逐一对应。
constexpr std::string_view kRegNames[] = {
    "",  // kMRegNone
    "%rax", "%rbx", "%rcx", "%rdx", "%rsi", "%rdi", "%rbp", "%rsp",
    "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15",
    "%eax", "%ebx", "%ecx", "%edx", "%esi", "%edi", "%ebp", "%esp",
    "%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d",
    "%al", "%bl", "%cl", "%dl", "%sil", "%dil", "%bpl", "%spl",
    "%r8b", "%r9b", "%r10b", "%r11b", "%r12b", "%r13b", "%r14b", "%r15b",
    "%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7",
    "%xmm8", "%xmm9", "%xmm10", "%xmm11", "%xmm12", "%xmm13", "%xmm14", "%xmm15",
    "%rip",
};
static_assert(std::size(kRegNames) == kMRegCount, "reg table out of sync");

bool is_reg_name(std::string_view s) {
    if (s.size() < 2 || s[0] != '%') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

// 十进制整数（可带负号）。
bool parse_decimal(std::string_view s, int64_t* out) {
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (i == s.size()) return false;
    }
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    if (neg) {
        constexpr uint64_t kMinMag = static_cast<uint64_t>(INT64_MAX) + 1;
        if (v > kMinMag) return false;
        if (v == kMinMag) {
            *out = INT64_MIN;
        } else {
            *out = -static_cast<int64_t>(v);
        }
    } else {
        *out = static_cast<int64_t>(v);
    }
    return true;
}

// 0x 前缀的小写十六进制；按无符号位模式存进 int64。
bool parse_hex(std::string_view s, int64_t* out) {
    if (s.size() < 3 || s[0] != '0' || s[1] != 'x') return false;
    uint64_t v = 0;
    for (size_t i = 2; i < s.size(); ++i) {
        char c = s[i];
        uint64_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint64_t>(c - 'a') + 10;
        } else {
            return false;
        }
        if (v > (UINT64_MAX - d) / 16) return false;
        v = v * 16 + d;
    }
    *out = static_cast<int64_t>(v);
    return true;
}

void append_hex(uint64_t v, std::string& out) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, v, 16);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
}

// 十进制直写（to_chars，无 std::string 临时）。
void append_dec(int64_t v, std::string& out) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
}

void render_operand(const MOperand& op, std::string& out) {
    if (op.indirect) out += '*';
    switch (op.kind) {
        case MOperandKind::Reg:
            // 静态表查名；表外名字（reg==kMRegNone）text 兜底。
            if (op.reg != kMRegNone) out += mreg_name(op.reg);
            else out += op.text;
            break;
        case MOperandKind::Label:
        case MOperandKind::Sym:
        case MOperandKind::Raw:
            out += op.text;
            break;
        case MOperandKind::Imm:
            out += '$';
            if (op.imm_hex) {
                out += "0x";
                append_hex(static_cast<uint64_t>(op.imm), out);
            } else {
                append_dec(op.imm, out);
            }
            break;
        case MOperandKind::Mem:
            // 表外基址 / 符号位移：canonical text 兜底（字节不变）。
            if (op.reg == kMRegNone || op.has_sym) { out += op.text; break; }
            if (op.has_disp) append_dec(op.imm, out);
            out += '(';
            out += mreg_name(op.reg);
            if (op.index != kMRegNone) {
                out += ',';
                out += mreg_name(op.index);
                if (op.has_scale) {
                    out += ',';
                    append_dec(op.scale, out);
                }
            }
            out += ')';
            break;
    }
}

}  // namespace

// 枚举下标 → 名字：编译期从 kOpNames 生成，渲染期 O(1)。
constexpr auto kOpNameByEnum = [] {
    std::array<std::string_view, 128> a{};
    for (const NamedOp& n : kOpNames) a[static_cast<size_t>(n.op)] = n.name;
    return a;
}();

const char* mop_name(MOp op) {
    const size_t i = static_cast<size_t>(op);
    if (i >= kOpNameByEnum.size() || kOpNameByEnum[i].empty()) return "";
    return kOpNameByEnum[i].data();
}

MOp mop_from_name(std::string_view name) {
    if (name.empty()) return MOp::kRaw;
    const char c0 = name[0];
    for (const NamedOp& n : kOpNames) {
        if (n.name[0] != c0) continue;  // 首字符过滤，免多数 strlen
        if (name == n.name) return n.op;
    }
    return MOp::kRaw;
}

std::string_view mreg_name(MReg r) { return kRegNames[r]; }

// 名字打包成 uint64 键：低字节起逐字符（含 '%'），最高字节放长度。
// 寄存器名 ≤6 字符，7 字节恰好装下；键比较 = 整数比较。
constexpr uint64_t pack_reg(std::string_view s) {
    uint64_t k = s.size();
    for (size_t i = 0; i < s.size(); ++i)
        k |= static_cast<uint64_t>(static_cast<unsigned char>(s[i])) << (8 * (i + 1));
    return k;
}

// 编译期按键排序的查找表：运行期二分，6 次整数比较命中（免线性扫描
// 65 条字符串——emit2 动态路径每个寄存器操作数都要查，必须是 O(log n)）。
constexpr auto kRegByKey = [] {
    std::array<std::pair<uint64_t, MReg>, kMRegCount - 1> a{};
    for (size_t i = 1; i < kMRegCount; ++i)
        a[i - 1] = {pack_reg(kRegNames[i]), static_cast<MReg>(i)};
    std::sort(a.begin(), a.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    return a;
}();

MReg mreg_from_name(std::string_view name) {
    if (name.size() < 3 || name.size() > 6 || name[0] != '%') return kMRegNone;
    const uint64_t k = pack_reg(name);
    size_t lo = 0, hi = kRegByKey.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kRegByKey[mid].first < k) lo = mid + 1;
        else hi = mid;
    }
    if (lo < kRegByKey.size() && kRegByKey[lo].first == k) return kRegByKey[lo].second;
    return kMRegNone;
}

void MOperand::set_text(std::string_view s) {
    *this = MOperand{};
    if (!parse_operand(s, false, this)) {
        kind = MOperandKind::Raw;
        text = s;
    }
}

const MOperand& mreg_operand(MReg r) {
    static const std::array<MOperand, kMRegCount> tbl = [] {
        std::array<MOperand, kMRegCount> a;
        for (size_t i = 1; i < kMRegCount; ++i) {
            a[i].kind = MOperandKind::Reg;
            a[i].reg = static_cast<MReg>(i);
            a[i].text = mreg_name(static_cast<MReg>(i));
        }
        return a;
    }();
    return tbl[r];
}

bool mop_is_branch(MOp op) {
    switch (op) {
        case MOp::kJmp:
        case MOp::kJa:
        case MOp::kJae:
        case MOp::kJb:
        case MOp::kJbe:
        case MOp::kJe:
        case MOp::kJne:
        case MOp::kJg:
        case MOp::kJge:
        case MOp::kJl:
        case MOp::kJle:
        case MOp::kJs:
        case MOp::kJns:
        case MOp::kJp:
            return true;
        default:
            return false;
    }
}

bool mop_is_call(MOp op) { return op == MOp::kCall; }

bool parse_operand(std::string_view text, bool as_label, MOperand* out) {
    MOperand op;
    std::string_view s = text;
    if (s.empty()) return false;
    if (s[0] == '*') {
        op.indirect = true;
        s.remove_prefix(1);
        if (s.empty()) return false;
    }
    if (s[0] == '$') {
        if (op.indirect) return false;
        const std::string_view imm_text = s;  // canonical text 含 '$'
        s.remove_prefix(1);
        if (s.empty()) return false;
        op.kind = MOperandKind::Imm;
        if (s.size() > 2 && s[0] == '0' && s[1] == 'x') {
            op.imm_hex = true;
            if (!parse_hex(s, &op.imm)) return false;
        } else if (!parse_decimal(s, &op.imm)) {
            return false;
        }
        op.text = std::string(imm_text);
        *out = std::move(op);
        return true;
    }
    if (is_reg_name(s)) {
        op.kind = MOperandKind::Reg;
        op.reg = mreg_from_name(s);  // 表外名字 kMRegNone，渲染走 text 兜底
        op.text = std::string(s);
        *out = std::move(op);
        return true;
    }
    if (s.back() == ')') {
        size_t lp = s.find('(');
        if (lp == std::string_view::npos) return false;
        std::string_view disp = s.substr(0, lp);
        std::string_view inner = s.substr(lp + 1, s.size() - lp - 2);
        std::string_view parts[3];
        int np = 0;
        size_t start = 0;
        for (size_t i = 0; i <= inner.size(); ++i) {
            if (i == inner.size() || inner[i] == ',') {
                if (np == 3) return false;
                parts[np++] = inner.substr(start, i - start);
                start = i + 1;
            }
        }
        if (np < 1) return false;
        const MReg base = mreg_from_name(parts[0]);
        if (base == kMRegNone) return false;  // 表外基址退化 Raw，字节不变
        op.kind = MOperandKind::Mem;
        op.reg = base;
        if (np == 2) {
            const MReg ix = mreg_from_name(parts[1]);
            if (ix == kMRegNone) return false;
            op.index = ix;
            op.scale = 1;
            op.has_scale = false;
        } else if (np == 3) {
            const MReg ix = mreg_from_name(parts[1]);
            if (ix == kMRegNone) return false;
            int64_t sc = 0;
            if (!parse_decimal(parts[2], &sc)) return false;
            if (sc != 1 && sc != 2 && sc != 4 && sc != 8) return false;
            op.index = ix;
            op.scale = static_cast<int>(sc);
            op.has_scale = true;
        }
        if (!disp.empty()) {
            int64_t d = 0;
            if (parse_decimal(disp, &d)) {
                op.imm = d;
                op.has_disp = true;
            } else {
                // 符号位移（如 .Lstr0(%rip)）：标记 has_sym，渲染走 canonical text。
                for (char c : disp) {
                    if (c == ' ' || c == ',' || c == '(' || c == ')' || c == '$' || c == '*')
                        return false;
                }
                op.has_sym = true;
                op.has_disp = false;
            }
        }
        op.text = std::string(s);
        *out = std::move(op);
        return true;
    }
    // 裸符号 / 标签（jmp 目标、call 目标）。
    for (char c : s) {
        if (c == ' ' || c == ',' || c == '(' || c == ')' || c == '$' || c == '*') return false;
    }
    op.kind = as_label ? MOperandKind::Label : MOperandKind::Sym;
    op.text = std::string(s);
    *out = std::move(op);
    return true;
}

MInstr parse_instr(std::string_view body, bool* ok) {
    MInstr ins;
    *ok = false;
    size_t sp = body.find(' ');
    std::string_view mn = sp == std::string_view::npos ? body : body.substr(0, sp);
    MOp op = mop_from_name(mn);
    if (op == MOp::kRaw) return ins;
    ins.op = op;
    if (sp == std::string_view::npos) {
        *ok = true;
        return ins;
    }
    std::string_view rest = body.substr(sp + 1);
    if (rest.empty()) return ins;
    std::vector<std::string_view> raw_ops;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) return ins;
            --depth;
        } else if (c == ',' && depth == 0) {
            raw_ops.push_back(rest.substr(start, i - start));
            start = i + 1;
        }
    }
    if (depth != 0) return ins;
    raw_ops.push_back(rest.substr(start));
    const bool as_label = mop_is_branch(op);
    ins.ops.reserve(raw_ops.size());
    for (std::string_view ro : raw_ops) {
        if (!ins.ops.empty()) {
            // 原发射格式固定 ", " 分隔：除第一个外每个操作数前恰有一个空格。
            if (ro.empty() || ro[0] != ' ') return ins;
            ro.remove_prefix(1);
        }
        MOperand o;
        if (!parse_operand(ro, as_label, &o)) return ins;
        ins.ops.push_back(std::move(o));
    }
    *ok = true;
    return ins;
}

MItem make_label(std::string name, bool indented) {
    MItem it;
    it.kind = MItem::Label;
    it.indented = indented;
    it.text = std::move(name);
    return it;
}

MItem make_directive(std::string body) {
    MItem it;
    it.kind = MItem::Directive;
    it.text = std::move(body);
    return it;
}

MItem make_comment(std::string line) {
    MItem it;
    it.kind = MItem::Comment;
    it.text = std::move(line);
    return it;
}

MItem make_raw(std::string text) {
    MItem it;
    it.kind = MItem::Raw;
    it.text = std::move(text);
    return it;
}

void parse_line_into(std::string_view line, MachFunc& f) {
    if (line.empty()) { f.push(make_raw("\n")); return; }
    if (line[0] != '\t') {
        if (line.back() == ':') {
            f.push(make_label(std::string(line.substr(0, line.size() - 1)), false));
            return;
        }
        if (line[0] == '#') { f.push(make_comment(std::string(line))); return; }
        f.push(make_raw(std::string(line) + "\n"));
        return;
    }
    std::string_view body = line.substr(1);
    if (body.empty()) { f.push(make_raw(std::string(line) + "\n")); return; }
    if (body[0] == '#') { f.push(make_comment(std::string(line))); return; }
    size_t sp = body.find(' ');
    // 数据段的 "	.Lcname: .asciz \"...\"" 同行 label+指示符形式。
    if (sp != std::string_view::npos && sp > 0 && body[sp - 1] == ':') {
        f.push(make_directive(std::string(body)));
        return;
    }
    if (body.back() == ':') {
        f.push(make_label(std::string(body.substr(0, body.size() - 1)), true));
        return;
    }
    if (body[0] == '.') { f.push(make_directive(std::string(body))); return; }
    bool ok = false;
    MInstr ins = parse_instr(body, &ok);
    if (!ok) { f.push(make_raw(std::string(line) + "\n")); return; }
    f.push_instr(std::move(ins));
}

void MachFunc::clear() {
    items.clear();
    insts.clear();
    bytes = 0;
}

void MachFunc::push(MItem it) {
    switch (it.kind) {
        case MItem::Label:
            bytes += it.text.size() + 3 + (it.indented ? 1 : 0);
            break;
        case MItem::Directive:
            bytes += it.text.size() + 2;
            break;
        case MItem::Comment:
            bytes += it.text.size() + 1;
            break;
        case MItem::Raw:
            bytes += it.text.size();
            break;
        case MItem::Instr:
            bytes += 16;
            for (const MOperand& o : insts[static_cast<size_t>(it.inst)].ops)
                bytes += o.text.size() + 8;
            break;
    }
    items.push_back(std::move(it));
}

void MachFunc::push_instr(MInstr ins) {
    insts.push_back(std::move(ins));
    MItem it;
    it.kind = MItem::Instr;
    it.inst = static_cast<int32_t>(insts.size() - 1);
    push(std::move(it));
}

void MachFunc::push_cached(const MItem& it, MInstr ins) {
    insts.push_back(std::move(ins));
    MItem copy = it;
    copy.inst = static_cast<int32_t>(insts.size() - 1);
    push(std::move(copy));
}

void MachFunc::push_line(std::string_view line) { parse_line_into(line, *this); }

void MachFunc::push_directive(std::string_view body) { push(make_directive(std::string(body))); }

void MachFunc::push_label(std::string_view name, bool indented) {
    push(make_label(std::string(name), indented));
}

void MachFunc::append_raw(std::string_view s) {
    if (s.empty()) return;
    if (!items.empty() && items.back().kind == MItem::Raw) {
        items.back().text.append(s);
    } else {
        items.push_back(make_raw(std::string(s)));
    }
    bytes += s.size();
}

void MachFunc::append(const char* first, const char* last) {
    if (last <= first) return;
    append_raw(std::string_view(first, static_cast<size_t>(last - first)));
}

void render_item(const MItem& it, const MachFunc& f, std::string& out) {
    switch (it.kind) {
        case MItem::Label:
            if (it.indented) out += '\t';
            out += it.text;
            out += ":\n";
            break;
        case MItem::Directive:
            out += '\t';
            out += it.text;
            out += '\n';
            break;
        case MItem::Comment:
            out += it.text;
            out += '\n';
            break;
        case MItem::Raw:
            out += it.text;
            break;
        case MItem::Instr: {
            const MInstr& ins = f.insts[static_cast<size_t>(it.inst)];
            out += '\t';
            out += mop_name(ins.op);
            const MOpVec& ops = ins.ops;
            if (!ops.empty()) {
                out += ' ';
                for (size_t i = 0; i < ops.size(); ++i) {
                    if (i != 0) out += ", ";
                    render_operand(ops[i], out);
                }
            }
            out += '\n';
            break;
        }
    }
}

std::string render(const MachFunc& f) {
    std::string out;
    out.reserve(f.bytes + 64);
    for (const MItem& it : f.items) render_item(it, f, out);
    return out;
}

}  // namespace pyadamas
