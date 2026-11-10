// codegen_regalloc.cpp —— 寄存器/栈槽分配与函数帧
#include "codegen_impl.h"

namespace py23 {

// 调用序列之外不会被指令生成器用到的临时寄存器（%r8–%r11）。
static const char* const kTempRegs[] = {"%r8", "%r9", "%r10", "%r11"};

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

// 变量提升：按被读写次数排序，把最热的几个局部变量放进寄存器。
// 没提升的变量继续住栈槽，语义完全一样，所以这一步错了最多是慢，
// 不会算错。
//
// 不做活跃性分析——一个变量要么整函数在寄存器里，要么整函数在栈上。
// 分支汇合、循环回边都因此天然正确，不需要活跃性 spill 逻辑。整数
// 用 callee-saved GPR，调用天然安全；浮点用 xmm8–15（xmm0–7 留给
// 参数和临时），在每次会正常返回的 call 前后统一写回/读出栈槽。
void AsmGen::spill_fp_slots() {
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            line(std::format("movsd {}, {}", slot_reg_[slot], smem(slot)));
        }
    }

void AsmGen::reload_fp_slots() {
        for (size_t j = 0; j < fp_promoted_slots_.size(); ++j) {
            if (fp_mask_valid_ && j < fp_spill_mask_.size() && !fp_spill_mask_[j])
                continue;
            int slot = fp_promoted_slots_[j];
            line(std::format("movsd {}, {}", smem(slot), slot_reg_[slot]));
        }
    }

// 浮点槽在调用点是否还需要：块级 live-in/live-out（只跟踪提升过的槽）。
void AsmGen::build_fp_liveness() {
        const int nb = static_cast<int>(f_->blocks.size());
        const size_t kf = fp_promoted_slots_.size();
        fp_live_in_.assign(static_cast<size_t>(nb), 0);
        fp_live_out_.assign(static_cast<size_t>(nb), 0);
        fp_live_off_ = std::getenv("PY23_NO_FPLIVE") != nullptr;
        if (kf == 0 || nb == 0 || fp_live_off_) return;

        std::vector<uint8_t> use(static_cast<size_t>(nb), 0);
        std::vector<uint8_t> def(static_cast<size_t>(nb), 0);
        for (int b = 0; b < nb; ++b) {
            // 只把「块内先读后写」的读算作 use：先 store 再 load 是本块
            // 自己定义的值，不构成对入口值的活跃需求。
            uint8_t defined = 0;
            uint8_t used = 0;
            for (const auto& in : f_->blocks[static_cast<size_t>(b)].insts) {
                if (in.idx < 0 || in.idx >= static_cast<int>(slot_fpidx_.size())) continue;
                int fi = slot_fpidx_[static_cast<size_t>(in.idx)];
                if (fi < 0) continue;
                uint8_t bit = static_cast<uint8_t>(1u << fi);
                if (in.op == IrOp::LoadSlot) {
                    if (!(defined & bit)) used |= bit;
                } else if (in.op == IrOp::StoreSlot) {
                    defined |= bit;
                }
            }
            use[static_cast<size_t>(b)] = used;
            def[static_cast<size_t>(b)] = defined;
        }

        std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const auto& insts = f_->blocks[static_cast<size_t>(b)].insts;
            auto add = [&](int t) {
                if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
            };
            if (insts.empty()) {
                add(b + 1);
                continue;
            }
            switch (insts.back().op) {
                case IrOp::Jump: add(insts.back().target); break;
                case IrOp::Branch:
                    add(insts.back().target);
                    add(insts.back().target2);
                    break;
                case IrOp::Return:
                case IrOp::GenYield:
                case IrOp::GenAwait:
                case IrOp::GenRethrow:
                case IrOp::GenDone:
                    break;
                default: add(b + 1); break;
            }
        }

        bool chg = true;
        while (chg) {
            chg = false;
            for (int b = nb - 1; b >= 0; --b) {
                uint8_t out = 0;
                for (int s : succ[static_cast<size_t>(b)])
                    out |= fp_live_in_[static_cast<size_t>(s)];
                uint8_t in = static_cast<uint8_t>(
                    use[static_cast<size_t>(b)] |
                    (out & ~def[static_cast<size_t>(b)]));
                if (out != fp_live_out_[static_cast<size_t>(b)] ||
                    in != fp_live_in_[static_cast<size_t>(b)]) {
                    fp_live_out_[static_cast<size_t>(b)] = out;
                    fp_live_in_[static_cast<size_t>(b)] = in;
                    chg = true;
                }
            }
        }
    }

