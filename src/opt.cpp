// opt.cpp —— O1/O2 的 IR 局部优化遍
//
// 目前包含：
//   - 常量折叠（整数/浮点算术、比较、一元运算、数值转换）
//   - 常量条件分支化简为 jump
//   - 纯标量指令的死代码消除（迭代到不动点）
//
// 这里只做语义保守的标量优化：不会删除可能抛异常的 Div/Mod/Pow、
// 带 RC 副作用的调用/容器分配，也不跨基本块搬移内存读写。
#include "opt.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace py23 {

namespace {

struct CVal {
    bool is_float = false;
    int64_t i = 0;
    double f = 0.0;
};

bool bin_is_safe_int(BinKind b) {
    switch (b) {
        case BinKind::Add:
        case BinKind::Sub:
        case BinKind::Mul:
        case BinKind::BitAnd:
        case BinKind::BitOr:
        case BinKind::BitXor:
            return true;
        default:
            return false;
    }
}

bool bin_is_safe_float(BinKind b) {
    return b == BinKind::Add || b == BinKind::Sub || b == BinKind::Mul;
}

void set_const_int(Inst& in, int64_t v) {
    in.op = IrOp::ConstInt;
    in.imm = v;
    in.fimm = 0.0;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

void set_const_float(Inst& in, double v) {
    in.op = IrOp::ConstFloat;
    in.imm = 0;
    in.fimm = v;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

bool inst_is_pure(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::ConstStr:
        case IrOp::ConstNone:
        case IrOp::FuncAddr:
        case IrOp::FuncCode:
        case IrOp::Mov:
        case IrOp::LoadSlot:
        case IrOp::LoadGlobal:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Len:
        case IrOp::IndexLoadUnchecked:
            return true;
        case IrOp::Bin: {
            TypePtr t = in.ty;
            if (t && (t->kind == TyKind::Str || t->kind == TyKind::Complex)) return false;
            if (ir_is_float(t)) return bin_is_safe_float(in.bop);
            return bin_is_safe_int(in.bop);
        }
        case IrOp::Convert:
            return in.ty && (in.ty->kind == TyKind::Bool || in.ty->kind == TyKind::Int ||
                             in.ty->kind == TyKind::Float);
        default:
            return false;
    }
}

void dce_function(IRFunction& fn) {
    if (fn.vreg_ty.empty()) return;
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<int> uses(fn.vreg_ty.size(), 0);
        auto touch = [&](VReg r) {
            if (r >= 0 && r < static_cast<VReg>(uses.size())) ++uses[r];
        };
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
            }
        for (auto& b : fn.blocks) {
            auto& v = b.insts;
            for (size_t i = 0; i < v.size();) {
                const Inst& in = v[i];
                bool dead = in.dst != kNoReg && in.dst >= 0 &&
                            in.dst < static_cast<VReg>(uses.size()) && uses[in.dst] == 0 &&
                            inst_is_pure(in);
                if (!dead) {
                    ++i;
                    continue;
                }
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                // uses 的计数已经过期；下一轮外层 while 重新统计。
                // 这里继续扫后面的指令是保守的：最多晚一轮删除。
            }
        }
    }
}

}  // namespace

void optimize_ir(IRModule& m, int opt_level) {
    if (opt_level < 2) return;

    for (auto& fptr : m.funcs) {
        IRFunction& fn = *fptr;

        // 扫描一遍常量定义，建立 vreg → 常量值。常量值是静态的，
        // 不依赖定义块是否执行，折叠不会改变控制流语义。
        std::unordered_map<VReg, CVal> consts;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                if (in.dst == kNoReg || in.dst < 0) continue;
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
}

}  // namespace py23
