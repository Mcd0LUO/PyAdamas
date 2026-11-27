// opt/exc_direct.cpp —— 方案 A 第二步：同一帧零开销 try 里的 raise 直接跳
// land_direct，绕开 _Unwind 展开。
//
// 为什么必须在 opt 层：降级是逐函数做的，跨函数 raise（如 excbench 的
// may_fail）在降级期不在任何 try 体里（current_try()==nullptr）；只有 -O2
// 内联之后它才落进调用方的保护区。所以「同一帧内直接跳」只有内联后的 IR
// 能判定。
//
// 判据**只依赖块布局区间**（不依赖块 id 连续、也不依赖块下标跨 pass 稳定）：
// codegen 的 .LEHB/.LEHE 就是按块布局发的 —— 保护区 = 布局下标
// [begin_idx, end_idx)。本 pass 紧跟内联（inliner 原地 splice 块），排在
// LICM/ifconv 之前，所以此刻 fn.blocks 的顺序就是布局顺序。
//
// 排除（保持老路）：嵌套 setjmp try（HandlerPush/HandlerPop 布局区间，
// 内层 handler 必须先跑）、嵌套零开销 try（取最内层保护区，不是 direct_ok
// 就不跳）、非 direct_ok 保护区（多 handler / finally / 非裸非 Exception）、
// 非 pa_raise 形态（raise 值 / raise ... from / 裸 raise）。
//
// 改写：call.rt pa_raise(type,msg) 前面插 slot.addr，改成
// call.rt pa_eh_raise_prep(type,msg,slot_addr) + jump land_direct。prep 把
// 异常对象写进约定槽位并设好 pending_exc（与抛之前逐项一致）；land_direct
// 里的 EhLeave 配对退出 pa_eh_depth（EhBegin 记过一次），RcCleanup 复刻
// 老路径的临时值水位清理。
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "ir/ir.h"

