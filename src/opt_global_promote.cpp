// opt_global_promote.cpp —— 循环内全局变量的寄存器化
//
// **问题**：模块级变量（`g = 0` 这种顶层赋值）是真正的全局对象，每次读写
// 都要 `movq ...(%rip)` 走内存。实测：
//
//     6400 万次 `g = g + 1`：
//       全局变量  131.1ms（2.05ns/次）
//       局部变量   15.0ms（0.23ns/次）
//     **慢 8.7 倍**。
//
// 而真实程序几乎都写成模块级变量，于是这个开销摊在每一个热循环上。
//
// **做法**：对**循环体内没有任何函数调用**的循环，把被读写的标量全局
// 缓存到一个新分配的局部槽里：
//   - 循环前置块：`slot = load.global g`
//   - 循环体内：`load.global g` -> `load.slot slot`，`store.global` -> `store.slot`
//   - 循环出口块：`store.global g, slot`
//
// **安全性前提**（缺一不可）：
//   1. 循环体（按 CFG 可达性算，不是块号区间）内没有 `call.*` —— 没有
//      任何东西能观察到全局的中间值，也没有别名写入。
//   2. 循环只有单一回边、单一出口。
//   3. 全局是标量类型（int/float/bool）；堆类型要处理 RC 与 GC 根。
//
// **踩过的两个坑**：
//   - 前置块不能插在 header 里：header 每轮执行，会把循环内累加的结果
//     覆盖回全局旧值。必须新建前置块并把循环外前驱改指向它。
//   - 循环体不能用「块号连续区间」判定：内联/LICM 之后块号连续 ≠ CFG
//     连续，`for.body` 可能 branch 到很远的块。必须走 CFG 可达性，
//     否则会把 `print` 这类调用漏在循环体之外，导致提升后读到旧值。
#include "opt_detail.h"

#include <algorithm>
#include <unordered_set>

