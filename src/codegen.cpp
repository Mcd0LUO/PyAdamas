// codegen.cpp —— IR → x86-64 汇编
//
// 位置模型分三层，每层解决一个问题：
//
//   1. 变量（slot）→ 固定的 callee-saved 寄存器。热变量整函数常驻，
//      `x += 1` 不再是一次读内存 + 一次写内存。
//   2. 临时值（vreg）→ 块内别名。`%v = load.slot x` 在 %v 只被用一次
//      且同块时根本不生成指令，直接让 %v 指向 x 的寄存器。
//   3. 结果回填 → 融合。`%v = a + b` 后面紧跟 `store.slot x, %v` 时，
//      %v 就地写进 x 的寄存器，`store.slot` 整条消失。
//
// 三层叠起来，`x = x + y` 从「读 x、读 y、加、写 x」四条内存指令变成
// 一条 `addq %y, %x`。剩下的值仍然走栈，语义上和之前完全一致。
//
// 寄存器都是 callee-saved（rbx, r12-r15），调用不会破坏它们，所以跨
// 调用不需要额外 spill。浮点值不进寄存器（xmm 全是 caller-saved）。
//
// 值的机器表示：
//   int/bool/None → 64 位整数（None 就是 0）
//   float         → IEEE 双精度
//   str/list/tuple→ 堆指针
//   func          → 代码地址
#include "codegen.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <queue>
#include <unordered_map>

namespace py23 {
namespace {

const char* kIntArgRegs[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
constexpr int kMaxIntArgs = 6;
constexpr int kMaxFpArgs = 8;

// 可用来长期持有变量的寄存器。全部是 callee-saved：被调用方保证
// 返回时原样恢复，所以跨函数调用不需要把值挪回栈。
// rbp 是帧指针，rsp 是栈指针，都不在其中。
const char* kSlotRegs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
constexpr int kMaxSlotRegs = 5;

// 一条指令里临时用的寄存器。和 kSlotRegs 不相交，指令生成中途可以
// 随便写，不会踩到变量。

struct ArgLoc {
    bool fp = false;
    int reg = -1;       // 寄存器下标
    int stack_off = -1; // 相对 rsp 的偏移
};

std::string asm_escape(const std::string& s) {
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
    std::string gen(const IRModule& m) {
        m_ = &m;
        out_.clear();

        if (!m.strings.empty()) {
            // 字符串对象里有一个指向自身字符区的绝对地址。放在 .rodata
            // 会让 PIE 链接器报 "relocation in read-only section"，
            // 必须用可重定位的只读段。
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.strings.size(); ++i) {
                const std::string& s = m.strings[i];
                out_ += "\t.align 8\n";
                // 隐藏 RC 头（64 字节）：next/prev/rc/dtor/release/size/kind/marked。
                // 字面量 rc = INT64_MIN 永生，不进 GC 注册表。
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                out_ += std::format(".Lstr{}:\n", i);
                out_ += std::format("\t.quad {}\n", s.size());
                out_ += "\t.quad 0\n";
                out_ += std::format("\t.quad .Lstr{}_c\n", i);
                out_ += std::format(".Lstr{}_c:\n", i);
                out_ += std::format("\t.ascii \"{}\"\n", asm_escape(s));
                out_ += "\t.byte 0\n";
            }
        }

        if (!m.globals.empty()) {
            out_ += "\t.bss\n";
            out_ += "\t.align 8\n";
            for (size_t i = 0; i < m.globals.size(); ++i)
                out_ += std::format(".Lg{}:\n\t.zero 8\n", i);

            // GC 根表：所有全局槽的地址。类型 unknown 的全局也可能存堆指针，
            // 所以不做类型过滤，运行时会按地址查注册表。
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            out_ += "\t.align 8\n";
            out_ += ".Lgroots:\n";
            for (size_t i = 0; i < m.globals.size(); ++i)
                out_ += std::format("\t.quad .Lg{}\n", i);
        } else {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            out_ += "\t.align 8\n.Lgroots:\n";
        }

        // 每个类一张静态 vtable：槽里是方法代码地址。对象头第一个
        // 字段指向它，虚调用就是「取表、取槽、间接跳转」。
        if (!m.classes.empty()) {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.classes.size(); ++i) {
                out_ += "\t.align 8\n";
                out_ += std::format(".Lvt{}:\n", i);
                if (m.classes[i].vtable.empty()) out_ += "\t.quad 0\n";
                for (int f : m.classes[i].vtable)
                    out_ += f < 0 ? "\t.quad 0\n" : std::format("\t.quad {}\n", func_label(f));
            }
        }

        // 每个函数一个静态闭包（code + 0 个捕获），用作无捕获函数值。
        // 布局和堆闭包一致：+0 code，+8 n_caps，+16 起是 cell。
        if (!m.funcs.empty()) {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                out_ += "\t.align 8\n";
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                out_ += std::format(".Lclos{}:\n", i);
                out_ += std::format("\t.quad {}\n", func_label(static_cast<int>(i)));
                out_ += "\t.quad 0\n";
            }
        }

        out_ += "\t.text\n";

        // 每个有堆字段的类生成一个析构器，对象 rc 归零时由运行时回调。
        // 只 dec 堆字段，字段偏移与对象布局一致（父类字段在前）。
        for (size_t i = 0; i < m.classes.size(); ++i) {
            const IRClass& c = m.classes[i];
            bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
            if (!has_heap) continue;
            out_ += "\n# ---- dtor class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Ldtor{}:\n", i);
            out_ += "\tpushq %rbx\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += "\tcall py23_rc_dec\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\tret\n";

