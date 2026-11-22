// opt_dict_fuse.cpp —— `if k in d: ... d[k] ...` 融合（0.284）
//
// contains+get 对同一个 key 做两次哈希+探测。运行时末次查找缓存实测
// +8.8% 已撤（0.283：缓存字段让相邻查找形成循环携带 store->load 依赖，
// STLF 串行化）。这版在 IR 层融合：一次查找，found 直接进分支寄存器，
// 值经帧槽中转（同一迭代内一写一读，无跨迭代依赖链）。
//
// 模式（跨块）：
//   B: %c = call.rt DictContains(d, k); <纯尾部>; branch %c, T, F
//   T: <纯前缀>; %v = call.rt DictGet(d2, k2); ...
// 守卫：d/d2、k/k2 同槽；T 的唯一前驱是 B 且 F!=T（否则别的路径进 T
// 会读到未写的值槽）；contains 之后到分支、T 入口到 get 之间都是纯
// 指令（不得有调用/存储改变 dict）；get 的值只在 T 内用（SSA 保证）。
// 改写：
//   B: %c = call.rt DictTryGet(d, k)，idx=新槽 S（运行时写借用值）
//   T: %v = load.slot S
// S 标记非 owned（借用，出口清理不得 dec）；堆类型槽天然不参与寄存器
// 提升，整型槽无 StoreSlot 定义 → 活跃分析视为入口 live-in → T 入口
// 从栈重载（运行时刚写的值），两条路都正确。
#include "opt_detail.h"

namespace pyadamas {
namespace optdetail {

namespace {

bool is_rt(const Inst& in, RtFn f) {
    return in.op == IrOp::CallRt && in.imm == static_cast<int64_t>(f);
}

// 两个 vreg 是否指向同一个来源：同槽（LoadSlot）/ 同全局（LoadGlobal）/ 同字符串
// 常量（ConstStr）。模块级代码（__pa_main）里 dict 与 key 通常是全局/常量，
// 只认 LoadSlot 会让模块级计数器永远匹配不上。
bool same_slot(const IRFunction& fn, VReg x, VReg y) {
    if (x == y) return true;
    auto origin_of = [&](VReg v) -> int64_t {
        if (v == kNoReg) return -1;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (in.dst == v) {
                    if (in.op == IrOp::LoadSlot)
                        return (int64_t{1} << 40) | static_cast<int64_t>(in.idx);
                    if (in.op == IrOp::LoadGlobal)
                        return (int64_t{2} << 40) | static_cast<int64_t>(in.idx);
                    if (in.op == IrOp::ConstStr)
                        return (int64_t{3} << 40) | static_cast<int64_t>(in.idx);
                    return -1;
                }
        return -1;
    };
    const int64_t a = origin_of(x);
    const int64_t c = origin_of(y);
    return a >= 0 && a == c;
}

bool pure_op(const Inst& in) {
    switch (in.op) {
        case IrOp::LoadSlot:
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::ConstStr:
        case IrOp::ConstNone:
        case IrOp::ConstBig:
        case IrOp::Mov:
        case IrOp::Bin:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Convert:
        case IrOp::Len:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool fuse_dict_tryget(IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    if (nb == 0) return false;
    std::vector<int> preds(static_cast<size_t>(nb), 0);
    for (const auto& b : fn.blocks) {
        if (b.insts.empty()) continue;
        const Inst& t = b.insts.back();
        if (t.op == IrOp::Jump) {
            if (t.target >= 0 && t.target < nb) preds[static_cast<size_t>(t.target)]++;
        } else if (t.op == IrOp::Branch) {
            if (t.target >= 0 && t.target < nb) preds[static_cast<size_t>(t.target)]++;
            if (t.target2 >= 0 && t.target2 < nb) preds[static_cast<size_t>(t.target2)]++;
        }
    }
    bool changed = false;
    for (int bi = 0; bi < nb; ++bi) {
        auto& ins = fn.blocks[static_cast<size_t>(bi)].insts;
        if (ins.size() < 2) continue;
        Inst& br = ins.back();
        if (br.op != IrOp::Branch) continue;
        const int T = br.target;
        if (T < 0 || T >= nb || T == bi || T == br.target2) continue;
        if (preds[static_cast<size_t>(T)] != 1) continue;
        int ci = -1;
        for (int i = static_cast<int>(ins.size()) - 2; i >= 0; --i) {
            if (ins[static_cast<size_t>(i)].dst == br.a &&
                is_rt(ins[static_cast<size_t>(i)], RtFn::DictContains) &&
                ins[static_cast<size_t>(i)].args.size() == 2) {
                ci = i;
                break;
            }
        }
        if (ci < 0) continue;
        bool tail_pure = true;
        for (int i = ci + 1; i + 1 < static_cast<int>(ins.size()); ++i)
            if (!pure_op(ins[static_cast<size_t>(i)])) { tail_pure = false; break; }
        if (!tail_pure) continue;
        auto& tins = fn.blocks[static_cast<size_t>(T)].insts;
        int gi = -1;
        for (int i = 0; i < static_cast<int>(tins.size()); ++i) {
            if (is_rt(tins[static_cast<size_t>(i)], RtFn::DictGet) &&
                tins[static_cast<size_t>(i)].args.size() == 2) {
                gi = i;
                break;
            }
            if (!pure_op(tins[static_cast<size_t>(i)])) break;
        }
        if (gi < 0) continue;
        Inst& cont = ins[static_cast<size_t>(ci)];
        Inst& get = tins[static_cast<size_t>(gi)];
        if (!same_slot(fn, cont.args[0], get.args[0]) ||
            !same_slot(fn, cont.args[1], get.args[1])) continue;
        const int S = fn.new_slot(get.ty);
        fn.slot_owned[static_cast<size_t>(S)] = false;
        cont.imm = static_cast<int64_t>(RtFn::DictTryGet);
        cont.idx = S;
        get.op = IrOp::LoadSlot;
        get.idx = S;
        get.args.clear();
        get.imm = 0;
        changed = true;
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
