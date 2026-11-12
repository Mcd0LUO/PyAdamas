// opt_loop.cpp —— 循环不变量外提与检查型下标版本化
#include "opt_detail.h"

namespace pyadamas {

using namespace optdetail;

namespace {

// ── 循环不变量外提（LICM） ─────────────────────────────────────
//
// 只搬不会抛异常的纯计算和只读装载：纯算术/比较/数值转换、循环内没被写过
// 的局部槽装载、循环内没有调用且没被写过的全局装载。检查型下标会抛越界，
// 外提会让零次循环也执行它，所以不搬。
// 外提点是一个新增前置块：循环外的前驱先跳到它，它再跳到循环头。这样它
// 只在真正进入循环时执行一次。

bool is_terminator(IrOp op) {
    switch (op) {
        case IrOp::Jump:
        case IrOp::Branch:
        case IrOp::Return:
        case IrOp::GenYield:
        case IrOp::GenAwait:
        case IrOp::GenRethrow:
        case IrOp::GenDone:
            return true;
        default:
            return false;
    }
}

struct Cfg {
    std::vector<std::vector<int>> succ;
    std::vector<std::vector<int>> pred;
    std::vector<char> reach;
};

Cfg build_cfg(const IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    Cfg g;
    g.succ.assign(static_cast<size_t>(nb), {});
    g.pred.assign(static_cast<size_t>(nb), {});
    for (int b = 0; b < nb; ++b) {
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            if (b + 1 < nb) g.succ[static_cast<size_t>(b)].push_back(b + 1);
            continue;
        }
        const Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            if (last.target >= 0 && last.target < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target);
        } else if (last.op == IrOp::Branch) {
            if (last.target >= 0 && last.target < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target);
            if (last.target2 >= 0 && last.target2 < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target2);
        } else if (!is_terminator(last.op) && b + 1 < nb) {
            g.succ[static_cast<size_t>(b)].push_back(b + 1);
        }
    }
    for (int b = 0; b < nb; ++b)
        for (int s : g.succ[static_cast<size_t>(b)]) g.pred[static_cast<size_t>(s)].push_back(b);
    g.reach.assign(static_cast<size_t>(nb), 0);
    if (nb > 0) {
        std::vector<int> work{0};
        g.reach[0] = 1;
        while (!work.empty()) {
            int b = work.back();
            work.pop_back();
            for (int s : g.succ[static_cast<size_t>(b)])
                if (!g.reach[static_cast<size_t>(s)]) {
                    g.reach[static_cast<size_t>(s)] = 1;
                    work.push_back(s);
                }
        }
    }
    return g;
}

// dom[b] 是 b 的支配块集合。
std::vector<std::vector<char>> compute_dom(const Cfg& g) {
    const int nb = static_cast<int>(g.succ.size());
    std::vector<std::vector<char>> dom(static_cast<size_t>(nb),
                                       std::vector<char>(static_cast<size_t>(nb), 0));
    for (int b = 0; b < nb; ++b)
        if (g.reach[static_cast<size_t>(b)])
            dom[static_cast<size_t>(b)].assign(static_cast<size_t>(nb), 1);
    if (nb == 0) return dom;
    dom[0].assign(static_cast<size_t>(nb), 0);
    dom[0][0] = 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < nb; ++b) {
            if (!g.reach[static_cast<size_t>(b)]) continue;
            std::vector<char> nd(static_cast<size_t>(nb), 1);
            bool first = true;
            for (int p : g.pred[static_cast<size_t>(b)]) {
                if (!g.reach[static_cast<size_t>(p)]) continue;
                if (first) {
                    nd = dom[static_cast<size_t>(p)];
                    first = false;
                    continue;
                }
                const auto& dp = dom[static_cast<size_t>(p)];
                for (size_t i = 0; i < nd.size(); ++i) nd[i] = nd[i] && dp[i];
            }
            nd[static_cast<size_t>(b)] = 1;
            if (nd != dom[static_cast<size_t>(b)]) {
                dom[static_cast<size_t>(b)] = std::move(nd);
                changed = true;
            }
        }
    }
    return dom;
}

