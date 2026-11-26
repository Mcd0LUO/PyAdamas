// opt/fold.cpp —— 常量判定辅助、纯指令判定与 DCE
#include "opt/detail.h"

namespace pyadamas {
namespace optdetail {

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

bool opt_may_raise(const Inst& in, const IRFunction* fn) {
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
            // 整数 / 浮点的除零、取模零都抛；其它算术只回绕。
            return in.bop == BinKind::Div || in.bop == BinKind::Mod ||
                   in.bop == BinKind::FloorDiv;
        case IrOp::Convert: {
            if (!in.ty) return false;
            // 只有「目标 Int（i64）」的转换可能抛：source 是 float 时 codegen 走
            // cvttsd2si + INT64_MIN 哨兵检查（NaN / ±inf / 越界 → ValueError /
            // OverflowError）。目标定宽整数走裸 cvttsd2si + 截断，Bool / Float /
            // 其它目标也不抛。
            if (in.ty->kind != TyKind::Int) return false;
            // 拿不到操作数类型：保守，目标 Int 的转换一律算可能抛。
            if (!fn) return true;
            TypePtr at = (in.a != kNoReg && static_cast<size_t>(in.a) < fn->vreg_ty.size())
                             ? fn->vreg_ty[static_cast<size_t>(in.a)]
                             : nullptr;
            if (!at) return true;
            // unknown 运行期可能是 float（装箱）——按可能抛处理。
            return ir_is_float(at) || at->kind == TyKind::Unknown;
        }
        default:
            return false;
    }
}

bool inst_is_pure(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::ConstStr:
        case IrOp::ConstNone:
        case IrOp::ConstBig:
        case IrOp::FuncAddr:
        case IrOp::FuncCode:
        case IrOp::Mov:
        case IrOp::LoadSlot:
        case IrOp::LoadGlobal:
        case IrOp::LoadExtern:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Len:
        case IrOp::IndexLoadUnchecked:
        case IrOp::FieldLoad:
        case IrOp::PtrLoad:
            return true;
        case IrOp::Bin: {
            TypePtr t = in.ty;
            if (t && (t->kind == TyKind::Str || t->kind == TyKind::Complex)) return false;
            if (ir_is_float(t)) return bin_is_safe_float(in.bop);
            return bin_is_safe_int(in.bop);
        }
        case IrOp::Convert: {
            // 转换本身无副作用，但 str 目标要走运行期调用 / 分配，按有副作用处理。
            // 其余数值目标：可能抛异常与否**只由 opt_may_raise 决定**（DCE 删掉
            // 一条会抛的指令会静默吞异常：`int(float("inf"))` 结果没人用的情况
            // 0.427 实测错值）。这里同样按保守口径（没有 fn 上下文）调用，
            // 与 0.676 的判定逐字一致。
            if (!in.ty) return false;
            if (in.ty->kind == TyKind::Str) return false;
            const bool numeric_target = in.ty->kind == TyKind::Bool ||
                                        in.ty->kind == TyKind::Int ||
                                        is_fixed_int(in.ty) || ir_is_float(in.ty);
            return numeric_target && !opt_may_raise(in, nullptr);
        }
        default:
            return false;
    }
}

void dce_function(IRFunction& fn) {
    if (fn.vreg_ty.empty()) return;
    // 使用计数：一趟线性扫描的 vector，而不是每轮 rebuild_use_def() 重建
    // 三张 unordered_map（inst_by_id / def_of / uses_of）——DCE 只需要
    // 「某个 vreg 被读了几次」这一个数值，而哈希表插入在大函数上占了
    // 编译时间的大头（nbody 的 simulate/advance 各 4000+ vreg）。
    //
    // 语义等价：删除一条纯指令时把它读过的 vreg 计数精确递减，等于下一轮
    // 重建索引后的结果（删指令只会让计数变小）。删除集合的不动点唯一
    //（「dst 在剩余指令里没人读」只能因删除而扩大），所以最终 IR 与
    // 原来「最多晚一轮删除」的逐轮实现逐位相同，指令顺序也不变。
    const size_t nv = fn.vreg_ty.size();
    std::vector<uint32_t> uses(nv, 0);
    auto add_uses = [&](const Inst& in, int delta) {
        auto add = [&](VReg r) {
            if (r != kNoReg && r < nv) {
                if (delta > 0) ++uses[r];
                else --uses[r];
            }
        };
        add(in.a);
        add(in.b);
        add(in.c);
        for (VReg r : in.args) add(r);
    };
    for (auto& b : fn.blocks)
        for (auto& in : b.insts) add_uses(in, +1);
    // 反复扫到不动点：删除一条纯指令会让它读过的 vreg 计数归零，可能让
    // **前面**已经扫过的指令变成死指令（旧实现靠外层 while 每轮重建索引
    // 做到这一点）。计数已经精确维护，同一趟里的删除立刻可见，所以第二轮
    // 通常只删掉这些「回头看」的残余。
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& b : fn.blocks) {
            auto& v = b.insts;
            for (size_t i = 0; i < v.size();) {
                const Inst& in = v[i];
                bool dead = in.dst != kNoReg && (in.dst >= nv || uses[in.dst] == 0) &&
                            inst_is_pure(in);
                if (!dead) {
                    ++i;
                    continue;
                }
                add_uses(in, -1);
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
            }
        }
    }
}

}  // namespace optdetail
}  // namespace pyadamas
