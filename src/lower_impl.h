// lower_impl.h —— Lowerer 的实现细节（只给 lower_*.cpp 用）
//
// 降级器按职责拆成多个实现文件：驱动/预扫描、语句、表达式、调用、
// 生成器与协程。这里放共享的 FuncCtx/ClosureMeta 和 Lowerer 声明。
#pragma once

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lower.h"
#include "module.h"

namespace pyadamas {

// ────────────────────────── 小工具 ──────────────────────────

// 只认字面量常量。默认参数、assert 消息这些地方够用；
// 更复杂的常量表达式留给后续的常量折叠。
struct ConstVal {
    TypePtr ty;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

inline std::optional<ConstVal> as_const(const Expr* e) {
    if (!e || e->kind != ExprKind::Constant) return std::nullopt;
    const auto& c = static_cast<const Constant&>(*e);
    ConstVal v;
    switch (c.ck) {
        case Constant::CK::None: v.ty = t_none(); return v;
        case Constant::CK::Bool: v.ty = t_bool(); v.i = c.b ? 1 : 0; return v;
        case Constant::CK::Int: v.ty = t_int(); v.i = c.i; return v;
        case Constant::CK::Float: v.ty = t_float(); v.f = c.f; return v;
        case Constant::CK::Complex: return std::nullopt;
        case Constant::CK::Str: v.ty = t_str(); v.s = c.s; return v;
        case Constant::CK::Bytes: v.ty = t_bytes(); v.s = c.s; return v;
        case Constant::CK::Ellipsis: return std::nullopt;
    }
    return std::nullopt;
}

inline BinKind map_bin(Tok t) {
    switch (t) {
        case Tok::PLUS: return BinKind::Add;
        case Tok::MINUS: return BinKind::Sub;
        case Tok::STAR: return BinKind::Mul;
        case Tok::SLASH: return BinKind::Div;
        case Tok::DOUBLESLASH: return BinKind::FloorDiv;
        case Tok::PERCENT: return BinKind::Mod;
        case Tok::DOUBLESTAR: return BinKind::Pow;
        case Tok::AMPER: return BinKind::BitAnd;
        case Tok::VBAR: return BinKind::BitOr;
        case Tok::CIRCUMFLEX: return BinKind::BitXor;
        case Tok::LSHIFT: return BinKind::Shl;
        case Tok::RSHIFT: return BinKind::Shr;
        default: return BinKind::Add;
    }
}

inline CmpKind map_cmp(CmpOp op) {
    switch (op) {
        case CmpOp::Lt: return CmpKind::Lt;
        case CmpOp::Le: return CmpKind::Le;
        case CmpOp::Gt: return CmpKind::Gt;
        case CmpOp::Ge: return CmpKind::Ge;
        case CmpOp::Eq: return CmpKind::Eq;
        case CmpOp::Ne: return CmpKind::Ne;
        case CmpOp::Is: return CmpKind::Eq;
        case CmpOp::IsNot: return CmpKind::Ne;
        default: return CmpKind::Eq;
    }
}

// 复合赋值号 → 二元运算符。两套 token 是分开的（`*=` 和 `*`），
// 直接丢给 map_bin 会落到 default 分支，`acc *= 10` 被悄悄编译成 `acc += 10`。
inline Tok aug_to_bin(Tok t) {
    switch (t) {
        case Tok::PLUSEQUAL: return Tok::PLUS;
        case Tok::MINUSEQUAL: return Tok::MINUS;
        case Tok::STAREQUAL: return Tok::STAR;
        case Tok::SLASHEQUAL: return Tok::SLASH;
        case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
        case Tok::PERCENTEQUAL: return Tok::PERCENT;
        case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
        case Tok::AMPEREQUAL: return Tok::AMPER;
        case Tok::VBAREQUAL: return Tok::VBAR;
        case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
        case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
        case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
        default: return t;
    }
}

struct CaptureVar {
    std::string name;
    TypePtr ty;
};

// 一个函数的闭包形态：env 是从祖先函数捕获的变量（cell 指针在闭包
// 对象里的下标），cells 是本函数自己的局部变量里被子孙函数捕获的。
struct ClosureMeta {
    std::vector<CaptureVar> env;
    std::unordered_map<std::string, int> env_index;
    std::vector<CaptureVar> cells;
    std::unordered_map<std::string, int> cell_index;
    // 自底向上传播后的需求集合：本函数及其子孙需要从祖先拿到的名字。
    std::unordered_set<std::string> needed;
};

struct FuncCtx {
    IRFunction* fn = nullptr;
    const Scope* scope = nullptr;
    std::unordered_map<std::string, int> slots;
    std::unordered_set<std::string> globals_decl;
    // match 捕获要在 case 结束后继续可见：case 里绑定时同时写一份到
    // 函数帧/全局。kind: 0 局部槽 / 1 cell / 2 全局。
    struct MatchEscape {
        int kind = 0;
        int idx = -1;
        TypePtr ty;
    };
    std::unordered_map<std::string, MatchEscape> match_escape;
    std::unordered_map<std::string, TypePtr> extra_locals;
    // 闭包：本函数需要从外层拿的变量，以及被内层捕获、需要转成 cell 的局部名。
    const ClosureMeta* closure = nullptr;
    std::unordered_map<std::string, int> cell_slots;   // 名字 → 存 cell 指针的槽
    std::unordered_map<std::string, int> param_slots;  // 被捕获形参的原始值槽
    int env_slot = -1;                                 // 存闭包指针（%r10）的槽
    // @classmethod 的 cls：entry 模块里槽位放类 id（整数，不参与 rc），
    // 表达式类型仍是那个类；非入口模块的槽位放运行期类对象指针，
    // cls(...) / cls.m(...) 走动态构造与动态分派。
    std::string class_self_name;
    const ClassInfo* class_self_ty = nullptr;
    bool class_self_dynamic = false;
    // 类体命名空间：类属性名 → 隐藏全局下标。只在降级类体语句时非空，
    // 让 `y = x + 1` 里的 x 解析到前面定义的类属性。
    std::unordered_map<std::string, int> class_body_attrs;
    // for k in range(len(a))：循环体里 a[k] 天然在界内。body 里一旦给 a/k
    // 赋值（写槽）就作废；调用等副作用由 side_effect_seq_ 统一挡。
    struct BoundsHint {
        int seq_slot = -1;
        int idx_slot = -1;
        bool valid = true;
        std::vector<std::pair<int, size_t>> patches;  // 待改成 unchecked 的位置
    };
    std::vector<BoundsHint*> bounds_hints;
    // 当前词法作用域内可证非负的槽（range 循环计数器/循环变量提示）。
    // load.slot 命中时给 vreg 打非负标；写入非非负值时作废（store_slot_raw）。
    std::unordered_set<int> nonneg_slots;
    // 嵌套函数直接引用自己的名字：用当前闭包指针，不需要 self cell。
    const FunctionDef* self_def = nullptr;
    TypePtr self_type;
    int self_idx = -1;
    // {break 目标块, continue 目标块}
    std::vector<std::pair<int, int>> loops;
    // 每个循环入口处的 cleanup 嵌套深度：break/continue 要跨过的清理层数。
    std::vector<size_t> loop_cleanup_depth;    // 词法上活跃的清理动作，按从外到内排列。return/break/continue 和
    // 异常 landing 都从这里生成对应动作。
    struct Cleanup {
        enum class Kind { With, AsyncWith, Finally, Handler, ExcDone, FileClose, LockRelease, TimeoutExit, TaskGroupCancel, SemRelease, ZeroCostEh } kind;
        // With / Finally / Handler：handler 记录所在的槽
        int handler_slot = -1;
        // Handler/With/Finally：setjmp 非 0 时跳转的异常展开块
        int land = -1;
        // With
        int mgr_slot = -1;
        int exit_slot = -1;
        int nargs = 0;
        TypePtr ret;
        // AsyncWith：await __aexit__() 的结果类型
        TypePtr inner_ret;
        // 装饰后的 __exit__/__aexit__：vtable 槽没填，走类属性里的包装函数
        ClassInfo* exit_cls = nullptr;
        // Finally
        std::vector<StmtPtr>* body = nullptr;
        // ZeroCostEh：零开销 try 的临时值水位槽（早退时要 RcCleanup 到它）。
        int mark_slot = -1;
        // ZeroCostEh：早退时若有 finally，要把 finalbody 内联进来跑一遍。
        // （body 字段复用上面的 Finally 定义）
        // finally 体词法上所属的循环：close 重放 break/continue 时恢复，
        // -1 表示不在循环里。
        int loop_brk = -1;
        int loop_cont = -1;
        size_t loop_depth = 0;
    };
    std::vector<Cleanup> cleanups;
    // 函数内 import 的模块别名；模块顶层的别名在 Lowerer::module_aliases_。
    std::unordered_map<std::string, ModuleInfoPtr> modules;
    bool is_module_top = false;
    int blk = 0;
    // 函数入口的临时值栈水位；return 时清理到它。
    VReg fn_mark = kNoReg;
    // 模块顶层函数：退出前释放全局变量并关掉临时栈。
    bool is_entry = false;
    // 零开销异常（默认开启）：走表驱动路径的 try 集合（AST 指针，只在本函数
    // 编译期间有效）。空集合表示本函数走 setjmp 路径。嵌套 try 时可能有多条，
    // 每个都降级成 LSDA/landing pad 保护区，多条保护区合成一份 LSDA。
    std::unordered_set<const Try*> zero_cost_tries;
    // 正在降级零开销 try 体：>0 时 raise 走 pa_eh_raise*（_Unwind 版）。
    // 只有这个函数已经没有 setjmp 兜底时才会 >0（见 collect_zero_cost_tries）。
    int zc_raise_depth = 0;
    // 表达式内部槽（and/or、条件表达式、链式比较）只是借用临时值，
    // 生命周期由临时栈负责；出口不能 dec，否则会重复释放。
    std::unordered_set<int> borrow_slots;
    // 本函数内产生、尚未被槽/全局/字段/返回值接管的堆值：所有权还在
    // 临时栈上，写入目标时可以「移动」而不必 inc 后再等 cleanup dec。
    std::unordered_set<VReg> fresh_vregs;
    // 本函数产生过多少个临时堆值。0 表示整条 RC 脚手架都可以省掉。
    size_t rc_pushes = 0;
    int fn_mark_block = -1;
    size_t fn_mark_pos = 0;
    // 生成器：槽位在堆帧里，临时值水位在每次 resume 时重新记录到槽里
    //（vreg 不跨 yield 存活）。
    bool is_generator = false;
    int gen_mark_slot = -1;
    // 跨挂起的异常上下文：cur/top 存本帧的 current_exc / exc_save_top，
    // tok 是进入 resume 时的运行时异常栈深度（见 pa_exc_enter/leave）。
    int exc_cur_slot = -1;
    int exc_top_slot = -1;
    int exc_tok_slot = -1;
    int gen_next_state = 1;
    // async def：同一个帧状态机，await 作为第三种挂起。
    bool is_coroutine = false;
    bool is_async_generator = false;
    TypePtr async_result;
    // 每个挂起点：续跑块 + 挂起时的清理栈快照（close 时按它跑 finally/with）。
    struct GenState {
        int id = 0;
        int cont = -1;
        std::vector<Cleanup> cleanups;
        // 该挂起点位于 `yield from` 委托中：注入的异常要转给子生成器，
        // 而不是在本帧重新抛出。cond 是委托循环的分发块，exc_slot 暂存
        // 待转发的 PyAdamasException*（非堆对象）。
        int delegate_cond = -1;
        int delegate_exc_slot = -1;
    };
    std::vector<GenState> gen_states;
};

class Lowerer {
public:
    Lowerer(IRModule& m, TypeChecker& tc, std::string file, LowerOptions options = {})
        : mod_(m), tc_(tc), file_(std::move(file)), opts_(std::move(options)) {}

