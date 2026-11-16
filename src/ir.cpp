// ir.cpp —— IR 的可读打印
//
// 打印器的用处不只是调试：`pyadamas ir x.py` 的输出是降级阶段的验收标准。
// 每条指令都带结果类型注释，能一眼看出前端推出来的类型对不对。
#include "ir.h"

namespace pyadamas {
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

// 重新扫描整个函数，重建稳定 ID 与 use-def 索引。
//
// 触发时机：任何会改动块/指令结构的 pass 之后（内联、循环旋转、
// 边界版本化、DCE 删除）。代价 O(指令数)，相对那些 pass 本身可忽略。
//
// 索引的语义（刻意保持简单）：
//   def_of[v]   = 定义 v 的最后一条指令 id（多次定义只留最后一条）
//   uses_of[v]  = 所有把 v 当操作数读的指令 id（按扫描顺序）
// 不做 SSA 那套精确单定义检查 —— 这个 IR 的 vreg 本来就是可多次定义的
// 临时值，索引只服务「快速找到使用点」，不承担正确性判定。
void IRFunction::rebuild_use_def() {
    inst_by_id.clear();
    def_of.clear();
    uses_of.clear();
    for (int b = 0; b < static_cast<int>(blocks.size()); ++b) {
        auto& insts = blocks[static_cast<size_t>(b)].insts;
        for (int k = 0; k < static_cast<int>(insts.size()); ++k) {
            Inst& in = insts[static_cast<size_t>(k)];
            const uint32_t id = ensure_inst_id(in);
            inst_by_id[id] = {b, k};
            if (in.dst != kNoReg) def_of[in.dst] = id;
            auto use = [&](VReg r) {
                if (r != kNoReg) uses_of[r].push_back(id);
            };
            use(in.a);
            use(in.b);
            use(in.c);
            for (VReg r : in.args) use(r);
        }
    }
}
// 调试期校验：op 与 idx.kind 是否匹配。
//
// 这是 Operand 存在的**主要理由**：原来 idx 是裸 int32_t，op 说它是
// 槽号、代码里却填了全局号，编译器完全不管。现在 kind 被记下来，
// 这里能在 dump / 断言时抓出来。迁移期 kind 可能是 None（存量代码还没
// 改成显式构造），所以 None 一律放行 —— 等迁移完成再收紧成必填。
static bool idx_kind_ok(IrOp op, Operand::Kind k) {
    if (k == Operand::None) return true;  // 迁移期放行
    switch (op) {
        case IrOp::LoadSlot:
        case IrOp::StoreSlot:
        case IrOp::SlotAddr:
            return k == Operand::Slot;
        case IrOp::LoadGlobal:
        case IrOp::StoreGlobal:
        case IrOp::GlobalAddr:
            return k == Operand::Global;
        case IrOp::ConstStr:
            return k == Operand::Str;
        case IrOp::FuncAddr:
        case IrOp::FuncCode:
        case IrOp::CallDirect:
            return k == Operand::Func;
        case IrOp::ClassAddr:
            return k == Operand::Class;
        case IrOp::CallExtern:
        case IrOp::LoadExtern:
        case IrOp::StoreExtern:
            return k == Operand::Extern;
        case IrOp::FieldLoad:
        case IrOp::FieldStore:
            return k == Operand::Field;
        default:
            return true;  // 该 op 不用 idx
    }
}

std::string Inst::text() const {
    if (std::getenv("PYADAMAS_IR_CHECK") && !idx_kind_ok(op, idx.kind))
        std::fprintf(stderr, "[ir] op=%s idx.kind=%d 不匹配（值 %d）\n",
                     irop_name(op), static_cast<int>(idx.kind), idx.value);
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
        case IrOp::GlobalAddr: return std::format("{} = global.addr g{}", vreg_str(dst), idx);
        case IrOp::ClassAddr: return std::format("{} = class.addr c{}", vreg_str(dst), imm);
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
        case IrOp::PtrLoad:
            return std::format("{} = ptr.load {}[{}]", vreg_str(dst), vreg_str(a), vreg_str(b));
        case IrOp::PtrStore:
            return std::format("ptr.store {}[{}] = {}", vreg_str(a), vreg_str(b), vreg_str(c));
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
        case IrOp::CallPtr: {
            std::string s = std::format("{} = call.ptr {}(", vreg_str(dst), vreg_str(a));
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::StructNew:
            return std::format("{} = struct.new {}", vreg_str(dst), imm);
        case IrOp::StructCopy:
            return std::format("{} = struct.copy {}, {}", vreg_str(dst), vreg_str(a), imm);
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
        case IrOp::CallExtern: {
            std::string s = std::format("{} = call.extern e{}(", vreg_str(dst), idx);
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += vreg_str(args[i]);
            }
            return s + ")";
        }
        case IrOp::LoadExtern:
            return std::format("{} = load.extern v{}", vreg_str(dst), idx);
        case IrOp::StoreExtern:
            return std::format("store.extern v{}, {}", idx, vreg_str(a));
        case IrOp::Setjmp:
            return std::format("{} = setjmp {}", vreg_str(dst), vreg_str(a));
        case IrOp::EhBegin:
            return "eh.begin";
        case IrOp::EhEnd:
            return "eh.end";
        case IrOp::EhLandingPad:
            return "eh.landing.pad";
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
        case IrOp::GenTakeExc: return std::format("{} = gen.take.exc", vreg_str(dst));
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
            case RtFn::IsInstanceClass:
            case RtFn::ClassNew:
            case RtFn::ClassName:
            case RtFn::ClassRepr:
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
                    if (fn == RtFn::RcIncUnknown || fn == RtFn::RcDecUnknown ||
                        registry_rtfn(fn))
                        return true;
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
        os << std::format("string #{} = {} (pub {})\n", i,
                          Constant::quote(m.strings[i].text), m.strings[i].pub);
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

}  // namespace pyadamas
