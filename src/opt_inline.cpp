// opt_inline.cpp —— 静态小函数内联与直接调用改写
#include "opt_detail.h"

namespace pyadamas {

using namespace optdetail;

namespace {

struct DefRef {
    IrOp op = IrOp::ConstNone;
    int64_t imm = 0;
    int32_t idx = -1;
};

int function_inst_count(const IRFunction& f) {
    int n = 0;
    for (const auto& b : f.blocks) n += static_cast<int>(b.insts.size());
    return n;
}

struct InlineFacts {
    // 槽 → 只读形参下标；-1 表示这个槽要在调用点复制一份。
    std::vector<int> subst;
    // 可能读到未赋值值的槽：内联入口要补一次清零。
    std::vector<bool> need_zero;
    // callee vreg 在 callee 内的使用次数，用于单返回直写的融合判断。
    std::vector<int> uses;
    int n_returns = 0;
};

InlineFacts analyze_callee(const IRFunction& callee) {
    InlineFacts f;
    const size_t ns = callee.slot_ty.size();
    f.subst.assign(ns, -1);
    f.need_zero.assign(ns, false);
    f.uses.assign(callee.vreg_ty.size(), 0);

    std::vector<bool> written(ns, false);
    for (const auto& b : callee.blocks) {
        for (const auto& in : b.insts) {
            if ((in.op == IrOp::StoreSlot || in.op == IrOp::SlotAddr) && in.idx >= 0 &&
                in.idx < static_cast<int>(ns))
                written[static_cast<size_t>(in.idx)] = true;
            if (in.op == IrOp::Return) ++f.n_returns;
            auto touch = [&](VReg r) {
                if (r != kNoReg && r < static_cast<VReg>(f.uses.size())) ++f.uses[r];
            };
            touch(in.a);
            touch(in.b);
            touch(in.c);
            for (VReg r : in.args) touch(r);
        }
    }
    for (size_t p = 0; p < callee.params.size(); ++p) {
        int s = callee.params[p].slot;
        if (s >= 0 && s < static_cast<int>(ns) && !written[static_cast<size_t>(s)])
            f.subst[static_cast<size_t>(s)] = static_cast<int>(p);
    }

    const int nb = static_cast<int>(callee.blocks.size());
    if (nb == 0 || ns == 0) return f;

    // 与后端 slots_needing_zero 相同的 must 数据流：入口视为形参已赋值，
    // 回边不影响入口；in 取所有前驱 out 的交集。
    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const auto& insts = callee.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
            continue;
        }
        const Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            succ[static_cast<size_t>(b)].push_back(last.target);
        } else if (last.op == IrOp::Branch) {
            succ[static_cast<size_t>(b)].push_back(last.target);
            succ[static_cast<size_t>(b)].push_back(last.target2);
        } else if (last.op != IrOp::Return && last.op != IrOp::GenYield &&
                   last.op != IrOp::GenAwait && last.op != IrOp::GenRethrow &&
                   last.op != IrOp::GenDone) {
            if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
        }
    }

    std::vector<bool> entry_assigned(ns, false);
    for (const auto& p : callee.params)
        if (p.slot >= 0 && p.slot < static_cast<int>(ns))
            entry_assigned[static_cast<size_t>(p.slot)] = true;

    std::vector<std::vector<bool>> in(static_cast<size_t>(nb),
                                      std::vector<bool>(ns, true));
    std::vector<std::vector<bool>> out(static_cast<size_t>(nb),
                                       std::vector<bool>(ns, true));
    in[0] = entry_assigned;
    out[0] = entry_assigned;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 0; b < nb; ++b) {
            std::vector<bool> o = in[static_cast<size_t>(b)];
            for (const auto& inst : callee.blocks[static_cast<size_t>(b)].insts)
                if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < static_cast<int>(ns))
                    o[static_cast<size_t>(inst.idx)] = true;
            out[static_cast<size_t>(b)] = std::move(o);
            for (int s : succ[static_cast<size_t>(b)]) {
                if (s <= 0 || s >= nb) continue;
                for (size_t i = 0; i < ns; ++i) {
                    bool v = in[static_cast<size_t>(s)][i] && out[static_cast<size_t>(b)][i];
                    if (v != in[static_cast<size_t>(s)][i]) {
                        in[static_cast<size_t>(s)][i] = v;
                        changed = true;
                    }
                }
            }
        }
    }

    for (int b = 0; b < nb; ++b) {
        std::vector<bool> assigned = in[static_cast<size_t>(b)];
        for (const auto& inst : callee.blocks[static_cast<size_t>(b)].insts) {
            if (inst.op == IrOp::SlotAddr && inst.idx >= 0 && inst.idx < static_cast<int>(ns)) {
                f.need_zero[static_cast<size_t>(inst.idx)] = true;
            } else if (inst.op == IrOp::LoadSlot && inst.idx >= 0 && inst.idx < static_cast<int>(ns) &&
                       !assigned[static_cast<size_t>(inst.idx)]) {
                f.need_zero[static_cast<size_t>(inst.idx)] = true;
            } else if (inst.op == IrOp::StoreSlot && inst.idx >= 0 && inst.idx < static_cast<int>(ns)) {
                assigned[static_cast<size_t>(inst.idx)] = true;
            }
        }
    }
    return f;
}

