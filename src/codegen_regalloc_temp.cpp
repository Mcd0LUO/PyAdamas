// codegen_regalloc_temp.cpp —— 块内临时寄存器与跨块 vreg 提升
//
// 从 codegen_regalloc.cpp 拆出（纯代码移动，函数体不变）：
// temp_reg_op_ok / assign_block_temp_regs / assign_global_vreg_regs /
// build_operand_caches / classify。块循环深度工具见 regalloc_loop_depth.h。
#include "codegen_impl.h"

#include "block_bitset.h"
#include "regalloc_loop_depth.h"

namespace pyadamas {

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
            case IrOp::ConstBig:
            case IrOp::FuncAddr:
            case IrOp::FuncCode:
            case IrOp::Mov:
            case IrOp::Un:
            case IrOp::Cmp:
            case IrOp::LoadSlot:
            case IrOp::StoreSlot:
            case IrOp::LoadGlobal:
            case IrOp::LoadExtern:
            case IrOp::StoreGlobal:
            case IrOp::Len:
            case IrOp::IndexLoadUnchecked:
            case IrOp::IndexStoreUnchecked:
                // BCE kind0 证明快路径（imm==1，0.335）：纯 mov，无调用、
                // 不碰 scratch_val（序列只用 r0/r1/scratch_ptr/nv=rax 或
                // sl，都在块内临时池 {r8,r9} 外）→ 借池安全。普通 unchecked
                // 存储仍有 kind 冲突慢路径（pa_list_set 会返回，破坏 r8-r11）
                // → 不安全。降级器发的 unchecked 存储 imm 恒 0，不走这里。
                if (in.op == IrOp::IndexStoreUnchecked) {
                    if (in.imm != 1) return false;
                    TypePtr ct = ty_of(in.c);
                    return ct && !ir_is_heap(ct) && ct->kind != TyKind::Unknown;
                }
                return true;
            case IrOp::FieldLoad:
            case IrOp::PtrLoad:
            case IrOp::FieldStore:
            case IrOp::SlotAddr:
            case IrOp::Jump:
            case IrOp::Branch:
            case IrOp::Return:
                return true;
            case IrOp::IndexLoad: {
                // 字符串下标会真的调 pa_str_char_at；列表下标只有不会
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
                    case BinKind::Div:
                        // 浮点除法只用 xmm0，不碰 %r8–%r11；整数除法要
                        // %rax/%rdx，块内临时寄存器方案不适用。
                        return in.ty && in.ty->kind == TyKind::Float;
                    default:
                        return false;  // mod/floordiv/pow 用 %r8 或调运行时
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

        // 块内 def/use 位置索引。原来是每块新建三张 unordered_map<VReg, …>：
        // 逐指令 find/insert + 每个结点一次 malloc，perf 显示
        // _M_insert_unique_node + emplace_uniq + malloc/free 合计约 15% 编译
        // 时间（大函数上）。改成按 vreg 直接下标 + 脏项复位：查询「存在/
        // 不存在」与取值完全一致；ranges 的起点是定义位置、各不相同，
        // 排序后与迭代顺序无关，因此分配结果不变。
        size_t nv = f_->vreg_ty.size();
        std::vector<int> def_pos(nv, -1);
        std::vector<const Inst*> def_inst(nv, nullptr);
        std::vector<int> last_use(nv, -1);
        std::vector<VReg> touched;
        auto ensure_slot = [&](VReg r) {
            if (static_cast<size_t>(r) < nv) return;
            nv = static_cast<size_t>(r) + 1;
            def_pos.resize(nv, -1);
            def_inst.resize(nv, nullptr);
            last_use.resize(nv, -1);
        };

        for (const auto& b : f_->blocks) {
            // GPR 池（r8-r11）与浮点池（xmm3-7）独立门控：整数除法类
            // 指令踢出 GPR 池但不踢浮点池（见 fp_temp_op_ok）。
            bool safe = true, fp_safe = true;
            for (const auto& in : b.insts) {
                if (safe && !temp_reg_op_ok(in)) safe = false;
                if (fp_safe && !fp_temp_op_ok(in)) fp_safe = false;
                if (!safe && !fp_safe) break;
            }
            if (!safe && !fp_safe) continue;

            // 上一块碰过的项复位 —— 等价于原来每块新建三张空表。
            for (VReg r : touched) {
                def_pos[r] = -1;
                def_inst[r] = nullptr;
                last_use[r] = -1;
            }
            touched.clear();
            int pos = 0;
            for (const auto& in : b.insts) {
                auto touch = [&](VReg r) {
                    if (r == kNoReg) return;
                    ensure_slot(r);
                    if (last_use[r] < 0 && def_pos[r] < 0) touched.push_back(r);
                    last_use[r] = pos;
                };
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
                if (in.dst != kNoReg) {
                    ensure_slot(in.dst);
                    if (last_use[in.dst] < 0 && def_pos[in.dst] < 0) touched.push_back(in.dst);
                    def_pos[in.dst] = pos;
                    def_inst[in.dst] = &in;
                }
                ++pos;
            }

            struct Range {
                int start;
                int end;
                VReg v;
            };
            // 分配一个寄存器池。除了线性扫描，还带拷贝合并提示：`v = a op b`
            // 且 a 的最后一个使用就是这条指令时，把 a 的寄存器直接分给 v。
            // 指令生成器看到目标寄存器就是左操作数时会原地算（`subsd b, a`），
            // 省掉一条 movsd / movq。
            //
            // 提示只接受本块临时池里的寄存器：跨块值和变量槽的寄存器在定义
            // 块之外还有读者，覆盖掉就错了。in_place_ok 再筛掉后端不能原地
            // 算的算子，避免白白占住寄存器。
            auto allocate = [&](std::vector<Range>& ranges, const std::vector<std::string>& pool,
                                auto in_place_ok) {
                std::ranges::sort(ranges, [](const Range& x, const Range& y) {
                    return x.start != y.start ? x.start < y.start : x.end < y.end;
                });
                // until[i]：池里第 i 个寄存器当前占用区间的最大结束位置。
                std::vector<int> until(pool.size(), -1);
                for (const auto& r : ranges) {
                    int pick = -1;
                    const Inst* di = def_inst[r.v];
                    if (di != nullptr && in_place_ok(*di)) {
                        VReg a = di->a;
                        const int lu = static_cast<size_t>(a) < nv ? last_use[a] : -1;
                        if (a != kNoReg && lu >= 0 && lu == r.start &&
                            a < vreg_reg_.size() && !vreg_reg_[a].empty() && !cross_block_[a]) {
                            for (size_t i = 0; i < pool.size(); ++i)
                                if (pool[i] == vreg_reg_[a] && until[i] <= r.start) {
                                    pick = static_cast<int>(i);
                                    break;
                                }
                        }
                    }
                    if (pick < 0) {
                        // 最近释放的优先，保持和原来 LIFO 自由表一致的行为。
                        for (size_t i = 0; i < pool.size(); ++i)
                            if (until[i] < r.start && (pick < 0 || until[i] >= until[pick]))
                                pick = static_cast<int>(i);
                    }
                    if (pick < 0) continue;  // 后面的区间更晚开始，仍可能排上
                    vreg_reg_[r.v] = pool[static_cast<size_t>(pick)];
                    until[static_cast<size_t>(pick)] =
                        std::max(until[static_cast<size_t>(pick)], r.end);
                }
            };

            if (safe) {
            std::vector<Range> ranges;
            for (size_t v = 0; v < nv; ++v) {
                const int d = def_pos[v];
                if (d < 0) continue;
                if (v >= f_->vreg_ty.size()) continue;
                if (cross_block_[v]) continue;
                if (last_use[v] < 0) continue;
                // unknown 的机器表示可能是浮点位模式（元素类型丢失的下标装载），
                // 浮点/可选类型同理：只给「确定是整数/指针」的值分配 GPR。
                TypePtr vt = f_->vreg_ty[v];
                if (!vt) continue;
                if (vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                    vt->kind == TyKind::Optional)
                    continue;
                IrOp op = def_inst[v]->op;
                // LoadSlot 有自己的别名机制；常量可以延迟成立即数，不用占寄存器。
                if (op == IrOp::LoadSlot || op == IrOp::ConstInt || op == IrOp::ConstNone)
                    continue;
                ranges.push_back(Range{d, last_use[v], static_cast<VReg>(v)});
            }
            allocate(ranges, block_temp_pool_, [](const Inst& in) {
                if (in.op != IrOp::Bin) return false;
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
                        return false;  // 除法/取模/幂走 rax/rdx 或运行时调用
                }
            });

            }
            if (fp_safe) {
            // 浮点临时值单独用 xmm3–7：和整型池互不影响。只收块内产生、
            // 块内用完且确定是 float 的纯计算值；LoadSlot 有自己的别名
            // 机制，会正常返回的调用类指令所在块进不到这里。
            std::vector<Range> fp_ranges;
            for (size_t v = 0; v < nv; ++v) {
                const int d = def_pos[v];
                if (d < 0) continue;
                if (v >= f_->vreg_ty.size()) continue;
                if (cross_block_[v]) continue;
                if (last_use[v] < 0) continue;
                TypePtr vt = f_->vreg_ty[v];
                if (!vt || vt->kind != TyKind::Float) continue;
                IrOp op = def_inst[v]->op;
                if (op != IrOp::Bin && op != IrOp::Un && op != IrOp::Convert &&
                    op != IrOp::ConstFloat && op != IrOp::Mov)
                    continue;
                fp_ranges.push_back(Range{d, last_use[v], static_cast<VReg>(v)});
            }
            allocate(fp_ranges, std::vector<std::string>(kFpTempRegs, kFpTempRegs + kMaxFpTempRegs),
                     [](const Inst& in) {
                         if (in.op != IrOp::Bin || !ir_is_float(in.ty)) return false;
                         return in.bop == BinKind::Add || in.bop == BinKind::Sub ||
                                in.bop == BinKind::Mul || in.bop == BinKind::Div;
                     });
            }
        }
    }