namespace pyadamas {
namespace optdetail {

namespace {

bool scalar_ty(const TypePtr& t) {
    if (!t) return false;
    return t->kind == TyKind::Int || t->kind == TyKind::Float ||
           t->kind == TyKind::Bool;
}

bool is_call_op(IrOp op) {
    switch (op) {
        case IrOp::Call:
        case IrOp::CallRt:
        case IrOp::CallExtern:
        case IrOp::CallPtr:
        case IrOp::CallDirect:
        case IrOp::VCall:
        case IrOp::VCallStatic:
        case IrOp::SuperVCall:
        case IrOp::Setjmp:
            return true;
        default:
            return false;
    }
}

std::vector<std::vector<int>> build_succ(const IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    auto add = [&](int b, int t) {
        if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
    };
    for (int b = 0; b < nb; ++b) {
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            add(b, b + 1);
            continue;
        }
        const Inst& last = insts.back();
        switch (last.op) {
            case IrOp::Jump: add(b, last.target); break;
            case IrOp::Branch:
                add(b, last.target);
                add(b, last.target2);
                break;
            case IrOp::Return:
            case IrOp::GenYield:
            case IrOp::GenAwait:
            case IrOp::GenRethrow:
            case IrOp::GenDone:
                break;
            default: add(b, b + 1); break;
        }
    }
    return succ;
}

}  // namespace

bool promote_globals_in_loops(IRFunction& fn, const std::vector<IRGlobal>& globals) {
    if (fn.has_handlers || fn.is_generator) return false;
    const size_t n_globals = globals.size();
    const int nb0 = static_cast<int>(fn.blocks.size());
    bool changed = false;

    for (int h = 0; h < nb0; ++h) {
        const auto& hb = fn.blocks[static_cast<size_t>(h)];
        if (hb.name.find("for.cond") == std::string::npos &&
            hb.name.find("while.cond") == std::string::npos)
            continue;
        if (hb.insts.empty() || hb.insts.back().op != IrOp::Branch) continue;
        const int body = hb.insts.back().target;
        const int exit = hb.insts.back().target2;
        if (body < 0 || exit < 0 || body >= nb0 || exit >= nb0) continue;

        auto succ = build_succ(fn);
        // 循环体：从 body 出发、不经过 header 也不经过 exit 的可达集。
        std::unordered_set<int> body_set;
        std::vector<int> work{body};
        while (!work.empty()) {
            const int x = work.back();
            work.pop_back();
            if (x == h || x == exit) continue;
            if (!body_set.insert(x).second) continue;
            for (int s : succ[static_cast<size_t>(x)]) work.push_back(s);
        }
        if (body_set.empty()) continue;

        // 单一回边 + 单一出口：循环体内只有一处跳回 header，且 header
        // 只有 body 这一个循环内前驱。
        int back_edges = 0;
        for (int b : body_set)
            for (int s : succ[static_cast<size_t>(b)])
                if (s == h) ++back_edges;
        if (back_edges != 1) continue;
        int exits = 0;
        for (int b : body_set)
            for (int s : succ[static_cast<size_t>(b)])
                if (s == exit) ++exits;
        if (exits != 1) continue;

        // 循环体内不能有任何函数调用（能观察到全局中间值）。
        bool has_call = false;
        for (int b : body_set) {
            for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
                if (is_call_op(in.op)) { has_call = true; break; }
            if (has_call) break;
        }
        if (has_call) continue;

        // 统计循环体内读写的标量全局。
        std::vector<int> touched;
        for (int b : body_set) {
            for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
                if (in.op != IrOp::LoadGlobal && in.op != IrOp::StoreGlobal) continue;
                if (in.idx < 0 || in.idx >= static_cast<int>(n_globals)) continue;
                if (!scalar_ty(globals[static_cast<size_t>(in.idx)].ty)) continue;
                if (std::find(touched.begin(), touched.end(), static_cast<int>(in.idx)) !=
                    touched.end())
                    continue;
                touched.push_back(static_cast<int>(in.idx));
            }
        }
        if (touched.empty()) continue;

        std::vector<std::pair<int, int>> g2s;
        for (int g : touched) {
            const int slot = static_cast<int>(fn.slot_ty.size());
            fn.slot_ty.push_back(t_int());
            fn.slot_owned.push_back(false);
            fn.slot_unknown.push_back(false);
            g2s.emplace_back(g, slot);
        }

        // 前置块：装载全局到槽，再跳到 header。新建块，把循环外前驱改指向它。
        {
            std::vector<Inst> pro;
            for (const auto& pr : g2s) {
                Inst ld;
                ld.op = IrOp::LoadGlobal;
                ld.idx = pr.first;
                ld.dst = static_cast<VReg>(fn.vreg_ty.size());
                fn.vreg_ty.push_back(t_int());
                ld.ty = t_int();
                pro.push_back(std::move(ld));
                Inst st;
                st.op = IrOp::StoreSlot;
                st.idx = pr.second;
                st.a = pro.back().dst;
                pro.push_back(std::move(st));
            }
            Inst jmp;
            jmp.op = IrOp::Jump;
            jmp.target = h;
            pro.push_back(std::move(jmp));
            Block pre;
            pre.name = "gpre";
            pre.insts = std::move(pro);
            const int pre_idx = static_cast<int>(fn.blocks.size());
            fn.blocks.push_back(std::move(pre));
            for (size_t k = 0; k < fn.blocks.size(); ++k) {
                if (body_set.count(static_cast<int>(k))) continue;
                if (static_cast<int>(k) == pre_idx) continue;
                for (auto& in : fn.blocks[k].insts) {
                    if (in.op == IrOp::Jump && in.target == h) in.target = pre_idx;
                    else if (in.op == IrOp::Branch) {
                        if (in.target == h) in.target = pre_idx;
                        if (in.target2 == h) in.target2 = pre_idx;
                    }
                }
            }
        }

        // 体内改写。
        for (int b : body_set) {
            for (auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
                if (in.op != IrOp::LoadGlobal && in.op != IrOp::StoreGlobal) continue;
                int slot = -1;
                for (const auto& pr : g2s)
                    if (pr.first == static_cast<int>(in.idx)) { slot = pr.second; break; }
                if (slot < 0) continue;
                in.op = (in.op == IrOp::LoadGlobal) ? IrOp::LoadSlot : IrOp::StoreSlot;
                in.idx = slot;
            }
        }

        // 出口写回。
        {
            std::vector<Inst> epi;
            for (const auto& pr : g2s) {
                Inst ld;
                ld.op = IrOp::LoadSlot;
                ld.idx = pr.second;
                ld.dst = static_cast<VReg>(fn.vreg_ty.size());
                fn.vreg_ty.push_back(t_int());
                ld.ty = t_int();
                epi.push_back(std::move(ld));
                Inst st;
                st.op = IrOp::StoreGlobal;
                st.idx = pr.first;
                st.a = epi.back().dst;
                epi.push_back(std::move(st));
            }
            auto& ei = fn.blocks[static_cast<size_t>(exit)].insts;
            ei.insert(ei.begin(), epi.begin(), epi.end());
        }
        changed = true;
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