// 在 bi 块的 ii 位置内联 callee。克隆块紧挨着调用块插入、cont 紧挨着
// 克隆块，入口指令并进调用块本身——这样 O2 的跳转消除能把入口跳转、
// 单返回块到 cont 的跳转全部折成顺序下落。
void inline_at(IRFunction& fn, size_t bi, size_t ii, const IRFunction& callee,
               const Inst& call, const InlineFacts& facts) {
    const int nb = static_cast<int>(callee.blocks.size());
    const int ns = static_cast<int>(callee.slot_ty.size());
    const size_t insert_at = bi + 1;
    const int clone_start = static_cast<int>(insert_at);
    const int cont = clone_start + nb;

    const bool direct_ret = facts.n_returns == 1 && call.dst != kNoReg;

    std::vector<int> slot_map(static_cast<size_t>(ns), -1);
    for (int s = 0; s < ns; ++s) {
        if (facts.subst[static_cast<size_t>(s)] >= 0) continue;
        slot_map[static_cast<size_t>(s)] = static_cast<int>(fn.slot_ty.size());
        fn.slot_ty.push_back(callee.slot_ty[static_cast<size_t>(s)]);
        // 所有权跟着槽一起搬：调用方帧记录要按这个清单释放内联体的槽。
        bool owned = static_cast<size_t>(s) < callee.slot_owned.size() &&
                     callee.slot_owned[static_cast<size_t>(s)];
        fn.slot_owned.push_back(owned);
        bool unk = static_cast<size_t>(s) < callee.slot_unknown.size() &&
                   callee.slot_unknown[static_cast<size_t>(s)];
        fn.slot_unknown.push_back(unk);
    }
    int ret_slot = -1;
    if (!direct_ret && call.dst != kNoReg) {
        ret_slot = static_cast<int>(fn.slot_ty.size());
        fn.slot_ty.push_back(call.ty ? call.ty : t_unknown());
        // 返回值只是中转一下，引用归调用点结果，槽本身不持有。
        fn.slot_owned.push_back(false);
        fn.slot_unknown.push_back(false);
    }

    std::vector<VReg> vmap(callee.vreg_ty.size());
    for (size_t v = 0; v < callee.vreg_ty.size(); ++v) {
        vmap[v] = static_cast<VReg>(fn.vreg_ty.size());
        fn.vreg_ty.push_back(callee.vreg_ty[v]);
    }
    bool want_zero = ret_slot >= 0;
    for (int s = 0; s < ns && !want_zero; ++s)
        if (facts.subst[static_cast<size_t>(s)] < 0 && facts.need_zero[static_cast<size_t>(s)])
            want_zero = true;
    VReg zero = kNoReg;
    if (want_zero) {
        zero = static_cast<VReg>(fn.vreg_ty.size());
        fn.vreg_ty.push_back(t_int());
    }

    // 只读形参的 LoadSlot 不生成指令，直接把该 vreg 重定向到调用点实参。
    // 预先扫完整棵 callee：块序不一定等于支配序，遇到再登记可能漏掉先出现的用点。
    std::unordered_map<VReg, VReg> reg_subst;
    for (const auto& b : callee.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::LoadSlot && in.idx >= 0 && in.idx < ns && in.dst != kNoReg) {
                int p = facts.subst[static_cast<size_t>(in.idx)];
                if (p >= 0) reg_subst[in.dst] = call.args[static_cast<size_t>(p)];
            }
    auto mv = [&](VReg r) -> VReg {
        if (r == kNoReg || r >= static_cast<VReg>(vmap.size())) return r;
        auto it = reg_subst.find(r);
        return it == reg_subst.end() ? vmap[r] : it->second;
    };

    // 调用点之后的尾部搬到 cont。
    std::vector<Inst> suffix;
    for (size_t j = ii + 1; j < fn.blocks[bi].insts.size(); ++j)
        suffix.push_back(std::move(fn.blocks[bi].insts[j]));
    fn.blocks[bi].insts.resize(ii);

    std::vector<Inst> entry_insts;
    if (zero != kNoReg) {
        Inst zc;
        zc.op = IrOp::ConstInt;
        zc.imm = 0;
        zc.dst = zero;
        zc.ty = t_int();
        entry_insts.push_back(std::move(zc));
    }
    for (int s = 0; s < ns; ++s) {
        if (facts.subst[static_cast<size_t>(s)] >= 0) continue;
        if (!facts.need_zero[static_cast<size_t>(s)] || slot_map[static_cast<size_t>(s)] < 0)
            continue;
        int dst_slot = slot_map[static_cast<size_t>(s)];
        TypePtr sty = fn.slot_ty[static_cast<size_t>(dst_slot)];
        bool owned = static_cast<size_t>(s) < callee.slot_owned.size() &&
                     callee.slot_owned[static_cast<size_t>(s)];
        bool unk = static_cast<size_t>(s) < callee.slot_unknown.size() &&
                   callee.slot_unknown[static_cast<size_t>(s)];
        const bool unk_ty = !sty || sty->kind == TyKind::Unknown;
        if ((ir_is_heap(sty) || unk_ty) && owned) {
            // 槽里可能还留着上一次执行内联体的值（调用点在循环里时很常见），
            // 直接清零会把那份引用丢掉。先按槽类型释放再清零；槽在函数序言
            // 已清零，第一次执行 dec(0) 是空操作。借用槽只清零不释放。
            VReg old = static_cast<VReg>(fn.vreg_ty.size());
            fn.vreg_ty.push_back(sty);
            Inst ld;
            ld.op = IrOp::LoadSlot;
            ld.idx = dst_slot;
            ld.dst = old;
            ld.ty = sty;
            entry_insts.push_back(std::move(ld));
            Inst dc;
            dc.op = IrOp::CallRt;
            dc.imm = static_cast<int64_t>((unk || unk_ty) ? RtFn::RcDecUnknown : RtFn::RcDec);
            dc.args = {old};
            entry_insts.push_back(std::move(dc));
        }
        Inst st;
        st.op = IrOp::StoreSlot;
        st.idx = dst_slot;
        st.a = zero;
        entry_insts.push_back(std::move(st));
    }
    for (size_t p = 0; p < callee.params.size(); ++p) {
        const IRParam& param = callee.params[p];
        if (param.slot < 0 || param.slot >= ns) continue;
        if (facts.subst[static_cast<size_t>(param.slot)] >= 0) continue;
        int dst_slot = slot_map[static_cast<size_t>(param.slot)];
        if (dst_slot < 0) continue;
        TypePtr pty = fn.slot_ty[static_cast<size_t>(dst_slot)];
        bool owned = static_cast<size_t>(param.slot) < callee.slot_owned.size() &&
                     callee.slot_owned[static_cast<size_t>(param.slot)];
        bool unk = static_cast<size_t>(param.slot) < callee.slot_unknown.size() &&
                   callee.slot_unknown[static_cast<size_t>(param.slot)];
        const bool p_unk_ty = !pty || pty->kind == TyKind::Unknown;
        if ((ir_is_heap(pty) || p_unk_ty) && owned) {
            // 上一次执行内联体时形参槽里的引用可能还在：调用方自己的
            // handler 捕获异常时不会走帧展开，覆盖前必须先释放。
            VReg old = static_cast<VReg>(fn.vreg_ty.size());
            fn.vreg_ty.push_back(pty);
            Inst ld;
            ld.op = IrOp::LoadSlot;
            ld.idx = dst_slot;
            ld.dst = old;
            ld.ty = pty;
            entry_insts.push_back(std::move(ld));
            Inst dc;
            dc.op = IrOp::CallRt;
            dc.imm = static_cast<int64_t>((unk || p_unk_ty) ? RtFn::RcDecUnknown : RtFn::RcDec);
            dc.args = {old};
            entry_insts.push_back(std::move(dc));
        }
        Inst st;
        st.op = IrOp::StoreSlot;
        st.idx = dst_slot;
        st.a = call.args[p];
        entry_insts.push_back(std::move(st));
    }

    auto clone_into = [&](std::vector<Inst>& out, const Inst& src, int base) {
        if (src.op == IrOp::LoadSlot && src.dst != kNoReg && reg_subst.contains(src.dst))
            return;
        Inst ins = src;
        ins.a = mv(src.a);
        ins.b = mv(src.b);
        ins.c = mv(src.c);
        for (VReg& r : ins.args) r = mv(r);
        if (ins.dst != kNoReg) ins.dst = mv(src.dst);
        if ((ins.op == IrOp::LoadSlot || ins.op == IrOp::StoreSlot ||
             ins.op == IrOp::SlotAddr) &&
            src.idx >= 0 && src.idx < ns)
            ins.idx = slot_map[static_cast<size_t>(src.idx)];
        if (base >= 0) {
            if (ins.op == IrOp::Jump)
                ins.target = base + src.target;
            else if (ins.op == IrOp::Branch) {
                ins.target = base + src.target;
                ins.target2 = base + src.target2;
            }
        }
        out.push_back(std::move(ins));
    };

    auto emit_return = [&](std::vector<Inst>& out, const Inst& src) {
        VReg rv = src.a == kNoReg ? kNoReg : mv(src.a);
        if (direct_ret) {
            bool fused = false;
            if (rv != kNoReg && !out.empty() && src.a < facts.uses.size() &&
                facts.uses[src.a] == 1 && type_eq(call.ty, out.back().ty)) {
                Inst& last = out.back();
                if (last.dst == rv && inst_is_pure(last)) {
                    last.dst = call.dst;
                    fused = true;
                }
            }
            if (!fused) {
                Inst res;
                res.dst = call.dst;
                res.ty = call.ty;
                if (rv == kNoReg) {
                    res.op = IrOp::ConstNone;
                } else {
                    res.op = IrOp::Mov;
                    res.a = rv;
                }
                out.push_back(std::move(res));
            }
        } else if (ret_slot >= 0) {
            Inst st;
            st.op = IrOp::StoreSlot;
            st.idx = ret_slot;
            st.a = rv == kNoReg ? zero : rv;
            out.push_back(std::move(st));
        } else if (rv != kNoReg && ir_is_heap(call.ty)) {
            Inst dc;
            dc.op = IrOp::CallRt;
            dc.imm = static_cast<int64_t>(RtFn::RcDec);
            dc.args = {rv};
            out.push_back(std::move(dc));
        }
    };

    // 单块直返的小函数最常见：整段就地插进调用块，不拆块，实参还能留在
    // 寄存器里。
    const bool one_block =
        nb == 1 && !callee.blocks[0].insts.empty() &&
        callee.blocks[0].insts.back().op == IrOp::Return;
    if (one_block) {
        std::vector<Inst>& out = fn.blocks[bi].insts;
        for (auto& in : entry_insts) out.push_back(std::move(in));
        for (const Inst& src : callee.blocks[0].insts) {
            if (src.op == IrOp::Return) {
                emit_return(out, src);
                break;
            }
            clone_into(out, src, -1);
        }
        for (auto& in : suffix) out.push_back(std::move(in));
        return;
    }

    std::vector<Block> fresh;
    fresh.reserve(fn.blocks.size() + static_cast<size_t>(nb) + 1);
    auto remap_old = [&](int t) -> int {
        return t >= static_cast<int>(insert_at) ? t + nb + 1 : t;
    };
    auto remap_old_block = [&](Block& b) {
        for (auto& in : b.insts) {
            if (in.op == IrOp::Jump)
                in.target = remap_old(in.target);
            else if (in.op == IrOp::Branch) {
                in.target = remap_old(in.target);
                in.target2 = remap_old(in.target2);
            }
        }
    };

    for (size_t k = 0; k < insert_at; ++k) {
        Block& b = fn.blocks[k];
        if (k != bi) remap_old_block(b);
        fresh.push_back(std::move(b));
    }
    Block& head = fresh.back();
    for (auto& in : entry_insts) head.insts.push_back(std::move(in));
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = clone_start;
        head.insts.push_back(std::move(j));
    }

    for (int cb = 0; cb < nb; ++cb) {
        const Block& srcb = callee.blocks[static_cast<size_t>(cb)];
        Block db;
        db.name = "inline." + srcb.name;
        for (const Inst& src : srcb.insts) {
            if (src.op == IrOp::Return) {
                emit_return(db.insts, src);
                Inst j;
                j.op = IrOp::Jump;
                j.target = cont;
                db.insts.push_back(std::move(j));
                break;
            }
            clone_into(db.insts, src, clone_start);
        }
        fresh.push_back(std::move(db));
    }

    Block contb;
    contb.name = "inline.cont";
    if (ret_slot >= 0) {
        Inst ld;
        ld.op = IrOp::LoadSlot;
        ld.idx = ret_slot;
        ld.dst = call.dst;
        ld.ty = call.ty;
        contb.insts.push_back(std::move(ld));
    }
    for (auto& in : suffix) {
        if (in.op == IrOp::Jump)
            in.target = remap_old(in.target);
        else if (in.op == IrOp::Branch) {
            in.target = remap_old(in.target);
            in.target2 = remap_old(in.target2);
        }
        contb.insts.push_back(std::move(in));
    }
    fresh.push_back(std::move(contb));

    for (size_t k = insert_at; k < fn.blocks.size(); ++k) {
        Block& b = fn.blocks[k];
        remap_old_block(b);
        fresh.push_back(std::move(b));
    }
    fn.blocks = std::move(fresh);
}

}  // namespace

