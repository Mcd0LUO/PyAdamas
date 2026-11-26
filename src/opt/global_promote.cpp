// opt/global_promote.cpp —— 循环内全局变量的寄存器化
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
// **做法**：把循环里读写的标量全局缓存到一个新分配的局部槽里：
//   - 循环前置块：`slot = load.global g`
//   - 循环体内：`load.global g` -> `load.slot slot`，`store.global` -> `store.slot`
//   - 循环出口块：`store.global g, slot`
//
// **安全性前提**（缺一不可）：
//   1. 循环体（按 CFG 可达性算，不是块号区间）内没有能观察全局中间值/写
//      全局的调用。入口模块（__pa_main）里例外：被提升的全局若「模块私有」
//      （除入口外没有函数引用、全模块未取址），任何被调函数都读不到它；
//      入口里未捕获异常等于进程终止，中途展开也没有观察者。
//      非入口模块可以被别的模块 import，全局对导入方可见 —— 仍要求循环
//      体无 `call.*` 且无可抛异常指令。
//   2. 循环只有单一回边、单一出口。
//   3. 全局是标量类型（int/float/bool）；堆类型要处理 RC 与 GC 根。
//
// **踩过的两个坑**：
//   - 前置块不能插在 header 里：header 每轮执行，会把循环内累加的结果
//     覆盖回全局旧值。必须新建前置块并把循环外前驱改指向它。
//   - 循环体不能用「块号连续区间」判定：内联/LICM 之后块号连续 ≠ CFG
//     连续，`for.body` 可能 branch 到很远的块。必须走 CFG 可达性，
//     否则会把 `print` 这类调用漏在循环体之外，导致提升后读到旧值。
#include "opt/detail.h"

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

// 从入口块 0 做可达性：降级/if-conversion 会留下**不可达的空块**占位，
// codegen 会把它们裁掉；空块只在可达时才按 fallthrough（b+1）算边。
std::vector<char> reachable_blocks(const IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    std::vector<char> reach(static_cast<size_t>(nb), 0);
    std::vector<int> work{0};
    while (!work.empty()) {
        const int b = work.back();
        work.pop_back();
        if (b < 0 || b >= nb || reach[static_cast<size_t>(b)]) continue;
        reach[static_cast<size_t>(b)] = 1;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            work.push_back(b + 1);
            continue;
        }
        const Inst& last = insts.back();
        switch (last.op) {
            case IrOp::Jump: work.push_back(last.target); break;
            case IrOp::Branch:
                work.push_back(last.target);
                work.push_back(last.target2);
                break;
            case IrOp::Return:
            case IrOp::GenYield:
            case IrOp::GenAwait:
            case IrOp::GenRethrow:
            case IrOp::GenDone:
                break;
            default: work.push_back(b + 1); break;
        }
    }
    return reach;
}

