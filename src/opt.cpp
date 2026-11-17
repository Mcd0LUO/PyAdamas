// opt.cpp —— O1/O2 优化驱动
//
// 折叠/DCE 见 opt_fold.cpp，内联与直接调用见 opt_inline.cpp，
// LICM 与下标版本化见 opt_loop.cpp；这里只按顺序调度。
#include "opt_detail.h"

namespace pyadamas {

using namespace optdetail;

void optimize_ir(IRModule& m, int opt_level) {
    if (opt_level < 2) return;

    // 常量折叠 + 常量分支化简 + DCE。内联会引入新的常量操作数，
    // 所以在内联前后各跑一遍。
    auto fold_all = [&]() {
    for (auto& fptr : m.funcs) {
        IRFunction& fn = *fptr;

        // 扫描一遍常量定义，建立 vreg → 常量值。常量值是静态的，
        // 不依赖定义块是否执行，折叠不会改变控制流语义。
        std::unordered_map<VReg, CVal> consts;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                if (in.dst == kNoReg) continue;
                if (in.op == IrOp::ConstInt) consts[in.dst] = CVal{false, in.imm, 0.0};
                else if (in.op == IrOp::ConstFloat)
                    consts[in.dst] = CVal{true, 0, in.fimm};
            }
        if (consts.empty()) {
            dce_function(fn);
            continue;
        }

        auto as_double = [&](VReg r, double& out) {
            auto it = consts.find(r);
            if (it == consts.end()) return false;
            out = it->second.is_float ? it->second.f : static_cast<double>(it->second.i);
            return true;
        };
        (void)as_double;

        for (auto& b : fn.blocks) {
            for (auto& in : b.insts) {
                // 常量条件分支 → jump
                if (in.op == IrOp::Branch) {
                    auto it = consts.find(in.a);
                    if (it == consts.end()) continue;
                    bool truth = it->second.is_float ? (it->second.f != 0.0 || it->second.f != it->second.f)
                                                     : (it->second.i != 0);
                    in.op = IrOp::Jump;
                    in.target = truth ? in.target : in.target2;
                    in.target2 = -1;
                    in.a = kNoReg;
                    continue;
                }

                auto ca = consts.find(in.a);
                auto cb = consts.find(in.b);
                bool has_a = ca != consts.end();
                bool has_b = cb != consts.end();

                if (in.op == IrOp::Bin && has_a && has_b) {
                    TypePtr t = in.ty;
                    if (!ir_is_float(t) && (ca->second.is_float || cb->second.is_float))
                        continue;  // 混合数值应已由 Convert 显式化，不猜
                    if (ir_is_float(t)) {
                        if (!bin_is_safe_float(in.bop)) continue;
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        double y = cb->second.is_float ? cb->second.f
                                                       : static_cast<double>(cb->second.i);
                        double v = 0.0;
                        if (in.bop == BinKind::Add) v = x + y;
                        else if (in.bop == BinKind::Sub) v = x - y;
                        else v = x * y;
                        set_const_float(in, v);
                        consts[in.dst] = CVal{true, 0, v};
                    } else {
                        if (!bin_is_safe_int(in.bop)) continue;
                        uint64_t x = static_cast<uint64_t>(ca->second.is_float
                                                               ? static_cast<int64_t>(ca->second.f)
                                                               : ca->second.i);
                        uint64_t y = static_cast<uint64_t>(cb->second.is_float
                                                               ? static_cast<int64_t>(cb->second.f)
                                                               : cb->second.i);
                        uint64_t v = 0;
                        switch (in.bop) {
                            case BinKind::Add: v = x + y; break;
                            case BinKind::Sub: v = x - y; break;
                            case BinKind::Mul: v = x * y; break;
                            case BinKind::BitAnd: v = x & y; break;
                            case BinKind::BitOr: v = x | y; break;
                            case BinKind::BitXor: v = x ^ y; break;
                            default: break;
                        }
                        set_const_int(in, static_cast<int64_t>(v));
                        consts[in.dst] = CVal{false, static_cast<int64_t>(v), 0.0};
                    }
                    continue;
                }

                if (in.op == IrOp::Un && has_a) {
                    if (ir_is_float(in.ty)) {
                        if (in.uop != UnKind::Neg) continue;
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        set_const_float(in, -x);
                        consts[in.dst] = CVal{true, 0, -x};
                    } else if (in.uop == UnKind::Neg) {
                        uint64_t x = static_cast<uint64_t>(ca->second.i);
                        set_const_int(in, static_cast<int64_t>(uint64_t(0) - x));
                        consts[in.dst] = CVal{false, static_cast<int64_t>(uint64_t(0) - x), 0.0};
                    } else if (in.uop == UnKind::BitNot) {
                        int64_t v = ~ca->second.i;
                        set_const_int(in, v);
                        consts[in.dst] = CVal{false, v, 0.0};
                    } else if (in.uop == UnKind::Not) {
                        bool x = ca->second.is_float ? (ca->second.f != 0.0) : (ca->second.i != 0);
                        set_const_int(in, x ? 0 : 1);
                        consts[in.dst] = CVal{false, x ? 0 : 1, 0.0};
                    }
                    continue;
                }

                if (in.op == IrOp::Convert && has_a) {
                    TypePtr to = in.ty;
                    if (!to) continue;
                    if (to->kind == TyKind::Bool) {
                        bool x = ca->second.is_float ? (ca->second.f != 0.0)
                                                     : (ca->second.i != 0);
                        set_const_int(in, x ? 1 : 0);
                        consts[in.dst] = CVal{false, x ? 1 : 0, 0.0};
                    } else if (to->kind == TyKind::Int) {
                        int64_t v;
                        if (ca->second.is_float) {
                            double d = ca->second.f;
                            // cvttsd2si 越界是未定义的；只在安全范围折叠。
                            if (!(d >= -9.2e18 && d <= 9.2e18)) continue;
                            v = static_cast<int64_t>(d);
                        } else {
                            v = ca->second.i;
                        }
                        set_const_int(in, v);
                        consts[in.dst] = CVal{false, v, 0.0};
                    } else if (to->kind == TyKind::Float) {
                        double v = ca->second.is_float ? ca->second.f
                                                        : static_cast<double>(ca->second.i);
                        set_const_float(in, v);
                        consts[in.dst] = CVal{true, 0, v};
                    }
                    continue;
                }

                if (in.op == IrOp::Cmp && has_a && has_b) {
                    bool ok = true;
                    bool res = false;
                    if (ca->second.is_float || cb->second.is_float) {
                        double x = ca->second.is_float ? ca->second.f
                                                       : static_cast<double>(ca->second.i);
                        double y = cb->second.is_float ? cb->second.f
                                                       : static_cast<double>(cb->second.i);
                        switch (in.cop) {
                            case CmpKind::Lt: res = x < y; break;
                            case CmpKind::Le: res = x <= y; break;
                            case CmpKind::Gt: res = x > y; break;
                            case CmpKind::Ge: res = x >= y; break;
                            case CmpKind::Eq: res = x == y; break;
                            case CmpKind::Ne: res = x != y; break;
                        }
                    } else {
                        int64_t x = ca->second.i;
                        int64_t y = cb->second.i;
                        switch (in.cop) {
                            case CmpKind::Lt: res = x < y; break;
                            case CmpKind::Le: res = x <= y; break;
                            case CmpKind::Gt: res = x > y; break;
                            case CmpKind::Ge: res = x >= y; break;
                            case CmpKind::Eq: res = x == y; break;
                            case CmpKind::Ne: res = x != y; break;
                        }
                    }
                    if (!ok) continue;
                    set_const_int(in, res ? 1 : 0);
                    consts[in.dst] = CVal{false, res ? 1 : 0, 0.0};
                    continue;
                }
            }
        }
        dce_function(fn);
    }
    };