namespace optdetail {

std::unordered_map<int, int> collect_static_func_globals(const IRModule& m) {
    std::unordered_map<int, int> out;
    // 非入口模块的全局符号可能被导入方改写，静态函数假设不成立。
    if (!m.is_entry) return out;
    struct G {
        int count = 0;
        int fidx = -1;
        bool ok = true;
    };
    std::vector<G> gs(m.globals.size());
    for (const auto& fp : m.funcs) {
        std::unordered_map<VReg, DefRef> defs;
        for (const auto& b : fp->blocks)
            for (const auto& in : b.insts)
                if (in.dst != kNoReg)
                    defs[in.dst] = DefRef{in.op, in.imm, in.idx};
        for (const auto& b : fp->blocks)
            for (const auto& in : b.insts) {
                if (in.op != IrOp::StoreGlobal || in.idx < 0 ||
                    in.idx >= static_cast<int>(gs.size()))
                    continue;
                G& g = gs[static_cast<size_t>(in.idx)];
                ++g.count;
                auto it = defs.find(in.a);
                if (it == defs.end() || it->second.op != IrOp::FuncAddr ||
                    it->second.imm < 0)
                    g.ok = false;
                else if (g.fidx < 0)
                    g.fidx = static_cast<int>(it->second.imm);
                else if (g.fidx != it->second.imm)
                    g.ok = false;
            }
    }
    for (size_t i = 0; i < gs.size(); ++i)
        if (gs[i].count == 1 && gs[i].ok && gs[i].fidx >= 0 &&
            gs[i].fidx < static_cast<int>(m.funcs.size()))
            out[static_cast<int>(i)] = gs[i].fidx;
    return out;
}

// 能沿静态调用边走回自身的函数。递归函数不内联：否则克隆体里的
// 递归调用会再次命中，把预算耗在无意义的展开上。
std::vector<char> recursive_functions(const IRModule& m,
                                      const std::unordered_map<int, int>& gfunc) {
    const size_t n = m.funcs.size();
    std::vector<std::vector<int>> edges(n);
    for (size_t fi = 0; fi < n; ++fi) {
        const IRFunction& f = *m.funcs[fi];
        std::unordered_map<VReg, DefRef> defs;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts)
                if (in.dst != kNoReg) defs[in.dst] = DefRef{in.op, in.imm, in.idx};
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::CallDirect) {
                    if (in.idx >= 0 && in.idx < static_cast<int>(n))
                        edges[fi].push_back(in.idx);
                    continue;
                }
                if (in.op != IrOp::Call) continue;
                int c = -1;
                auto it = defs.find(in.a);
                if (it != defs.end()) {
                    if (it->second.op == IrOp::FuncAddr)
                        c = static_cast<int>(it->second.imm);
                    else if (it->second.op == IrOp::LoadGlobal) {
                        auto git = gfunc.find(static_cast<int>(it->second.idx));
                        if (git != gfunc.end()) c = git->second;
                    }
                }
                if (c >= 0 && c < static_cast<int>(n)) edges[fi].push_back(c);
            }
    }
    std::vector<char> rec(n, 0);
    std::vector<char> seen(n, 0);
    for (size_t start = 0; start < n && !rec[start]; ++start) {
        std::fill(seen.begin(), seen.end(), 0);
        std::vector<int> stack = edges[start];
        while (!stack.empty()) {
            int v = stack.back();
            stack.pop_back();
            if (v == static_cast<int>(start)) {
                rec[start] = 1;
                break;
            }
            if (seen[static_cast<size_t>(v)]) continue;
            seen[static_cast<size_t>(v)] = 1;
            for (int w : edges[static_cast<size_t>(v)]) stack.push_back(w);
        }
    }
    return rec;
}