bool numeric_ty(const TypePtr& t) {
    return t && (t->kind == TyKind::Bool || t->kind == TyKind::Int ||
                 t->kind == TyKind::Float);
}

bool hoist_loop(IRFunction& fn, int h, const std::vector<char>& body,
                const std::vector<std::vector<char>>& dom, size_t n_globals) {
    const int nb = static_cast<int>(fn.blocks.size());
    const auto in_loop = [&](int b) { return body[static_cast<size_t>(b)] != 0; };

    // 循环外前驱；没有就没法插前置块。
    std::vector<int> outside;
    for (int b = 0; b < nb; ++b) {
        if (in_loop(b)) continue;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool to_h = false;
        if (!insts.empty()) {
            const Inst& last = insts.back();
            if (last.op == IrOp::Jump)
                to_h = last.target == h;
            else if (last.op == IrOp::Branch)
                to_h = last.target == h || last.target2 == h;
            else if (!is_terminator(last.op) && b + 1 == h)
                to_h = true;
        } else if (b + 1 == h) {
            to_h = true;
        }
        if (to_h) outside.push_back(b);
    }
    if (outside.empty()) return false;

    // 循环内的写/调用事实。
    std::vector<char> slot_written(fn.slot_ty.size(), 0);
    std::vector<char> global_written(n_globals, 0);
    bool has_call = false;
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            switch (in.op) {
                case IrOp::StoreSlot:
                    if (in.idx >= 0 && in.idx < static_cast<int>(slot_written.size()))
                        slot_written[static_cast<size_t>(in.idx)] = 1;
                    break;
                case IrOp::StoreGlobal:
                    if (in.idx >= 0 && in.idx < static_cast<int>(global_written.size()))
                        global_written[static_cast<size_t>(in.idx)] = 1;
                    break;
                case IrOp::Call:
                case IrOp::CallRt:
                case IrOp::CallExtern:
                case IrOp::CallDirect:
                case IrOp::VCall:
                case IrOp::VCallStatic:
                case IrOp::SuperVCall:
                    has_call = true;
                    break;
                default:
                    break;
            }
        }
    }
    // 地址被取过的槽不搬（可能被间接写）。
    std::vector<char> slot_addr(fn.slot_ty.size(), 0);
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::SlotAddr && in.idx >= 0 &&
                in.idx < static_cast<int>(slot_addr.size()))
                slot_addr[static_cast<size_t>(in.idx)] = 1;

    std::unordered_map<VReg, int> def_block;
    std::unordered_set<VReg> multi_def;
    for (int b = 0; b < nb; ++b)
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
            if (in.dst != kNoReg) {
                auto [it, fresh] = def_block.emplace(in.dst, b);
                if (!fresh) multi_def.insert(in.dst);
            }

    std::vector<Inst> hoisted;
    std::unordered_set<VReg> hoisted_dst;

    auto operand_ok = [&](VReg r) {
        if (r == kNoReg) return true;
        if (hoisted_dst.contains(r)) return true;
        if (multi_def.contains(r)) return false;
        auto it = def_block.find(r);
        if (it == def_block.end()) return true;  // 形参等无定义值
        int db = it->second;
        if (in_loop(db)) return false;
        return dom[static_cast<size_t>(h)][static_cast<size_t>(db)] != 0;
    };
    auto operand_numeric = [&](VReg r) {
        return r != kNoReg && r < static_cast<VReg>(fn.vreg_ty.size()) &&
               numeric_ty(fn.vreg_ty[r]);
    };
    auto hoistable = [&](const Inst& in) {
        if (in.dst == kNoReg) return false;
        switch (in.op) {
            case IrOp::ConstInt:
            case IrOp::ConstFloat:
            case IrOp::ConstStr:
            case IrOp::ConstNone:
            case IrOp::FuncAddr:
                return true;
            case IrOp::Mov:
                return operand_ok(in.a);
            case IrOp::Cmp:
                return operand_ok(in.a) && operand_ok(in.b) && operand_numeric(in.a) &&
                       operand_numeric(in.b);
            case IrOp::Un: {
                TypePtr at = in.a < static_cast<VReg>(fn.vreg_ty.size())
                                 ? fn.vreg_ty[in.a]
                                 : nullptr;
                if (!numeric_ty(at)) return false;
                return operand_ok(in.a);
            }
            case IrOp::Bin: {
                if (!numeric_ty(in.ty)) return false;
                bool ok = ir_is_float(in.ty) ? bin_is_safe_float(in.bop)
                                             : bin_is_safe_int(in.bop);
                return ok && operand_ok(in.a) && operand_ok(in.b) && operand_numeric(in.a) &&
                       operand_numeric(in.b);
            }
            case IrOp::Convert: {
                TypePtr at = in.a < static_cast<VReg>(fn.vreg_ty.size())
                                 ? fn.vreg_ty[in.a]
                                 : nullptr;
                return numeric_ty(in.ty) && numeric_ty(at) && operand_ok(in.a);
            }
            case IrOp::LoadSlot: {
                if (in.idx < 0 || in.idx >= static_cast<int>(slot_written.size()))
                    return false;
                return !slot_written[static_cast<size_t>(in.idx)] &&
                       !slot_addr[static_cast<size_t>(in.idx)];
            }
            case IrOp::LoadGlobal: {
                if (in.idx < 0 || in.idx >= static_cast<int>(global_written.size()))
                    return false;
                return !has_call && !global_written[static_cast<size_t>(in.idx)];
            }
            default:
                return false;
        }
    };

    bool again = true;
    while (again) {
        again = false;
        for (int b = 0; b < nb; ++b) {
            if (!in_loop(b)) continue;
            auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
            for (size_t i = 0; i < insts.size();) {
                if (!hoistable(insts[i])) {
                    ++i;
                    continue;
                }
                hoisted.push_back(std::move(insts[i]));
                hoisted_dst.insert(hoisted.back().dst);
                insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(i));
                again = true;
            }
        }
    }
    if (hoisted.empty()) return false;

    const int pidx = nb;
    for (int b : outside) {
        auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool terminated = !insts.empty() && is_terminator(insts.back().op);
        bool redirected = false;
        if (!insts.empty()) {
            Inst& last = insts.back();
            if (last.op == IrOp::Jump && last.target == h) {
                last.target = pidx;
                redirected = true;
            } else if (last.op == IrOp::Branch) {
                if (last.target == h) {
                    last.target = pidx;
                    redirected = true;
                }
                if (last.target2 == h) {
                    last.target2 = pidx;
                    redirected = true;
                }
            }
        }
        if (!terminated && !redirected && b + 1 == h) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = pidx;
            insts.push_back(std::move(j));
        }
    }

    Block pre;
    pre.name = "licm.pre";
    pre.insts = std::move(hoisted);
    Inst j;
    j.op = IrOp::Jump;
    j.target = h;
    pre.insts.push_back(std::move(j));
    fn.blocks.push_back(std::move(pre));
    return true;
}

