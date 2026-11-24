// opt/devirt.cpp —— 经全局/槽函数变量的间接调用去虚拟化（devirt）
//
// 目标形态（内层循环里每次迭代都白跑一遍统一约定的经典写法）：
//
//   %21 = load.global g1        ; func(int, int) -> float
//   %24 = call %21(%22, %23)    ; float
//
// 间接调用按「统一调用约定」装填：第 i 个实参进第 i 个**整型**寄存器，被调
// 一侧再由 closure shim 按真实签名重排（浮点实参/返回值都要搬一遍）。全局里
// 装的是哪个函数在编译期其实是唯一的，只要能证明这一点，就可以把它改写成
// call.direct（真实 ABI 装填 + 直接下标），省掉 shim，并让后端/向量化/内联
// 看到的不再是一个不透明调用。
//
// 判据全部是 IR 结构性质（与源码名字/基准无关），任一条证不出来就放过：
//
//   1. 被调操作数由 load.global / load.slot 定义（可穿过 mov 别名链）；
//   2. 全局：整个模块里**恰好一次** store.global，且它的值在同函数内可解析成
//      func.addr（函数下标）；全局从未被 global.addr 取过地址（否则可能被
//      间接改写）。只对入口模块生效 —— 非入口模块导出的全局符号会被导入方
//      改写，静态唯一性不成立。
//   3. 槽：函数内「可能到达该调用点」的 store.slot 恰好一条，且它**支配**调用
//      点（形参槽在入口有一份调用方给的初始值、不在 IR 里，只有支配才证得出调
//      用点看到的一定是这条 store 的值）；值同样解析成已知函数；槽从未被
//      slot.addr 取过地址；函数内不含 setjmp/EH 标记（longjmp 绕开 CFG，可达
//      性/支配性都推不出槽值）。
//   4. 被调函数：非生成器、无闭包环境（env_slot < 0，直调没有 %r10 传递通道）、
//      形参个数与实参一致、实参与返回类型逐项 type_eq —— 只有类型逐项相同，
//      「统一约定 + shim」才与真实 ABI 逐位等价（实参寄存器类别、栈实参位置、
//      返回值寄存器都一致）。
//   5. 调用点所在函数不是生成器/协程（帧槽在创建时已固定，不做任何形状改动）。
//
// 改写：op = call.direct、idx = 函数下标（Operand::func）、a 清空；args 原样，
// ty 不变，实参个数/顺序一个字都不动。
#include "opt/opt.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace pyadamas {
namespace {

using DefMap = std::unordered_map<VReg, const Inst*>;

// 被调者是怎么证出来的（只用于诊断输出/统计）。
enum class Via : uint8_t { None, Global, Slot, FuncAddr };

const char* via_name(Via v) {
    switch (v) {
        case Via::Global: return "global";
        case Via::Slot: return "slot";
        case Via::FuncAddr: return "func.addr";
        case Via::None: break;
    }
    return "?";
}

// 函数内的槽位/控制流事实（每个函数算一次，供所有候选点复用）。
struct FuncFacts {
    DefMap defs;
    // SlotAddr 取过地址的槽：取地址后无法证明没人间接写。
    std::vector<char> slot_addr;
    // 含 setjmp / EH 标记：longjmp 进入 landing pad 的路径不在 CFG 里。
    bool has_eh = false;
    // 每个块的后继（只用于可达性判断）。
    std::vector<std::vector<int>> succ;
};

FuncFacts analyze_func(const IRFunction& fn) {
    FuncFacts ff;
    const size_t nb = fn.blocks.size();
    ff.slot_addr.assign(fn.slot_ty.size(), 0);
    ff.succ.assign(nb, {});
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts)
            if (in.dst != kNoReg) ff.defs[in.dst] = &in;
    for (size_t bi = 0; bi < nb; ++bi) {
        const auto& insts = fn.blocks[bi].insts;
        for (const auto& in : insts) {
            if (in.op == IrOp::SlotAddr) {
                const int s = in.idx.value;
                if (s >= 0 && static_cast<size_t>(s) < ff.slot_addr.size())
                    ff.slot_addr[static_cast<size_t>(s)] = 1;
            } else if (in.op == IrOp::Setjmp || in.op == IrOp::EhBegin ||
                       in.op == IrOp::EhLandingPad) {
                ff.has_eh = true;
            }
        }
        if (insts.empty()) {
            if (bi + 1 < nb) ff.succ[bi].push_back(static_cast<int>(bi + 1));
            continue;
        }
        const Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            if (last.target >= 0) ff.succ[bi].push_back(last.target);
        } else if (last.op == IrOp::Branch) {
            if (last.target >= 0) ff.succ[bi].push_back(last.target);
            if (last.target2 >= 0) ff.succ[bi].push_back(last.target2);
        } else if (!is_terminator(last)) {
            if (bi + 1 < nb) ff.succ[bi].push_back(static_cast<int>(bi + 1));
        }
    }
    return ff;
}

