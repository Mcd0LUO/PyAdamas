// ir.cpp —— IR 的可读打印
//
// 打印器的用处不只是调试：`py23 ir x.py` 的输出是降级阶段的验收标准。
// 每条指令都带结果类型注释，能一眼看出前端推出来的类型对不对。
#include "ir.h"

namespace py23 {
namespace {

std::string vreg_str(VReg r) {
    if (r == kNoReg) return "_";
    return std::format("%{}", r);
}

std::string ty_note(const TypePtr& t) {
    if (!t) return "";
    return std::format("  ; {}", type_name(t));
}

}  // namespace

std::string Inst::text() const {
    switch (op) {
        case IrOp::ConstInt: return std::format("{} = const.int {}", vreg_str(dst), imm);
        case IrOp::ConstFloat: return std::format("{} = const.float {}", vreg_str(dst), fimm);
        case IrOp::ConstStr: return std::format("{} = const.str #{}", vreg_str(dst), idx);
        case IrOp::ConstNone: return std::format("{} = const.none", vreg_str(dst));
        case IrOp::FuncAddr: return std::format("{} = func.addr f{}", vreg_str(dst), imm);
        case IrOp::FuncCode: return std::format("{} = func.code f{}", vreg_str(dst), imm);
        case IrOp::Mov: return std::format("{} = mov {}", vreg_str(dst), vreg_str(a));
        case IrOp::Bin:
            return std::format("{} = bin.{} {}, {}", vreg_str(dst), bin_name(bop), vreg_str(a),
                               vreg_str(b));
        case IrOp::Un:
            return std::format("{} = un.{} {}", vreg_str(dst), un_name(uop), vreg_str(a));
        case IrOp::Cmp:
            return std::format("{} = cmp.{} {}, {}", vreg_str(dst), cmp_name(cop), vreg_str(a),
                               vreg_str(b));
        case IrOp::Convert:
            return std::format("{} = convert {}", vreg_str(dst), vreg_str(a));
        case IrOp::LoadSlot: return std::format("{} = load.slot s{}", vreg_str(dst), idx);
        case IrOp::StoreSlot: return std::format("store.slot s{}, {}", idx, vreg_str(a));
        case IrOp::LoadGlobal: return std::format("{} = load.global g{}", vreg_str(dst), idx);
        case IrOp::StoreGlobal: return std::format("store.global g{}, {}", idx, vreg_str(a));
        case IrOp::MakeList:
        case IrOp::MakeTuple: {
            std::string s = std::format("{} = {}{{", vreg_str(dst),
                                        op == IrOp::MakeList ? "list.new" : "tuple.new");
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + "}";
        }
        case IrOp::IndexLoad:
            return std::format("{} = index.get {}[{}]", vreg_str(dst), vreg_str(a), vreg_str(b));
        case IrOp::IndexLoadUnchecked:
            return std::format("{} = index.get.unchecked {}[{}]", vreg_str(dst), vreg_str(a),
                               vreg_str(b));
        case IrOp::IndexStore:
            return std::format("index.set {}[{}] = {}", vreg_str(a), vreg_str(b), vreg_str(c));
        case IrOp::Len: return std::format("{} = len {}", vreg_str(dst), vreg_str(a));
        case IrOp::ObjNew: return std::format("{} = obj.new c{}", vreg_str(dst), imm);
        case IrOp::FieldLoad:
            return std::format("{} = field.get {}+{}", vreg_str(dst), vreg_str(a), imm);
        case IrOp::FieldStore:
            return std::format("field.set {}+{} = {}", vreg_str(a), imm, vreg_str(b));
        case IrOp::VCall: {
            std::string s = std::format("{} = vcall {}[{}]({}", vreg_str(dst), vreg_str(a), imm,
                                        vreg_str(a));
            for (size_t i = 0; i < args.size(); ++i) s += ", " + vreg_str(args[i]);
            return s + ")";
        }
        case IrOp::VCallStatic: {
            std::string s = std::format("{} = vcall.static {}[{}](", vreg_str(dst), vreg_str(a),
                                        imm);
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::SuperVCall: {
            std::string s = std::format("{} = super[{}] {}[{}]({}", vreg_str(dst), idx,
                                        vreg_str(a), imm, vreg_str(a));
            for (size_t i = 0; i < args.size(); ++i) s += ", " + vreg_str(args[i]);
            return s + ")";
        }
        case IrOp::CallDirect: {
            std::string s = std::format("{} = call.direct f{}({}", vreg_str(dst), idx,
                                        "");
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::Call: {
            std::string s = std::format("{} = call {}(", vreg_str(dst), vreg_str(a));
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::CallRt: {
            std::string s = std::format("{} = call.rt {}({}", vreg_str(dst),
                                        rt_name(static_cast<RtFn>(imm)), "");
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::Setjmp:
            return std::format("{} = setjmp {}", vreg_str(dst), vreg_str(a));
        case IrOp::ImportModule:
            return std::format("import.module m{}", imm);
        case IrOp::SlotAddr: return std::format("{} = slot.addr s{}", vreg_str(dst), idx);
        case IrOp::GenNew: return std::format("{} = gen.new g{}", vreg_str(dst), imm);
        case IrOp::GenSetSlot:
            return std::format("gen.set {}, s{}, {}", args.empty() ? "_" : vreg_str(args[0]), idx,
                               args.size() > 1 ? vreg_str(args[1]) : "_");
        case IrOp::GenGetState: return std::format("{} = gen.state", vreg_str(dst));
        case IrOp::GenClosing: return std::format("{} = gen.closing", vreg_str(dst));
        case IrOp::GenSent: return std::format("{} = gen.sent", vreg_str(dst));
        case IrOp::GenYield:
            return std::format("gen.yield {}, state={}", vreg_str(a), imm);
        case IrOp::GenAwait:
            return std::format("gen.await {}, state={}", vreg_str(a), imm);
        case IrOp::GenGetExc: return std::format("{} = gen.exc", vreg_str(dst));
        case IrOp::GenRethrow: return "gen.rethrow";
        case IrOp::GenDone:
            return a == kNoReg ? "gen.done" : std::format("gen.done {}", vreg_str(a));
        case IrOp::Jump: return std::format("jump bb{}", target);
        case IrOp::Branch:
            return std::format("branch {}, bb{}, bb{}", vreg_str(a), target, target2);
        case IrOp::Return:
            return a == kNoReg ? "return none" : std::format("return {}", vreg_str(a));
    }
    return "?";
}

bool ir_type_contains_unknown(const TypePtr& t) {
    // 容器元素类型可能递归指回容器自身，限制深度避免无限下钻。
    struct Walk {
        static bool go(const TypePtr& x, int depth) {
            if (!x || depth > 8) return false;
            if (x->kind == TyKind::Unknown) return true;
            for (const auto& s : x->sub)
                if (go(s, depth + 1)) return true;
            if (x->fn) {
                for (const auto& p : x->fn->pos)
                    if (go(p.ty, depth + 1)) return true;
                for (const auto& p : x->fn->kwonly)
                    if (go(p.ty, depth + 1)) return true;
                if (go(x->fn->ret, depth + 1)) return true;
                if (go(x->fn->vararg_ty, depth + 1)) return true;
                if (go(x->fn->kwarg_ty, depth + 1)) return true;
                if (go(x->fn->async_result, depth + 1)) return true;
            }
            return false;
        }
    };
    return Walk::go(t, 0);
}

bool ir_module_needs_unknown_index(const IRModule& m) {
    // asyncio / 生成器运行时内部无条件地用 unknown 保守 retain 等待结果
    // 和帧值（rt_async.c / rt_task.c / rt_agen.c），这些调用在 IR 里
    // 看不出来，只能按模块整体保守打开。
    if (!m.generators.empty()) return true;
    for (const auto& name : m.imports)
        if (name == "asyncio") return true;
    for (const auto& g : m.globals)
        if (ir_type_contains_unknown(g.ty)) return true;

    // 运行期类型检查也靠这张注册表确认「这个机器字确实是某类堆对象」：
    // super()、isinstance、异常匹配、unknown 值上的容器/字符串模式。
    // 这些调用即使操作数静态类型确定也会走索引，不能关。
    auto registry_rtfn = [](RtFn fn) {
        switch (fn) {
            case RtFn::IsInstance:
            case RtFn::IsNamedClass:
            case RtFn::IsException:
            case RtFn::IsSeq:
            case RtFn::IsDict:
            case RtFn::StrEqUnknown:
            case RtFn::ComplexEqUnknown:
            case RtFn::IsStr:
            case RtFn::IsComplex:
                return true;
            default:
                return false;
        }
    };

    for (const auto& fp : m.funcs) {
        const IRFunction& f = *fp;
        if (ir_type_contains_unknown(f.ret)) return true;
        for (const auto& p : f.params)
            if (ir_type_contains_unknown(p.ty)) return true;
        for (const auto& t : f.slot_ty)
            if (ir_type_contains_unknown(t)) return true;
        for (const auto& t : f.vreg_ty)
            if (ir_type_contains_unknown(t)) return true;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::SuperVCall) return true;
                if (in.op == IrOp::CallRt) {
                    RtFn fn = static_cast<RtFn>(in.imm);
                    if (fn == RtFn::RcIncUnknown || registry_rtfn(fn)) return true;
                }
            }
    }
    return false;
}

void dump_ir(const IRModule& m, std::ostream& os) {
    os << std::format("; globals: {}\n", m.globals.size());
    for (size_t i = 0; i < m.globals.size(); ++i)
        os << std::format("global g{} {} : {}\n", i, m.globals[i].name,
                          type_name(m.globals[i].ty));
    for (size_t i = 0; i < m.strings.size(); ++i)
        os << std::format("string #{} = {}\n", i, Constant::quote(m.strings[i]));
    for (size_t i = 0; i < m.classes.size(); ++i) {
        const auto& c = m.classes[i];
        os << std::format("class c{} {} ({} fields, vtable:", i, c.name, c.n_fields);
        for (int f : c.vtable) os << std::format(" f{}", f);
        os << ")\n";
    }
    os << "\n";

    for (size_t fi = 0; fi < m.funcs.size(); ++fi) {
        const auto& f = *m.funcs[fi];
        os << std::format("func f{} {}(", fi, f.name);
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) os << ", ";
            os << std::format("s{}:{}", f.params[i].slot, type_name(f.params[i].ty));
        }
        os << std::format(") -> {} {{\n", type_name(f.ret));
        os << "  slots:";
        for (size_t i = 0; i < f.slot_ty.size(); ++i)
            os << std::format(" s{}={}", i, type_name(f.slot_ty[i]));
        os << "\n";

        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            const auto& b = f.blocks[bi];
            os << std::format("bb{} ({}):\n", bi, b.name);
            for (const auto& in : b.insts)
                os << "  " << in.text()
                   << (in.dst != kNoReg ? ty_note(in.ty) : std::string()) << "\n";
        }
        os << "}\n\n";
    }
}

}  // namespace py23
