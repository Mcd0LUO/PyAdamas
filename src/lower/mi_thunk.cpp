// lower/mi_thunk.cpp —— 多继承 adjustor thunk（0.509）
//
// 非 layout 基类的字段块在子类布局里被追加到后面（0.506 的布局实体化）。
// 该基类的方法体按它自己的布局算 self.<字段> 偏移，所以调用前要把 self 挪到
// 该字段块的起点。直调路径在 method.cpp 里做（调用点 delta）；虚调用走 vtable，
// 被调方在运行期不知道具体类，只能让槽里放一个「调整 self 再直调真实实现」的
// 小程序 —— 这就是 thunk，走与 ctor wrapper 相同的合成函数流程拿自己的 fidx。
#include "lower/impl.h"

#include <format>

namespace pyadamas {

int Lowerer::build_mi_thunk(int target_fidx, int delta_bytes) {
    // add_func 会 append 到 mod_.funcs：先把形参/返回类型拷出来再用，
    // 不依赖 callee 指针在 append 之后仍然有效。
    std::vector<IRParam> params = mod_.funcs[static_cast<size_t>(target_fidx)]->params;
    TypePtr rty = mod_.funcs[static_cast<size_t>(target_fidx)]->ret;
    if (!rty) rty = t_unknown();

    std::string label = std::format("__pa_mithunk_{}_{}", target_fidx, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int tidx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = rty;
    wf->mi_thunk = true;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<VReg> args;
    for (auto& p : params) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        args.push_back(load_slot(s, T));
    }
    inc_params();

    // self += 8 * 块起始下标（字段下标 0 在对象头部 +8 处）。
    if (!args.empty() && delta_bytes != 0) {
        Inst ad;
        ad.op = IrOp::Bin;
        ad.bop = BinKind::Add;
        ad.a = args[0];
        ad.b = const_int(delta_bytes);
        args[0] = def(t_int(), std::move(ad));
    }

    Inst cd;
    cd.op = IrOp::CallDirect;
    cd.idx = Operand::func(target_fidx);
    cd.args = std::move(args);
    // 返回值所有权直接透传：callee 返回的是 owned 引用，thunk 只是转发，
    // 不能走 def_heap（那会把结果也挂进临时栈，函数结束时多 dec 一次）。
    VReg r = def(rty, std::move(cd));
    emit_return(r, rty);
    finalize_param_borrows();
    finalize_borrow_slots();
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);
    pop_frame();
    return tidx;
}

void Lowerer::install_mi_thunks(const ClassInfoPtr& ci, int cid) {
    auto& vt = mod_.classes[static_cast<size_t>(cid)].vtable;
    // 沿 layout 链收集「带字段的非 layout 基类」→ 块起始下标：子类（单继承
    // layout base）没有自己的 block 表，但它的前缀就是 layout base 的布局。
    std::vector<std::pair<const ClassInfo*, int>> blocks;
    for (const ClassInfo* k = ci.get(); k; k = k->layout_base) {
        if (k != ci.get()) {
            // 祖先已经给它自己的槽装过 thunk：直接复用那个 fidx。
        }
        for (auto& e : k->base_field_offsets)
            if (e.second > 0) blocks.push_back({e.first, e.second});
    }
    if (blocks.empty()) return;
    // layout 链上每个类已装的 thunk（槽 → fidx），给子类复用，避免重复生成。
    std::unordered_map<int, int> reuse;
    for (const ClassInfo* k = ci.get(); k; k = k->layout_base) {
        auto kcid = class_ids_.find(k);
        if (kcid == class_ids_.end()) continue;
        const auto& kvt = mod_.classes[static_cast<size_t>(kcid->second)].vtable;
        for (size_t s = 0; s < kvt.size() && s < vt.size(); ++s) {
            int f = kvt[s];
            if (f >= 0 && f < static_cast<int>(mod_.funcs.size()) &&
                mod_.funcs[static_cast<size_t>(f)]->mi_thunk)
                reuse[static_cast<int>(s)] = f;
        }
    }
    for (auto& entry : blocks) {
        const ClassInfo* base = entry.first;
        int start = entry.second;
        if (!base) continue;
        auto bcid = class_ids_.find(base);
        if (bcid == class_ids_.end()) continue;
        const auto& bvt = mod_.classes[static_cast<size_t>(bcid->second)].vtable;
        for (const MethodInfo& m : base->methods) {
            if (m.kind != MethodKind::Normal || m.decorated || m.slot < 0) continue;
            if (m.slot >= static_cast<int>(vt.size())) continue;
            if (m.slot >= static_cast<int>(bvt.size())) continue;
            int cur_f = vt[static_cast<size_t>(m.slot)];
            if (cur_f >= 0 && cur_f < static_cast<int>(mod_.funcs.size()) &&
                mod_.funcs[static_cast<size_t>(cur_f)]->mi_thunk)
                continue;  // 已经是 thunk（本类或继承来的）
            int fidx = bvt[static_cast<size_t>(m.slot)];
            if (fidx < 0 || fidx >= static_cast<int>(mod_.funcs.size())) continue;
            // 槽里必须是该基类的真实实现（没被更靠前的类覆盖）。祖先布局里
            // 已经装过 thunk 的，先看 cur_f 是不是那个真实实现。
            auto rit = reuse.find(m.slot);
            if (rit != reuse.end()) {
                vt[static_cast<size_t>(m.slot)] = rit->second;
                continue;
            }
            if (std::getenv("PYADAMAS_SUPER_DEBUG"))
                std::fprintf(stderr, "  inst %s.%s slot=%d cur=%d base=%d env=%d\n",
                             base->name.c_str(), m.name.c_str(), m.slot, cur_f, fidx,
                             fidx >= 0 && fidx < static_cast<int>(mod_.funcs.size())
                                 ? mod_.funcs[static_cast<size_t>(fidx)]->env_slot
                                 : -99);
            if (cur_f != fidx) continue;
            const IRFunction* callee = mod_.funcs[static_cast<size_t>(fidx)].get();
            if (!callee || callee->env_slot >= 0) continue;
            vt[static_cast<size_t>(m.slot)] = build_mi_thunk(fidx, 8 * start);
        }
    }
}
}  // namespace pyadamas