// 从 from 出发能否沿 CFG 到达 to。from == to 时要求存在经过它的环（同块内
// 「store 在调用点之后」只有靠上一轮迭代才可能到达）。
bool block_reaches(const FuncFacts& ff, int from, int to) {
    const int nb = static_cast<int>(ff.succ.size());
    if (from < 0 || from >= nb || to < 0 || to >= nb) return false;
    std::vector<char> seen(static_cast<size_t>(nb), 0);
    std::vector<int> stack;
    auto push = [&](int s) {
        if (s < 0 || s >= nb || seen[static_cast<size_t>(s)]) return false;
        seen[static_cast<size_t>(s)] = 1;
        stack.push_back(s);
        return true;
    };
    for (int s : ff.succ[static_cast<size_t>(from)]) {
        if (s == to) return true;
        push(s);
    }
    while (!stack.empty()) {
        const int b = stack.back();
        stack.pop_back();
        for (int s : ff.succ[static_cast<size_t>(b)]) {
            if (s == to) return true;
            push(s);
        }
    }
    return false;
}

// d 块是否支配 c 块：把 d 从 CFG 里删掉后 c 是否还能从入口到达。
//
// 槽这一侧必须用**支配**而不是「可能到达」：形参槽在入口处有一份调用方给的
// 初始值（不在 IR 里），若唯一那条 store.slot 只在某个分支里执行，调用点完全
// 可能读到初始值 —— 「唯一可达的 store」证不出这件事，只有支配才证得出。
bool dominates_block(const FuncFacts& ff, int d, int c) {
    if (d == c) return true;
    const int nb = static_cast<int>(ff.succ.size());
    if (d < 0 || d >= nb || c < 0 || c >= nb) return false;
    std::vector<char> seen(static_cast<size_t>(nb), 0);
    std::vector<int> stack;
    if (d != 0) {
        seen[0] = 1;
        stack.push_back(0);
    }
    while (!stack.empty()) {
        const int b = stack.back();
        stack.pop_back();
        if (b == c) return false;
        for (int s : ff.succ[static_cast<size_t>(b)]) {
            if (s == d || s < 0 || s >= nb || seen[static_cast<size_t>(s)]) continue;
            seen[static_cast<size_t>(s)] = 1;
            stack.push_back(s);
        }
    }
    return true;
}

// 全局写入事实。
struct GlobalFact {
    int stores = 0;
    bool value_ok = true;   // 所有写入都可解析成同一个 func.addr
    bool addr_taken = false;
    int fidx = -1;
};

const Inst* def_of(const FuncFacts& ff, VReg v) {
    auto it = ff.defs.find(v);
    return it == ff.defs.end() ? nullptr : it->second;
}