// ── 检查型下标外提（循环版本化） ─────────────────────────────
//
// 循环里 `row[i]` 这类下标如果 base 和 index 都是循环不变量，且循环内
// 没有调用、没有下标写入（长度和元素都不会变），就可以在循环入口检一次
// 边界：通过就走“无检查”的循环副本，不通过走原循环（越界照常在第一次
// 执行的迭代里抛）。两版循环共用槽，只有一版会执行。
// 负下标落到原循环，语义不变；检查型下标本身不会被外提执行，零次循环
// 不会因此多抛异常。

bool version_bounds_loop(IRFunction& fn, int h, const std::vector<char>& body,
                         const std::vector<std::vector<char>>& dom) {
    const int nb = static_cast<int>(fn.blocks.size());
    const auto in_loop = [&](int b) { return body[static_cast<size_t>(b)] != 0; };

    // 循环体内有调用或下标写入时，长度/元素可能在迭代间变化。
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            switch (in.op) {
                case IrOp::Call:
                case IrOp::CallRt:
                case IrOp::CallExtern:
                case IrOp::CallDirect:
                case IrOp::VCall:
                case IrOp::VCallStatic:
                case IrOp::SuperVCall:
                case IrOp::IndexStore:
                    return false;
                default:
                    break;
            }
        }
    }

    std::unordered_map<VReg, int> def_block;
    std::unordered_set<VReg> multi_def;
    std::unordered_set<VReg> used_outside;
    for (int b = 0; b < nb; ++b) {
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            auto touch = [&](VReg r) {
                if (r != kNoReg && !in_loop(b)) used_outside.insert(r);
            };
            touch(in.a);
            touch(in.b);
            touch(in.c);
            for (VReg r : in.args) touch(r);
            if (in.dst != kNoReg) {
                auto [it, fresh] = def_block.emplace(in.dst, b);
                if (!fresh) multi_def.insert(in.dst);
            }
        }
    }
    auto invariant = [&](VReg r) {
        if (r == kNoReg || multi_def.contains(r)) return false;
        auto it = def_block.find(r);
        if (it == def_block.end()) return true;
        if (in_loop(it->second)) return false;
        return dom[static_cast<size_t>(h)][static_cast<size_t>(it->second)] != 0;
    };

    struct Cand {
        VReg dst = kNoReg;
        VReg base = kNoReg;
        VReg idx = kNoReg;
        TypePtr ty;
    };
    std::vector<Cand> cands;
    std::unordered_set<VReg> cand_dsts;
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            if (in.op != IrOp::IndexLoad || in.dst == kNoReg) continue;
            if (used_outside.contains(in.dst)) continue;
            if (!invariant(in.a) || !invariant(in.b)) continue;
            TypePtr bt = in.a < static_cast<VReg>(fn.vreg_ty.size())
                             ? fn.vreg_ty[in.a]
                             : nullptr;
            TypePtr it = in.b < static_cast<VReg>(fn.vreg_ty.size())
                             ? fn.vreg_ty[in.b]
                             : nullptr;
            if (!bt || (bt->kind != TyKind::List && bt->kind != TyKind::Tuple)) continue;
            if (!numeric_ty(it)) continue;
            cands.push_back(Cand{in.dst, in.a, in.b, in.ty});
            cand_dsts.insert(in.dst);
        }
    }
    if (cands.empty()) return false;

    // 循环外前驱（和 LICM 一样，插前置块需要至少一个入口边）。
    std::vector<int> outside;
    for (int b = 0; b < nb; ++b) {
        if (in_loop(b)) continue;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool to_h = false;
        if (!insts.empty()) {
            const Inst& last = insts.back();
            if (last.op == IrOp::Jump)
                to_h = last.target == h;
            else if (last.op == IrOp::Branch)
                to_h = last.target == h || last.target2 == h;
            else if (!is_terminator(last.op) && b + 1 == h)
                to_h = true;
        } else if (b + 1 == h) {
            to_h = true;
        }
        if (to_h) outside.push_back(b);
    }
    if (outside.empty()) return false;

    std::vector<int> body_sorted;
    for (int b = 0; b < nb; ++b)
        if (in_loop(b)) body_sorted.push_back(b);

    const int guard_idx = nb;
    const int fast_entry_idx = nb + 1;
    const int clone_base = nb + 2;
    std::unordered_map<int, int> block_map;
    for (size_t k = 0; k < body_sorted.size(); ++k)
        block_map[body_sorted[k]] = clone_base + static_cast<int>(k);

    auto alloc_v = [&](TypePtr t) {
        VReg v = static_cast<VReg>(fn.vreg_ty.size());
        fn.vreg_ty.push_back(t ? t : t_unknown());
        return v;
    };

    // 快速循环里每个候选下标对应一个外提好的元素。
    std::vector<VReg> elems;
    std::unordered_map<VReg, VReg> redirect;
    for (const auto& c : cands) {
        VReg e = alloc_v(c.ty);
        elems.push_back(e);
        redirect[c.dst] = e;
    }

    std::vector<VReg> vmap(fn.vreg_ty.size(), kNoReg);
    for (int b : body_sorted) {
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            if (in.dst == kNoReg || cand_dsts.contains(in.dst)) continue;
            vmap[in.dst] = alloc_v(in.ty);
        }
    }
    auto remap_v = [&](VReg r) -> VReg {
        if (r == kNoReg) return r;
        auto it = redirect.find(r);
        if (it != redirect.end()) return it->second;
        if (r < static_cast<VReg>(vmap.size()) && vmap[r] != kNoReg) return vmap[r];
        return r;
    };

    std::vector<Inst> guard;
    VReg ok = kNoReg;
    for (const auto& c : cands) {
        VReg len = alloc_v(t_int());
        Inst ln;
        ln.op = IrOp::Len;
        ln.a = c.base;
        ln.dst = len;
        ln.ty = t_int();
        guard.push_back(std::move(ln));

        VReg zero = alloc_v(t_int());
        Inst zc;
        zc.op = IrOp::ConstInt;
        zc.imm = 0;
        zc.dst = zero;
        zc.ty = t_int();
        guard.push_back(std::move(zc));

        VReg ge = alloc_v(t_bool());
        Inst g;
        g.op = IrOp::Cmp;
        g.cop = CmpKind::Ge;
        g.a = c.idx;
        g.b = zero;
        g.dst = ge;
        g.ty = t_bool();
        guard.push_back(std::move(g));

        VReg lt = alloc_v(t_bool());
        Inst l;
        l.op = IrOp::Cmp;
        l.cop = CmpKind::Lt;
        l.a = c.idx;
        l.b = len;
        l.dst = lt;
        l.ty = t_bool();
        guard.push_back(std::move(l));

        VReg hit = alloc_v(t_int());
        Inst an;
        an.op = IrOp::Bin;
        an.bop = BinKind::BitAnd;
        an.a = ge;
        an.b = lt;
        an.dst = hit;
        an.ty = t_int();
        guard.push_back(std::move(an));

        if (ok == kNoReg) {
            ok = hit;
        } else {
            VReg both = alloc_v(t_int());
            Inst an2;
            an2.op = IrOp::Bin;
            an2.bop = BinKind::BitAnd;
            an2.a = ok;
            an2.b = hit;
            an2.dst = both;
            an2.ty = t_int();
            guard.push_back(std::move(an2));
            ok = both;
        }
    }
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = ok;
        br.target = fast_entry_idx;
        br.target2 = h;
        guard.push_back(std::move(br));
    }

    std::vector<Inst> fast_entry;
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        Inst ld;
        ld.op = IrOp::IndexLoadUnchecked;
        ld.a = cands[ci].base;
        ld.b = cands[ci].idx;
        ld.dst = elems[ci];
        ld.ty = cands[ci].ty;
        fast_entry.push_back(std::move(ld));
    }
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = block_map[h];
        fast_entry.push_back(std::move(j));
    }

    std::vector<Block> clones;
    for (int b : body_sorted) {
        Block cb;
        cb.name = "ver." + fn.blocks[static_cast<size_t>(b)].name;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            if (in.op == IrOp::IndexLoad && cand_dsts.contains(in.dst)) continue;
            Inst out = in;
            out.a = remap_v(in.a);
            out.b = remap_v(in.b);
            out.c = remap_v(in.c);
            for (VReg& r : out.args) r = remap_v(r);
            if (out.dst != kNoReg) out.dst = vmap[in.dst];
            if (out.op == IrOp::Jump) {
                auto m = block_map.find(out.target);
                if (m != block_map.end()) out.target = m->second;
            } else if (out.op == IrOp::Branch) {
                auto m = block_map.find(out.target);
                if (m != block_map.end()) out.target = m->second;
                auto m2 = block_map.find(out.target2);
                if (m2 != block_map.end()) out.target2 = m2->second;
            }
            cb.insts.push_back(std::move(out));
        }
        clones.push_back(std::move(cb));
    }

    // 循环外前驱改跳到守卫块。
    for (int b : outside) {
        auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool terminated = !insts.empty() && is_terminator(insts.back().op);
        bool redirected = false;
        if (!insts.empty()) {
            Inst& last = insts.back();
            if (last.op == IrOp::Jump && last.target == h) {
                last.target = guard_idx;
                redirected = true;
            } else if (last.op == IrOp::Branch) {
                if (last.target == h) {
                    last.target = guard_idx;
                    redirected = true;
                }
                if (last.target2 == h) {
                    last.target2 = guard_idx;
                    redirected = true;
                }
            }
        }
        if (!terminated && !redirected && b + 1 == h) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = guard_idx;
            insts.push_back(std::move(j));
        }
    }

    Block gb;
    gb.name = "ver.guard";
    gb.insts = std::move(guard);
    fn.blocks.push_back(std::move(gb));

    Block fb;
    fb.name = "ver.entry";
    fb.insts = std::move(fast_entry);
    fn.blocks.push_back(std::move(fb));

    for (auto& c : clones) fn.blocks.push_back(std::move(c));
    return true;
}

}  // namespace