void AsmGen::assign_slot_regs() {
        int ns = static_cast<int>(f_->slot_ty.size());
        slot_reg_.assign(ns, std::string());
        slot_fpidx_.assign(ns, -1);
        fp_promoted_slots_.clear();
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
                    in.idx < ns && !addr_taken[in.idx])
                    ++score[in.idx];
            }

        // 槽类型可能被 unknown 吞掉（类型合并不上时），但实际流动的值
        // 仍是浮点。这类槽不能进 GPR——结果融合会把 movsd 直接写到
        // 整数寄存器上，汇编器都过不了。槽类型确定是 float 的才进 xmm，
        // 类型模糊但带浮点值的槽留在内存里，两种指令都能落。
        std::vector<bool> float_valued(ns, false);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::StoreSlot && in.idx >= 0 && in.idx < ns &&
                    in.a != kNoReg && in.a < f_->vreg_ty.size() &&
                    ir_is_float(f_->vreg_ty[in.a]))
                    float_valued[in.idx] = true;
                if (in.op == IrOp::LoadSlot && in.idx >= 0 && in.idx < ns &&
                    in.dst != kNoReg && in.dst < f_->vreg_ty.size() &&
                    ir_is_float(f_->vreg_ty[in.dst]))
                    float_valued[in.idx] = true;
            }

        std::vector<std::pair<int, int>> hot_int, hot_fp;
        for (int i = 0; i < ns; ++i) {
            if (score[i] <= 0) continue;
            if (ir_is_float(f_->slot_ty[i])) {
                if (opt_ >= 1) hot_fp.push_back({score[i], i});
            } else if (!float_valued[i]) {
                hot_int.push_back({score[i], i});
            }
        }
        auto hotter = [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        };
        std::ranges::sort(hot_int, hotter);
        std::ranges::sort(hot_fp, hotter);

        int k = std::min<int>(kMaxSlotRegs, static_cast<int>(hot_int.size()));
        for (int i = 0; i < k; ++i) slot_reg_[hot_int[i].second] = kSlotRegs[i];
        n_saved_regs_ = k;

        int kf = std::min<int>(kMaxFpSlotRegs, static_cast<int>(hot_fp.size()));
        for (int i = 0; i < kf; ++i) {
            slot_reg_[hot_fp[i].second] = kFpSlotRegs[i];
            slot_fpidx_[hot_fp[i].second] = i;
            fp_promoted_slots_.push_back(hot_fp[i].second);
        }
    }

// 块内临时寄存器：%r8–%r11 只在调用序列里被用到（参数/函数地址），
// 所以「整块没有任何调用类指令」时可以把它们借给块内临时值。区间用
// 线性扫描分配，定义/使用不重叠的临时值复用同一个寄存器；块结束时
// 全部归还（值本身也不会跨块使用，候选要求 !cross_block_）。

bool AsmGen::temp_reg_op_ok(const Inst& in) const {
        switch (in.op) {
            case IrOp::ConstInt:
            case IrOp::ConstFloat:
            case IrOp::ConstStr:
            case IrOp::ConstNone:
            case IrOp::FuncAddr:
            case IrOp::FuncCode:
            case IrOp::Mov:
            case IrOp::Un:
            case IrOp::Cmp:
            case IrOp::LoadSlot:
            case IrOp::StoreSlot:
            case IrOp::LoadGlobal:
            case IrOp::StoreGlobal:
            case IrOp::Len:
            case IrOp::IndexLoadUnchecked:
            case IrOp::FieldLoad:
            case IrOp::FieldStore:
            case IrOp::SlotAddr:
            case IrOp::Jump:
            case IrOp::Branch:
            case IrOp::Return:
                return true;
            case IrOp::IndexLoad: {
                // 字符串下标会真的调 py23_str_char_at；列表下标只有不会
                // 返回的越界慢路径，借用寄存器是安全的。
                TypePtr bt = ty_of(in.a);
                return !(bt && bt->kind == TyKind::Str);
            }
            case IrOp::Bin: {
                TypePtr t = in.ty;
                if (t && t->kind == TyKind::Str) return false;
                switch (in.bop) {
                    case BinKind::Add:
                    case BinKind::Sub:
                    case BinKind::Mul:
                    case BinKind::BitAnd:
                    case BinKind::BitOr:
                    case BinKind::BitXor:
                    case BinKind::Shl:
                    case BinKind::Shr:
                        return true;
                    default:
                        return false;  // div/mod/floordiv/pow 用 %r8 或调运行时
                }
            }
            case IrOp::Convert: {
                TypePtr t = in.ty;
                return t && (t->kind == TyKind::Bool || t->kind == TyKind::Int ||
                             t->kind == TyKind::Float);
            }
            default:
                return false;
        }
    }