    void run(Module& ast);

    std::vector<LowerError>& errors() { return errors_; }

private:
    IRModule& mod_;
    TypeChecker& tc_;
    std::string file_;
    LowerOptions opts_;
    std::vector<LowerError> errors_;

    std::vector<std::unique_ptr<FuncCtx>> frames_;
    FuncCtx* st_ = nullptr;
    // 推导式的循环变量是独立作用域，不能污染函数槽表。这里按嵌套
    // 层次保存「名字 → 槽」，查找时内层优先。
    std::vector<std::unordered_map<std::string, int>> comp_slots_;

    // 模块导入：顶层别名 + 已插入的外部全局（按链接符号去重）。
    std::unordered_map<std::string, ModuleInfoPtr> module_aliases_;
    std::unordered_map<std::string, int> extern_globals_;
    // from m import C：类不是运行时值，只登记编译期元数据。
    std::unordered_map<std::string, ImportedClass> imported_classes_;
    // from math/sys import f：原生函数只能直接调用。
    struct NativeRef {
        std::string module;
        std::string member;
    };
    std::unordered_map<std::string, NativeRef> native_aliases_;
    // `q: asyncio.Queue[float] = asyncio.Queue()`：构造调用自己推不出元素
    // 类型，由带标注的赋值把目标类型暂存到这里。
    TypePtr pending_queue_ty_;

    std::unordered_map<std::string, int> global_ids_;
    std::unordered_map<std::string, FunctionDef*> func_defs_;
    // 含 async for 的推导式会挂起，只能作为整条语句/赋值/返回值的根表达式：
    // 嵌套在别的表达式里时，外层还有未完成的临时值，挂起会一起清掉。
    const Expr* comp_ctx_node_ = nullptr;
    // 正在编译生成器 close 路径里的 finally 体：此时遇到挂起点要报
    // RuntimeError（generator/coroutine ignored GeneratorExit），不能真挂起。
    int close_replay_depth_ = 0;
    // 闭包捕获分析：函数树 + 每个函数的捕获形态。
    std::unordered_map<const FunctionDef*, FunctionDef*> func_parent_;
    std::unordered_map<const FunctionDef*, std::vector<FunctionDef*>> func_children_;
    std::vector<FunctionDef*> func_roots_;
    std::unordered_map<const FunctionDef*, ClosureMeta> closure_meta_;
    // 类：ClassInfo 是 typecheck 的产物，IRClass 是它的运行时形态。
    // 用 ClassInfo 指针做键，同名类重定义时不会串。
    std::unordered_map<std::string, ClassInfoPtr> classes_;
    std::unordered_map<const ClassInfo*, int> class_ids_;
    // (声明类, 属性名) → 模块隐藏全局下标。子类读基类属性时用同一份。
    std::unordered_map<const ClassInfo*, std::unordered_map<std::string, int>> class_attr_globals_;
    // 没有任何本模块类继承的类（跨模块继承在导入方单独处理）：
    // 方法调用可以绕过 vtable 直接调用具体实现。
    std::unordered_set<const ClassInfo*> sealed_classes_;
    // 方法帧 → 运行时类。零参 super() 只在这个方法自己的帧里有效。
    std::unordered_map<const IRFunction*, const ClassInfo*> fn_owner_class_;
    // 降级期副作用计数：调用、del、切片赋值等会自增。for/推导式用它
    // 判断循环体是否可能改变容器长度，进而决定下标免检查是否安全。
    int side_effect_seq_ = 0;
    // 当前正在降级的源码行，用来给指令打行号（DWARF .loc 用）。
    int cur_line_ = 0;