// ── 跨块 vreg 提升 ──────────────────────────────────────────
    //
    // 块内临时寄存器只给「定义和使用同块」的值；跨块的值（循环不变量、
    // 合并结果、循环边界等）一直住栈槽。这里把局部变量没用到的
    // callee-saved 寄存器按「使用次数 × 循环深度」分给最热的跨块整型 vreg：
    //   - 寄存器整函数独占，不与其他值复用，所以不需要活跃性分析；
    //   - callee-saved 跨函数调用不会被破坏，only has_handlers 的函数
    //     例外（longjmp 会回滚），整函数跳过；
    //   - 不做支配性校验：IR 的 vreg 定义先于使用，整函数独占寄存器
    //     天然满足「定义后一直有效」。
    /* ── 失败记录：统一浮点寄存器分配 ────────────────────────────────
 *
 * 试过把「命名变量槽」和「跨块 vreg」放进同一个池、按 spill cost 竞争
 * xmm8–15（取代原来 assign_slot_regs 先占满、assign_global_vreg_regs 捡剩
 * 的两趟分配）。**失败**，两个原因：
 *
 * 1. `slot_reg_[s]` 是「整函数在寄存器」的布尔标记，代码生成器（slot_op）
 *    在任意指令处看到非空就直接用寄存器，**不做区间判断、不在边界插 move**。
 *    所以给「只活跃一小段」的槽分配寄存器是错的：区间外的读写会拿到过期
 *    寄存器值。实测 mandelbrot 的 s6 只活跃 7 个位置，分到 %xmm12 后
 *    输出从 2990360 变成 1。
 *
 * 2. 加「只收整函数活跃的槽」的限制后覆盖的槽太少 —— 实测 spectral_norm
 *    从 28ms 退化到 94ms（3.4x）。而让不活跃的槽「参与竞争但不占位」
 *    同样退化（93ms）。
 *
 * 结论：要真正统一，必须先让代码生成器支持「区间边界插 move」，即
 * live range splitting。那是独立的一轮工作，不是分配器内部能解决的。
 *
 * 现状保留原版的两趟分配，跨块浮点 vreg 只在「无调用」函数里提升
 * （那里没有调用点 spill 开销，纯赚）。 */