void AsmGen::assign_block_temp_regs() {
        if (opt_ < 1 || gen_) return;

        for (const auto& b : f_->blocks) {
            bool safe = true;
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) {
                    safe = false;
                    break;
                }
            if (!safe) continue;

            std::unordered_map<VReg, int> def_pos;
            std::unordered_map<VReg, IrOp> def_op;
            std::unordered_map<VReg, int> last_use;
            int pos = 0;
            for (const auto& in : b.insts) {
                auto touch = [&](VReg r) {
                    if (r != kNoReg) last_use[r] = pos;
                };
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
                if (in.dst != kNoReg) {
                    def_pos[in.dst] = pos;
                    def_op[in.dst] = in.op;
                }
                ++pos;
            }

            struct Range {
                int start;
                int end;
                VReg v;
            };
            std::vector<Range> ranges;
            for (const auto& [v, d] : def_pos) {
                if (v >= static_cast<VReg>(f_->vreg_ty.size())) continue;
                if (cross_block_[v]) continue;
                auto lu = last_use.find(v);
                if (lu == last_use.end()) continue;
                // unknown 的机器表示可能是浮点位模式（元素类型丢失的下标装载），
                // 浮点/可选类型同理：只给「确定是整数/指针」的值分配 GPR。
                TypePtr vt = f_->vreg_ty[v];
                if (!vt) continue;
                if (vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                    vt->kind == TyKind::Optional)
                    continue;
                IrOp op = def_op[v];
                // LoadSlot 有自己的别名机制；常量可以延迟成立即数，不用占寄存器。
                if (op == IrOp::LoadSlot || op == IrOp::ConstInt || op == IrOp::ConstNone)
                    continue;
                ranges.push_back(Range{d, lu->second, v});
            }
            std::ranges::sort(ranges, [](const Range& x, const Range& y) {
                return x.start != y.start ? x.start < y.start : x.end < y.end;
            });

            std::priority_queue<std::pair<int, std::string>, std::vector<std::pair<int, std::string>>,
                                std::greater<>>
                active;
            std::vector<std::string> free(block_temp_pool_.begin(), block_temp_pool_.end());
            for (const auto& r : ranges) {
                while (!active.empty() && active.top().first < r.start) {
                    free.push_back(active.top().second);
                    active.pop();
                }
                if (free.empty()) continue;  // 后面的区间更晚开始，仍可能排上
                std::string reg = std::move(free.back());
                free.pop_back();
                vreg_reg_[r.v] = reg;
                active.push({r.end, std::move(reg)});
            }
        }
    }