    // ── 错误 ────────────────────────────────────────────────────

    void err(int line, int col, std::string msg) {
        errors_.push_back({line, col, std::move(msg)});
    }

    void unsupported(const Stmt& s, const char* what) {
        err(s.line, s.col, std::format("{} is not supported by the code generator yet", what));
    }

    // ── 块与值 ──────────────────────────────────────────────────

    Block& cur() { return st_->fn->blocks[st_->blk]; }

    int new_block(std::string name) {
        // 别用聚合初始化：Block 加了 id 字段后会错位（-Werror 会抓，
        // 但更稳妥的是显式赋值）。
        Block nb;
        nb.name = std::move(name);
        st_->fn->blocks.push_back(std::move(nb));
        return static_cast<int>(st_->fn->blocks.size()) - 1;
    }

    void switch_to(int b) { st_->blk = b; }

    // noreturn 调用之后把当前块收尾，避免后端把后续块当成顺序落下。
    void end_unreachable() {
        int dead = new_block("unreachable");
        Inst j;
        j.op = IrOp::Jump;
        j.target = dead;
        emit(std::move(j));
    }

    VReg alloc(TypePtr ty) {
        VReg r = static_cast<VReg>(st_->fn->vreg_ty.size());
        st_->fn->vreg_ty.push_back(ty ? std::move(ty) : t_unknown());
        st_->fn->vreg_nonneg.push_back(0);
        return r;
    }

    // 非负性判定与值来源规则（定义在 lower_idiom.cpp）。
    bool nn(VReg v) const;
    uint8_t nonneg_of(const Inst& in);
    // 非负提示窗口（定义在 lower_idiom.cpp）：入口擦目标槽旧提示、
    // 按条件打标；退出擦本层键。嵌套同名循环不得恢复外层提示。
    bool enter_nonneg_window(For& f, VReg start, int ctr, bool dyn_step, int64_t step,
                             int& target_slot);
    void exit_nonneg_window(bool nn_hint, int ctr, int target_slot);

    VReg def(TypePtr ty, Inst in) {
        VReg r = alloc(ty);
        st_->fn->vreg_nonneg[static_cast<size_t>(r)] = nonneg_of(in);
        in.dst = r;
        in.ty = ty;
        if (in.line == 0) in.line = cur_line_;
        st_->fn->ensure_inst_id(in);
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
        return r;
    }

    void emit(Inst in) {
        in.dst = kNoReg;
        if (in.line == 0) in.line = cur_line_;
        st_->fn->ensure_inst_id(in);
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
    }

    bool terminated() const { return st_->fn->blocks[st_->blk].terminated(); }

    /* 按静态类型算 ListAppend 的 heap 参数：0 非堆 / 1 堆 / 2 unknown。 */
    VReg heap_arg(TypePtr t) {
        if (!t) return const_int(2);
        if (t->kind == TyKind::Unknown) return const_int(2);
        return const_int(ir_is_heap(t) ? 1 : 0);
    }

    VReg const_int(int64_t v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v;
        return def(t_int(), std::move(in));
    }

    VReg const_bool(bool v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v ? 1 : 0;
        return def(t_bool(), std::move(in));
    }

    VReg const_float(double v) {
        Inst in;
        in.op = IrOp::ConstFloat;
        in.fimm = v;
        return def(t_float(), std::move(in));
    }

    VReg const_none() {
        Inst in;
        in.op = IrOp::ConstNone;
        return def(t_none(), std::move(in));
    }

    VReg const_str(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstStr;
        in.idx = mod_.intern_string(s);
        return def(t_str(), std::move(in));
    }

    // bytes 字面量：与同内容的 str 字面量共用字节但 pub 不同，必须单独 intern。
    VReg const_bytes(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstStr;
        in.idx = mod_.intern_bytes(s);
        return def(t_bytes(), std::move(in));
    }

    VReg const_literal(const LiteralValue& v);

    int new_temp_slot(TypePtr ty) { return st_->fn->new_slot(std::move(ty)); }

    int new_borrow_slot(TypePtr ty) {
        int s = new_temp_slot(std::move(ty));
        st_->borrow_slots.insert(s);
        st_->fn->slot_owned[static_cast<size_t>(s)] = false;
        st_->fn->slot_unknown[static_cast<size_t>(s)] = false;
        return s;
    }

    void store_borrow_slot(int slot, VReg v) { store_slot_raw(slot, v); }

    VReg load_slot(int slot, TypePtr ty);

    VReg load_global(int gi, TypePtr ty);

    void store_slot_raw(int slot, VReg v) {
        for (auto* h : st_->bounds_hints)
            if (h->seq_slot == slot || h->idx_slot == slot) h->valid = false;
        // 写入不可证非负的值 → 作废该槽的非负提示（后续 load 不再打标）。
        if (st_->nonneg_slots.count(slot) && !nn(v)) st_->nonneg_slots.erase(slot);
        Inst in;
        in.op = IrOp::StoreSlot;
        in.idx = slot;
        in.a = v;
        emit(std::move(in));
    }

    void store_global_raw(int gi, VReg v) {
        Inst in;
        in.op = IrOp::StoreGlobal;
        in.idx = gi;
        in.a = v;
        emit(std::move(in));
    }

    // ── 引用计数 ────────────────────────────────────────────────
    //
    // 所有权约定：
    //   槽 / 全局 / 字段 / 列表元素 持有引用；
    //   load / field.get / list.get 只是借用，不增加计数；
    //   每个新堆值产生时带 rc=1 进临时栈，语句边界统一清理。
    // unknown 类型不插桩：机器表示可能是整数，按指针 dec 会崩。

    void rc_inc(VReg v) { call_void(RtFn::RcInc, {v}); }
    void rc_dec(VReg v) { call_void(RtFn::RcDec, {v}); }
    void rc_temp_take(VReg v) { call_void(RtFn::RcTempTake, {v}); }
    // unknown 值可能是整数也可能是堆指针，交给运行时按注册表判断。
    void rc_inc_unknown(VReg v) { call_void(RtFn::RcIncUnknown, {v}); }
    void rc_dec_unknown(VReg v) { call_void(RtFn::RcDecUnknown, {v}); }