void AsmGen::assign_global_vreg_regs() {
        // **每个函数都要清空**：这些是 AsmGen 的成员，跨函数累积。
        //
        // 原来只清 fp_promoted_slots_（在 assign_slot_regs 里），漏了
        // fp_promoted_vregs_ —— 于是上一个函数提升的 vreg 编号会带到下一个
        // 函数，而 vreg 编号是函数内局部的：同一个编号在下一个函数里可能
        // 是整型，spill 就生成 `movsd %r9, 80(%rsp)`（%r9 不是 xmm）。
        // 实测 spectral_norm 的 mul_ata_2 把 v=39（int）当浮点写回。
        fp_promoted_vregs_.clear();
        const size_t nv = f_->vreg_ty.size();
        vreg_reg_.assign(nv, std::string());

        block_temp_pool_.assign(target_abi().block_temps,
                                 target_abi().block_temps + target_abi().n_block_temps);
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
            if (!is_const[v]) continue;
            // P3：unknown dst 的常量不折叠（跳过 tag 写入）。
            if (tags_enabled_ && v < f_->vreg_ty.size() &&
                is_unk_ty(f_->vreg_ty[v]))
                continue;
            global_imm_[static_cast<VReg>(v)] =
                std::format("${}", const_imm[v]);
        }

        struct Cand {
            int v;
            int uses;
        };
        // 使用点按所在块的循环深度加权：内层循环里读一次，实际执行次数
        // 是循环外读一次的几十倍，寄存器应该给它。
        std::vector<int> depth = block_loop_depth(*f_);
        std::vector<int64_t> weight(nv, 0);
        {
            int bi = 0;
            for (const auto& b : f_->blocks) {
                int d = depth[static_cast<size_t>(bi)];
                int64_t w = 1;
                for (int i = 0; i < d && i < 4; ++i) w *= 10;
                auto add = [&](VReg r) {
                    if (r != kNoReg && r < nv) weight[r] += w;
                };
                for (const auto& in : b.insts) {
                    add(in.a);
                    add(in.b);
                    add(in.c);
                    for (VReg r : in.args) add(r);
                }
                ++bi;
            }
        }
        std::vector<Cand> cands;
        for (size_t v = 0; v < nv; ++v) {
            if (!cross_block_[v]) continue;
            if (global_imm_.contains(static_cast<VReg>(v))) continue;
            TypePtr vt = f_->vreg_ty[v];
            if (!vt || vt->kind == TyKind::Float || vt->kind == TyKind::Unknown ||
                vt->kind == TyKind::Optional)
                continue;
            if (use_count_[v] == 0 || def_block_[v] < 0) continue;
            // RcMark 的值必须留在栈槽里：它是 RC 临时栈水位，跨整个函数
            // 存活。放进 callee 寄存器看似安全，但它会和槽寄存器/其它
            // vreg 的复用产生冲突，cleanup 拿到错水位 —— 实测 fasta 报
            // invalid temporary stack mark、binary_trees 报 refcount 错误。
            if (block_use_.is_rc_mark(static_cast<VReg>(v))) continue;
            cands.push_back(Cand{static_cast<int>(v), static_cast<int>(weight[v])});
        }
        // RA_OPT（默认关）：P2 只读槽 load 别名 + P1 同块调用块候选。
        // 关闭时整段跳过，候选池与基线逐位相同（asm 逐字节不变）。
        // 门槛（opt_>=2 / 非生成器 / 非 has_handlers）由本函数入口统一把守。
        std::vector<Cand> ra_extra;
        if (ra_opt_) {
            const std::vector<char> aliased = ra_opt_readonly_alias();
            for (const auto& [v, w] : ra_opt_call_block_cands(weight))
                if (!aliased[static_cast<size_t>(v)])
                    ra_extra.push_back(Cand{v, w});
            // 已别名的 vreg 已有位置（槽寄存器），从候选池剔除，
            // 不再另占一个寄存器。
            std::erase_if(cands, [&](const Cand& c) {
                return aliased[static_cast<size_t>(c.v)] != 0;
            });
        }
        // flag-on 时候选可能全被别名剔除：整型分配循环空转即可，后面的
        // 无调用块临时池 / 浮点提升路径照常走（与「原本有候选」一致）。
        if (cands.empty() && !ra_opt_) return;
        std::ranges::sort(cands, [](const Cand& a, const Cand& b) {
            return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
        });
        // **P1 只捡跨块候选挑剩的寄存器，不按权重插队。**
        // 实测教训（nbody −1.5%）：P1 按权重混排会把已有跨块 vreg 挤出
        // 寄存器落栈，weight（使用次数×10^深度）误判冷热时就是净回归。
        // 排在跨块候选之后追加 → 现有分配一个不动，P1 只填真正的空位，
        // 结构性保证「不劣化已有分配」。
        if (ra_opt_ && !ra_extra.empty()) {
            std::ranges::sort(ra_extra, [](const Cand& a, const Cand& b) {
                return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
            });
            cands.insert(cands.end(), ra_extra.begin(), ra_extra.end());
        }
        // **split 模式下 vreg 不得使用 kSlotRegs() 池。**
        //
        // split 已经把这些寄存器独占分配给变量槽（slot_loc_），而且
        // slot_reg_ 是空的（split 独占整型分配）—— 所以下面这段
        // 「从 n_saved_regs_ 继续往 kSlotRegs() 分配」会把同一个物理
        // 寄存器同时分给「槽」和「跨块 vreg」，两边互相覆盖。
        //
        // 实测 nqueens：循环上界槽拿到 %rbp，同一函数的跨块 vreg 又
        // 拿到 %rbp，循环条件被写坏 -> IndexError。
        //
        // 正确做法：split 模式下跳过这一段，vreg 只走下面的
        // %r8-%r11（block_temps，调用点会保存）等不与槽冲突的路径。
        // split 模式下跳过 kSlotRegs() 分配（那些寄存器归槽所有），
        // 但**不能直接 return** —— 后面还有 block_temps（%r8-%r11）
        // 的分配路径，跨块 vreg 需要它。
        //
        // 曾经在这里 return，结果跨块 vreg（如 LICM 提升的循环上界）
        // 拿不到任何寄存器，只能留在栈上被反复重载；更糟的是
        // %r8-%r11 在调用点会被破坏，而那些 vreg 的栈副本没人维护 ——
        // 实测 nqueens N=1 就抛 IndexError。
        int next = n_saved_regs_;
        size_t ci = 0;
        // **vreg 从 n_saved_regs_ 开始用，正好是 split 之后剩下的那部分。**
        //
        // split 的槽占 kSlotRegs()[base .. base+k)，而 n_saved_regs_ 已被
        // 设成 base+k（见 assign_slot_regs_split 末尾的 max 逻辑）。所以从
        // n_saved_regs_ 起分配不会与槽冲突。
        //
        // 曾经在 split 模式下整段跳过，结果跨块 vreg（LICM 提升的循环上界、
        // 函数返回值等）只能落栈，每次使用要「load 到临时 + 再 store 临时」
        // 两条指令 —— 这才是 split 慢 27% 的主因，不是双写。
        for (; ci < cands.size() && next < kMaxSlotRegs(); ++ci)
            vreg_reg_[static_cast<size_t>(cands[ci].v)] = kSlotRegs()[next++];
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
            for (; ci < cands.size() && used < 2 && used < target_abi().n_block_temps;
                 ++ci, ++used)
                vreg_reg_[static_cast<size_t>(cands[ci].v)] = target_abi().block_temps[used];
            if (used > 0)
                block_temp_pool_.assign(target_abi().block_temps + used,
                                        target_abi().block_temps + target_abi().n_block_temps);

        }
        // 跨块浮点 vreg 提升：只在「无调用」函数里做。
        //
        // SysV 下 xmm8–15 是 caller-saved，有调用的函数需要每个调用点",
        // spill/reload。这套机制存在（fp_promoted_vregs_），但实测收益为
        // 负：nbody 的 advance 有 7 个浮点变量 + 4 个跨块临时值竞争 8 个
        // xmm，变量优先的分配已经是最优；把临时值也塞进来反而挤掉变量。
        //
        // 保留无调用函数里的提升（那里没有调用点开销，纯赚）。
        bool call_free_fp = true;
        for (const auto& b : f_->blocks) {
            for (const auto& in : b.insts)
                if (!temp_reg_op_ok(in)) { call_free_fp = false; break; }
            if (!call_free_fp) break;
        }
        if (call_free_fp) {
            size_t fp_base = fp_promoted_slots_.size();
            if (fp_base < static_cast<size_t>(kMaxFpSlotRegs())) {
                std::vector<Cand> fcands;
                for (size_t v = 0; v < nv; ++v) {
                    if (!cross_block_[v] || !vreg_reg_[v].empty()) continue;
                    TypePtr vt = f_->vreg_ty[v];
                    if (!vt || vt->kind != TyKind::Float) continue;
                    if (use_count_[v] == 0 || def_block_[v] < 0) continue;
                    fcands.push_back(Cand{static_cast<int>(v), static_cast<int>(weight[v])});
                }
                std::ranges::sort(fcands, [](const Cand& a, const Cand& b) {
                    return a.uses != b.uses ? a.uses > b.uses : a.v < b.v;
                });
                size_t used_fp = 0;
                for (const auto& cd : fcands) {
                    if (fp_base + used_fp >= static_cast<size_t>(kMaxFpSlotRegs())) break;
                    vreg_reg_[static_cast<size_t>(cd.v)] = kFpSlotRegs()[fp_base + used_fp];
                    fp_promoted_vregs_.push_back(cd.v);
                    ++used_fp;
                }
            }
        }
    }