            // GC 清扫用：只把指向可达对象的字段引用减掉，不释放任何东西。
            out_ += "\n# ---- gc release class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgcrel{}:\n", i);
            out_ += "\tpushq %rbx\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += "\tcall py23_gc_unref_child\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\tret\n";
        }

        for (size_t i = 0; i < m.funcs.size(); ++i) emit_function(*m.funcs[i], static_cast<int>(i));

        // 真正的入口：调用降级出来的 __py23_main，返回它的返回值
        out_ += "\t.globl main\n";
        out_ += "\t.type main, @function\n";
        out_ += "main:\n";
        out_ += "\tpushq %rbp\n";
        out_ += "\tmovq %rsp, %rbp\n";
        // GC 根表 + 栈顶。栈顶取 main 的 rsp，收集器向下扫描到当前帧。
        out_ += "\tleaq .Lgroots(%rip), %rdi\n";
        out_ += std::format("\tmovq ${}, %rsi\n", m.globals.size());
        out_ += "\tmovq %rsp, %rdx\n";
        out_ += "\tcall py23_gc_init\n";
        if (m.entry >= 0) out_ += std::format("\tcall {}\n", func_label(m.entry));
        out_ += "\tpopq %rbp\n";
        out_ += "\tret\n";
        out_ += "\t.section .note.GNU-stack,\"\",@progbits\n";

        return std::move(out_);
    }

