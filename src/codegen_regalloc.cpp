// codegen_regalloc.cpp —— 寄存器/栈槽分配与函数帧
#include "codegen_impl.h"

namespace py23 {

// 栈帧 = push 下来的 callee-saved + 局部区。SysV 要求 call 时
    // rsp 是 16 的倍数，而 push 的个数会影响这个余数，所以把两者
    // 合起来算，只补最少的字节。
void AsmGen::frame_size(int total_slots) {
        int base = 8 * (n_saved_regs_ + total_slots);
        int aligned = (base + 15) & ~15;
        int pad = aligned - 8 * n_saved_regs_;
        if (pad > 0) line(std::format("subq ${}, %rsp", pad));
    }

// 尾声：先把 rsp 退回 push 的寄存器下面，逐个弹回，再弹 rbp。
    // 不能用 `movq %rbp, %rsp`——那会把 callee-saved 的槽一起丢掉。
void AsmGen::emit_epilogue() {
        if (n_saved_regs_ > 0) line(std::format("leaq {}(%rbp), %rsp", -8 * n_saved_regs_));
        else line("movq %rbp, %rsp");
        for (int i = n_saved_regs_ - 1; i >= 0; --i)
            line(std::format("popq {}", gen_ ? "%r15" : kSlotRegs[i]));
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
void AsmGen::assign_vreg_slots() {
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
void AsmGen::assign_slot_regs() {
        int ns = static_cast<int>(f_->slot_ty.size());
        slot_reg_.assign(ns, std::string());
        n_saved_regs_ = 0;
        if (ns == 0) return;
        // 含 setjmp 的函数：longjmp 会把 callee-saved 寄存器恢复到 setjmp
        // 时刻，寄存器里的变量会悄悄回退。这类函数全部走内存槽。
        if (f_->has_handlers) return;
        if (f_->is_generator) return;

        // 被取过地址的槽必须留在内存里：寄存器里的副本不会跟着地址写更新。
        std::vector<bool> addr_taken(ns, false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts)
                if (in.op == IrOp::SlotAddr && in.idx >= 0 && in.idx < ns) addr_taken[in.idx] = true;

        std::vector<int> score(ns, 0);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if ((in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot) && in.idx >= 0 &&
                    in.idx < ns && !ir_is_float(f_->slot_ty[in.idx]) && !addr_taken[in.idx])
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
std::vector<ArgLoc> AsmGen::classify(const std::vector<TypePtr>& types) const {
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

}  // namespace py23
