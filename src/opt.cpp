// opt.cpp —— O1/O2 的 IR 局部优化遍
//
// 目前包含：
//   - 常量折叠（整数/浮点算术、比较、一元运算、数值转换）
//   - 常量条件分支化简为 jump
//   - 纯标量指令的死代码消除（迭代到不动点）
//   - 静态小函数内联（只读形参直代、单返回直写、块紧邻插入）
//   - 静态调用直接化
//   - 循环不变量外提（不抛异常的纯计算与只读装载）
//   - 检查型下标外提（循环版本化，入口检一次边界）
//
// 这里只做语义保守的标量优化：不会删除可能抛异常的 Div/Mod/Pow、
// 带 RC 副作用的调用/容器分配，也不跨基本块搬移内存读写。
#include "opt.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace py23 {

namespace {

struct CVal {
    bool is_float = false;
    int64_t i = 0;
    double f = 0.0;
};

bool bin_is_safe_int(BinKind b) {
    switch (b) {
        case BinKind::Add:
        case BinKind::Sub:
        case BinKind::Mul:
        case BinKind::BitAnd:
        case BinKind::BitOr:
        case BinKind::BitXor:
            return true;
        default:
            return false;
    }
}

bool bin_is_safe_float(BinKind b) {
    return b == BinKind::Add || b == BinKind::Sub || b == BinKind::Mul;
}

void set_const_int(Inst& in, int64_t v) {
    in.op = IrOp::ConstInt;
    in.imm = v;
    in.fimm = 0.0;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

void set_const_float(Inst& in, double v) {
    in.op = IrOp::ConstFloat;
    in.imm = 0;
    in.fimm = v;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

bool inst_is_pure(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::ConstStr:
        case IrOp::ConstNone:
        case IrOp::FuncAddr:
        case IrOp::FuncCode:
        case IrOp::Mov:
        case IrOp::LoadSlot:
        case IrOp::LoadGlobal:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Len:
        case IrOp::IndexLoadUnchecked:
            return true;
        case IrOp::Bin: {
            TypePtr t = in.ty;
            if (t && (t->kind == TyKind::Str || t->kind == TyKind::Complex)) return false;
            if (ir_is_float(t)) return bin_is_safe_float(in.bop);
            return bin_is_safe_int(in.bop);
        }
        case IrOp::Convert:
            return in.ty && (in.ty->kind == TyKind::Bool || in.ty->kind == TyKind::Int ||
                             in.ty->kind == TyKind::Float);
        default:
            return false;
    }
}

void dce_function(IRFunction& fn) {
    if (fn.vreg_ty.empty()) return;
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<int> uses(fn.vreg_ty.size(), 0);
        auto touch = [&](VReg r) {
            if (r != kNoReg && r < static_cast<VReg>(uses.size())) ++uses[r];
        };
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
            }
        for (auto& b : fn.blocks) {
            auto& v = b.insts;
            for (size_t i = 0; i < v.size();) {
                const Inst& in = v[i];
                bool dead = in.dst != kNoReg &&
                            in.dst < static_cast<VReg>(uses.size()) && uses[in.dst] == 0 &&
                            inst_is_pure(in);
                if (!dead) {
                    ++i;
                    continue;
                }
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                // uses 的计数已经过期；下一轮外层 while 重新统计。
                // 这里继续扫后面的指令是保守的：最多晚一轮删除。
            }
        }
    }
}

struct DefRef {
    IrOp op = IrOp::ConstNone;
    int64_t imm = 0;
    int32_t idx = -1;
};

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