// ── 生成期字符串缓存 ────────────────────────────────────────
void AsmGen::build_operand_caches() {
        block_labels_.resize(f_->blocks.size());
        const std::string bp = num_label(".Lf", fi_);
        for (size_t i = 0; i < block_labels_.size(); ++i) {
            char buf[24];
            auto r = std::to_chars(buf, buf + sizeof buf, i);
            block_labels_[i] = bp + "_b";
            block_labels_[i].append(buf, r.ptr);
        }
        vreg_mem_.resize(f_->vreg_ty.size());
        for (size_t r = 0; r < vreg_mem_.size(); ++r)
            vreg_mem_[r] = std::format("{}(%rsp)", vreg_off(static_cast<VReg>(r)));
        smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < smem_cache_.size(); ++s)
            smem_cache_[s] = std::format("{}(%rsp)", slot_off(static_cast<int>(s)));
        gen_smem_cache_.resize(f_->slot_ty.size());
        for (size_t s = 0; s < gen_smem_cache_.size(); ++s)
            gen_smem_cache_[s] =
                std::format("{}(%r15)", 8 * (kGenHeader + static_cast<int>(s)));
        // P3：平行 tag 区操作数缓存（设计 §3.2）。必须在 tag 字分配
        // （emit_function 开头）与 layout_frame 之后构建。
        vreg_tag_mem_.assign(f_->vreg_ty.size(), std::string());
        slot_tag_mem_.assign(f_->slot_ty.size(), std::string());
        for (size_t v = 0; v < vreg_tag_.size(); ++v)
            if (vreg_tag_[v] >= 0)
                vreg_tag_mem_[v] = std::format("{}(%rsp)", tag_off(vreg_tag_[v]));
        for (size_t s = 0; s < slot_tag_.size(); ++s)
            if (slot_tag_[s] >= 0)
                slot_tag_mem_[s] = std::format("{}(%rsp)", tag_off(slot_tag_[s]));
    }