// 函数内「可能到达 (bi,ii) 这个调用点」的槽存储；多于一条就返回 nullptr。
// 同时要求这条 store **支配**调用点：只「可能到达」不够 —— 形参槽的初始值
// （调用方给的）不在 IR 里，分支里的唯一 store 不支配调用点时，调用点可能
// 读到那份初始值，改直调就错了。
const Inst* reaching_store(const IRFunction& fn, const FuncFacts& ff, int slot, int bi, int ii) {
    if (ff.has_eh) return nullptr;  // longjmp 绕过 CFG 可达性
    if (slot < 0 || static_cast<size_t>(slot) >= ff.slot_addr.size()) return nullptr;
    if (ff.slot_addr[static_cast<size_t>(slot)]) return nullptr;
    const Inst* found = nullptr;
    int fbi = -1, fii = -1;
    for (int bi2 = 0; bi2 < static_cast<int>(fn.blocks.size()); ++bi2) {
        const auto& insts = fn.blocks[static_cast<size_t>(bi2)].insts;
        for (int ii2 = 0; ii2 < static_cast<int>(insts.size()); ++ii2) {
            const Inst& in = insts[static_cast<size_t>(ii2)];
            if (in.op != IrOp::StoreSlot || in.idx.value != slot) continue;
            bool reaches;
            if (bi2 == bi)
                reaches = ii2 < ii || block_reaches(ff, bi2, bi2);  // 块在环上 → 上轮可达
            else
                reaches = block_reaches(ff, bi2, bi);
            if (!reaches) continue;
            if (found) return nullptr;  // 多条可能到达：证不出唯一
            found = &in;
            fbi = bi2;
            fii = ii2;
        }
    }
    if (!found) return nullptr;
    // 支配：同块要求写在前；跨块要求该块支配调用块。
    const bool dom = (fbi == bi) ? (fii < ii) : dominates_block(ff, fbi, bi);
    if (!dom) return nullptr;
    return found;
}

// 把一个 vreg 解析成「已知函数下标」。allow_global/allow_slot 控制允许穿透
// 到哪一层（存储值那一侧不允许再穿全局，避免依赖扫描顺序）。
int resolve_value(const IRFunction& fn, const FuncFacts& ff,
                  const std::vector<GlobalFact>& gf, const std::vector<IRGlobal>& globals,
                  bool entry_module, VReg v, int bi, int ii, Via* via, bool allow_global,
                  bool allow_slot, int depth) {
    if (v == kNoReg || depth > 8) return -1;
    const Inst* d = def_of(ff, v);
    if (!d) return -1;
    switch (d->op) {
        case IrOp::FuncAddr: {
            // 注意：func.addr 的函数下标在 **imm** 里（CallDirect 才用 idx）。
            const int f = static_cast<int>(d->imm);
            if (f < 0) return -1;
            *via = Via::FuncAddr;
            return f;
        }
        case IrOp::Mov:
            return resolve_value(fn, ff, gf, globals, entry_module, d->a, bi, ii, via,
                                 allow_global, allow_slot, depth + 1);
        case IrOp::LoadGlobal: {
            if (!allow_global) return -1;
            // 非入口模块导出的全局符号会被导入方改写（store.global 出现在别的
            // 模块里，本模块扫不到），「整模块只写一次」不成立。
            if (!entry_module) return -1;
            const int g = d->idx.value;
            if (g < 0 || static_cast<size_t>(g) >= gf.size()) return -1;
            const IRGlobal& gv = globals[static_cast<size_t>(g)];
            // 全局本身必须是函数类型；外部符号由别的模块分配/写入，不接。
            if (gv.external || !gv.ty || gv.ty->kind != TyKind::Func) return -1;
            const GlobalFact& f = gf[static_cast<size_t>(g)];
            if (f.stores != 1 || !f.value_ok || f.addr_taken || f.fidx < 0) return -1;
            *via = Via::Global;
            return f.fidx;
        }
        case IrOp::LoadSlot: {
            if (!allow_slot) return -1;
            const int s = d->idx.value;
            if (s < 0 || static_cast<size_t>(s) >= fn.slot_ty.size()) return -1;
            const TypePtr& sty = fn.slot_ty[static_cast<size_t>(s)];
            if (!sty || sty->kind != TyKind::Func) return -1;
            const Inst* st = reaching_store(fn, ff, s, bi, ii);
            if (!st) return -1;
            // 槽里存的值再解析一次（不再允许嵌套槽，避免自依赖）。
            const int f = resolve_value(fn, ff, gf, globals, entry_module, st->a, bi, ii, via,
                                        true, false, depth + 1);
            if (f < 0) return -1;
            *via = Via::Slot;
            return f;
        }
        default:
            return -1;
    }
}

TypePtr vreg_type(const IRFunction& fn, VReg v) {
    if (v == kNoReg || v >= fn.vreg_ty.size()) return nullptr;
    return fn.vreg_ty[v];
}