std::vector<std::vector<int>> build_succ(const IRFunction& fn,
                                        const std::vector<char>& reach) {
    const int nb = static_cast<int>(fn.blocks.size());
    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    auto add = [&](int b, int t) {
        if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
    };
    for (int b = 0; b < nb; ++b) {
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            // 不可达空块没有后继（codegen 裁剪）；可达空块才是 fallthrough。
            if (reach[static_cast<size_t>(b)]) add(b, b + 1);
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

// 可能抛异常的指令。提升把「每轮写全局」推迟成「循环出口写回」：若循环中途
// 抛异常并展开，全局会停在进入循环前的值。是否可观察取决于函数：
//   * 入口函数（__pa_main）里未捕获异常 = 进程终止，之后没有代码读全局 → 可观察不到；
//   * 其它函数可能被调用方 catch 后读全局 → 必须要求循环内没有可能抛异常的指令。
bool is_raising_op(const Inst& in) {
    switch (in.op) {
        case IrOp::IndexLoad:
        case IrOp::IndexStore:
        case IrOp::Setjmp:
        case IrOp::Call:
        case IrOp::CallRt:
        case IrOp::CallExtern:
        case IrOp::CallPtr:
        case IrOp::CallDirect:
        case IrOp::VCall:
        case IrOp::VCallStatic:
        case IrOp::SuperVCall:
            return true;
        case IrOp::Bin:
            return in.bop == BinKind::Div || in.bop == BinKind::Mod ||
                   in.bop == BinKind::FloorDiv;
        case IrOp::Convert:
            // 0.676：float → int 可能在运行期抛（NaN / ±inf / 越界，见 codegen 的
            // cvttsd2si 哨兵检查）。漏掉这一类，异常展开会跳过循环出口的写回，
            // 调用方 catch 后读到**陈旧**的全局值（实测 -O2 与 -O0 不同）。
            // 保守取「目标类型是 Int」的全部转换（这里拿不到操作数类型；多拒绝几个
            // int→int 转换只是少一档优化，不会错）。
            return in.ty && in.ty->kind == TyKind::Int;
        default:
            return false;
    }
}

// 零开销异常：函数里出现 EH 标记就整函数跳过（展开会跳过循环出口的写回）。
bool has_eh_markers(const IRFunction& fn) {
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::EhBegin || in.op == IrOp::EhEnd ||
                in.op == IrOp::EhLandingPad)
                return true;
    return false;
}

// 入口模块里「私有」的全局：除 __pa_main 外没有任何函数引用，且全模块
// 没有对它取址。
//
// 依据：入口模块的符号前缀固定是 __entry__（driver/build.cpp），
// `import __entry__` 被驱动显式拒绝 —— 别的编译单元拿不到这些变量。
// 模块内除入口函数外无人引用，则任何被调函数（用户方法、闭包、生成器、
// 导入模块的代码）都观察不到它的中间值；动态全局访问
// （globals/eval/exec）在代码生成阶段直接报错，不构成通路。
//
// GlobalAddr 一律取消资格：地址可能逃逸给 C 或用户代码，别名写入无法
// 追踪。非入口模块不适用（它们可以被别的模块 import）。
std::vector<char> entry_private_globals(const IRModule& m) {
    const size_t ng = m.globals.size();
    std::vector<char> priv(ng, 1);
    for (const auto& fptr : m.funcs) {
        const IRFunction& f = *fptr;
        const bool entry = f.name == "__pa_main";
        for (const auto& b : f.blocks) {
            for (const auto& in : b.insts) {
                if (in.idx < 0 || static_cast<size_t>(in.idx) >= ng) continue;
                if (in.op == IrOp::GlobalAddr) {
                    priv[static_cast<size_t>(in.idx)] = 0;
                    continue;
                }
                if (!entry &&
                    (in.op == IrOp::LoadGlobal || in.op == IrOp::StoreGlobal))
                    priv[static_cast<size_t>(in.idx)] = 0;
            }
        }
    }
    return priv;
}

}  // namespace