// ── 函数 ────────────────────────────────────────────────────
std::vector<ArgLoc> AsmGen::classify(const std::vector<TypePtr>& types) const {
        std::vector<ArgLoc> locs(types.size());
        int gi = 0, fi = 0, sp = 0;
        // Win64 按位置分配：第 i 个实参固定占第 i 个槽，与类型无关，
        // 浮点实参在第 i 个位置就用 xmmI；SysV 按类型分计数，两套独立序列。
        const bool pos = target_abi().positional_args;
        for (size_t i = 0; i < types.size(); ++i) {
            bool fp = ir_is_float(types[i]);
            if (pos) {
                if (i < static_cast<size_t>(kMaxIntArgs()) &&
                    i < static_cast<size_t>(kMaxFpArgs())) {
                    locs[i].fp = fp;
                    locs[i].reg = static_cast<int>(i);
                } else {
                    locs[i].fp = fp;
                    locs[i].stack_off = out_base() + sp++ * 8;
                }
                continue;
            }
            if (fp && fi < kMaxFpArgs()) {
                locs[i].fp = true;
                locs[i].reg = fi++;
            } else if (!fp && gi < kMaxIntArgs()) {
                locs[i].fp = false;
                locs[i].reg = gi++;
            } else {
                locs[i].fp = fp;
                locs[i].stack_off = out_base() + sp++ * 8;
            }
        }
        return locs;
    }

}  // namespace pyadamas