private:
    const IRModule* m_ = nullptr;
    const IRFunction* f_ = nullptr;
    int fi_ = 0;
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
    std::unordered_map<int, int> block_first_store_;
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

    // 变量的现在位置：寄存器（提升过）或栈槽。
    std::string slot_op(int slot) const {
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
    void frame_size(int total_slots) {
        int base = 8 * (n_saved_regs_ + total_slots);
        int aligned = (base + 15) & ~15;
        int pad = aligned - 8 * n_saved_regs_;
        if (pad > 0) line(std::format("subq ${}, %rsp", pad));
    }

    // 尾声：先把 rsp 退回 push 的寄存器下面，逐个弹回，再弹 rbp。
    // 不能用 `movq %rbp, %rsp`——那会把 callee-saved 的槽一起丢掉。
    void emit_epilogue() {
        if (n_saved_regs_ > 0) line(std::format("leaq {}(%rbp), %rsp", -8 * n_saved_regs_));
        else line("movq %rbp, %rsp");
        for (int i = n_saved_regs_ - 1; i >= 0; --i)
            line(std::format("popq {}", kSlotRegs[i]));
        line("popq %rbp");
        line("ret");
    }

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
    void assign_vreg_slots() {
        size_t nv = f_->vreg_ty.size();
        vreg_slot_.assign(nv, 0);
        n_vreg_slots_ = 0;
        use_count_.assign(nv, 0);
        cross_block_.assign(nv, false);
        def_block_.assign(nv, -1);
        if (nv == 0) return;

        constexpr int kInf = INT32_MAX;
        std::vector<int> def_pos(nv, -1);
        std::vector<int> first_use(nv, kInf), last_use(nv, -1);

        auto touch = [&](VReg r, int bi) {
            if (r == kNoReg || r >= nv) return;
            ++use_count_[r];
            if (pos_ < first_use[r]) first_use[r] = pos_;
            last_use[r] = pos_;
            // 定义在别的块，或者干脆还没定义（前向引用）→ 跨块
            if (def_block_[r] != bi) cross_block_[r] = true;
        };

        pos_ = 0;
        int bi = 0;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts) {
                if (in.a != kNoReg) touch(in.a, bi);
                if (in.b != kNoReg) touch(in.b, bi);
                if (in.c != kNoReg) touch(in.c, bi);
                for (VReg r : in.args) touch(r, bi);
                if (in.dst != kNoReg && in.dst < nv) {
                    def_pos[in.dst] = pos_;
                    def_block_[in.dst] = bi;
                }
                ++pos_;
            }
            ++bi;
        }
        const int total_pos = pos_;

        struct Range {
            int start, end, vreg;
        };
        std::vector<Range> ranges;
        ranges.reserve(nv);
        for (size_t v = 0; v < nv; ++v) {
            int d = def_pos[v];
            int fu = first_use[v];
            int lu = last_use[v];
            if (d < 0 && fu == kInf) continue;  // 既没定义也没使用
            if (cross_block_[v]) {
                // 独占：区间覆盖整个函数，不会与任何人重叠
                ranges.push_back({0, total_pos + 1, static_cast<int>(v)});
                continue;
            }
            int start = (d < 0) ? fu : (fu == kInf ? d : std::min(d, fu));
            int end = std::max(d, lu);
            ranges.push_back({start, end, static_cast<int>(v)});
        }
        std::ranges::sort(ranges, [](const Range& a, const Range& b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        });

        // 按结束位置维护活跃区间，释放已经过期的槽
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>,
                            std::greater<>>
            active;
        std::vector<int> free_slots;
        for (const auto& r : ranges) {
            while (!active.empty() && active.top().first < r.start) {
                free_slots.push_back(active.top().second);
                active.pop();
            }
            int s;
            if (!free_slots.empty()) {
                s = free_slots.back();
                free_slots.pop_back();
            } else {
                s = n_vreg_slots_++;
            }
            vreg_slot_[r.vreg] = s;
            active.push({r.end, s});
        }
    }

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
    void assign_slot_regs() {
        int ns = static_cast<int>(f_->slot_ty.size());
        slot_reg_.assign(ns, std::string());
        n_saved_regs_ = 0;
        if (ns == 0) return;
        // 含 setjmp 的函数：longjmp 会把 callee-saved 寄存器恢复到 setjmp
        // 时刻，寄存器里的变量会悄悄回退。这类函数全部走内存槽。
        if (f_->has_handlers) return;

        std::vector<int> score(ns, 0);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if ((in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot) && in.idx >= 0 &&
                    in.idx < ns && !ir_is_float(f_->slot_ty[in.idx]))
                    ++score[in.idx];
            }

        std::vector<std::pair<int, int>> hot;  // {次数, 槽}
        for (int i = 0; i < ns; ++i)
            if (score[i] > 0) hot.push_back({score[i], i});
        std::ranges::sort(hot, [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });

        int k = std::min<int>(kMaxSlotRegs, static_cast<int>(hot.size()));
        for (int i = 0; i < k; ++i) {
            slot_reg_[hot[i].second] = kSlotRegs[i];
            n_saved_regs_ = i + 1;
        }
    }

    // ── 函数 ────────────────────────────────────────────────────

    std::vector<ArgLoc> classify(const std::vector<TypePtr>& types) const {
        std::vector<ArgLoc> locs(types.size());
        int gi = 0, fi = 0, sp = 0;
        for (size_t i = 0; i < types.size(); ++i) {
            bool fp = ir_is_float(types[i]);
            if (fp && fi < kMaxFpArgs) {
                locs[i].fp = true;
                locs[i].reg = fi++;
            } else if (!fp && gi < kMaxIntArgs) {
                locs[i].fp = false;
                locs[i].reg = gi++;
            } else {
                locs[i].fp = fp;
                locs[i].stack_off = sp++ * 8;
            }
        }
        return locs;
    }

    void emit_function(const IRFunction& f, int fi) {
        f_ = &f;
        fi_ = fi;
        n_slots_ = static_cast<int>(f.slot_ty.size());
        assign_vreg_slots();
        assign_slot_regs();

        out_ += std::format("\n# ---- {} ({}) ----\n", f.name, type_name(f.ret));
        label(func_label(fi));
        line("pushq %rbp");
        line("movq %rsp, %rbp");
        for (int i = 0; i < n_saved_regs_; ++i) line(std::format("pushq {}", kSlotRegs[i]));
        frame_size(n_slots_ + n_vreg_slots_);

        // 非参数变量清零。`x: int` 这种纯声明在 lower 里不生成任何指令，
        // 靠的是「槽初值为 0」，而栈帧里的内存是脏的——不显式清掉就会
        // 读到上一个函数留下的垃圾。参数槽下面马上被实参覆盖，跳过。
        {
            std::vector<bool> is_param(static_cast<size_t>(n_slots_), false);
            for (const auto& p : f.params)
                if (p.slot >= 0 && p.slot < n_slots_) is_param[p.slot] = true;
            for (int s = 0; s < n_slots_; ++s)
                if (!is_param[s]) line(std::format("movq $0, {}", slot_op(s)));
        }

        // 形参落位
        std::vector<TypePtr> ptypes;
        ptypes.reserve(f.params.size());
        for (auto& p : f.params) ptypes.push_back(p.ty);
        auto locs = classify(ptypes);
        for (size_t i = 0; i < f.params.size(); ++i) {
            const auto& p = f.params[i];
            const auto& l = locs[i];
            if (l.stack_off >= 0) {
                line(std::format("movq {}(%rbp), %rax", 16 + l.stack_off));
                move_to(slot_op(p.slot), "%rax");
            } else if (l.fp) {
                line(std::format("movsd %xmm{}, {}", l.reg, smem(p.slot)));
            } else {
                move_to(slot_op(p.slot), kIntArgRegs[l.reg]);
            }
        }

        // 有捕获的函数从 %r10 拿闭包环境；调用方在 call 前设置。
        if (f.env_slot >= 0) line(std::format("movq %r10, {}", slot_op(f.env_slot)));

        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            label(block_label(static_cast<int>(bi)));
            gen_block(f.blocks[bi]);
        }

        bool empty_func = f.blocks.empty() || f.blocks.back().insts.empty();
        if (empty_func || !f.blocks.back().terminated()) {
            // 正常路径不会走到这里，除非 lower 漏了终止指令
            line("xorl %eax, %eax");
            emit_epilogue();
        }
    }

    // ── 基本块 ──────────────────────────────────────────────────
    //
    // 别名只在块内有效，块边界一律清空——跨块的值永远走栈槽，所以
    // 不需要在块之间传播任何状态，也就不可能读到过期的寄存器。

    bool can_fuse(const Inst& in, const Inst& nx) const {
        if (in.dst == kNoReg) return false;
        if (in.op == IrOp::StoreSlot || in.op == IrOp::StoreGlobal) return false;
        if (nx.op != IrOp::StoreSlot || nx.a != in.dst) return false;
        if (nx.idx < 0 || nx.idx >= static_cast<int>(slot_reg_.size())) return false;
        if (slot_reg_[nx.idx].empty()) return false;
        if (in.dst >= use_count_.size()) return false;
        // 只有「定义后在本块里正好被用一次」才能就地写变量槽。
        // 用两次就不行了：第二次读到的会是新值。
        return use_count_[in.dst] == 1 && !cross_block_[in.dst];
    }

    bool can_fuse_branch(const Inst& in, const Inst& nx) const {
        if (in.op != IrOp::Cmp || nx.op != IrOp::Branch) return false;
        if (in.dst == kNoReg || nx.a != in.dst) return false;
        if (!single_use_in_block(in.dst)) return false;
        // 浮点要走 setnp 处理 NaN，字符串走三路比较，都直接吃掉标志位，
        // 没有能直接用的条件跳转。
        TypePtr t = ty_of(in.a);
        if (ir_is_float(t)) return false;
        if (t && t->kind == TyKind::Str) return false;
        return true;
    }

    // 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
    void gen_cmp_branch(const Inst& in, const Inst& br) {
        comment(std::format("{} -> {}", in.text(), br.text()));
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        bool sa_mem = is_mem_operand(sa);
        bool sb_mem = is_mem_operand(sb);
        if (sa_mem && sb_mem) {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        } else if (sa_mem || sa[0] == '%') {
            line(std::format("cmpq {}, {}", sb, sa));
        } else {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        }
        const char* jcc = "je";
        switch (in.cop) {
            case CmpKind::Lt: jcc = "jl"; break;
            case CmpKind::Le: jcc = "jle"; break;
            case CmpKind::Gt: jcc = "jg"; break;
            case CmpKind::Ge: jcc = "jge"; break;
            case CmpKind::Eq: jcc = "je"; break;
            case CmpKind::Ne: jcc = "jne"; break;
        }
        line(std::format("{} {}", jcc, block_label(br.target)));
        line(std::format("jmp {}", block_label(br.target2)));
    }

    void gen_block(const Block& b) {
        alias_.clear();
        const_val_.clear();

        // 预扫描：每个槽在本块内第一次被 store 的位置，以及每个 vreg
        // 在本块内第一次被读的位置。LoadSlot 会把临时值直接绑到变量
        // 寄存器上，这个绑定只在「读之后、写之前」成立，所以要先确认
        // 中间没有对同一变量的赋值。
        block_first_store_.clear();
        block_first_use_.clear();
        for (int i = 0; i < static_cast<int>(b.insts.size()); ++i) {
            const Inst& in = b.insts[i];
            if (in.op == IrOp::StoreSlot && !block_first_store_.contains(in.idx))
                block_first_store_[in.idx] = i;
            VReg ops[3] = {in.a, in.b, in.c};
            for (VReg r : ops)
                if (r != kNoReg && !block_first_use_.contains(r)) block_first_use_[r] = i;
            for (VReg r : in.args)
                if (!block_first_use_.contains(r)) block_first_use_[r] = i;
        }

        size_t i = 0;
        while (i < b.insts.size()) {
            const Inst& in = b.insts[i];
            dst_loc_.clear();

            cur_pos_ = static_cast<int>(i);

            // 比较 + 分支连在一起时不需要把布尔值真的算出来
            if (i + 1 < b.insts.size() && can_fuse_branch(in, b.insts[i + 1])) {
                dst_loc_.clear();
                gen_cmp_branch(in, b.insts[i + 1]);
                i += 2;
                continue;
            }

            int fused_slot = -1;
            if (i + 1 < b.insts.size() && can_fuse(in, b.insts[i + 1])) {
                fused_slot = b.insts[i + 1].idx;
                dst_loc_ = slot_reg_[fused_slot];
            }

            // 常量干脆不生成指令，让使用者把它当立即数。`s += 1` 因此
            // 是 `addq $1, %rbx`，而不是「先 movq $1 到栈上再读回来」。
            // 只处理 32 位范围内的整数：x86 的 movq 立即数只有
            // 符号扩展的 32 位，更大的必须走 movabsq。
            if (fused_slot < 0 && is_deferable_const(in) && single_use_in_block(in.dst)) {
                const_val_[in.dst] = in.imm;
                ++i;
                continue;
            }

            gen_inst(in);
            dst_loc_.clear();

            if (fused_slot >= 0) {
                // 融合掉的 store.slot 仍然改了变量，指向它的别名要失效
                invalidate_slot(fused_slot);
                ++i;
            }
            ++i;
        }
    }

    bool single_use_in_block(VReg v) const {
        return v != kNoReg && v < use_count_.size() && use_count_[v] == 1 && !cross_block_[v];
    }

    bool is_deferable_const(const Inst& in) const {
        if (in.dst == kNoReg) return false;
        if (in.op == IrOp::ConstNone) return true;
        if (in.op != IrOp::ConstInt) return false;
        return in.imm >= INT32_MIN && in.imm <= INT32_MAX;
    }

    void invalidate_slot(int slot) {
        if (slot < 0 || slot >= static_cast<int>(slot_reg_.size())) return;
        const std::string& r = slot_reg_[slot];
        if (r.empty()) return;
        for (auto it = alias_.begin(); it != alias_.end();)
            it = (it->second == r) ? alias_.erase(it) : std::next(it);
    }

    // ── 指令 ────────────────────────────────────────────────────

    void gen_inst(const Inst& in) {
        comment(in.text());
        switch (in.op) {
            case IrOp::ConstInt: gen_const_int(in); break;
            case IrOp::ConstFloat: gen_const_float(in); break;
            case IrOp::ConstStr:
                line(std::format("leaq .Lstr{}(%rip), %rax", in.idx));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ConstNone:
                move_to(dst_op(in.dst), "$0");
                break;
            case IrOp::FuncAddr:
                line(std::format("leaq .Lclos{}(%rip), %rax", in.imm));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::FuncCode:
                line(std::format("leaq {}(%rip), %rax", func_label(static_cast<int>(in.imm))));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::Mov:
                move_to(dst_op(in.dst), src_op(in.a));
                break;
            case IrOp::Bin: gen_bin(in); break;
            case IrOp::Un: gen_un(in); break;
            case IrOp::Cmp: gen_cmp(in); break;
            case IrOp::Convert: gen_convert(in); break;
            case IrOp::LoadSlot: gen_load_slot(in); break;
            case IrOp::StoreSlot:
                move_to(slot_op(in.idx), src_op(in.a));
                invalidate_slot(in.idx);
                break;
            case IrOp::LoadGlobal:
                line(std::format("movq .Lg{}(%rip), %rax", in.idx));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::StoreGlobal:
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, .Lg{}(%rip)", in.idx));
                break;
            case IrOp::MakeList: gen_make_list(in); break;
            case IrOp::MakeTuple: gen_make_list(in); break;
            case IrOp::IndexLoad: gen_index_load(in); break;
            case IrOp::IndexStore: gen_index_store(in); break;
            case IrOp::Len:
                line(std::format("movq {}, %rax", src_op(in.a)));
                line("movq (%rax), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ObjNew: gen_obj_new(in); break;
            case IrOp::FieldLoad: gen_field_load(in); break;
            case IrOp::FieldStore: gen_field_store(in); break;
            case IrOp::VCall: gen_vcall(in); break;
            case IrOp::Call: gen_call(in); break;
            case IrOp::CallRt: gen_call_rt(in); break;
            case IrOp::Setjmp: gen_setjmp(in); break;
            case IrOp::Jump: line(std::format("jmp {}", block_label(in.target))); break;
            case IrOp::Branch: gen_branch(in); break;
            case IrOp::Return: gen_return(in); break;
        }
    }

    // x = y 这种加载能不能省掉，看两件事：
    //   1. 变量 x 在寄存器里（否则读出来就得写回一个栈槽，省不了）
    //   2. 这个临时值只被读一次，且读的时候中间没有对 x 的赋值
    // 都满足时，临时值直接指向 x 的寄存器，整条指令消失。
    void gen_load_slot(const Inst& in) {
        std::string sop = slot_op(in.idx);
        // 融合目标非空时**必须**真的写过去，不能走别名——别名只是
        // 让 %v 指向变量的寄存器，而 `store.slot s, %v` 已经被吞掉，
        // 变量的寄存器不会有任何变化，值就悄悄丢了。
        if (dst_loc_.empty() && sop[0] == '%' && in.dst != kNoReg && in.dst < use_count_.size() &&
            use_count_[in.dst] == 1 && !cross_block_[in.dst]) {
            // 只有「本条 load 之后、%v 第一次被读之前」的赋值才会
            // 覆盖寄存器里的值。更早的赋值早就生效了，不算冲突。
            auto st = block_first_store_.find(in.idx);
            auto us = block_first_use_.find(in.dst);
            bool clobbered = st != block_first_store_.end() && us != block_first_use_.end() &&
                             st->second > cur_pos_ && st->second < us->second;
            if (!clobbered) {
                alias_[in.dst] = sop;
                return;
            }
        }
        move_to(dst_op(in.dst), sop);
    }

    void gen_const_int(const Inst& in) {
        std::string d = dst_op(in.dst);
        if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) {
            line(std::format("movq ${}, {}", in.imm, d));
        } else {
            line(std::format("movabsq ${}, %rax", in.imm));
            line(std::format("movq %rax, {}", d));
        }
    }

    void gen_const_float(const Inst& in) {
        uint64_t bits = 0;
        std::memcpy(&bits, &in.fimm, sizeof(bits));
        line(std::format("movabsq $0x{:x}, %rax", bits));
        line("movq %rax, %xmm0");
        line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
    }

    void gen_bin(const Inst& in) {
        TypePtr ty = in.ty;
        if (ty && ty->kind == TyKind::Str) {
            if (in.bop != BinKind::Add) {
                line("# unsupported string operator");
                return;
            }
            line(std::format("movq {}, %rdi", src_op(in.a)));
            line(std::format("movq {}, %rsi", src_op(in.b)));
            line("call py23_str_concat");
            move_to(dst_op(in.dst), "%rax");
            return;
        }
        if (ir_is_float(ty)) {
            gen_bin_fp(in);
        } else {
            gen_bin_int(in);
        }
    }

    void gen_bin_int(const Inst& in) {
        const std::string sa = src_op(in.a);
        const std::string sb = src_op(in.b);
        std::string d = dst_op(in.dst);

        // 目标位置恰好装着左操作数时，可以就地算：`x = x + y` 变成
        // 一条 `addq y, x`。这是变量提升之后最常见的形状，省掉
        // 「搬到 rax、算、搬回去」两条指令。
        if (d[0] == '%' && sa == d) {
            switch (in.bop) {
                case BinKind::Add: line(std::format("addq {}, {}", sb, d)); return;
                case BinKind::Sub: line(std::format("subq {}, {}", sb, d)); return;
                case BinKind::Mul: line(std::format("imulq {}, {}", sb, d)); return;
                case BinKind::BitAnd: line(std::format("andq {}, {}", sb, d)); return;
                case BinKind::BitOr: line(std::format("orq {}, {}", sb, d)); return;
                case BinKind::BitXor: line(std::format("xorq {}, {}", sb, d)); return;
                case BinKind::Shl:
                    line(std::format("movq {}, %rcx", sb));
                    line(std::format("shlq %cl, {}", d));
                    return;
                case BinKind::Shr:
                    line(std::format("movq {}, %rcx", sb));
                    line(std::format("sarq %cl, {}", d));
                    return;
                default: break;  // 除法/取模/幂要用 rax/rdx，走通用路径
            }
        }

        line(std::format("movq {}, %rax", sa));
        switch (in.bop) {
            case BinKind::Add: line(std::format("addq {}, %rax", sb)); break;
            case BinKind::Sub: line(std::format("subq {}, %rax", sb)); break;
            case BinKind::Mul: line(std::format("imulq {}, %rax", sb)); break;
            case BinKind::BitAnd: line(std::format("andq {}, %rax", sb)); break;
            case BinKind::BitOr: line(std::format("orq {}, %rax", sb)); break;
            case BinKind::BitXor: line(std::format("xorq {}, %rax", sb)); break;
            case BinKind::Shl:
                line(std::format("movq {}, %rcx", sb));
                line("shlq %cl, %rax");
                break;
            case BinKind::Shr:
                line(std::format("movq {}, %rcx", sb));
                line("sarq %cl, %rax");
                break;
            case BinKind::Div:
                line(std::format("movq {}, %rcx", sb));
                gen_div_guard();
                line("cqto");
                line("idivq %rcx");
                break;
            case BinKind::Mod: gen_mod(in); break;
            case BinKind::FloorDiv: gen_floordiv(in); break;
            case BinKind::Pow:
                line(std::format("movq {}, %rdi", sa));
                line(std::format("movq {}, %rsi", sb));
                line("call py23_pow_i64");
                break;
        }
        move_to(d, "%rax");
    }

    // idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。
    void gen_div_guard() {
        std::string ok = fresh_label("divok");
        std::string ok2 = fresh_label("divok2");
        line("testq %rcx, %rcx");
        line(std::format("jne {}", ok));
        line("call py23_panic_div0");
        label(ok);
        line("cmpq $-1, %rcx");
        line(std::format("jne {}", ok2));
        line("movabsq $0x8000000000000000, %r8");
        line("cmpq %r8, %rax");
        line(std::format("jne {}", ok2));
        line("call py23_panic_overflow");
        label(ok2);
    }

    // x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
    void gen_floordiv(const Inst& in) {
        std::string done = fresh_label("fdiv");
        line(std::format("movq {}, %rcx", src_op(in.b)));
        gen_div_guard();
        line("cqto");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        line(std::format("je {}", done));
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        line(std::format("jns {}", done));
        line("decq %rax");
        label(done);
    }

    // Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
    void gen_mod(const Inst& in) {
        std::string done = fresh_label("mod");
        line(std::format("movq {}, %rcx", src_op(in.b)));
        gen_div_guard();
        line("cqto");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        line(std::format("je {}", done));
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        line(std::format("jns {}", done));
        line("addq %rcx, %rdx");
        label(done);
        line("movq %rdx, %rax");
    }

    void gen_bin_fp(const Inst& in) {
        line(std::format("movsd {}, %xmm0", src_op(in.a)));
        switch (in.bop) {
            case BinKind::Add: line(std::format("addsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Sub: line(std::format("subsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Mul: line(std::format("mulsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Div: line(std::format("divsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::FloorDiv:
                // floor(a / b)：先除再向下取整。roundsd 要 SSE4.1，
                // 为了兼容性走 libm。
                line(std::format("divsd {}, %xmm0", src_op(in.b)));
                line("call floor");
                break;
            case BinKind::Mod: {
                // C 的 fmod 余数与被除数同号，Python 的 % 与除数同号，
                // 两者在异号时差一个 |b|，这里补上。xmm1 是调用者保存的，
                // fmod 回来必须重新装 b。余数为 0 时 Python 取除数的符号，
                // 所以 -0.0 % 3.0 是 0.0 而不是 -0.0。
                std::string done = fresh_label("fmod");
                std::string nz = fresh_label("fmodnz");
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                line("call fmod");
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                line("xorpd %xmm2, %xmm2");
                line("ucomisd %xmm2, %xmm0");
                line(std::format("jp {}", done));  // NaN 原样返回
                line(std::format("jne {}", nz));
                line("movq %xmm1, %rdx");
                line("movabsq $0x8000000000000000, %rax");
                line("andq %rdx, %rax");
                line("movq %rax, %xmm0");  // ±0.0，符号跟除数
                line(std::format("jmp {}", done));
                label(nz);
                line("movq %xmm0, %rax");
                line("movq %xmm1, %rdx");
                line("xorq %rdx, %rax");
                line(std::format("jns {}", done));  // 同号，无需修正
                line("addsd %xmm1, %xmm0");
                label(done);
                break;
            }
            case BinKind::Pow:
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                line("call pow");
                break;
            default:
                line("# unsupported float operator");
                break;
        }
        line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
    }

    void gen_un(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (in.uop == UnKind::Not) {
            emit_truth(in.a, "%al");
            line("xorb $1, %al");
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (ir_is_float(ty)) {
            if (in.uop == UnKind::Neg) {
                line(std::format("movsd {}, %xmm0", src_op(in.a)));
                line("movabsq $0x8000000000000000, %rax");
                line("movq %rax, %xmm1");
                line("xorpd %xmm1, %xmm0");
                line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            }
            return;
        }
        line(std::format("movq {}, %rax", src_op(in.a)));
        switch (in.uop) {
            case UnKind::Neg: line("negq %rax"); break;
            case UnKind::BitNot: line("notq %rax"); break;
            default: break;
        }
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

    // 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
    void emit_truth(VReg r, const char* byte_reg) {
        (void)byte_reg;
        TypePtr ty = ty_of(r);
        if (ir_is_float(ty)) {
            line(std::format("movsd {}, %xmm0", src_op(r)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            line("setne %al");
            line("setp %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            line(std::format("movq {}, %rax", src_op(r)));
            line("cmpq $0, (%rax)");
            line("setne %al");
            return;
        }
        line(std::format("cmpq $0, {}", src_op(r)));
        line("setne %al");
    }

    void gen_cmp(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ty && ty->kind == TyKind::Str) {
            line(std::format("movq {}, %rdi", src_op(in.a)));
            line(std::format("movq {}, %rsi", src_op(in.b)));
            if (in.cop == CmpKind::Eq || in.cop == CmpKind::Ne) {
                line("call py23_str_eq");
                if (in.cop == CmpKind::Ne) line("xorq $1, %rax");
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            // 大小比较走三路比较。早先这里只留了一句注释、什么都不生成，
            // 于是 `a < b` 静默拿到槽里的残留值——恒为 False。
            line("call py23_str_cmp");
            line("cmpq $0, %rax");
            switch (in.cop) {
                case CmpKind::Lt: line("setl %al"); break;
                case CmpKind::Le: line("setle %al"); break;
                case CmpKind::Gt: line("setg %al"); break;
                case CmpKind::Ge: line("setge %al"); break;
                default: break;
            }
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (ir_is_float(ty)) {
            gen_cmp_fp(in);
            return;
        }
        // cmp 不写坏操作数，所以左操作数已经在寄存器/栈上时不必先搬进
        // rax。但有两个限制：cmp 不能两个操作数都在内存；立即数只能
        // 放在第一操作数，直接写会把减法方向搞反。
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        bool sa_mem = is_mem_operand(sa);
        bool sb_mem = is_mem_operand(sb);
        if (sa_mem && sb_mem) {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        } else if (sa_mem || sa[0] == '%') {
            line(std::format("cmpq {}, {}", sb, sa));
        } else {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        }
        switch (in.cop) {
            case CmpKind::Lt: line("setl %al"); break;
            case CmpKind::Le: line("setle %al"); break;
            case CmpKind::Gt: line("setg %al"); break;
            case CmpKind::Ge: line("setge %al"); break;
            case CmpKind::Eq: line("sete %al"); break;
            case CmpKind::Ne: line("setne %al"); break;
        }
        line("movzbq %al, %rax");
        move_to(dst_op(in.dst), "%rax");
    }

    void gen_cmp_fp(const Inst& in) {
        line(std::format("movsd {}, %xmm0", src_op(in.a)));
        line(std::format("ucomisd {}, %xmm0", src_op(in.b)));
        switch (in.cop) {
            case CmpKind::Lt:
                line("setb %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Le:
                line("setbe %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Gt:
                line("seta %al");
                break;
            case CmpKind::Ge:
                line("setae %al");
                break;
            case CmpKind::Eq:
                line("sete %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Ne:
                line("setne %al");
                line("setp %cl");
                line("orb %cl, %al");
                break;
        }
        line("movzbq %al, %rax");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

    void gen_convert(const Inst& in) {
        TypePtr from = ty_of(in.a);
        TypePtr to = in.ty;
        if (!to) return;
        if (to->kind == TyKind::Bool) {
            emit_truth(in.a, "%al");
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (to->kind == TyKind::Float) {
            if (ir_is_float(from)) {
                // 已经是 double，只搬一次
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            // cvtsi2sd 的源必须是寄存器或内存，立即数不能直接转
            std::string sa = src_op(in.a);
            if (!is_mem_operand(sa) && sa[0] != '%') {
                line(std::format("movq {}, %rax", sa));
                sa = "%rax";
            }
            line(std::format("cvtsi2sdq {}, %xmm0", sa));
            line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            return;
        }
        if (to->kind == TyKind::Int) {
            if (ir_is_float(from)) {
                line(std::format("cvttsd2siq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
            } else {
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
            }
            return;
        }
        if (to->kind == TyKind::Str) {
            // 源类型已经是 str：什么都不用做，只补一条搬运
            if (from && from->kind == TyKind::Str) {
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            if (ir_is_float(from)) {
                line(std::format("movsd {}, %xmm0", src_op(in.a)));
                line("call py23_str_from_f64");
            } else if (from && from->kind == TyKind::Bool) {
                line(std::format("movq {}, %rdi", src_op(in.a)));
                line("call py23_str_from_bool");
            } else if (from && from->kind == TyKind::None) {
                line("call py23_str_from_none");
            } else {
                line(std::format("movq {}, %rdi", src_op(in.a)));
                line("call py23_str_from_i64");
            }
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
    }

    void gen_make_list(const Inst& in) {
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        line(std::format("movq ${}, %rdi", in.args.size()));
        line(std::format("movq ${}, %rsi", in.imm));
        line("call py23_list_new");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
        for (VReg a : in.args) {
            line(std::format("movq {}, %rdi", dst_op(in.dst)));
            line(std::format("movq {}, %rsi", src_op(a)));
            line("call py23_list_append");
        }
    }

    void gen_index_load(const Inst& in) {
        TypePtr ot = ty_of(in.a);
        bool is_str = ot && ot->kind == TyKind::Str;
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq {}, %rsi", src_op(in.b)));
        line(is_str ? "call py23_str_char_at" : "call py23_list_get");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

    void gen_index_store(const Inst& in) {
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq {}, %rsi", src_op(in.b)));
        line(std::format("movq {}, %rdx", src_op(in.c)));
        line("call py23_list_set");
    }

    // ── 对象 ────────────────────────────────────────────────────

    // 对象布局：+0 是 vtable 指针，字段从 +8 开始，每个 8 字节。
    // 属性访问因此是一条固定偏移的 movq，没有查表、没有哈希。
    void gen_obj_new(const Inst& in) {
        const IRClass& c = m_->classes[static_cast<size_t>(in.imm)];
        line(std::format("movq ${}, %rdi", 8 * (c.n_fields + 1)));
        bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
        if (has_heap) {
            line(std::format("leaq .Ldtor{}(%rip), %rsi", in.imm));
            line(std::format("leaq .Lgcrel{}(%rip), %rdx", in.imm));
        } else {
            line("xorl %esi, %esi");
            line("xorl %edx, %edx");
        }
        line("call py23_obj_new");
        line(std::format("leaq .Lvt{}(%rip), %rcx", in.imm));
        line("movq %rcx, (%rax)");
        move_to(dst_op(in.dst), "%rax");
    }

    void gen_field_load(const Inst& in) {
        line(std::format("movq {}, %rax", src_op(in.a)));
        line(std::format("movq {}(%rax), %rax", in.imm));
        move_to(dst_op(in.dst), "%rax");
    }

    void gen_field_store(const Inst& in) {
        line(std::format("movq {}, %rax", src_op(in.a)));
        line(std::format("movq {}, %rcx", src_op(in.b)));
        line(std::format("movq %rcx, {}(%rax)", in.imm));
    }

    // 虚调用：self 固定占第一个整数参数（%rdi），其余实参照常分类。
    // 函数地址在装参数之前取到 %r11 —— %r11 不在参数序列里，
    // 装参数的过程不会碰它。
    void gen_vcall(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size() + 1);
        types.push_back(t_unknown());  // self：指针，非浮点
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int stack_bytes = stack_args * 8;
        int alloc = (stack_bytes + 15) & ~15;

        line(std::format("movq {}, %r10", src_op(in.a)));
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        line(std::format("movq {}, %rdi", src_op(in.a)));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line("xorl %r10d, %r10d");
        line("xorl %eax, %eax");
        line("call *%r11");
        if (alloc) line(std::format("addq ${}, %rsp", alloc));

        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

    void gen_branch(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ir_is_float(ty)) {
            line(std::format("movsd {}, %xmm0", src_op(in.a)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            line(std::format("jne {}", block_label(in.target)));
            line(std::format("jp {}", block_label(in.target)));
            line(std::format("jmp {}", block_label(in.target2)));
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            line(std::format("movq {}, %rax", src_op(in.a)));
            line("cmpq $0, (%rax)");
            line(std::format("jne {}", block_label(in.target)));
            line(std::format("jmp {}", block_label(in.target2)));
            return;
        }
        line(std::format("cmpq $0, {}", src_op(in.a)));
        line(std::format("jne {}", block_label(in.target)));
        line(std::format("jmp {}", block_label(in.target2)));
    }

    void gen_return(const Inst& in) {
        if (in.a == kNoReg) {
            line("xorl %eax, %eax");
        } else if (is_fp(in.a)) {
            line(std::format("movsd {}, %xmm0", src_op(in.a)));
        } else {
            line(std::format("movq {}, %rax", src_op(in.a)));
        }
        emit_epilogue();
    }

    // ── 调用 ────────────────────────────────────────────────────

    void gen_call(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int stack_bytes = stack_args * 8;
        int alloc = (stack_bytes + 15) & ~15;

        // 函数值是闭包对象指针：+0 是代码地址。先取代码到 r11，
        // 参数装完后再把闭包指针放进 r10 传给被调函数当环境。
        line(std::format("movq {}, %r11", src_op(in.a)));
        line("movq (%r11), %r11");
        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            if (locs[i].stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", locs[i].stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line(std::format("movq {}, %r10", src_op(in.a)));
        line("xorl %eax, %eax");
        line("call *%r11");
        if (alloc) line(std::format("addq ${}, %rsp", alloc));

        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

    void gen_setjmp(const Inst& in) {
        // handler 记录的第一个字段就是 jmp_buf，所以记录指针可以直接
        // 作为 setjmp 的参数。longjmp 回来后 $rax 非 0。
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line("call setjmp");
        line("movslq %eax, %rax");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

    void gen_call_rt(const Inst& in) {
        auto fn = static_cast<RtFn>(in.imm);
        switch (fn) {
            case RtFn::ListNew:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::ListAppend:
            case RtFn::ListGet:
            case RtFn::ListSetLen:
            case RtFn::StrConcat:
            case RtFn::StrCharAt:
            case RtFn::ClosureNew:
            case RtFn::DictGet:
            case RtFn::DictContains:
            case RtFn::DictKeyAt:
            case RtFn::StrContains:
            case RtFn::DictUpdate:
            case RtFn::ListExtend:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::ListSet:
            case RtFn::DictNew:
            case RtFn::DictSet:
            case RtFn::ListContains:
            case RtFn::DictGetOr:
            case RtFn::DictRemove:
            case RtFn::RangeNew:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                break;
            case RtFn::ListSlice:
            case RtFn::StrSlice:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                line(std::format("movq {}, %rcx", src_op(in.args[3])));
                break;
            case RtFn::DictPop:
            case RtFn::ListSliceAssign:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                line(std::format("movq {}, %rcx", src_op(in.args[3])));
                if (in.args.size() > 4)
                    line(std::format("movq {}, %r8", src_op(in.args[4])));
                break;
            case RtFn::Raise:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::StrFromI64:
            case RtFn::StrFromBool:
            case RtFn::PrintI64:
            case RtFn::PrintBool:
            case RtFn::Panic:
            case RtFn::RcInc:
            case RtFn::RcDec:
            case RtFn::RcTemp:
            case RtFn::RcCleanup:
            case RtFn::RcIncUnknown:
            case RtFn::DictKeys:
            case RtFn::DictValues:
            case RtFn::DictItems:
            case RtFn::DictPopItem:
            case RtFn::DictPopLast:
            case RtFn::DictClear:
            case RtFn::StrToList:
            case RtFn::HandlerLeave:
            case RtFn::HandlerDiscard:
            case RtFn::ExcMatches:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                break;
            case RtFn::StrFromF64:
            case RtFn::PrintF64:
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
                break;
            case RtFn::PrintStr:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                break;
            case RtFn::StrFromNone:
            case RtFn::PanicDiv0:
            case RtFn::PanicOverflow:
            case RtFn::PrintNone:
            case RtFn::PrintSep:
            case RtFn::PrintEnd:
            case RtFn::RcMark:
            case RtFn::RcShutdown:
            case RtFn::GcShutdown:
            case RtFn::HandlerPush:
            case RtFn::ExcAccept:
            case RtFn::ExcDone:
            case RtFn::ExcClear:
            case RtFn::ExcReraise:
            case RtFn::ExcTypeName:
            case RtFn::ExcMessage:
            case RtFn::Reraise:
                break;
        }
        line(std::format("call {}", rt_name(fn)));
        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }
};

}  // namespace

std::string generate_asm(const IRModule& m) {
    AsmGen g;
    return g.gen(m);
}

}  // namespace py23
