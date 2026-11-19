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
    block_by_id.clear();
    def_of.clear();
    uses_of.clear();
    for (int b = 0; b < static_cast<int>(blocks.size()); ++b) {
        block_by_id[ensure_block_id(blocks[static_cast<size_t>(b)])] = b;
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
        case IrOp::IndexStoreUnchecked:
            return std::format("index.set.unchecked {}[{}] = {}", vreg_str(a), vreg_str(b),
                               vreg_str(c));
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

/* 判断一个类型里的 unknown 是否只出现在**容器元素**位置。
 *
 * 这是方案 B 的核心：全局 rc_index 的作用是「拿一个机器字反查它是不是堆对象」。
 * 只有下面两种情况真的需要它：
 *
 *   1. 一个**槽 / 返回值 / 参数**的静态类型是 unknown —— 这个字可能是堆指针，
 *      也可能是整数，release 时必须反查；
 *   2. 容器元素类型是 unknown 且该容器**没有**逐元素位图覆盖。
 *
 * 而 `list[unknown]` 这种「unknown 只在元素位置」的情况，方案 A 的 heap_bits
 * 已经在写入点按静态类型记录了每个元素是不是堆指针，销毁时不需要反查。
 *
 * 所以：容器（list/tuple/set/dict 的 value）内部的 unknown 不算「需要索引」。 */
static bool unknown_only_in_containers(const TypePtr& t, int depth) {
    if (!t || depth > 8) return true;
    switch (t->kind) {
        case TyKind::Unknown:
            return false;   // 裸 unknown：需要索引
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Tuple:
            return true;    // 元素由位图覆盖，不往子类型下钻
        case TyKind::Dict:
            // key 走 key_kind 的哈希比较，不参与 dec；value 由 own_kind + 位图覆盖。
            return true;
        case TyKind::Optional:
            // Optional[unknown] 等价于裸 unknown。
            if (t->sub.empty()) return true;
            return unknown_only_in_containers(t->sub[0], depth + 1);
        case TyKind::Func:
            if (t->fn) {
                for (const auto& p : t->fn->pos)
                    if (!unknown_only_in_containers(p.ty, depth + 1)) return false;
                for (const auto& p : t->fn->kwonly)
                    if (!unknown_only_in_containers(p.ty, depth + 1)) return false;
                if (!unknown_only_in_containers(t->fn->ret, depth + 1)) return false;
                if (!unknown_only_in_containers(t->fn->vararg_ty, depth + 1)) return false;
                if (!unknown_only_in_containers(t->fn->kwarg_ty, depth + 1)) return false;
            }
            return true;
        default:
            return true;
    }
}

/* 判断模块能否关闭**索引维护**（方案 C）。
 *
 * 与 needs_unknown_index 的区别：
 *   - needs_unknown_index=true  → 代码里有「反查这个字是不是堆对象」的
 *     依赖，索引必须建；
 *   - needs_unknown_index=false → exact-RC，索引完全不建；
 *   - **本函数**回答第三种情况：代码里有 unknown 值，但那些值只走
 *     「容器位图 / 帧记录」等不依赖索引的路径 —— 索引建了也没人查，
 *     维护它是纯开销。
 *
 * 实测依据（关闭索引维护跑全量 205 个用例）：45 个失败全部集中在
 * 生成器 / 协程 / 异常类 / 类对象 / 裸 unknown 槽这几类。反过来说，
 * 不含这些特性的模块可以安全关闭 —— bench_rc_gen 正是这种，省 22ms。
 *
 * 保守起见这里只放行**明确安全**的特征组合，任何一项可疑都返回 false。 */
bool ir_module_can_skip_index_maint(const IRModule& m) {
    // 生成器 / 协程：运行时用 unknown 保守 retain 帧值，依赖索引。
    if (!m.generators.empty()) return false;
    // **只要不是纯内置运算就排除。**
    //
    // 运行时内部的实现细节会用 pa_rc_inc_unknown / pa_rc_dec_unknown 处理
    // 动态值，那些调用在 IR 里只是一个普通 CallRt，看不出它依赖索引：
    //   - 185_fnmatch / 186_copy → 列表元素丢失；
    //   - 86_popen → 字符串被多包一层引号（IsStr 判断失效）。
    //
    // m.imports 记不到 subprocess 这类内置模块（它们不走 import 列表），
    // 所以用更硬的判据：**全局变量只允许内置标量 / 容器类型**。
    //
    // 86_popen 的全局是 `subprocess.Popen`，它的运行时实现会调用 IsStr 之类
    // 依赖索引的判定（表现为字符串被多包一层引号）。只放行纯内置类型的模块。
    if (m.funcs.size() > 1) return false;
    if (!m.imports.empty()) return false;
    for (const auto& g : m.globals) {
        if (!g.ty) continue;
        switch (g.ty->kind) {
            case TyKind::Unknown:
            case TyKind::None:
            case TyKind::Bool:
            case TyKind::Int:
            case TyKind::Float:
            case TyKind::Str:
            case TyKind::Bytes:
            case TyKind::List:
            case TyKind::Dict:
            case TyKind::Tuple:
            case TyKind::Set:
            case TyKind::Range:
            case TyKind::Optional:
                break;
            default:
                // Class / File / Popen / Regex / DateTime … 都可能依赖索引。
                return false;
        }
    }
    for (const auto& fp : m.funcs) {
        const IRFunction& f = *fp;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op != IrOp::CallRt) continue;
                RtFn fn = static_cast<RtFn>(in.imm);
                switch (fn) {
                    // 类 / 异常 / 运行期类型检查：靠索引确认「这个字是哪个类」。
                    case RtFn::IsInstance:
                    case RtFn::IsInstanceClass:
                    case RtFn::ClassNew:
                    case RtFn::ClassName:
                    case RtFn::ClassRepr:
                    case RtFn::IsNamedClass:
                    case RtFn::IsException:
                    case RtFn::IsSeq:
                    case RtFn::IsDict:
                    case RtFn::IsStr:
                    case RtFn::IsComplex:
                    case RtFn::StrEqUnknown:
                    case RtFn::ComplexEqUnknown:
                    // unknown 真值 / repr 打印 / 通用打印都要靠索引确认
                    // 「这个机器字确实是堆对象」；索引只读（跳过维护）会让
                    // 字面量 str 探不到（空串恒真、打印出地址）。
                    case RtFn::TruthAny:
                    case RtFn::PrintAny:
                    case RtFn::PrintAnyRepr:
                        return false;
                    // CPython 桥：结果是 unknown 机器字，float 盒/容器在
                    // 运行时分配，必须进存活索引才能被探针/print/temp 认出。
                    // 索引只读（skip maint）会让这些对象「看不见」。
                    case RtFn::CpyCall:
                    case RtFn::RcTempUnknown:
                        return false;
                    default:
                        break;
                }
            }
        // 裸 unknown 的槽 / 返回值 / 参数：dec 时可能反查。
        if (!unknown_only_in_containers(f.ret, 0)) return false;
        for (const auto& p : f.params)
            if (!unknown_only_in_containers(p.ty, 0)) return false;
        for (const auto& t : f.slot_ty)
            if (!unknown_only_in_containers(t, 0)) return false;
    }
    return true;
}

