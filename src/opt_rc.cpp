// opt_rc.cpp —— RC 冗余消除
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
#include "opt_detail.h"

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

}  // namespace optdetail
}  // namespace pyadamas