bool inline_calls_in_function(IRModule& m, IRFunction& fn, int fi,
                              const std::unordered_map<int, int>& gfunc,
                              const std::vector<char>& recursive) {
    // 生成器/协程帧的槽数在 gen.new 创建时已经固定，优化期再追加槽会越界。
    if (fn.is_generator) return false;

    bool any = false;
    int sites = 0;
    int growth = 0;
    while (sites < 24 && growth < 256) {
        std::unordered_map<VReg, DefRef> defs;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (in.dst != kNoReg)
                    defs[in.dst] = DefRef{in.op, in.imm, in.idx};

        bool did = false;
        for (size_t bi = 0; bi < fn.blocks.size() && !did; ++bi) {
            for (size_t ii = 0; ii < fn.blocks[bi].insts.size(); ++ii) {
                const Inst call = fn.blocks[bi].insts[ii];
                int cidx = -1;
                if (call.op == IrOp::CallDirect) {
                    // 去虚化/静态调用已经是直接下标，不需要再解析函数值。
                    cidx = call.idx;
                } else if (call.op == IrOp::Call) {
                    auto dit = defs.find(call.a);
                    if (dit != defs.end()) {
                        if (dit->second.op == IrOp::FuncAddr)
                            cidx = static_cast<int>(dit->second.imm);
                        else if (dit->second.op == IrOp::LoadGlobal) {
                            auto git = gfunc.find(static_cast<int>(dit->second.idx));
                            if (git != gfunc.end()) cidx = git->second;
                        }
                    }
                } else {
                    continue;
                }
                if (cidx < 0 || cidx == fi || cidx == m.entry) continue;
                if (recursive[static_cast<size_t>(cidx)]) continue;
                const IRFunction& callee = *m.funcs[static_cast<size_t>(cidx)];
                if (!callee.can_inline || callee.is_generator || callee.has_handlers ||
                    callee.blocks.empty() || callee.params.size() != call.args.size())
                    continue;
                int n = function_inst_count(callee);
                if (n > 48 || callee.blocks.size() > 12) continue;

                InlineFacts facts = analyze_callee(callee);
                inline_at(fn, bi, ii, callee, call, facts);
                growth += n + 2;
                ++sites;
                did = true;
                any = true;
                break;
            }
        }
        if (!did) break;
    }
    return any;
}