    // 把刚产生的堆值的所有权从临时栈转给目标。成功返回 true，调用方
    // 跳过 retain；目标类型和源类型都不是堆时返回 false（无需持有引用）。
    bool move_fresh(VReg v, const TypePtr& dst) {
        if (!st_->fresh_vregs.count(v)) return false;
        TypePtr src = vreg_ty(v);
        if (!ir_is_heap(dst) && !ir_is_heap(src)) return false;
        st_->fresh_vregs.erase(v);
        rc_temp_take(v);
        // take 把引用移出临时栈，净 push 减一：stmt / 循环的 mark/cleanup
        // 消除按净 push 判断，push 后立刻被槽/容器 take 的值（queue_sim 的
        // 元组 append、rest = [] 赋值）不再留下水位脚手架。fresh_vregs 的
        // 每个插入点都配对了 push_temp，计数不会下溢。异常路径不受影响：
        // 被消的 cleanup 本来就不在展开路径上跑，push 与 take 之间抛异常
        // 滞留的临时值由外层水位兜底（try 的 mark_slot landing pad、caller
        // 的语句水位、入口 fn_mark）——与既有循环/推导式消除同款不变量。
        --st_->rc_pushes;
        return true;
    }
    // 槽/字段/全局按「是否可能装着非堆值」选释放方式。
    void release_slot_value(int slot, TypePtr ty, bool maybe_unknown) {
        VReg v = load_slot(slot, ty);
        if (maybe_unknown) rc_dec_unknown(v);
        else rc_dec(v);
    }

    VReg rc_mark() { return call_runtime(RtFn::RcMark, t_int(), {}); }
    void rc_cleanup(VReg mark) { call_void(RtFn::RcCleanup, {mark}); }

    VReg rc_mark_at(int& block, size_t& pos) {
        VReg m = rc_mark();
        block = st_->blk;
        pos = cur().insts.size() - 1;
        return m;
    }

    void push_temp(VReg v) {
        ++st_->rc_pushes;
        call_void(RtFn::RcTemp, {v});
    }