// 被调函数与调用点是否在 ABI 上完全对齐。
bool callee_matches(const IRModule& m, const IRFunction& fn, const Inst& in, int cidx) {
    if (cidx < 0 || static_cast<size_t>(cidx) >= m.funcs.size()) return false;
    if (cidx == m.entry) return false;  // 模块入口带收尾序列，保守不接
    const IRFunction& callee = *m.funcs[static_cast<size_t>(cidx)];
    // 生成器入口签名不同（参数由创建包装写进堆帧）；闭包环境要靠 %r10 传。
    if (callee.is_generator) return false;
    if (callee.env_slot >= 0) return false;
    if (callee.params.size() != in.args.size()) return false;
    for (size_t i = 0; i < in.args.size(); ++i) {
        if (!type_eq(vreg_type(fn, in.args[i]), callee.params[i].ty)) return false;
    }
    // 结果没人用时不读返回寄存器，返回类型不必对齐；否则必须逐项相同
    // （浮点走 %xmm0、整数走 %rax，错了就是静默读错寄存器）。
    if (in.dst != kNoReg && !type_eq(in.ty, callee.ret)) return false;
    return true;
}

}  // namespace

bool devirt_module(IRModule& m) {
    const bool dbg = std::getenv("PYADAMAS_DEVIRT_DBG") != nullptr;
    if (m.funcs.empty()) return false;

    // ── 全局写入事实：整模块扫一遍 ───────────────────────────────────
    std::vector<GlobalFact> gf(m.globals.size());
    for (const auto& fp : m.funcs) {
        const IRFunction& fn = *fp;
        const FuncFacts ff = analyze_func(fn);
        for (const auto& b : fn.blocks) {
            for (const auto& in : b.insts) {
                if (in.op == IrOp::StoreGlobal) {
                    const int g = in.idx.value;
                    if (g < 0 || static_cast<size_t>(g) >= gf.size()) continue;
                    GlobalFact& f = gf[static_cast<size_t>(g)];
                    ++f.stores;
                    Via via = Via::None;
                    const int fidx = resolve_value(fn, ff, gf, m.globals, m.is_entry, in.a,
                                                   -1, -1, &via, false, false, 0);
                    if (fidx < 0) f.value_ok = false;
                    else if (f.fidx < 0) f.fidx = fidx;
                    else if (f.fidx != fidx) f.value_ok = false;
                } else if (in.op == IrOp::GlobalAddr) {
                    const int g = in.idx.value;
                    if (g >= 0 && static_cast<size_t>(g) < gf.size())
                        gf[static_cast<size_t>(g)].addr_taken = true;
                }
            }
        }
    }

    int n_global = 0, n_slot = 0, n_funcaddr = 0;
    bool changed = false;
    for (size_t fi = 0; fi < m.funcs.size(); ++fi) {
        IRFunction& fn = *m.funcs[fi];
        if (fn.is_generator) continue;
        const FuncFacts ff = analyze_func(fn);
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            auto& insts = fn.blocks[bi].insts;
            for (size_t ii = 0; ii < insts.size(); ++ii) {
                Inst& in = insts[ii];
                if (in.op != IrOp::Call || in.a == kNoReg) continue;
                Via via = Via::None;
                const int cidx = resolve_value(fn, ff, gf, m.globals, m.is_entry, in.a,
                                               static_cast<int>(bi), static_cast<int>(ii),
                                               &via, true, true, 0);
                if (cidx < 0) continue;
                if (!callee_matches(m, fn, in, cidx)) continue;
                if (dbg)
                    std::fprintf(stderr, "[devirt] %s: call %%%u -> f%d %s (via %s)\n",
                                 fn.name.c_str(), in.a, cidx,
                                 m.funcs[static_cast<size_t>(cidx)]->name.c_str(),
                                 via_name(via));
                if (via == Via::Global) ++n_global;
                else if (via == Via::Slot) ++n_slot;
                else ++n_funcaddr;
                in.op = IrOp::CallDirect;
                in.idx = Operand::func(cidx);
                in.a = kNoReg;
                changed = true;
            }
        }
    }
    if (dbg && changed)
        std::fprintf(stderr, "[devirt] total %d site(s): global=%d slot=%d func.addr=%d\n",
                     n_global + n_slot + n_funcaddr, n_global, n_slot, n_funcaddr);
    return changed;
}

}  // namespace pyadamas