namespace optdetail {

bool licm_function(IRFunction& fn, size_t n_globals) {
    if (fn.has_handlers || fn.is_generator) return false;
    bool any = false;
    for (int guard = 0; guard < 64; ++guard) {
        const int nb = static_cast<int>(fn.blocks.size());
        if (nb < 2) break;
        Cfg g = build_cfg(fn);
        auto dom = compute_dom(g);

        std::vector<std::vector<char>> bodies(static_cast<size_t>(nb));
        bool found = false;
        for (int n = 0; n < nb; ++n) {
            if (!g.reach[static_cast<size_t>(n)]) continue;
            for (int h : g.succ[static_cast<size_t>(n)]) {
                if (h < 0 || h >= nb || !dom[static_cast<size_t>(n)][static_cast<size_t>(h)])
                    continue;
                auto& body = bodies[static_cast<size_t>(h)];
                if (body.empty()) {
                    body.assign(static_cast<size_t>(nb), 0);
                    body[static_cast<size_t>(h)] = 1;
                }
                std::vector<int> work{n};
                while (!work.empty()) {
                    int x = work.back();
                    work.pop_back();
                    if (x == h || body[static_cast<size_t>(x)]) continue;
                    body[static_cast<size_t>(x)] = 1;
                    for (int p : g.pred[static_cast<size_t>(x)]) work.push_back(p);
                }
                found = true;
            }
        }
        if (!found) break;

        bool changed = false;
        for (int h = 0; h < nb && !changed; ++h) {
            if (bodies[static_cast<size_t>(h)].empty()) continue;
            changed = hoist_loop(fn, h, bodies[static_cast<size_t>(h)], dom, n_globals);
        }
        if (!changed) break;
        any = true;
    }
    return any;
}

bool version_bounds_function(IRFunction& fn) {
    if (fn.has_handlers || fn.is_generator) return false;
    std::unordered_set<int> done;
    bool any = false;
    for (int guard = 0; guard < 8; ++guard) {
        const int nb = static_cast<int>(fn.blocks.size());
        if (nb < 2) break;
        Cfg g = build_cfg(fn);
        auto dom = compute_dom(g);

        std::vector<std::vector<char>> bodies(static_cast<size_t>(nb));
        bool found = false;
        for (int n = 0; n < nb; ++n) {
            if (!g.reach[static_cast<size_t>(n)]) continue;
            for (int h : g.succ[static_cast<size_t>(n)]) {
                if (h < 0 || h >= nb || !dom[static_cast<size_t>(n)][static_cast<size_t>(h)])
                    continue;
                auto& body = bodies[static_cast<size_t>(h)];
                if (body.empty()) {
                    body.assign(static_cast<size_t>(nb), 0);
                    body[static_cast<size_t>(h)] = 1;
                }
                std::vector<int> work{n};
                while (!work.empty()) {
                    int x = work.back();
                    work.pop_back();
                    if (x == h || body[static_cast<size_t>(x)]) continue;
                    body[static_cast<size_t>(x)] = 1;
                    for (int p : g.pred[static_cast<size_t>(x)]) work.push_back(p);
                }
                found = true;
            }
        }
        if (!found) break;

        bool changed = false;
        for (int h = 0; h < nb && !changed; ++h) {
            if (bodies[static_cast<size_t>(h)].empty() || done.contains(h)) continue;
            if (version_bounds_loop(fn, h, bodies[static_cast<size_t>(h)], dom)) {
                done.insert(h);
                changed = true;
            } else {
                done.insert(h);
            }
        }
        if (!changed) break;
        any = true;
    }
    return any;
}

// ── 循环旋转 ───────────────────────────────────────────────────
//
// while 循环的形状是「头块算条件 + branch；体块结尾 jmp 头块」。每次迭代
// 都要执行一条无条件 jmp。把头的条件计算克隆成一个 latch 块、插在体块
// 后面，体块的回边改成跳到 latch：条件求值次数不变（k+1 次），但
// 「jmp 头块」被 latch 末尾的 cmp + 条件跳转替代，而体块→latch 是相邻块，
// 代码生成会省掉这条 jmp。
//
// 前提（任一不满足就跳过这个循环）：
//  - 函数没有 handler / 不是生成器（块下标改动不碰 landing pad）；
//  - 头块不是函数入口块（入口块的前缀是 rc_mark 之类的序言）；
//  - 头块末尾是 cmp+branch，且 cmp 的操作数在头块内定义时只被头块使用；
//  - 指向头块的回边恰好一条，且该块以 jump 头块结尾。
bool rotate_one_loop(IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    for (int h = 1; h < nb; ++h) {
        auto& hi = fn.blocks[static_cast<size_t>(h)].insts;
        if (hi.size() < 2) continue;
        const Inst& br = hi.back();
        if (br.op != IrOp::Branch || br.a == kNoReg) continue;
        const Inst& cmp = hi[hi.size() - 2];
        if (cmp.op != IrOp::Cmp || cmp.dst != br.a) continue;

        // 从头块可达的块才算循环体；前置块也有「jump 头块」，那是入口边。
        std::vector<char> reach(static_cast<size_t>(nb), 0);
        {
            std::vector<int> stack = {h};
            reach[static_cast<size_t>(h)] = 1;
            while (!stack.empty()) {
                int b = stack.back();
                stack.pop_back();
                if (b < 0 || b >= nb) continue;
                auto& bi = fn.blocks[static_cast<size_t>(b)].insts;
                if (bi.empty()) continue;
                const Inst& last = bi.back();
                auto visit = [&](int t) {
                    if (t >= 0 && t < nb && !reach[static_cast<size_t>(t)]) {
                        reach[static_cast<size_t>(t)] = 1;
                        stack.push_back(t);
                    }
                };
                if (last.op == IrOp::Jump) visit(last.target);
                else if (last.op == IrOp::Branch) {
                    visit(last.target);
                    visit(last.target2);
                } else if (last.op != IrOp::Return && last.op != IrOp::GenYield &&
                           last.op != IrOp::GenAwait && last.op != IrOp::GenRethrow &&
                           last.op != IrOp::GenDone) {
                    visit(b + 1);
                }
            }
        }

        // 头块必须有循环外的前驱（真正的入口边）；否则它只是上一轮旋转
        // 加出来的 latch，再转一次只会复制出死块。
        bool has_outside_pred = false;
        for (int b = 0; b < nb && !has_outside_pred; ++b) {
            if (reach[static_cast<size_t>(b)]) continue;
            auto& bi = fn.blocks[static_cast<size_t>(b)].insts;
            if (bi.empty()) {
                has_outside_pred = (b + 1 == h);
                continue;
            }
            const Inst& last = bi.back();
            if (last.op == IrOp::Jump) {
                has_outside_pred = (last.target == h);
            } else if (last.op == IrOp::Branch) {
                has_outside_pred = (last.target == h || last.target2 == h);
            } else if (last.op != IrOp::Return && last.op != IrOp::GenYield &&
                       last.op != IrOp::GenAwait && last.op != IrOp::GenRethrow &&
                       last.op != IrOp::GenDone) {
                has_outside_pred = (b + 1 == h);
            }
        }
        if (!has_outside_pred) continue;

        // 唯一回边（只数循环体内跳回头块的块）
        int B = -1;
        bool multi = false;
        for (int b = 0; b < nb; ++b) {
            if (b == h) continue;
            if (!reach[static_cast<size_t>(b)]) continue;
            auto& bi = fn.blocks[static_cast<size_t>(b)].insts;
            if (!bi.empty() && bi.back().op == IrOp::Jump && bi.back().target == h) {
                if (B >= 0) {
                    multi = true;
                    break;
                }
                B = b;
            }
        }
        if (multi || B < 0) continue;
        int E = -1;
        if (br.target == B) E = br.target2;
        else if (br.target2 == B) E = br.target;
        else continue;
        if (E == h || E == B) continue;

        // 头块里 cmp 之前定义的 vreg 不能在别的块被使用：克隆版只会
        // 在入口执行一次，外部读者会拿到过期值。
        bool external_use = false;
        for (size_t k = 0; k + 1 < hi.size() && !external_use; ++k) {
            VReg dv = hi[k].dst;
            if (dv == kNoReg) continue;
            for (int b = 0; b < nb && !external_use; ++b) {
                if (b == h) continue;
                for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
                    if (in.a == dv || in.b == dv || in.c == dv) {
                        external_use = true;
                        break;
                    }
                    for (VReg r : in.args)
                        if (r == dv) {
                            external_use = true;
                            break;
                        }
                    if (external_use) break;
                }
            }
        }
        if (external_use) continue;

        // 克隆头块（含 branch）到 latch，vreg 全部换新。
        Block latch;
        latch.name = "rot.latch";
        std::unordered_map<VReg, VReg> sub;
        auto mv = [&](VReg r) {
            auto it = sub.find(r);
            return it == sub.end() ? r : it->second;
        };
        latch.insts.reserve(hi.size());
        for (const Inst& src : hi) {
            Inst in = src;
            in.a = mv(src.a);
            in.b = mv(src.b);
            in.c = mv(src.c);
            for (VReg& r : in.args) r = mv(r);
            if (src.dst != kNoReg) {
                VReg nv = static_cast<VReg>(fn.vreg_ty.size());
                fn.vreg_ty.push_back(src.ty);
                sub[src.dst] = nv;
                in.dst = nv;
            }
            latch.insts.push_back(std::move(in));
        }

        // 插入位置 B+1：所有 >= B+1 的块整体后移一格。
        auto bump = [&](int t) { return t >= B + 1 ? t + 1 : t; };
        for (auto& blk : fn.blocks) {
            if (blk.insts.empty()) continue;
            Inst& last = blk.insts.back();
            if (last.op == IrOp::Jump) {
                last.target = bump(last.target);
            } else if (last.op == IrOp::Branch) {
                last.target = bump(last.target);
                last.target2 = bump(last.target2);
            }
        }
        for (auto& in : latch.insts) {
            if (in.op == IrOp::Jump) {
                in.target = bump(in.target);
            } else if (in.op == IrOp::Branch) {
                in.target = bump(in.target);
                in.target2 = bump(in.target2);
            }
        }
        Inst& bj = fn.blocks[static_cast<size_t>(B)].insts.back();
        bj.target = B + 1;
        fn.blocks.insert(fn.blocks.begin() + (B + 1), std::move(latch));
        return true;
    }
    return false;
}

bool rotate_loops_function(IRFunction& fn) {
    if (fn.has_handlers || fn.is_generator) return false;
    bool any = false;
    for (int i = 0; i < 24; ++i)
        if (rotate_one_loop(fn)) any = true;
        else break;
    return any;
}

}  // namespace optdetail
}  // namespace pyadamas