    void erase_mark(int block, size_t pos, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        if (pos < ins.size() && ins[pos].op == IrOp::CallRt &&
            static_cast<RtFn>(ins[pos].imm) == RtFn::RcMark && ins[pos].dst == mark)
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    void erase_cleanup(int block, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        std::erase_if(ins, [&](const Inst& in) {
            return in.op == IrOp::CallRt && static_cast<RtFn>(in.imm) == RtFn::RcCleanup &&
                   in.args.size() == 1 && in.args[0] == mark;
        });
    }

    // 产生一个堆值：把它交给临时栈，调用方不需要再管这个初始引用。
    VReg def_heap(TypePtr ty, Inst in) {
        VReg v = def(ty ? ty : t_unknown(), std::move(in));
        if (ir_is_heap(ty)) {
            push_temp(v);
            st_->fresh_vregs.insert(v);
        }
        return v;
    }

    // 运行时按机器字返回的值（生成器 next/send）静态类型只能是 unknown；
    // 用 mov 补一个静态类型，后续 store_slot 才能按堆类型 retain。
    VReg retype(VReg v, TypePtr ty) {
        if (!ty || type_eq(vreg_ty(v), ty)) return v;
        Inst mv;
        mv.op = IrOp::Mov;
        mv.a = v;
        mv.ty = ty;
        return def(ty, std::move(mv));
    }

    // 队列元素的运行时类别：0 裸值 / 1 引用。unknown 保守按引用。
    static int queue_kind(const TypePtr& t) {
        if (!t || t->kind != TyKind::AsyncQueue || t->sub.empty()) return 1;
        TypePtr e = t->sub[0];
        if (!e || e->kind == TyKind::Unknown) return 1;
        return ir_is_heap(e) ? 1 : 0;
    }

    // 列表元素所有权类别，必须和 codegen 传给 pa_list_new 的值一致。
    // 3 = 未定：类型里看不出元素种类（裸 list / []），空容器先记 3，第一次
    // 写入时由 pa_list_prep 按运行时值定型。
    static int list_kind(const TypePtr& t);

    // 容器一开始就有元素（或由运行时自行填充）时不能停在「未定」3：
    // 直接降级为 2，保持和原来一致的保守所有权语义。
    static int list_kind_filled(const TypePtr& t) {
        int k = list_kind(t);
        return k == 3 ? 2 : k;
    }

    // kind 1/2 且源类型已知是堆，写入前 retain。kind 2 只 retain 不 release
    //（类型看不见，销毁时不敢按元素 dec），代价是泄漏。源类型是 unknown
    // 时不能直接 rc_inc（可能是整数），走注册表保守判断。
    void retain_elem(VReg v, int kind) {
        if (kind == 0) return;
        TypePtr vt = vreg_ty(v);
        if (ir_is_heap(vt)) {
            rc_inc(v);
            return;
        }
        if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
    }

    // 列表元素写入前的所有权交接。容器未定型（3）、静态种类与值冲突、或值
    // 是 unknown 时交给运行时的 pa_list_prep 定型并 retain；其余情况按静态
    // 种类直接 retain，省掉一次调用。
    void store_list_elem(VReg l, const TypePtr& lt, VReg v, bool owned) {
        int lk = list_kind(lt);
        int vk = own_kind(vreg_ty(v));
        if (lk == 3 || (lk != 2 && vk == 2) || (lk == 0 && vk == 1) || (lk == 1 && vk == 0)) {
            call_void(RtFn::ListPrep, {l, v, const_int(vk), const_int(owned ? 1 : 0)});
            return;
        }
        if (!owned) retain_elem(v, lk);
    }

    // 哈希/比较类别：0 int/bool，1 str，2 float，3 其他（按机器字比较）。
    static int key_kind(const TypePtr& t);

    // 所有权类别：0 非堆，1 堆，2 unknown（保守 retain，注册表释放）。
    static int own_kind(const TypePtr& t) {
        if (ir_is_heap(t)) return 1;
        if (!t || t->kind == TyKind::Unknown) return 2;
        return 0;
    }

    // 排序比较器的种类：0 int/bool、1 float、2 str、其它按机器字。
    static int sort_kind(const TypePtr& t) {
        if (!t) return 3;
        if (t->kind == TyKind::Int || t->kind == TyKind::Bool) return 0;
        if (t->kind == TyKind::Float) return 1;
        if (t->kind == TyKind::Str) return 2;
        return 3;
    }

    // 内置序列构造（list/sorted/enumerate/...）的入参规整成运行时列表：
    // str → 字符列表，dict/set → 键列表，list/tuple 原样，生成器顺序消费。
    VReg seq_as_list(Expr* e, TypePtr& elem_out);
    // 生成器物化成 list：惰性值没有 len/下标，只能顺序消费一次。
    VReg gen_to_list(VReg gen, TypePtr gen_ty);
    // 生成器流式迭代：每个元素执行一次 body(ev, elem_ty, cond_blk, end_blk)。
    // ev 是借用引用；body 里跳 cond_blk 继续下一轮、跳 end_blk 提前结束。
    void emit_gen_for_each(VReg gen, TypePtr gen_ty,
                           const std::function<void(VReg, TypePtr, int, int)>& body);

    // 槽/全局/字段按**目标类型**决定是否 retain：出口按目标类型释放，
    // 只按源类型加引用会在「unknown 值写进带标注的堆槽」时失衡
    // （源类型未知就漏掉 inc，出口却照常 dec）。unknown 源用运行时
    // 保守 inc，整数在这里是安全的 no-op。
    void retain_for_store(const TypePtr& dst, VReg v);

    void store_slot(int slot, VReg v);

    void store_global(int gi, VReg v);

    void store_field(TypePtr fty, VReg obj, int off, VReg v);

    void inc_params() {
        for (auto& p : st_->fn->params)
            if (ir_is_heap(p.ty)) rc_inc(load_slot(p.slot, p.ty));
    }

    // 函数出口释放所有堆槽：形参在入口 retain 过，局部变量在写入时 retain
    // 过。返回值必须先 rc_inc，再走到这里，否则槽一释放就成悬空指针。
    // 释放后把槽清零：内联会把槽留在调用方帧里跨多次执行复用，残留的
    // 悬空指针会让下一次赋值的「load 旧值 + dec」二次释放。
    void release_slots();

    // 只读形参不需要「入口 retain + 出口 release」：实参在调用期间由
    // 调用方持有，函数体既没覆盖形参槽、也没把它存进别处时，这对 RC
    // 纯属多余。方法调用的 self 绝大多数是这种形态，省掉的是每次
    // 调用两条运行时调用。判定在函数降级完成后做：
    //   - 入口 retain 是入口块开头按形参顺序排的两条指令（load + rc_inc）；
    //   - 出口 release 只由 release_slots 发出，形态是 load(param) + rc_dec，
    //     只读形参意味着函数体里不会覆盖它，所以这种形态只可能是出口释放。
    // 不记录指令下标：降低过程中语句/循环的 mark 擦除会让下标失效。
    void finalize_param_borrows();

    void release_globals();

    void emit_return(VReg v, TypePtr vty);

    // ── 名字解析 ────────────────────────────────────────────────

    int global_idx(const std::string& name);

    int global_idx_as(const std::string& name, TypePtr ty);

    // ── 闭包 ────────────────────────────────────────────────────

    const ClosureMeta* closure() const { return st_ ? st_->closure : nullptr; }

    bool is_cell(const std::string& name) const {
        return st_ && st_->cell_slots.count(name) > 0;
    }

    int comp_slot(const std::string& name) const {
        for (auto it = comp_slots_.rbegin(); it != comp_slots_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return -1;
    }

    bool is_env(const std::string& name) const {
        auto* m = closure();
        return m && st_->env_slot >= 0 && m->env_index.count(name) > 0;
    }

    TypePtr captured_ty(const std::string& name) const {
        auto* m = closure();
        if (!m) return nullptr;
        if (auto it = m->cell_index.find(name); it != m->cell_index.end())
            return m->cells[static_cast<size_t>(it->second)].ty;
        if (auto it = m->env_index.find(name); it != m->env_index.end())
            return m->env[static_cast<size_t>(it->second)].ty;
        return nullptr;
    }

    VReg load_cell(const std::string& name) {
        TypePtr T = captured_ty(name);
        int slot = st_->cell_slots.at(name);
        return load_slot(slot, mk_list(T ? T : t_unknown()));
    }

    // cell 是单元素 list[T]；读出来是借用引用，写进去由 list_set 管旧值。
    VReg read_cell(const std::string& name) {
        VReg c = load_cell(name);
        TypePtr T = captured_ty(name);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = c;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    }

    VReg read_env(const std::string& name);

    VReg cell_for_capture(const std::string& name);

    // 写回捕获变量：先按 cell 的元素类别 retain，list_set 负责 dec 旧值
    //（kind 1）；kind 2 只 retain 不 release，宁可泄漏。
    void write_cell(const std::string& name, VReg v, TypePtr vty);

    VReg zero_value(TypePtr T);

    // 被内层函数捕获的局部变量在入口转成单元素 list[T]（cell）。
    // 形参先用原始槽接实参，再把值 retain 进 cell。
    void create_cells();

    void bind_name_value(const std::string& name, VReg v, TypePtr vty) {
        if (is_cell(name)) {
            write_cell(name, v, vty);
        } else if (is_local(name)) {
            store_slot(st_->slots[name], v);
        } else {
            store_global(global_idx(name), v);
        }
    }

    TypePtr name_ty(const std::string& name);

    bool is_local(const std::string& name) const {
        if (!st_) return false;
        if (comp_slot(name) >= 0) return true;
        return st_->slots.count(name) > 0 || is_env(name);
    }

    // 一个名字的存储类型：cell/env 看捕获类型，局部看槽，模块级看全局表。
    TypePtr storage_ty(const std::string& name) {
        if (auto t = captured_ty(name)) return t;
        if (st_->globals_decl.count(name) || !is_local(name)) return mod_.globals[global_idx(name)].ty;
        return st_->fn->slot_ty[st_->slots[name]];
    }

    // 当前作用域里有没有这个名字（局部槽、捕获变量或 global 声明）。
    bool is_bound_here(const std::string& name) const {
        if (!st_) return false;
        if (st_->globals_decl.count(name)) return true;
        return is_local(name);
    }

    // 内置名可以被用户遮蔽：模块里定义了 print，它就不再是内置。
    bool is_builtin(const std::string& name) const {
        if (is_bound_here(name)) return false;
        if (auto* ms = tc_.module_scope())
            if (ms->vars.count(name)) return false;
        return true;
    }

    // ── 语句 ────────────────────────────────────────────────────

    void stmts(std::vector<StmtPtr>& body);

    void stmt(Stmt& s);

    void lower_if(If& f);
    void lower_while(While& f);
    void lower_for(For& f);
    void lower_for_range(For& f, Call& c);
    // 切片拷贝惯用法：for k in range(lo,hi): dst.append(src[k]) → 批量拷贝。
    bool lower_copy_range_idiom(For& f, Call& c);
    void lower_for_range_obj(For& f, VReg r);
    void lower_for_range_core(For& f, VReg start, VReg stop, VReg step_v, bool dyn_step,
                               int64_t step, const Call* c);
    void lower_for_seq(For& f);
    // for i, v in enumerate(xs) / for a, b in zip(xs, ys)：直接计数循环，
    // 不物化 [下标, 值] 的元组表。
    void lower_for_enumerate(For& f, Call& c);
    void lower_for_zip(For& f, Call& c);
    void lower_for_file(For& f);
    void lower_with(With& w);
    void lower_with_item(With& w, size_t idx);
    void emit_cleanup_exits(size_t down_to);
    int push_handler();
    VReg setjmp_handler(int slot);
    VReg emit_exit_call(const FuncCtx::Cleanup& c, bool exceptional);
    void lower_try_core(Try& t);
    void lower_finally(Try& t);
    VReg lower_exc_match(Expr& type);
    // except / raise 名字位置的异常类解析：内置异常名或本模块/导入的自定义
    // 异常类；out 是运行期注册表里的类型名（导入类用定义模块里的原名）。
    bool exc_type_name(Expr& e, std::string& out);
    // except `RichExc as e` 是否富异常类（有字段/方法），是则返回那个类。
    const ClassInfo* rich_exc_handler(ExceptHandler* h);
    VReg raise_exception(const std::string& type, VReg msg);
    // ── 零开销异常（Z5，默认开启；--no-zero-cost-exc 可回退）─────
    // 在 AST 上逐个判定函数里的 try 是否适合表驱动路径，满足的收进
    // st_->zero_cost_tries 集合（可多条）。规则见 lower_exc.cpp 的实现注释。
    void collect_zero_cost_tries(FunctionDef& f);
    // 零开销 try 的降级：发 EhBegin/EhEnd 保护区 + EhLandingPad；landing pad
    // 取回异常后直接跳进现有的 except 分发块（ExcMatches/ExcAcceptValue/
    // ExcDone 一行不改）。
    void lower_try_zero_cost(Try& t);
    // 零开销 try 体里的 raise：把老 RtFn 映射到 _Unwind 版本；其余地方原样
    // 返回（老路径一个字都不变）。
    RtFn raise_rt(RtFn f) const {
        if (!st_ || st_->zc_raise_depth <= 0) return f;
        switch (f) {
            case RtFn::Raise: return RtFn::EhRaise;
            case RtFn::RaiseValueFrom: return RtFn::EhRaiseValueFrom;
            case RtFn::Reraise: return RtFn::EhReraise;
            default: return f;
        }
    }
    void lower_funcdef(FunctionDef& f);
    // @functools.wraps(...)：没有运行期语义，装饰器链里整层跳过。
    bool is_wraps_decorator(const Expr* e) const;
    // @functools.lru_cache / @lru_cache(...)：降级期合成带缓存的包装闭包。
    bool is_lru_cache_decorator(const Expr* e) const;
    // 把值包成单元素 cell（list[T]），用于手工构造闭包环境。
    VReg make_cell_value(VReg v, TypePtr T);
    VReg build_partial_wrapper(Call& c, VReg fn, TypePtr fty, const PartialPlan& plan,
                               int line, int col);
    VReg build_lru_wrapper(const std::string& base, VReg fn, TypePtr fty, int line, int col);
    void lower_assert(Assert& a);
    void lower_delete(Delete& d);
    void lower_match(Match& m);
    void lower_pattern(Pattern& p, VReg value, TypePtr ty, int fail, bool in_or = false,
                       std::unordered_set<std::string>* or_local = nullptr);
    void delete_target(Expr& target);
    void lower_try(Try& t);
    void lower_raise(Raise& r);
    void lower_import(Import& im);
    void lower_import_from(ImportFrom& im);
    ModuleInfoPtr resolve_module_alias(const std::string& name) const;
    void ensure_module_init(const ModuleInfoPtr& mod);
    int extern_global(const std::string& symbol, const std::string& debug_name, TypePtr ty);

    // 模块属性读取（m.x / m.f），返回外部全局槽；类导入留给 0.14b。
    std::optional<int> module_member(Attribute& at, ModuleInfoPtr* owner = nullptr);
    VReg construct_imported(const ImportedClass& ic, Call& c, TypePtr cls_ty);
    VReg call_native(const std::string& module, const std::string& member, Call& c,
                     const FuncInfoPtr& info);
    bool builtin_value(const std::string& module, const std::string& member, VReg& out,
                       TypePtr& ty);
    static std::optional<RtFn> native_fn(const std::string& module, const std::string& member);
    int compile_ctor_wrapper(ClassDef& clsdef, const ClassInfoPtr& ci, int cid);

    // ── 类 ──────────────────────────────────────────────────────

    void collect_classes(std::vector<StmtPtr>& body);
    void lower_classdef(ClassDef& c);

    // 把一个函数体编译成 IRFunction，返回它的下标。普通函数和方法共用。
    int compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                              const ClassInfo* owner = nullptr);
    // 含 yield / await 的函数：编译 resume 状态机 + 创建包装，返回创建包装下标。
    // frame_kind: 0 生成器 / 1 协程 / 2 异步生成器
    int compile_suspendable_function(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                                     int frame_kind, const ClassInfo* owner = nullptr);
    VReg lower_yield(Expr& y);
    VReg lower_yield_from(YieldFrom& y);
    // close 路径重放 finally 体时遇到挂起点：按 CPython 报 RuntimeError，
    // 不产生挂起。返回 true 表示已经发出 raise，调用方直接返回即可。
    bool reject_suspend_in_close();
    // 把 v 作为一次 yield 挂起，返回恢复后 send 进来的值。
    VReg emit_yield(VReg v, TypePtr vt);
    VReg lower_await(Expr& e);
    // 把「等待 w」编译成挂起；返回恢复后 sent 槽里的结果值（默认堆结果已入临时栈）。
    // active_limit 用于清理路径：只把仍然挂在运行时栈上的外层 handler 计入快照。
    // push_result_temp=false 时由调用方在 handler_leave 之后再 push_temp。
    VReg suspend_on_awaitable(VReg w, TypePtr result_ty, int line, int col,
                              size_t active_limit = static_cast<size_t>(-1),
                              bool push_result_temp = true);
    void lower_async_with_item(With& w, size_t idx);
    void lower_async_for(For& f, VReg seq, TypePtr seq_ty);
    void gen_finish(VReg v = kNoReg, TypePtr vty = nullptr);
    void cleanup_gen_mark();
    // resume 入口 / 挂起出口的异常上下文切换（except 体里的 yield/await）。
    void exc_enter();
    void exc_leave();
    VReg slot_addr_of(int slot);

    // str 顺序迭代游标：分配一对相邻的 int64 槽（cp / off），两个槽都取过
    // 地址（保证不被寄存器提升，运行时直接按 PyAdamasStrCursor* 读写），
    // 初始化为「无缓存」(-1, 0)。返回 cp 槽号；off 槽号是 cp+1。
    int new_str_cursor() {
        int cp = new_temp_slot(t_int());
        int off = new_temp_slot(t_int());
        VReg a1 = slot_addr_of(cp);
        VReg a2 = slot_addr_of(off);
        (void)a1;
        (void)a2;
        store_slot(cp, const_int(-1));
        store_slot(off, const_int(0));
        return cp;
    }

    // 取 str 的第 idx 个码点（1 码点串）。带游标时顺序访问是 O(1)；
    // 非顺序访问回退到从头扫，与 pa_str_char_at 一致。
    VReg str_char_at(VReg seq, VReg idx, int cur_slot) {
        if (cur_slot < 0) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::StrCharAt);
            in.args = {seq, idx};
            VReg v = def(t_str(), std::move(in));
            push_temp(v);
            return v;
        }
        VReg curp = slot_addr_of(cur_slot);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::StrCharAtCursor);
        in.args = {seq, idx, curp};
        VReg v = def(t_str(), std::move(in));
        push_temp(v);
        return v;
    }
    // 生成器 resume 重进时 C 栈帧是新的：把跨挂起仍然活跃的 vreg
    // 在挂起前溢写到堆帧槽，恢复后再装回来（见 lower_gen.cpp）。
    void spill_across_suspends();
    void lower_for_gen(For& f, VReg seq, TypePtr seq_ty);
    VReg lower_asyncio(const std::string& member, Call& c);
    VReg lower_itertools(const std::string& member, Call& c);
    VReg lower_operator(const std::string& member, Call& c);
    // operator.itemgetter：合成一个闭包，内部做 obj[key] 下标读取。
    VReg build_itemgetter(Call& c, TypePtr fty);
    VReg lower_collections(const std::string& member, Call& c);
    VReg lower_functools(const std::string& member, Call& c);
    VReg lower_datetime(const std::string& member, Call& c);
    VReg lower_popen(Call& c);
    VReg lower_subprocess_run(Call& c);
    VReg stream_mode(Expr& e);
    VReg lower_pathlib(const std::string& member, Call& c);
    // random 里带 rng 首参的几个函数：模块级传 0（运行时走全局 rng）。
    VReg lower_random_extra(const std::string& member, Call& c);
    VReg lower_json(const std::string& member, Call& c);
    // 把静态类型编码成 json 运行时描述符；不支持的类型就地报错。
    bool json_desc(const TypePtr& t, std::string& out, int line, int col);
    // CPython 桥（from python import）：参数类型描述符 + 调用降级。
    bool cpy_desc(const TypePtr& t, std::string& out, int line, int col);
    VReg lower_cpy_call(const std::string& proxy_name, const std::string& attr, Call& c);