// 调用点的函数值能静态解析、且 callee 不读闭包环境时，把闭包间接调用
// 改成直接调用：省掉一次全局装载和一次依赖装载。
bool rewrite_direct_calls(IRModule& m, IRFunction& fn,
                          const std::unordered_map<int, int>& gfunc) {
    std::unordered_map<VReg, DefRef> defs;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts)
            if (in.dst != kNoReg) defs[in.dst] = DefRef{in.op, in.imm, in.idx};

    bool changed = false;
    for (auto& b : fn.blocks) {
        for (auto& in : b.insts) {
            if (in.op != IrOp::Call) continue;
            int cidx = -1;
            auto dit = defs.find(in.a);
            if (dit != defs.end()) {
                if (dit->second.op == IrOp::FuncAddr)
                    cidx = static_cast<int>(dit->second.imm);
                else if (dit->second.op == IrOp::LoadGlobal) {
                    auto git = gfunc.find(static_cast<int>(dit->second.idx));
                    if (git != gfunc.end()) cidx = git->second;
                }
            }
            if (cidx < 0 || cidx == m.entry) continue;
            const IRFunction& callee = *m.funcs[static_cast<size_t>(cidx)];
            // 有闭包环境的 callee 需要 %r10 传环境，不能走直接调用。
            if (callee.env_slot >= 0 || callee.params.size() != in.args.size()) continue;
            in.op = IrOp::CallDirect;
            in.idx = cidx;
            in.a = kNoReg;
            changed = true;
        }
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