bool ir_module_needs_unknown_index(const IRModule& m) {
    // asyncio / 生成器运行时内部无条件地用 unknown 保守 retain 等待结果
    // 和帧值（rt_async.c / rt_task.c / rt_agen.c），这些调用在 IR 里
    // 看不出来，只能按模块整体保守打开。
    if (!m.generators.empty()) return true;
    for (const auto& name : m.imports)
        if (name == "asyncio") return true;
    for (const auto& g : m.globals)
        // **全局变量：用 maybe_unknown 标志判断，而不是看类型。**
        //
        // 一个全局变量的静态类型是 unknown，并不代表它的**当前值**可能是
        // 非堆整数：lower_impl_slots 已经维护了 maybe_unknown —— 只有真正
        // 以 unknown 值写入过才置位，且旧值释放本来就走 rc_dec_unknown。
        //
        // 而 rc_dec_unknown 在索引关闭（exact-RC）时有完整回退路径
        // （untracked leaf 看 marked 位、真容器遍历堆链），语义正确。
        // 所以这类全局变量不再阻塞 exact-RC。
        // 裸 unknown 全局变量**必须**保留索引：它的值可能是整数也可能是堆指针，
        // 而 exact-RC 模式下 unknown_is_heap 的回退（marked 位 + 遍历堆链）
        // 在对象已被 GC 释放后会读悬空内存（实测 heap-use-after-free）。
        if (!unknown_only_in_containers(g.ty, 0)) return true;

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
            // unknown 的真值判断与 repr 打印都要靠注册表确认「这个机器字
            // 确实是堆对象」：索引一关，字面量 str 会被当成整数（真值恒真、
            // 打印出地址）—— filter(None, 异构列表) 的静默错就来自这里。
            case RtFn::TruthAny:
            case RtFn::PrintAny:
            case RtFn::PrintAnyRepr:
            // CPython 桥结果是 unknown 机器字（可能是堆对象）：
            // RcTempUnknown / print / marshalling 探针都靠注册表，
            // 用了桥的模块必须保留索引。
            case RtFn::CpyCall:
                return true;
            default:
                return false;
        }
    };

    for (const auto& fp : m.funcs) {
        const IRFunction& f = *fp;
        // **方案 B：按位置区分 unknown 的危害。**
        //
        // 裸 unknown 出现在槽 / 返回值 / 参数上时，这个机器字可能是堆指针也
        // 可能是整数，release 必须靠全局索引反查 —— 保留索引。
        //
        // 但 list[unknown] 这种「unknown 只作为容器元素」的情况不需要：
        // 方案 A 的 heap_bits 已在写入点按静态类型记下每个元素是不是堆指针，
        // 销毁时按位图 dec，不查索引。放宽这一条让 bench_rc_gen 这类
        // 「混合类型列表」的模块也能走 exact-RC（实测 83ms → 56ms）。
        if (!unknown_only_in_containers(f.ret, 0)) return true;
        for (const auto& p : f.params)
            if (!unknown_only_in_containers(p.ty, 0)) return true;
        for (const auto& t : f.slot_ty)
            if (!unknown_only_in_containers(t, 0)) return true;
        // **vreg 上的 unknown：区分「只是过手给 rc_*_unknown」和「真的被当值用」。**
        //
        // 形如 `%62 = load.global g4` 然后 `pa_rc_dec_unknown(%62)` 的 vreg，
        // 它的 unknown 唯一去处就是 rc_dec_unknown —— 那个函数在索引关闭时
        // 有完整回退（untracked leaf 看 marked、真容器遍历堆链），不需要索引。
        //
        // 其余用途（参与运算、比较、传给别的运行时函数）才真的需要索引。
        std::vector<char> unknown_only_for_rc(f.vreg_ty.size(), 0);
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op != IrOp::CallRt) continue;
                RtFn fn = static_cast<RtFn>(in.imm);
                // 这些函数把参数**当容器对象**用（拿的是列表指针），不按
                // 「可能是整数」对待，所以操作数上的 unknown 不需要索引。
                bool container_use = fn == RtFn::ListPrep || fn == RtFn::ListAppend ||
                                    fn == RtFn::ListExtend || fn == RtFn::ListSetLen ||
                                    fn == RtFn::ListCopyRange ||
                                    fn == RtFn::RcIncUnknown || fn == RtFn::RcDecUnknown;
                if (!container_use) continue;
                for (const auto& a : in.args)
                    if (a < unknown_only_for_rc.size()) unknown_only_for_rc[a] = 1;
            }
        for (size_t vi = 0; vi < f.vreg_ty.size(); ++vi)
            if (!unknown_only_in_containers(f.vreg_ty[vi], 0) && !unknown_only_for_rc[vi]) return true;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op == IrOp::SuperVCall) return true;
                if (in.op == IrOp::CallRt) {
                    RtFn fn = static_cast<RtFn>(in.imm);
                    // RcIncUnknown / RcDecUnknown：只在**操作数是裸 unknown**
                    // 时才需要索引。若该 vreg 的静态类型是具体容器（元素由
                    // 位图覆盖），这些调用是容器内部的元素计数，不反查。
                    if (fn == RtFn::RcIncUnknown || fn == RtFn::RcDecUnknown) {
                        // 同上：裸 unknown 操作数保留索引。
                        TypePtr at = !in.args.empty() && in.args[0] < f.vreg_ty.size()
                                          ? f.vreg_ty[in.args[0]]
                                          : nullptr;
                        if (!unknown_only_in_containers(at, 0)) return true;
                        continue;
                    }
                    if (registry_rtfn(fn)) return true;
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