bool promote_globals_in_loops(IRFunction& fn, const IRModule& m) {
    if (fn.has_handlers || fn.is_generator) return false;
    if (has_eh_markers(fn)) return false;
    // 入口函数：未捕获异常等于进程终止，推迟写回不产生可观察差异（见 is_raising_op）。
    const bool is_entry = fn.name == "__pa_main";
    const std::vector<IRGlobal>& globals = m.globals;
    // 入口模块的全局可能是「模块私有」的：除 __pa_main 外无人引用。这样
    // 循环体里的用户调用也观察不到被提升的全局中间值。非入口模块可以被
    // import，全局对导入方可见，不做这个假设。
    const std::vector<char> private_g =
        is_entry ? entry_private_globals(m) : std::vector<char>();
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

        const std::vector<char> reach = reachable_blocks(fn);
        auto succ = build_succ(fn, reach);
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
        // 空体块（可达时是 fallthrough）没法安全推理，直接跳过。
        if (fn.blocks[static_cast<size_t>(body)].insts.empty()) continue;

        // 单一回边 + 单一出口：循环体内只有一处跳回 header，且 header
        // 只有 body 这一个循环内前驱。
        int back_edges = 0;
        for (int b : body_set)
            for (int s : succ[static_cast<size_t>(b)])
                if (s == h) ++back_edges;
        if (back_edges != 1) continue;
        // 单一出口：header 与 loop body 到 exit 的边合计恰好 1 条。
        // 原判据只数 body→exit，而 while/for 的出口边都在 header 上（body 只
        // 回边到 header）→ 所有 while 循环都被误拒（这也是 sieve 等模块级循环
        // 永远吃不到全局提升的原因）。
        int exits = 0;
        for (int s : succ[static_cast<size_t>(h)])
            if (s == exit) ++exits;
        for (int b : body_set)
            for (int s : succ[static_cast<size_t>(b)])
                if (s == exit) ++exits;
        if (exits != 1) continue;
        // exit 只能从循环内进入：写回指令插在 exit 开头，外部路径执行到它
        // 会把槽里的旧值写回全局（静默错值）。
        {
            bool ok = true;
            for (int b = 0; b < nb0 && ok; ++b) {
                bool to_exit = false;
                for (int s : succ[static_cast<size_t>(b)])
                    if (s == exit) { to_exit = true; break; }
                if (!to_exit) continue;
                if (b != h && body_set.count(b) == 0) ok = false;
            }
            if (!ok) continue;
        }
        // body 块也不能被循环外的路径直接进入（否则外部执行到体内会读写槽）。
        {
            bool ok = true;
            for (int b : body_set) {
                for (int p = 0; p < nb0 && ok; ++p) {
                    bool to_b = false;
                    for (int s : succ[static_cast<size_t>(p)])
                        if (s == b) { to_b = true; break; }
                    if (!to_b) continue;
                    if (p != h && body_set.count(p) == 0) ok = false;
                }
                if (!ok) break;
            }
            if (!ok) continue;
        }
        // 循环集合 = header + body。
        std::unordered_set<int> loop_set = body_set;
        loop_set.insert(h);

        // 统计循环体内读写的标量全局；入口函数只保留「模块私有」的那些。
        // 先算 touched 再判调用：可提升的集合已经过私有性过滤，调用门禁
        // 才能按集合下结论。
        std::vector<int> touched;
        for (int b : loop_set) {
            for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
                if (in.op != IrOp::LoadGlobal && in.op != IrOp::StoreGlobal) continue;
                if (in.idx < 0 || in.idx >= static_cast<int>(n_globals)) continue;
                if (!scalar_ty(globals[static_cast<size_t>(in.idx)].ty)) continue;
                if (is_entry &&
                    (private_g.empty() || !private_g[static_cast<size_t>(in.idx)]))
                    continue;
                if (std::find(touched.begin(), touched.end(), static_cast<int>(in.idx)) !=
                    touched.end())
                    continue;
                touched.push_back(static_cast<int>(in.idx));
            }
        }
        if (touched.empty()) continue;

        // 调用门禁（touched 已过滤成可提升的全局）。
        //
        // 入口函数：待提升的全局都满足「除 __pa_main 外无人引用、未取址」，
        // 因此循环体里的任何用户调用（Call/VCall/闭包/生成器回调）都读不到
        // 也写不到它们；call.rt 另有「RtFn 没有全局索引参数」的保证。未捕获
        // 异常在入口函数里等于进程终止，推迟写回不产生可观察差异。
        //
        // 非入口函数：模块可被 import，全局对导入方可见 —— 保持原判据：
        // 循环体有调用就拒绝；且不能有可抛异常指令（中途展开会跳过出口
        // 写回，调用方 catch 后读到旧全局值）。
        if (!is_entry) {
            bool has_call = false;
            for (int b : loop_set) {
                for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
                    if (is_call_op(in.op)) { has_call = true; break; }
                if (has_call) break;
            }
            if (has_call) continue;
            bool has_raise = false;
            for (int b : loop_set) {
                for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
                    if (is_raising_op(in)) { has_raise = true; break; }
                if (has_raise) break;
            }
            if (has_raise) continue;
        }

        // 槽类型必须是全局的真实类型：float 全局必须走 xmm 槽，不能当 int 槽
        // （否则 load/store 选错寄存器，静默错值）。
        std::vector<std::pair<int, int>> g2s;
        for (int g : touched) {
            TypePtr gt = globals[static_cast<size_t>(g)].ty;
            if (!gt) gt = t_int();
            const int slot = static_cast<int>(fn.slot_ty.size());
            fn.slot_ty.push_back(gt);
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
                TypePtr gt = fn.slot_ty[static_cast<size_t>(pr.second)];
                fn.vreg_ty.push_back(gt ? gt : t_int());
                ld.ty = gt;
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

        // 循环内改写（含 header：header 的循环条件也可能读被提升的全局，
        // 不改成槽会读到循环期间的旧全局值）。
        for (int b : loop_set) {
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
                TypePtr gt = fn.slot_ty[static_cast<size_t>(pr.second)];
                fn.vreg_ty.push_back(gt ? gt : t_int());
                ld.ty = gt;
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