namespace pyadamas {
namespace {

bool exc_direct_on() {
    static const bool on = std::getenv("PYADAMAS_EXC_DIRECT") != nullptr;
    return on;
}

struct ZcRange {
    int begin = 0;    // 保护区起点块布局下标（.LEHB 所在块）
    int end = 0;      // 终点块布局下标（.LEHE 所在块），不含
    int direct = -1;  // land_direct 块布局下标（-1 = 该保护区不能直接跳）
    int slot = -1;    // 异常对象约定槽号
};

bool contains(const ZcRange& r, int bi) { return bi >= r.begin && bi < r.end; }

bool in_ranges(const std::vector<std::pair<int, int>>& rs, int bi) {
    for (const auto& r : rs)
        if (bi >= r.first && bi <= r.second) return true;
    return false;
}

}  // namespace

bool exc_direct_function(IRFunction& fn) {
    if (!exc_direct_on() || fn.eh_regions.empty()) return false;
    // 块 id → 布局下标：内联会重建块表，先重建 block_by_id 再解析。
    fn.rebuild_use_def();
    std::vector<ZcRange> regions;
    for (const auto& er : fn.eh_regions) {
        const int b = fn.block_index(er.begin_block);
        const int e = fn.block_index(er.end_block);
        if (b < 0 || e < 0 || b >= e) continue;
        ZcRange r;
        r.begin = b;
        r.end = e;
        if (er.direct_ok && er.direct_block != 0 && er.direct_slot >= 0) {
            r.direct = fn.block_index(er.direct_block);
            r.slot = er.direct_slot;
            if (r.direct < 0) r.direct = -1;
        }
        regions.push_back(r);
    }
    // setjmp 保护区间：HandlerPush/HandlerPop 按布局配对（栈式，支持嵌套）。
    std::vector<std::pair<int, int>> sj_ranges;
    {
        std::vector<int> open;
        for (int bi = 0; bi < static_cast<int>(fn.blocks.size()); ++bi)
            for (const auto& in : fn.blocks[static_cast<size_t>(bi)].insts) {
                if (in.op != IrOp::CallRt) continue;
                const RtFn f = static_cast<RtFn>(in.imm);
                if (f == RtFn::HandlerPush) open.push_back(bi);
                else if ((f == RtFn::HandlerLeave || f == RtFn::HandlerDiscard) &&
                         !open.empty()) {
                    // 早退路径会先 emit 一条 leave，此时配对提前关区间（保守：
                    // 之后同层的 raise 不跳）—— 正确性优先。
                    sj_ranges.emplace_back(open.back(), bi);
                    open.pop_back();
                }
            }
    }
    // cause 是否常量 0（无 from / from None）：只接受「没有真实 cause 链」的
    // 值 raise —— raise ... from e 的 prep 语义未做逐项对拍，先留老路。
    // 1 = 目前只见过 const.int 0 定义；2 = 还有别的定义；0 = 未定义。
    std::unordered_map<VReg, char> zero_def;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts) {
            if (in.dst == kNoReg) continue;
            if (in.op == IrOp::ConstInt && in.imm == 0) {
                if (zero_def[in.dst] != 2) zero_def[in.dst] = 1;
            } else {
                zero_def[in.dst] = 2;
            }
        }
    bool changed = false;
    for (const ZcRange& r : regions) {
        if (r.direct < 0) continue;
        for (int bi = r.begin; bi < r.end; ++bi) {
            if (bi < 0 || bi >= static_cast<int>(fn.blocks.size())) continue;
            // 嵌套：更内层的保护区（含不可直跳的）先跑，不能跳外层。
            bool inner = false;
            for (const ZcRange& o : regions)
                if (o.begin > r.begin && contains(o, bi)) inner = true;
            if (inner || in_ranges(sj_ranges, bi)) continue;
            std::vector<Inst>& insts = fn.blocks[static_cast<size_t>(bi)].insts;
            if (insts.empty()) continue;
            Inst& call = insts.back();
            // 三类可直跳的抛出形态（其余一律老路）：
            //   pa_raise(type,msg)                        ← raise Exc(...) / raise Exc
            //   pa_raise_value_from(v, 0)                ← raise ValueError("x") 等值 raise
            //   pa_raise_instance(inst, type)            ← raise <富异常实例>
            if (call.op != IrOp::CallRt) continue;
            if (call.args.size() != 2 || call.dst != kNoReg) continue;
            const RtFn f = static_cast<RtFn>(call.imm);
            RtFn prep_fn = RtFn::EhRaisePrep;
            if (f == RtFn::RaiseValueFrom || f == RtFn::EhRaiseValueFrom) {
                if (zero_def[call.args[1]] != 1) continue;  // 有真实 cause 链：老路
                prep_fn = RtFn::EhRaisePrepValue;
            } else if (f == RtFn::RaiseInstance) {
                prep_fn = RtFn::EhRaisePrepInstance;
            } else if (f != RtFn::Raise && f != RtFn::EhRaise) {
                continue;
            }
            // slot.addr + prep + jump：prep 不是终止指令，块以 jump 收尾。
            const VReg sv = static_cast<VReg>(fn.vreg_ty.size());
            fn.vreg_ty.push_back(t_unknown());
            if (fn.vreg_nonneg.size() < fn.vreg_ty.size()) fn.vreg_nonneg.push_back(0);
            Inst sa;
            sa.op = IrOp::SlotAddr;
            sa.idx = Operand::slot(r.slot);
            sa.dst = sv;
            fn.ensure_inst_id(sa);
            insts.insert(insts.end() - 1, std::move(sa));
            Inst& prep = insts.back();
            prep.imm = static_cast<int64_t>(prep_fn);
            prep.args.push_back(sv);
            Inst jump;
            jump.op = IrOp::Jump;
            jump.target = r.direct;
            fn.ensure_inst_id(jump);
            insts.push_back(std::move(jump));
            changed = true;
        }
    }
    return changed;
}

}  // namespace pyadamas
