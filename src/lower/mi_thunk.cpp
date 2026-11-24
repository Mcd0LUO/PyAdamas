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
    if (ci->base_field_offsets.empty()) return;
    auto& vt = mod_.classes[static_cast<size_t>(cid)].vtable;
    for (auto& entry : ci->base_field_offsets) {
        const ClassInfo* base = entry.first;
        int start = entry.second;
        if (!base || start <= 0) continue;
        auto bcid = class_ids_.find(base);
        if (bcid == class_ids_.end()) continue;
        const auto& bvt = mod_.classes[static_cast<size_t>(bcid->second)].vtable;
        for (const MethodInfo& m : base->methods) {
            if (m.kind != MethodKind::Normal || m.decorated || m.slot < 0) continue;
            if (m.slot >= static_cast<int>(vt.size())) continue;
            if (m.slot >= static_cast<int>(bvt.size())) continue;
            int fidx = bvt[static_cast<size_t>(m.slot)];
            if (fidx < 0 || fidx >= static_cast<int>(mod_.funcs.size())) continue;
            // 被更靠前的类覆盖过就跳过：这时槽里不是该基类的实现。
            if (vt[static_cast<size_t>(m.slot)] != fidx) continue;
            const IRFunction* callee = mod_.funcs[static_cast<size_t>(fidx)].get();
            if (!callee || callee->env_slot >= 0) continue;
            vt[static_cast<size_t>(m.slot)] = build_mi_thunk(fidx, 8 * start);
        }
    }
}

}  // namespace pyadamas
