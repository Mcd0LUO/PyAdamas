// lower_idiom.cpp —— 循环惯用法识别
//
// 第一条：切片拷贝。queue_sim 的热路径形态
//
//     rest = []
//     for k in range(head, n):
//         rest.append(pending[k])
//
// 语义上是 rest = pending[head:n]，但 Python 切片对负索引/越界的处理与
// range 循环不同（切片归一化负索引、越界静默截断；循环按 k 逐个取，
// 越界抛 IndexError），所以不能直接改写成切片语法。这里识别后调用
// pa_list_copy_range：快路径一次扩容 + memcpy + 批量 retain，慢路径
// 逐元素 pa_list_get + prep + append，异常行为与原循环逐位一致。
//
// 匹配是纯 AST + 静态类型检查，不发射任何 IR；安全护栏（见下）全过
// 才发射。失败返回 false，调用点退回原循环降级，不留痕迹。
#include "lower_impl.h"

namespace pyadamas {

bool Lowerer::lower_copy_range_idiom(For& f, Call& c) {
    // ── 结构匹配 ─────────────────────────────────────────────
    // 循环体恰好一条 dst.append(src[k])，无 else，步长常量 1。
    if (!f.orelse.empty() || f.body.size() != 1) return false;
    if (f.target->kind != ExprKind::Name) return false;
    auto& kname = static_cast<Name&>(*f.target);
    if (c.args.empty() || c.args.size() > 3 || !c.keywords.empty()) return false;
    if (c.args.size() == 3) {
        auto cv = as_const(c.args[2].get());
        // 步长必须是整型/布尔常量 1（和 lower_for_range 的口径一致）；
        // 0 / 负数 / 浮点 / 动态步长都不匹配。
        if (!cv || cv->i != 1) return false;
        if (cv->ty->kind != TyKind::Int && cv->ty->kind != TyKind::Bool) return false;
    }
    if (f.body[0]->kind != StmtKind::Expr) return false;
    Expr& e = *static_cast<ExprStmt&>(*f.body[0]).value;
    if (e.kind != ExprKind::Call) return false;
    auto& call = static_cast<Call&>(e);
    if (call.args.size() != 1 || !call.keywords.empty()) return false;
    if (call.func->kind != ExprKind::Attribute) return false;
    auto& at = static_cast<Attribute&>(*call.func);
    if (at.attr != "append" || at.value->kind != ExprKind::Name) return false;
    auto& dst = static_cast<Name&>(*at.value);
    if (call.args[0]->kind != ExprKind::Subscript) return false;
    auto& sub = static_cast<Subscript&>(*call.args[0]);
    if (sub.value->kind != ExprKind::Name || sub.slice->kind != ExprKind::Name) return false;
    auto& src = static_cast<Name&>(*sub.value);
    if (static_cast<Name&>(*sub.slice).id != kname.id) return false;
    // 循环变量不能同时是 dst/src：循环里 k 每轮被重新赋值，
    // 批量版只求值一次 dst/src，语义会分叉。
    if (dst.id == kname.id || src.id == kname.id) return false;

    // 静态类型：两侧都必须是 list（append 才是内置 append，index.get
    // 才是列表取值；用户类实例 / unknown 一律不匹配）。
    TypePtr dt = tc_.type_of(&dst);
    TypePtr st = tc_.type_of(&src);
    if (!dt || dt->kind != TyKind::List || !st || st->kind != TyKind::List) return false;

    // ── 语义护栏 ─────────────────────────────────────────────
    // 批量版只在正常结束时补 k = hi-1；循环中途抛异常时 k 的中间进度
    // 无法还原。要保证这种分叉不可观测：
    //   1. 当前位置不在任何 try/with/finally 里（cleanups 空）——异常
    //      必然离开本函数，本函数的局部 k 随帧销毁；
    //   2. k 是普通局部槽——不是全局（外部 catcher 可读）、不是闭包
    //      cell/env（外层函数可读）、不是推导式槽。
    if (!st_->cleanups.empty()) return false;
    const std::string& kid = kname.id;
    if (st_->globals_decl.count(kid) || !is_local(kid)) return false;
    if (is_cell(kid) || is_env(kid) || comp_slot(kid) >= 0) return false;

    // ── 发射 ─────────────────────────────────────────────────
    // 求值顺序与循环一致：range 实参先算，dst/src 在「体」里才读。
    VReg lo = c.args.size() >= 2 ? expr(*c.args[0]) : const_int(0);
    VReg hi = expr(*c.args.back());
    VReg dv = expr(dst);
    VReg sv = expr(src);
    // heap 参数 = 源元素静态种类，和 append 降级点的 heap_arg 同口径。
    TypePtr et = st->sub.empty() ? nullptr : st->sub[0];
    int64_t heap = (!et || et->kind == TyKind::Unknown) ? 2 : (ir_is_heap(et) ? 1 : 0);
    call_void(RtFn::ListCopyRange, {dv, sv, lo, hi, const_int(heap)});

    // 循环变量终值：至少迭代一次时 k == hi-1；零迭代保持 k 原值。
    int setblk = new_block("copyrange.kfinal");
    int endblk = new_block("copyrange.end");
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = lo;
    cmp.b = hi;
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = setblk;
    br.target2 = endblk;
    emit(std::move(br));
    switch_to(setblk);
    Inst subi;
    subi.op = IrOp::Bin;
    subi.bop = BinKind::Sub;
    subi.a = hi;
    subi.b = const_int(1);
    assign_to(kname, def(t_int(), std::move(subi)), t_int());
    Inst j;
    j.op = IrOp::Jump;
    j.target = endblk;
    emit(std::move(j));
    switch_to(endblk);
    return true;
}

}  // namespace pyadamas