    VReg lower_construct(Call& c, const ClassInfoPtr& cls);

    // 按签名重排实参：位置、关键字、默认值、数值转换。
    std::vector<VReg> bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn);

    // 容器元素跨数值域（list[int] ↔ list[float]）需要逐元素转换，
    // 现阶段一律报错，避免按错误的机器表示读槽位。
    bool bad_elem(const TypePtr& from, const TypePtr& to);

    // extern 变量：登记到模块表并返回下标。同一个 C 符号只登记一次。
    int extern_var_idx(const CVar& v) {
        for (size_t i = 0; i < mod_.extern_vars.size(); ++i)
            if (mod_.extern_vars[i].symbol == v.symbol) return static_cast<int>(i);
        IRExternVar ev;
        ev.symbol = v.symbol;
        ev.kind = v.kind;
        mod_.extern_vars.push_back(std::move(ev));
        return static_cast<int>(mod_.extern_vars.size()) - 1;
    }

    // extern 变量在 PyAdamas 侧的类型；ptr[Struct] 保留结构体指针身份。
    TypePtr cvar_type(const CVar& v) {
        TypePtr t = ckind_type(v.kind);
        if (v.kind == CKind::Ptr && !v.struct_name.empty()) {
            if (StructInfo* st = tc_.struct_info(v.struct_name))
                t = mk_optional(mk_struct_ptr(st));
        } else if (v.kind == CKind::Ptr && v.elem != CKind::None && t) {
            t->celem = v.elem;
        }
        return t;
    }

    void field_store(VReg obj, int off, VReg v, uint8_t width = 8, bool unsig = false) {
        Inst in;
        in.op = IrOp::FieldStore;
        in.a = obj;
        in.imm = off;
        in.b = v;
        in.fwidth = width;
        in.funsigned = unsig;
        emit(std::move(in));
    }

    VReg field_load(VReg obj, int off, TypePtr ty, uint8_t width = 8, bool unsig = false) {
        Inst in;
        in.op = IrOp::FieldLoad;
        in.a = obj;
        in.imm = off;
        in.fwidth = width;
        in.funsigned = unsig;
        return def(ty, std::move(in));
    }

    // 值类型 struct：分配一块 payload（带 RC 头，但头在 payload 之前，
    // 交给 C 的地址就是 payload 地址），以及按值复制一份。
    VReg struct_new(StructInfo* s) {
        Inst in;
        in.op = IrOp::StructNew;
        in.imm = s->size;
        return def_heap(mk_struct(s), std::move(in));
    }

    VReg struct_copy(VReg v, StructInfo* s) {
        Inst in;
        in.op = IrOp::StructCopy;
        in.a = v;
        in.imm = s->size;
        return def_heap(mk_struct(s), std::move(in));
    }

    // struct 的特殊形式与构造。
    VReg lower_struct_builtin(const std::string& name, Call& c);
    VReg lower_struct_construct(Call& c, StructInfo* s);

    // Optional[T] 上的属性访问必须先确认不是 None。类型收窄做不到
    // 逐条路径成立，这里用一次运行时检查兜底：解引用空指针的段错误
    // 不带任何上下文，比一条 panic 难查得多。
    VReg guard_not_none(VReg v, const std::string& what);

    // 剥掉 Optional 外壳，返回内层类型；不是 Optional 就原样返回。
    static TypePtr strip_optional(const TypePtr& t) {
        if (t && t->kind == TyKind::Optional && !t->sub.empty()) return t->sub[0];
        return t;
    }

    // type(x) 用的 Python 侧类型名。
    static std::string py_type_name(const TypePtr& t);

    void assign_to(Expr& target, VReg v, TypePtr vty);

    // 类属性：每个 (声明类, 名字) 在模块里占一个隐藏全局，子类共用同一份。
    struct ClassAttrStorage {
        int slot = -1;    // 存储槽
        int flag = -1;    // 子类「已拥有自己一份」的标志；-1 表示无需判断
        int parent = -1;  // 标志为 0 时读哪里
    };
    int class_attr_global(const ClassInfo* owner, const std::string& name, TypePtr ty);
    ClassAttrStorage class_attr_storage(const ClassInfo* recv, const ClassAttrInfo* ca);
    int class_attr_slot(const ClassInfo* recv, const ClassAttrInfo* ca);
    VReg class_attr_load(const ClassInfo* recv, const ClassAttrInfo* ca);
    VReg class_attr_load_dyn(VReg obj, const ClassInfo* stat, const ClassAttrInfo* ca);
    void class_attr_store(const ClassInfo* recv, const ClassAttrInfo* ca, VReg v, TypePtr vty);
    // 接收者是类本身（`C.x` / @classmethod 的 `cls.x`）而不是实例
    bool is_class_receiver(const Attribute& a);
    const ClassInfo* class_receiver_cls(const Attribute& a, TypePtr ot);

    // ── 表达式 ──────────────────────────────────────────────────

    VReg expr(Expr& e);

    VReg lower_constant(Constant& c);
    VReg lower_binop(BinOp& b);
    VReg lower_percent_format(BinOp& b, TypePtr lt, TypePtr ty);
    VReg lower_unary(UnaryOp& u);
    VReg lower_boolop(BoolOp& b);
    VReg lower_compare(Compare& c);
    // 容器相等：list/tuple 逐元素比，dict 按键值对（顺序无关），set 按键。
    // 元素类型在编译期已知，所以比较方式也能定下来；不适用时返回 kNoReg。
    VReg lower_container_eq(VReg l, TypePtr lt, VReg r, TypePtr rt);
    // 容器元素的比较链：每个字符说明该层元素怎么比（i/s/f/x/l/d/t）。
    void append_elem_chain(TypePtr container, std::string& chain);
    VReg lower_call(Call& c);
    VReg lower_builtin_call(const std::string& name, Call& c);
    // 按内建函数类别拆开的降级入口（lower_builtin_*.cpp）；
    // 名字不归自己管时返回 kNoReg。
    VReg lower_builtin_reflect(const std::string& name, Call& c);
    VReg lower_builtin_seq(const std::string& name, Call& c);
    VReg lower_builtin_container(const std::string& name, Call& c);
    VReg lower_builtin_text(const std::string& name, Call& c);
    VReg lower_builtin_misc(const std::string& name, Call& c);
    VReg lower_minmax(Call& c, bool is_min);
    VReg lower_sum(Call& c);
    VReg lower_anyall(Call& c, bool is_any);
    VReg lower_print(Call& c);
    VReg lower_method_call(Attribute& at, Call& c);
    // 方法值：把 obj.m / C.m 合成成普通函数值的闭包。
    VReg lower_method_value(Attribute& at);
    // 按接收者类型拆开的方法调用降级（lower_method_*.cpp）。
    VReg lower_method_native(Attribute& at, Call& c, VReg recv, TypePtr recv_ty, bool opt);
    VReg lower_method_generator(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_async_generator(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_task(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_future(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_dict(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_set(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_seq(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
    VReg lower_method_str(Attribute& at, Call& c, VReg recv);
    VReg lower_method_bytes(Attribute& at, Call& c, VReg recv);
    VReg lower_method_random(Attribute& at, Call& c, VReg recv);
    VReg lower_method_regex(Attribute& at, Call& c, VReg recv);
    VReg lower_method_match(Attribute& at, Call& c, VReg recv);
    VReg lower_method_file(Attribute& at, Call& c, VReg recv);
    VReg lower_method_int(Call& c, VReg recv);
    VReg lower_cimport_call(Attribute& at, Call& c);
    // 免写直调：handle == kNoReg 走默认符号表，否则按 dylib 句柄 dlsym。
    VReg lower_c_lazy_call(Call& c, const std::string& symbol, VReg handle);
    VReg lower_super_call(Attribute& at, Call& c);
    // 协议 dunder：__str__/__repr__/__eq__/__ne__/__len__。
    // 静态类型是用户类且 MRO 里有可用实现时构造 VCall（子类覆盖生效），
    // 否则返回 kNoReg，由调用点退回默认行为。
    VReg dunder_call(VReg recv, TypePtr recv_ty, const char* name,
                     std::vector<VReg> args, TypePtr want_ret,
                     const std::vector<TypePtr>* arg_tys = nullptr);
    // 实参是异常类名（内置异常或自定义异常类）时返回 true 并写回类名。
    // throw()/athrow() 接受异常类，按无消息实例处理。
    bool exc_class_name(Expr& e, std::string& out);
    VReg dunder_str(VReg v, TypePtr ty, bool prefer_repr);
    VReg dunder_len(VReg v, TypePtr ty);
    VReg dunder_bool(VReg v, TypePtr ty);
    // 把已知类型的值物化成 list（seq_as_list 的核心，供 VReg 复用）。
    VReg value_as_list(VReg x, TypePtr xt, TypePtr& elem_out);
    // 容器元素隐式数值提升（list[int] → list[float] / tuple 同理）：
    // 逐元素转换并重新分配。不支持时返回 kNoReg。
    VReg promote_container(VReg v, TypePtr from, TypePtr to);
    // 运行期类对象：本地类读类定义点写入的隐藏全局；导入类按需构造。
    VReg local_class_object(const std::string& name);
    VReg imported_class_object(const ImportedClass& ic);
    // 按 ClassInfo 指针找导入类元数据（from m import C 的绑定表 + 模块别名
    // 下的类）。用于类值构造跨模块异常实例等场景。
    const ImportedClass* find_imported_class(const ClassInfo* cls);
    VReg lower_dunder_eq(VReg l, TypePtr lt, VReg r, TypePtr rt, bool ne);
    // 类作为 dict/set key：__hash__ 和 __eq__ 都存在时返回两个 vtable
    // 槽号，否则 (-1, -1)（运行时按机器字哈希/比较）。
    std::pair<int64_t, int64_t> dunder_key_slots(const TypePtr& key_ty) const;
    // 算术/比较 dunder：左操作数（或右操作数的反射版）有实现时返回调用
    // 结果；没有可用实现返回 kNoReg。aug=true 时先试 __iadd__ 这类。
    VReg lower_dunder_binop(VReg l, TypePtr lt, VReg r, TypePtr rt, Tok op, bool aug);
    VReg lower_dunder_cmp(VReg l, TypePtr lt, VReg r, TypePtr rt, CmpOp op);
    // 真值：类实例有 __len__ 时返回 len(...) != 0 的 bool vreg，
    // 否则原样返回（后端按类型做指针/数值判断）。
    VReg truth_bool(VReg v, TypePtr ty);
    VReg class_default_str(TypePtr ty);
    VReg lower_subscript(Subscript& s);
    VReg lower_list_like(std::vector<ExprPtr>& elts, TypePtr ty, bool is_tuple);
    VReg lower_dict(DictExpr& d, TypePtr ty);
    VReg lower_set(SetExpr& s, TypePtr ty);
    VReg lower_comp(Expr& e);
    VReg lower_ifexp(IfExp& e);
    VReg lower_joined(JoinedStr& j);
    VReg convert_to(VReg v, TypePtr from, TypePtr to);
    VReg coerce(VReg v, TypePtr from, TypePtr to);
    TypePtr vreg_ty(VReg v) const {
        return v < st_->fn->vreg_ty.size() ? st_->fn->vreg_ty[v] : t_unknown();
    }

    VReg call_runtime(RtFn f, TypePtr ret, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        return def(std::move(ret), std::move(in));
    }

    // 返回 void 的运行时函数不分配结果寄存器，省掉一条无用的 movq
    void call_void(RtFn f, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        emit(std::move(in));
    }

    void print_value(VReg v, TypePtr ty, int line, int col);
    // print(..., file=f)：值先转成字符串再 FileWrite；print_file_slot_ >= 0
    // 时由 print_value 分派到这里。
    void print_value_to_file(VReg v, TypePtr ty, int line, int col);
    // 列表/元组：按静态元素类型逐个打印，元素递归走上面的两条路径。
    void print_seq(VReg v, TypePtr ty, bool tuple, bool to_file, int line, int col);
    // 容器里的元素按 repr 语义打印（CPython 打印容器用元素 repr）。
    void print_elem(VReg ev, TypePtr et, bool to_file, int line, int col);
    // 遍历一个列表逐元素打印，元素之间用 ", " 分隔；回调拿到元素和下标。
    void print_each(VReg list, TypePtr et, bool to_file,
                    const std::function<void(VReg, VReg)>& body);
    // dict：{key: value, ...}；set：{elem, ...}。都按 repr 语义。
    void print_dict(VReg v, TypePtr ty, bool to_file, int line, int col);
    // 空容器打印 empty_text，非空但内容看不清时打印 placeholder。
    void print_empty_or_placeholder(VReg v, const std::string& empty_text,
                                     const std::string& placeholder, bool to_file);
    void print_set(VReg v, TypePtr ty, bool to_file, int line, int col);
    // 往 stdout 或 print(..., file=f) 写一段字面量。
    void print_write(const std::string& s, bool to_file);
    int print_file_slot_ = -1;

    // ── 预扫描 ──────────────────────────────────────────────────

    void hoist_globals(std::vector<StmtPtr>& body);
    void collect_funcdefs(std::vector<StmtPtr>& body, FunctionDef* parent = nullptr);
    void compute_closure_meta(FunctionDef* f);
    TypePtr capture_type(const FunctionDef* f, const std::string& name);
    void bind_name(Expr& target);
    void scan_globals(Stmt& s, bool enter_functions = false);

    void push_frame(IRFunction* fn, const Scope* scope) {
        auto ctx = std::make_unique<FuncCtx>();
        ctx->fn = fn;
        ctx->scope = scope;
        frames_.push_back(std::move(ctx));
        st_ = frames_.back().get();
    }

    void pop_frame() {
        frames_.pop_back();
        st_ = frames_.empty() ? nullptr : frames_.back().get();
    }
};


}  // namespace pyadamas