int function_inst_count(const IRFunction& f) {
    int n = 0;
    for (const auto& b : f.blocks) n += static_cast<int>(b.insts.size());
    return n;
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
    }
    int ret_slot = -1;
    if (!direct_ret && call.dst != kNoReg) {
        ret_slot = static_cast<int>(fn.slot_ty.size());
        fn.slot_ty.push_back(call.ty ? call.ty : t_unknown());
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
        Inst st;
        st.op = IrOp::StoreSlot;
        st.idx = slot_map[static_cast<size_t>(s)];
        st.a = zero;
        entry_insts.push_back(std::move(st));
    }
    for (size_t p = 0; p < callee.params.size(); ++p) {
        const IRParam& param = callee.params[p];
        if (param.slot < 0 || param.slot >= ns) continue;
        if (facts.subst[static_cast<size_t>(param.slot)] >= 0) continue;
        int dst_slot = slot_map[static_cast<size_t>(param.slot)];
        if (dst_slot < 0) continue;
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
                if (call.op != IrOp::Call) continue;
                int cidx = -1;
                auto dit = defs.find(call.a);
                if (dit != defs.end()) {
                    if (dit->second.op == IrOp::FuncAddr)
                        cidx = static_cast<int>(dit->second.imm);
                    else if (dit->second.op == IrOp::LoadGlobal) {
                        auto git = gfunc.find(static_cast<int>(dit->second.idx));
                        if (git != gfunc.end()) cidx = git->second;
                    }
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

}  // namespace

void optimize_ir(IRModule& m, int opt_level) {
    if (opt_level < 2) return;

    // 常量折叠 + 常量分支化简 + DCE。内联会引入新的常量操作数，
    // 所以在内联前后各跑一遍。
    auto fold_all = [&]() {
    for (auto& fptr : m.funcs) {
        IRFunction& fn = *fptr;

        // 扫描一遍常量定义，建立 vreg → 常量值。常量值是静态的，
        // 不依赖定义块是否执行，折叠不会改变控制流语义。
        std::unordered_map<VReg, CVal> consts;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                if (in.dst == kNoReg) continue;
                if (in.op == IrOp::ConstInt) consts[in.dst] = CVal{false, in.imm, 0.0};
                else if (in.op == IrOp::ConstFloat)
                    consts[in.dst] = CVal{true, 0, in.fimm};
            }
        if (consts.empty()) {
            dce_function(fn);
            continue;
        }

        auto as_double = [&](VReg r, double& out) {
            auto it = consts.find(r);
            if (it == consts.end()) return false;
            out = it->second.is_float ? it->second.f : static_cast<double>(it->second.i);
            return true;
        };
        (void)as_double;

        for (auto& b : fn.blocks) {
            for (auto& in : b.insts) {
                // 常量条件分支 → jump
                if (in.op == IrOp::Branch) {
                    auto it = consts.find(in.a);
                    if (it == consts.end()) continue;
                    bool truth = it->second.is_float ? (it->second.f != 0.0 || it->second.f != it->second.f)
                                                     : (it->second.i != 0);
                    in.op = IrOp::Jump;
                    in.target = truth ? in.target : in.target2;
                    in.target2 = -1;
                    in.a = kNoReg;
                    continue;
                }

                auto ca = consts.find(in.a);
                auto cb = consts.find(in.b);
                bool has_a = ca != consts.end();
                bool has_b = cb != consts.end();

                if (in.op == IrOp::Bin && has_a && has_b) {
                    TypePtr t = in.ty;
                    if (!ir_is_float(t) && (ca->second.is_float || cb->second.is_float))
                        continue;  // 混合数值应已由 Convert 显式化，不猜
                    if (ir_is_float(t)) {
                        if (!bin_is_safe_float(in.bop)) continue;
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        double y = cb->second.is_float ? cb->second.f
                                                       : static_cast<double>(cb->second.i);
                        double v = 0.0;
                        if (in.bop == BinKind::Add) v = x + y;
                        else if (in.bop == BinKind::Sub) v = x - y;
                        else v = x * y;
                        set_const_float(in, v);
                        consts[in.dst] = CVal{true, 0, v};
                    } else {
                        if (!bin_is_safe_int(in.bop)) continue;
                        uint64_t x = static_cast<uint64_t>(ca->second.is_float
                                                               ? static_cast<int64_t>(ca->second.f)
                                                               : ca->second.i);
                        uint64_t y = static_cast<uint64_t>(cb->second.is_float
                                                               ? static_cast<int64_t>(cb->second.f)
                                                               : cb->second.i);
                        uint64_t v = 0;
                        switch (in.bop) {
                            case BinKind::Add: v = x + y; break;
                            case BinKind::Sub: v = x - y; break;
                            case BinKind::Mul: v = x * y; break;
                            case BinKind::BitAnd: v = x & y; break;
                            case BinKind::BitOr: v = x | y; break;
                            case BinKind::BitXor: v = x ^ y; break;
                            default: break;
                        }
                        set_const_int(in, static_cast<int64_t>(v));
                        consts[in.dst] = CVal{false, static_cast<int64_t>(v), 0.0};
                    }
                    continue;
                }

                if (in.op == IrOp::Un && has_a) {
                    if (ir_is_float(in.ty)) {
                        if (in.uop != UnKind::Neg) continue;
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        set_const_float(in, -x);
                        consts[in.dst] = CVal{true, 0, -x};
                    } else if (in.uop == UnKind::Neg) {
                        uint64_t x = static_cast<uint64_t>(ca->second.i);
                        set_const_int(in, static_cast<int64_t>(uint64_t(0) - x));
                        consts[in.dst] = CVal{false, static_cast<int64_t>(uint64_t(0) - x), 0.0};
                    } else if (in.uop == UnKind::BitNot) {
                        int64_t v = ~ca->second.i;
                        set_const_int(in, v);
                        consts[in.dst] = CVal{false, v, 0.0};
                    } else if (in.uop == UnKind::Not) {
                        bool x = ca->second.is_float ? (ca->second.f != 0.0) : (ca->second.i != 0);
                        set_const_int(in, x ? 0 : 1);
                        consts[in.dst] = CVal{false, x ? 0 : 1, 0.0};
                    }
                    continue;
                }

                if (in.op == IrOp::Convert && has_a) {
                    TypePtr to = in.ty;
                    if (!to) continue;
                    if (to->kind == TyKind::Bool) {
                        bool x = ca->second.is_float ? (ca->second.f != 0.0)
                                                     : (ca->second.i != 0);
                        set_const_int(in, x ? 1 : 0);
                        consts[in.dst] = CVal{false, x ? 1 : 0, 0.0};
                    } else if (to->kind == TyKind::Int) {
                        int64_t v;
                        if (ca->second.is_float) {
                            double d = ca->second.f;
                            // cvttsd2si 越界是未定义的；只在安全范围折叠。
                            if (!(d >= -9.2e18 && d <= 9.2e18)) continue;
                            v = static_cast<int64_t>(d);
                        } else {
                            v = ca->second.i;
                        }
                        set_const_int(in, v);
                        consts[in.dst] = CVal{false, v, 0.0};
                    } else if (to->kind == TyKind::Float) {
                        double v = ca->second.is_float ? ca->second.f
                                                        : static_cast<double>(ca->second.i);
                        set_const_float(in, v);
                        consts[in.dst] = CVal{true, 0, v};
                    }
                    continue;
                }

                if (in.op == IrOp::Cmp && has_a && has_b) {
                    bool ok = true;
                    bool res = false;
                    if (ca->second.is_float || cb->second.is_float) {
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        double y = cb->second.is_float ? cb->second.f
                                                       : static_cast<double>(cb->second.i);
                        switch (in.cop) {
                            case CmpKind::Lt: res = x < y; break;
                            case CmpKind::Le: res = x <= y; break;
                            case CmpKind::Gt: res = x > y; break;
                            case CmpKind::Ge: res = x >= y; break;
                            case CmpKind::Eq: res = x == y; break;
                            case CmpKind::Ne: res = x != y; break;
                        }
                    } else {
                        int64_t x = ca->second.i;
                        int64_t y = cb->second.i;
                        switch (in.cop) {
                            case CmpKind::Lt: res = x < y; break;
                            case CmpKind::Le: res = x <= y; break;
                            case CmpKind::Gt: res = x > y; break;
                            case CmpKind::Ge: res = x >= y; break;
                            case CmpKind::Eq: res = x == y; break;
                            case CmpKind::Ne: res = x != y; break;
                        }
                    }
                    if (!ok) continue;
                    set_const_int(in, res ? 1 : 0);
                    consts[in.dst] = CVal{false, res ? 1 : 0, 0.0};
                    continue;
                }
            }
        }
        dce_function(fn);
    }
    };

    fold_all();

    // 再内联静态小函数，然后把剩余静态调用改成直接调用。
    auto gfunc = collect_static_func_globals(m);
    auto recursive = recursive_functions(m, gfunc);
    for (size_t i = 0; i < m.funcs.size(); ++i)
        inline_calls_in_function(m, *m.funcs[i], static_cast<int>(i), gfunc, recursive);
    for (auto& fptr : m.funcs) rewrite_direct_calls(m, *fptr, gfunc);

    // 内联/直接化后的 CFG 再做循环不变量外提。
    for (auto& fptr : m.funcs) licm_function(*fptr, m.globals.size());

    // 循环内不变量的检查型下标：入口检一次边界，通过则走无检查的循环副本。
    for (auto& fptr : m.funcs) version_bounds_function(*fptr);

    // 内联带进来的实参常量继续折；顺带清掉失效的 load.global / func.addr。
    fold_all();
}

}  // namespace py23