// ── 跨块 vreg 提升 ──────────────────────────────────────────
    //
    // 块内临时寄存器只给「定义和使用同块」的值；跨块的值（循环不变量、
    // 合并结果、循环边界等）一直住栈槽。这里把局部变量没用到的
    // callee-saved 寄存器按使用次数分给最热的跨块整型 vreg：
    //   - 寄存器整函数独占，不与其他值复用，所以不需要活跃性分析；
    //   - callee-saved 跨函数调用不会被破坏，only has_handlers 的函数
    //     例外（longjmp 会回滚），整函数跳过；
    //   - 不做支配性校验：IR 的 vreg 定义先于使用，整函数独占寄存器
    //     天然满足「定义后一直有效」。
    void AsmGen::assign_global_vreg_regs() {
        const size_t nv = f_->vreg_ty.size();
        vreg_reg_.assign(nv, std::string());
        block_temp_pool_.assign(kTempRegs, kTempRegs + 4);
        global_imm_.clear();
        if (nv == 0) return;
        if (opt_ < 2 || gen_ || f_->has_handlers) return;

        // 跨块的 const.int 整函数当立即数：寄存器、栈槽都不占。所有使用
        // 点生成的都是 movq $imm / addq $imm / cmpq $imm 形态（比较和
        // 分支对立即数位置有专门处理，转换会先搬进 %rax）。
        std::vector<int64_t> const_imm(nv, 0);
        std::vector<uint8_t> is_const(nv, 0);
        for (const auto& b : f_->blocks)
            for (const auto& in : b.insts) {
                if (in.op != IrOp::ConstInt || in.dst == kNoReg || in.dst >= nv) continue;
                if (in.imm < INT32_MIN || in.imm > INT32_MAX) continue;
                const_imm[static_cast<size_t>(in.dst)] = in.imm;
                is_const[static_cast<size_t>(in.dst)] = 1;
            }
        for (size_t v = 0; v < nv; ++v) {
            if (is_const[v])
                global_imm_[static_cast<VReg>(v)] =
                    std::format("${}", const_imm[v]);
        }

        struct Cand {
            int v;
            int uses;
        };
        std::vector<Cand> cands;
        for (size_t v = 0; v < nv; ++v) {
            if (!cross_block_[v]) continue;
            if (global_imm_.contains(static_cast<VReg>(v))) continue;
            TypePtr vt = f_->vreg_ty[v];
            if (!vt || vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                vt->kind == TyKind::Optional)
                continue;
            if (use_count_[v] == 0 || def_block_[v] < 0) continue;
            cands.push_back(Cand{static_cast<int>(v), use_count_[v]});
        }
        if (cands.empty()) return;
        std::ranges::sort(cands, [](const Cand& a, const Cand& b) {
            return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
        });
        int next = n_saved_regs_;
        size_t ci = 0;
        for (; ci < cands.size() && next < kMaxSlotRegs; ++ci)
            vreg_reg_[static_cast<size_t>(cands[ci].v)] = kSlotRegs[next++];
        n_saved_regs_ = next;

        // 无调用函数里 %r8–%r11 也不会被任何指令破坏：再拿至多两个
        // 承载最热的跨块值，剩下的留给块内临时寄存器。
        bool call_free = true;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) {
                    call_free = false;
                    break;
                }
            if (!call_free) break;
        }
        if (call_free) {
            int used = 0;
            for (; ci < cands.size() && used < 2; ++ci, ++used)
                vreg_reg_[static_cast<size_t>(cands[ci].v)] = kTempRegs[used];
            if (used > 0) block_temp_pool_.assign(kTempRegs + used, kTempRegs + 4);

            // 无调用函数里 xmm8–15 也不会被破坏：空闲的拿走放跨块浮点值。
            if (!std::getenv("PY23_NO_FPX")) {
                size_t fp_base = fp_promoted_slots_.size();
                if (fp_base < static_cast<size_t>(kMaxFpSlotRegs)) {
                    std::vector<Cand> fcands;
                    for (size_t v = 0; v < nv; ++v) {
                        if (!cross_block_[v] || !vreg_reg_[v].empty()) continue;
                        TypePtr vt = f_->vreg_ty[v];
                        if (!vt || vt->kind != TyKind::Float) continue;
                        if (use_count_[v] == 0 || def_block_[v] < 0) continue;
                        fcands.push_back(Cand{static_cast<int>(v), use_count_[v]});
                    }
                    std::ranges::sort(fcands, [](const Cand& a, const Cand& b) {
                        return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
                    });
                    size_t used_fp = 0;
                    for (const auto& cd : fcands) {
                        if (used_fp >= 2 || fp_base + used_fp >= static_cast<size_t>(kMaxFpSlotRegs))
                            break;
                        vreg_reg_[static_cast<size_t>(cd.v)] =
                            kFpSlotRegs[fp_base + used_fp];
                        ++used_fp;
                    }
                }
            }
        }
    }

// ── 生成期字符串缓存 ────────────────────────────────────────
void AsmGen::build_operand_caches() {
        block_labels_.resize(f_->blocks.size());
        for (size_t i = 0; i < block_labels_.size(); ++i)
            block_labels_[i] = std::format(".Lf{}_b{}", fi_, i);
        vreg_mem_.resize(f_->vreg_ty.size());
        for (size_t r = 0; r < vreg_mem_.size(); ++r)
            vreg_mem_[r] = std::format("{}(%rbp)", vreg_off(static_cast<VReg>(r)));
        smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < smem_cache_.size(); ++s)
            smem_cache_[s] = std::format("{}(%rbp)", slot_off(static_cast<int>(s)));
        gen_smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < gen_smem_cache_.size(); ++s)
            gen_smem_cache_[s] =
                std::format("{}(%r15)", 8 * (kGenHeader + static_cast<int>(s)));
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