    fold_all();

    // 再内联静态小函数，然后把剩余静态调用改成直接调用。
    auto gfunc = collect_static_func_globals(m);
    auto recursive = recursive_functions(m, gfunc);
    for (size_t i = 0; i < m.funcs.size(); ++i)
        inline_calls_in_function(m, *m.funcs[i], static_cast<int>(i), gfunc, recursive);
    for (auto& fptr : m.funcs) rewrite_direct_calls(m, *fptr, gfunc);
    // 直接化新产生的 call.direct 再给一轮内联机会：静态可解析的闭包调用
    // 现在也是直接下标了。调用图没变（只是边的种类变了），复用 recursive。
    for (size_t i = 0; i < m.funcs.size(); ++i)
        inline_calls_in_function(m, *m.funcs[i], static_cast<int>(i), gfunc, recursive);

    // 内联/直接化后的 CFG 再做循环不变量外提。
    for (auto& fptr : m.funcs) licm_function(*fptr, m.globals.size());

    // 短路布尔值经临时槽往返的形态：转发 + 分支线程化，让 cmp+branch 融合。
    for (auto& fptr : m.funcs) forward_slot_loads(*fptr);

    // 字典计数器模式融合：`d[k] = d.get(k, def) + delta` 一次搞定。
    // 放在内联之后（内联可能带进新形态）、CSE 之前。
    for (auto& fptr : m.funcs) fuse_dict_bump(*fptr);

    // 循环内不变量的检查型下标：入口检一次边界，通过则走无检查的循环副本。
    for (auto& fptr : m.funcs) version_bounds_function(*fptr);

    // 最后做循环旋转：体块回边改走条件 latch，消掉每轮的无条件跳转。
    for (auto& fptr : m.funcs) rotate_loops_function(*fptr);

    // 旋转后 CFG 变了，再跑一次槽位转发，收掉新的 load+branch 形态。
    for (auto& fptr : m.funcs) forward_slot_loads(*fptr);

    // 跨块 CSE：条件块算过的纯表达式（如循环条件里的 zr*zr）在体块里复用。
    for (auto& fptr : m.funcs) cse_function(*fptr);

    // RC 冗余消除：降低器为异常安全发射的 temp/temp_take 有些会互相抵消。
    // 放在所有 CFG 变换之后 —— 前面的内联/旋转会产生新的相邻形态。
    for (auto& fptr : m.funcs) rc_redundant_pairs(*fptr);

    // 内联带进来的实参常量继续折；顺带清掉失效的 load.global / func.addr。
    fold_all();
}

}  // namespace pyadamas
