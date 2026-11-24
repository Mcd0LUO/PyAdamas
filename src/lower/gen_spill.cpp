// lower/gen_spill.cpp —— 跨挂起点的槽位溢出
#include "lower/impl.h"

namespace pyadamas {

void Lowerer::spill_across_suspends() {
    IRFunction* fn = st_->fn;
    if (!fn || st_->gen_states.empty()) return;
    const int nb = static_cast<int>(fn->blocks.size());
    const int nv = static_cast<int>(fn->vreg_ty.size());
    if (nb == 0 || nv == 0) return;

    auto for_each_read = [](const Inst& in, auto&& f) {
        if (in.a != kNoReg) f(in.a);
        if (in.b != kNoReg) f(in.b);
        if (in.c != kNoReg) f(in.c);
        for (VReg r : in.args)
            if (r != kNoReg) f(r);
    };

    // 块内 use/def：块首未定义就读取的才算 live-in 需求。
    std::vector<std::vector<uint8_t>> use(nb, std::vector<uint8_t>(nv, 0));
    std::vector<std::vector<uint8_t>> def(nb, std::vector<uint8_t>(nv, 0));
    for (int b = 0; b < nb; ++b) {
        for (const Inst& in : fn->blocks[static_cast<size_t>(b)].insts) {
            for_each_read(in, [&](VReg r) {
                if (static_cast<int>(r) < nv &&
                    !def[static_cast<size_t>(b)][static_cast<size_t>(r)])
                    use[static_cast<size_t>(b)][static_cast<size_t>(r)] = 1;
            });
            if (in.dst != kNoReg && static_cast<int>(in.dst) < nv)
                def[static_cast<size_t>(b)][static_cast<size_t>(in.dst)] = 1;
        }
    }

    std::unordered_map<int, int> cont_of_state;
    for (auto& gs : st_->gen_states) cont_of_state[gs.id] = gs.cont;

    // 计算活跃性用的后继关系。挂起本身是终止指令，但续跑块在「值还活着」
    // 这一点上必须算作后继：否则跨挂起仍在用的 vreg 不会被判成 live-out，
    // 挂起前不落槽、续跑后也不装回来，续跑块里读到的是上一轮留下的垃圾。
    std::vector<std::vector<int>> succ(nb);
    for (int b = 0; b < nb; ++b) {
        const auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
        if (ins.empty()) {
            if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
            continue;
        }
        auto add = [&](int t) {
            if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
        };
        switch (ins.back().op) {
            case IrOp::Jump: add(ins.back().target); break;
            case IrOp::Branch:
                add(ins.back().target);
                add(ins.back().target2);
                break;
            case IrOp::GenYield:
            case IrOp::GenAwait: {
                auto it = cont_of_state.find(static_cast<int>(ins.back().imm));
                if (it != cont_of_state.end()) add(it->second);
                break;
            }
            case IrOp::Return:
            case IrOp::GenRethrow:
            case IrOp::GenDone: break;
            default:
                if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
                break;
        }
    }

    // 支配集：spill 只处理「定义块支配挂起点」的 vreg（见下面 live 过滤的注释）。
    std::vector<std::vector<int>> pred(nb);
    for (int b = 0; b < nb; ++b)
        for (int s : succ[static_cast<size_t>(b)])
            pred[static_cast<size_t>(s)].push_back(b);
    std::vector<std::vector<uint8_t>> dom(nb, std::vector<uint8_t>(nb, 0));
    for (int b = 0; b < nb; ++b)
        for (int d = 0; d < nb; ++d) dom[static_cast<size_t>(b)][static_cast<size_t>(d)] = (b == 0) ? 1 : 1;
    // 初始化：入口只支配自己；其它块先全 1，再迭代收缩。
    for (int b = 0; b < nb; ++b) {
        if (b == 0) {
            for (int d = 0; d < nb; ++d) dom[static_cast<size_t>(b)][static_cast<size_t>(d)] = (d == 0);
        } else {
            for (int d = 0; d < nb; ++d) dom[static_cast<size_t>(b)][static_cast<size_t>(d)] = 1;
        }
    }
    bool dom_changed = true;
    while (dom_changed) {
        dom_changed = false;
        for (int b = 1; b < nb; ++b) {
            std::vector<uint8_t> nd(static_cast<size_t>(nb), 0);
            if (!pred[static_cast<size_t>(b)].empty()) {
                for (int d = 0; d < nb; ++d) nd[static_cast<size_t>(d)] = 1;
                for (int p : pred[static_cast<size_t>(b)])
                    for (int d = 0; d < nb; ++d)
                        if (!dom[static_cast<size_t>(p)][static_cast<size_t>(d)])
                            nd[static_cast<size_t>(d)] = 0;
            }
            nd[static_cast<size_t>(b)] = 1;
            if (nd != dom[static_cast<size_t>(b)]) {
                dom[static_cast<size_t>(b)] = std::move(nd);
                dom_changed = true;
            }
        }
    }

    std::vector<std::vector<uint8_t>> live_in(nb, std::vector<uint8_t>(nv, 0));
    std::vector<std::vector<uint8_t>> live_out(nb, std::vector<uint8_t>(nv, 0));
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = nb - 1; b >= 0; --b) {
            std::vector<uint8_t> out(nv, 0);
            for (int s : succ[static_cast<size_t>(b)])
                for (int v = 0; v < nv; ++v)
                    out[static_cast<size_t>(v)] |=
                        live_in[static_cast<size_t>(s)][static_cast<size_t>(v)];
            std::vector<uint8_t> in(nv, 0);
            for (int v = 0; v < nv; ++v)
                in[static_cast<size_t>(v)] = static_cast<uint8_t>(
                    use[static_cast<size_t>(b)][static_cast<size_t>(v)] |
                    (out[static_cast<size_t>(v)] &
                     ~def[static_cast<size_t>(b)][static_cast<size_t>(v)]));
            if (out != live_out[static_cast<size_t>(b)] ||
                in != live_in[static_cast<size_t>(b)]) {
                live_out[static_cast<size_t>(b)] = std::move(out);
                live_in[static_cast<size_t>(b)] = std::move(in);
                changed = true;
            }
        }
    }

    // 同一个 vreg 在多个挂起点之间复用一个溢出槽。
    std::unordered_map<VReg, int> spill_slot;
    auto slot_for = [&](VReg v) {
        auto it = spill_slot.find(v);
        if (it != spill_slot.end()) return it->second;
        int s = fn->new_slot(fn->vreg_ty[static_cast<size_t>(v)]);
        spill_slot.emplace(v, s);
        return s;
    };

    for (int b = 0; b < nb; ++b) {
        auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
        if (ins.empty()) continue;
        const IrOp term_op = ins.back().op;
        if (term_op != IrOp::GenYield && term_op != IrOp::GenAwait) continue;
        const int state_id = static_cast<int>(ins.back().imm);
        auto cit = cont_of_state.find(state_id);
        if (cit == cont_of_state.end()) continue;
        const int c = cit->second;
        if (c < 0 || c >= nb) continue;

        std::vector<VReg> live;
        for (int v = 0; v < nv; ++v) {
            if (!live_in[static_cast<size_t>(c)][static_cast<size_t>(v)]) continue;
            // 只溢出「在挂起点之前一定已经定义」的 vreg。
            //
            // 缺陷（0.437 定位）：活性分析只看 vreg 编号，而降低期 vreg 编号
            // 不是 SSA、也没有 phi —— 不同块里的不同值可能共用编号（本例里
            // %47..%52 在 g.close 等后置块里被定义，却被判成「挂起前 live」）。
            // 挂起前对从未定义过的编号做 rc_inc 会读到栈上垃圾（实测
            // `await 已取消的 task` 崩溃：refcount increment on a freed object）。
            // 判据：存在某个定义块 d（in.dst == v）支配挂起块 b；否则跳过。
            bool defined_before = false;
            for (int d = 0; d < nb && !defined_before; ++d) {
                if (!dom[static_cast<size_t>(b)][static_cast<size_t>(d)]) continue;
                for (const Inst& din : fn->blocks[static_cast<size_t>(d)].insts) {
                    if (din.dst == static_cast<VReg>(v)) {
                        defined_before = true;
                        break;
                    }
                }
            }
            if (!defined_before) continue;
            live.push_back(static_cast<VReg>(v));
        }

        // 挂起后的续跑块：先把值从堆帧槽装回 vreg，所有权转给临时栈。
        if (!live.empty()) {
            std::vector<Inst> post;
            for (VReg v : live) {
                TypePtr ty = fn->vreg_ty[static_cast<size_t>(v)];
                int slot = slot_for(v);
                Inst ld;
                ld.op = IrOp::LoadSlot;
                ld.idx = Operand::slot(slot);
                ld.ty = ty;
                ld.dst = v;
                post.push_back(std::move(ld));
                if (ir_is_heap(ty)) {
                    Inst tp;
                    tp.op = IrOp::CallRt;
                    tp.imm = static_cast<int64_t>(RtFn::RcTemp);
                    tp.args = {v};
                    post.push_back(std::move(tp));
                    // 槽已经不再持有这份引用；不清零的话帧销毁会二次释放。
                    Inst zero;
                    zero.op = IrOp::ConstInt;
                    zero.imm = 0;
                    VReg z = static_cast<VReg>(fn->vreg_ty.size());
                    fn->vreg_ty.push_back(t_int());
                    zero.dst = z;
                    zero.ty = t_int();
                    post.push_back(std::move(zero));
                    Inst cl;
                    cl.op = IrOp::StoreSlot;
                    cl.idx = Operand::slot(slot);
                    cl.a = z;
                    post.push_back(std::move(cl));
                } else if (!ty || ty->kind == TyKind::Unknown) {
                    Inst tp;
                    tp.op = IrOp::CallRt;
                    tp.imm = static_cast<int64_t>(RtFn::RcTempUnknown);
                    tp.args = {v};
                    post.push_back(std::move(tp));
                }
            }
            auto& cont_insts = fn->blocks[static_cast<size_t>(c)].insts;
            cont_insts.insert(cont_insts.begin(), post.begin(), post.end());
        }

        // 挂起前：retain 一份再写进堆帧槽。必须赶在 RcCleanup 之前，
        // 否则临时栈先把产生者那份引用放掉了，写进去的就是悬空指针。
        if (live.empty()) continue;
        int cleanup_pos = -1;
        for (int i = static_cast<int>(ins.size()) - 2; i >= 0; --i) {
            if (ins[static_cast<size_t>(i)].op == IrOp::CallRt &&
                static_cast<RtFn>(ins[static_cast<size_t>(i)].imm) == RtFn::RcCleanup) {
                cleanup_pos = i;
                break;
            }
        }
        size_t at = cleanup_pos >= 0 ? static_cast<size_t>(cleanup_pos) : ins.size() - 1;
        std::vector<Inst> pre;
        for (VReg v : live) {
            TypePtr ty = fn->vreg_ty[static_cast<size_t>(v)];
            int slot = slot_for(v);
            if (ir_is_heap(ty)) {
                Inst inc;
                inc.op = IrOp::CallRt;
                inc.imm = static_cast<int64_t>(RtFn::RcInc);
                inc.args = {v};
                pre.push_back(std::move(inc));
            } else if (!ty || ty->kind == TyKind::Unknown) {
                Inst inc;
                inc.op = IrOp::CallRt;
                inc.imm = static_cast<int64_t>(RtFn::RcIncUnknown);
                inc.args = {v};
                pre.push_back(std::move(inc));
            }
            Inst st;
            st.op = IrOp::StoreSlot;
            st.idx = Operand::slot(slot);
            st.a = v;
            pre.push_back(std::move(st));
        }
        ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(at), pre.begin(), pre.end());
    }
}



}  // namespace pyadamas
