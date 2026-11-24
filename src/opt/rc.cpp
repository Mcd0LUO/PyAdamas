// opt/rc.cpp —— RC 冗余消除
//
// 降低器为了保证异常安全，会在每个「新产生的堆值」后面发射
// `pa_rc_temp(v)` 把引用挂到临时栈（异常展开时由 cleanup 释放）。
// 但有些位置会紧接着再发一条操作，让这两条指令互相抵消或重复：
//
//   1. `pa_rc_temp(v)` 紧跟 `pa_rc_temp_take(v)` —— 纯抵消，两条都能删；
//   2. `pa_rc_temp(v)` 紧跟 `pa_rc_inc(v)` —— take 走栈、inc 补引用，
//      但后续若立刻把 v 交给容器（list.new/dict.set 会接管引用），
//      这一次 inc 就是多余的。
//
// 实测（188 个测试用例的 IR 汇总）：683 处形态 1、795 处形态 2。
// `bench_rc_gen` 的热循环里两种形态每轮各出现一次，合计 30 万次冗余 RC。
//
// **只做形态 1**：它是严格语义等价的（push 后立刻 pop 同一值，栈水位不变，
// 引用计数也不变）。形态 2 需要证明「后续接管者一定存在」，跨块数据流分析
// 才能安全判定，不在这一步做。
#include "opt/detail.h"

namespace pyadamas {
namespace optdetail {

/* 消除 `pa_rc_temp(v)` 紧跟 `pa_rc_temp_take(v)` 的成对指令。
 *
 * 安全性：
 *   - temp 把 v 压栈（栈水位 +1），take 从栈里移除同一个 v（水位 -1）；
 *   - 两条都不改 rc（temp 接管的是产生者已有的引用，take 只是把它移出栈）；
 *   - 所以删掉这两条后，栈水位和引用计数都与原来一致。
 *
 * 要求：两条必须**相邻**且操作数相同。中间有任何指令都不动 —— 那可能是
 * 会抛异常的调用，temp 的保护是必要的。 */
bool rc_redundant_pairs(IRFunction& fn) {
    bool changed = false;
    for (auto& b : fn.blocks) {
        auto& ins = b.insts;
        for (size_t i = 0; i + 1 < ins.size(); ++i) {
            const Inst& a = ins[i];
            const Inst& c = ins[i + 1];
            if (a.op != IrOp::CallRt || c.op != IrOp::CallRt) continue;
            RtFn fa = static_cast<RtFn>(a.imm);
            RtFn fc = static_cast<RtFn>(c.imm);
            if (fa != RtFn::RcTemp || fc != RtFn::RcTempTake) continue;
            if (a.args.size() != 1 || c.args.size() != 1) continue;
            if (a.args[0] != c.args[0]) continue;
            // 命中：两条都删掉。IR 没有 Nop 指令，直接 erase。
            // 先删后面的，避免前面的下标失效。
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(i + 1));
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            if (i > 0) --i;   // 前一条可能也能与新的后继配对
        }
    }
    return changed;
}


/* 把 pa_rc_dec_unknown(v) 特化为 pa_rc_dec(v)：当 v 的静态类型是已知
 * 堆类型（list/str/dict/tuple/class/...）时，unknown 版多做的小整数
 * 排除 + 堆注册表查询全是冗余 —— 静态类型已经保证 v 是堆指针（或 0，
 * pa_rc_dec 对 NULL 安全）。
 *
 * 判据是 IR 结构：CallRt RtFn::RcDecUnknown 且唯一实参 vreg 的静态类型
 * ir_is_heap() 为真。适用于所有「槽从 unknown 升级成堆类型后残留
 * dec_unknown」的形态，不限具体变量/函数。
 *
 * 与 opt/inline.cpp 里同类重写一致（那边按 unk_ty 判定，这里按 vreg 类型）。
 */
bool rc_unknown_specialize(IRFunction& fn) {
    bool changed = false;
    const size_t nv = fn.vreg_ty.size();
    for (auto& b : fn.blocks) {
        for (auto& in : b.insts) {
            if (in.op != IrOp::CallRt || in.imm != static_cast<int64_t>(RtFn::RcDecUnknown)) continue;
            if (in.args.size() != 1) continue;
            VReg v = in.args[0];
            if (v == kNoReg || static_cast<size_t>(v) >= nv) continue;
            if (!ir_is_heap(fn.vreg_ty[static_cast<size_t>(v)])) continue;
            in.imm = static_cast<int64_t>(RtFn::RcDec);
            changed = true;
        }
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
