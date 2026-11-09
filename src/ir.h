// ir.h —— 中级表示
//
// 形态：函数内 → 基本块 → 线性三地址码，虚拟寄存器编号。
//
// 为什么不是 SSA：Python 语义里变量天生可变（循环里反复赋值是常态），
// 一上来就构造 phi 会把 for/while/break 的降级搞得非常绕，收益却要等
// 到真正的优化遍才兑现。现在用「可变槽位 + 虚拟寄存器」：值有编号，
// 变量有固定槽位，后端直接按槽分配栈帧，语义简单且不会错。
// 后续要加 SSA 是在这层之上再做一次构造，不必回头改前端。
//
// 为什么类型挂在指令上：前端已经推完了每个表达式的类型，后端选指令
// 宽度（i64 / f64 / 指针）全靠它。丢掉类型再让后端猜是这类项目里最
// 常见的返工原因。
#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "typecheck.h"

namespace py23 {

using VReg = uint32_t;
inline constexpr VReg kNoReg = 0xFFFFFFFFu;

// 二元运算。与 Tok 分开：Tok 是词法记号，可能包含用不到的组合；
// IR 只保留真正有语义的算子，后端 switch 才不用写 default 兜底。
enum class BinKind : uint8_t {
    Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
    BitAnd, BitOr, BitXor, Shl, Shr,
};

enum class UnKind : uint8_t { Neg, Pos, BitNot, Not };

enum class CmpKind : uint8_t { Lt, Le, Gt, Ge, Eq, Ne };

inline const char* bin_name(BinKind k) {
    switch (k) {
        case BinKind::Add: return "add";
        case BinKind::Sub: return "sub";
        case BinKind::Mul: return "mul";
        case BinKind::Div: return "div";
        case BinKind::FloorDiv: return "floordiv";
        case BinKind::Mod: return "mod";
        case BinKind::Pow: return "pow";
        case BinKind::BitAnd: return "and";
        case BinKind::BitOr: return "or";
        case BinKind::BitXor: return "xor";
        case BinKind::Shl: return "shl";
        case BinKind::Shr: return "shr";
    }
    return "?";
}

inline const char* un_name(UnKind k) {
    switch (k) {
        case UnKind::Neg: return "neg";
        case UnKind::Pos: return "pos";
        case UnKind::BitNot: return "bitnot";
        case UnKind::Not: return "not";
    }
    return "?";
}

inline const char* cmp_name(CmpKind k) {
    switch (k) {
        case CmpKind::Lt: return "lt";
        case CmpKind::Le: return "le";
        case CmpKind::Gt: return "gt";
        case CmpKind::Ge: return "ge";
        case CmpKind::Eq: return "eq";
        case CmpKind::Ne: return "ne";
    }
    return "?";
}

// 运行时库函数。名字固定，后端按枚举直接生成调用，
// 不经过符号查找，也不允许用户代码直接引用。
enum class RtFn : uint8_t {
    ListNew,     // (i64 cap) -> list*
    ListAppend,  // (list*, i64) -> void
    ListGet,     // (list*, i64) -> i64
    ListSet,     // (list*, i64, i64) -> void
    ListSetLen,  // (list*, i64) -> void
    StrConcat,   // (str*, str*) -> str*
    StrCharAt,   // (str*, i64) -> str*
    StrFromI64,  // (i64) -> str*
    StrFromF64,  // (f64) -> str*
    StrFromBool, // (i64) -> str*
    StrFromNone, // () -> str*
    PrintI64,    // (i64) -> void
    PrintF64,    // (f64) -> void
    PrintBool,   // (i64) -> void
    PrintStr,    // (str*) -> void
    PrintNone,   // () -> void
    PrintSep,    // () -> void  打印一个空格
    PrintEnd,    // () -> void  打印换行
    Panic,       // (str*) -> noreturn
    PanicDiv0,   // () -> noreturn  整数除以零
    PanicOverflow, // () -> noreturn  INT64_MIN / -1
    RcInc,       // (ptr) -> void
    RcDec,       // (ptr) -> void
    RcIncUnknown,// (i64) -> i64  按堆注册表保守 retain（unknown 值）
    RcTemp,      // (ptr) -> void  表达式临时值入栈，接管产生者的引用
    RcMark,      // () -> i64      临时值栈水位
    RcCleanup,   // (i64) -> void  释放到指定水位
    RcShutdown,  // () -> void     程序退出，释放临时栈自身的存储
    GcShutdown,  // () -> void     程序退出，释放所有剩余堆对象（含环）
    ClosureNew,  // (i64 code, i64 n_caps) -> closure*
    DictNew,      // (i64 key_kind, i64 key_own, i64 val_own) -> dict*
    DictSet,      // (dict*, i64 key, i64 val) -> void
    DictGet,      // (dict*, i64 key) -> i64
    DictContains, // (dict*, i64 key) -> i64
    DictKeyAt,    // (dict*, i64 index) -> i64
    ListContains, // (list*, i64 value, i64 cmp_kind) -> i64
    StrContains,  // (str*, str*) -> i64
    DictGetOr,    // (dict*, i64 key, i64 default) -> i64
    DictKeys,     // (dict*) -> list*
    DictValues,   // (dict*) -> list*
    DictItems,    // (dict*) -> list*
    DictUpdate,   // (dict*, dict*) -> void
    ListSlice,    // (list*, i64 start, i64 stop, i64 step) -> list*
    StrSlice,     // (str*, i64 start, i64 stop, i64 step) -> str*
    DictPop,      // (dict*, i64 key, i64 has_default, i64 def) -> i64
    DictPopItem,  // (dict*) -> (i64, i64)
    DictPopLast,  // (dict*) -> i64
    DictRemove,   // (dict*, i64 key, i64 missing_ok) -> void
    DictClear,    // (dict*) -> void
    ListSliceAssign, // (list*, i64 start, i64 stop, i64 step, list*) -> void
    StrToList,    // (str*) -> list*
    ListExtend,   // (list* dst, list* src) -> void
    RangeNew,     // (i64 start, i64 stop, i64 step) -> list*
    HandlerPush,  // () -> handler*   压入异常 handler，记录临时栈水位
    HandlerLeave, // (handler*) -> void 弹出并清理临时栈
    HandlerDiscard, // (handler*) -> void 只弹出，不清理临时值
    ExcMatches,   // (str* type) -> i64
    ExcAccept,    // () -> str*       把 pending 转为 current，返回消息
    ExcDone,      // () -> void
    ExcClear,     // () -> void
    ExcReraise,   // () -> void       noreturn
    ExcTypeName,  // () -> str*
    ExcMessage,   // () -> str*
    Raise,        // (str* type, str* msg) -> void  noreturn
    Reraise,      // () -> void       noreturn  裸 raise
};

inline const char* rt_name(RtFn f) {
    switch (f) {
        case RtFn::ListNew: return "py23_list_new";
        case RtFn::ListAppend: return "py23_list_append";
        case RtFn::ListGet: return "py23_list_get";
        case RtFn::ListSet: return "py23_list_set";
        case RtFn::ListSetLen: return "py23_list_set_len";
        case RtFn::StrConcat: return "py23_str_concat";
        case RtFn::StrCharAt: return "py23_str_char_at";
        case RtFn::StrFromI64: return "py23_str_from_i64";
        case RtFn::StrFromF64: return "py23_str_from_f64";
        case RtFn::StrFromBool: return "py23_str_from_bool";
        case RtFn::StrFromNone: return "py23_str_from_none";
        case RtFn::PrintI64: return "py23_print_i64";
        case RtFn::PrintF64: return "py23_print_f64";
        case RtFn::PrintBool: return "py23_print_bool";
        case RtFn::PrintStr: return "py23_print_str";
        case RtFn::PrintNone: return "py23_print_none";
        case RtFn::PrintSep: return "py23_print_sep";
        case RtFn::PrintEnd: return "py23_print_end";
        case RtFn::Panic: return "py23_panic";
        case RtFn::PanicDiv0: return "py23_panic_div0";
        case RtFn::PanicOverflow: return "py23_panic_overflow";
        case RtFn::RcInc: return "py23_rc_inc";
        case RtFn::RcDec: return "py23_rc_dec";
        case RtFn::RcIncUnknown: return "py23_rc_inc_unknown";
        case RtFn::RcTemp: return "py23_rc_temp";
        case RtFn::RcMark: return "py23_rc_mark";
        case RtFn::RcCleanup: return "py23_rc_cleanup";
        case RtFn::RcShutdown: return "py23_rc_shutdown";
        case RtFn::GcShutdown: return "py23_gc_shutdown";
        case RtFn::ClosureNew: return "py23_closure_new";
        case RtFn::DictNew: return "py23_dict_new";
        case RtFn::DictSet: return "py23_dict_set";
        case RtFn::DictGet: return "py23_dict_get";
        case RtFn::DictContains: return "py23_dict_contains";
        case RtFn::DictKeyAt: return "py23_dict_key_at";
        case RtFn::ListContains: return "py23_list_contains";
        case RtFn::StrContains: return "py23_str_contains";
        case RtFn::DictGetOr: return "py23_dict_get_or";
        case RtFn::DictKeys: return "py23_dict_keys";
        case RtFn::DictValues: return "py23_dict_values";
        case RtFn::DictItems: return "py23_dict_items";
        case RtFn::DictUpdate: return "py23_dict_update";
        case RtFn::ListSlice: return "py23_list_slice";
        case RtFn::StrSlice: return "py23_str_slice";
        case RtFn::DictPop: return "py23_dict_pop";
        case RtFn::DictPopItem: return "py23_dict_popitem";
        case RtFn::DictPopLast: return "py23_dict_pop_last";
        case RtFn::DictRemove: return "py23_dict_remove";
        case RtFn::DictClear: return "py23_dict_clear";
        case RtFn::ListSliceAssign: return "py23_list_slice_assign";
        case RtFn::StrToList: return "py23_str_to_list";
        case RtFn::ListExtend: return "py23_list_extend";
        case RtFn::RangeNew: return "py23_range_new";
        case RtFn::HandlerPush: return "py23_handler_push";
        case RtFn::HandlerLeave: return "py23_handler_leave";
        case RtFn::HandlerDiscard: return "py23_handler_discard";
        case RtFn::ExcMatches: return "py23_exc_matches";
        case RtFn::ExcAccept: return "py23_exc_accept";
        case RtFn::ExcDone: return "py23_exc_done";
        case RtFn::ExcClear: return "py23_exc_clear";
        case RtFn::ExcReraise: return "py23_exc_reraise";
        case RtFn::ExcTypeName: return "py23_exc_type_name";
        case RtFn::ExcMessage: return "py23_exc_message";
        case RtFn::Raise: return "py23_raise";
        case RtFn::Reraise: return "py23_reraise";
    }
    return "?";
}

enum class IrOp : uint8_t {
    // ── 常量 ──────────────────────────────────────────────
    ConstInt,    // dst = imm
    ConstFloat,  // dst = fimm
    ConstStr,    // dst = strings[idx]
    ConstNone,   // dst = None

    // ── 值 ────────────────────────────────────────────────
    FuncAddr,  // dst = &static_closure[imm]（无条件/无捕获函数值）
    FuncCode,  // dst = &funcs[imm]（裸代码地址，只给 ClosureNew 用）
    Mov,       // dst = a

    // ── 运算 ──────────────────────────────────────────────
    Bin,  // dst = a <bop> b
    Un,   // dst = <uop> a
    Cmp,  // dst = a <cop> b   （结果是 bool）
    Convert,  // dst = a 转成 ty（int/float/bool/str）

    // ── 变量 ──────────────────────────────────────────────
    LoadSlot,    // dst = slots[idx]
    StoreSlot,   // slots[idx] = a
    LoadGlobal,  // dst = globals[idx]
    StoreGlobal, // globals[idx] = a

    // ── 容器 ──────────────────────────────────────────────
    MakeList,   // dst = <ty>[](args...)
    MakeTuple,  // dst = (args...)
    IndexLoad,  // dst = a[b]
    IndexStore, // a[b] = c
    Len,        // dst = len(a)

    // ── 对象 ──────────────────────────────────────────────
    ObjNew,     // dst = new <class imm>[n_fields]，头部写 vtable 指针
    FieldLoad,  // dst = a->field[imm]（imm 是字节偏移）
    FieldStore, // a->field[imm] = b
    VCall,      // dst = a->vtable[imm](a, args...)  第一个实参固定是 self

    // ── 调用 ──────────────────────────────────────────────
    Call,    // dst = a(args...)（a 是函数指针）
    CallRt,  // dst = <RtFn(imm)>(args...)
    Setjmp,  // dst = setjmp(a)（a 是 handler 记录，异常跳回时返回非 0）

    // ── 控制流 ────────────────────────────────────────────
    Jump,    // -> target
    Branch,  // a ? target : target2
    Return,  // return a（a == kNoReg 表示 None）
};

inline const char* irop_name(IrOp op) {
    switch (op) {
        case IrOp::ConstInt: return "const.int";
        case IrOp::ConstFloat: return "const.float";
        case IrOp::ConstStr: return "const.str";
        case IrOp::ConstNone: return "const.none";
        case IrOp::FuncAddr: return "func.addr";
        case IrOp::FuncCode: return "func.code";
        case IrOp::Mov: return "mov";
        case IrOp::Bin: return "bin";
        case IrOp::Un: return "un";
        case IrOp::Cmp: return "cmp";
        case IrOp::Convert: return "convert";
        case IrOp::LoadSlot: return "load.slot";
        case IrOp::StoreSlot: return "store.slot";
        case IrOp::LoadGlobal: return "load.global";
        case IrOp::StoreGlobal: return "store.global";
        case IrOp::MakeList: return "list.new";
        case IrOp::MakeTuple: return "tuple.new";
        case IrOp::IndexLoad: return "index.get";
        case IrOp::IndexStore: return "index.set";
        case IrOp::Len: return "len";
        case IrOp::ObjNew: return "obj.new";
        case IrOp::FieldLoad: return "field.get";
        case IrOp::FieldStore: return "field.set";
        case IrOp::VCall: return "vcall";
        case IrOp::Call: return "call";
        case IrOp::CallRt: return "call.rt";
        case IrOp::Setjmp: return "setjmp";
        case IrOp::Jump: return "jump";
        case IrOp::Branch: return "branch";
        case IrOp::Return: return "return";
    }
    return "?";
}

struct Inst {
    IrOp op = IrOp::ConstNone;
    BinKind bop = BinKind::Add;
    UnKind uop = UnKind::Neg;
    CmpKind cop = CmpKind::Eq;

    VReg dst = kNoReg;
    VReg a = kNoReg, b = kNoReg, c = kNoReg;

    int64_t imm = 0;
    double fimm = 0.0;
    int32_t idx = -1;     // 槽号 / 全局号 / 字符串号 / 函数号
    int32_t target = -1;  // 跳转目标块
    int32_t target2 = -1;

    TypePtr ty;  // 结果类型；无结果时为 nullptr

    std::vector<VReg> args;  // MakeList/MakeTuple/Call 的操作数

    std::string text() const;
};

struct Block {
    std::string name;
    std::vector<Inst> insts;

    bool terminated() const {
        return !insts.empty() && (insts.back().op == IrOp::Jump ||
                                  insts.back().op == IrOp::Branch ||
                                  insts.back().op == IrOp::Return);
    }
};

struct IRParam {
    std::string name;
    TypePtr ty;
    int slot = -1;
};

// 一个全局变量。数据段里就是一个 .quad，初始为 0 —— 0 正好也是 None
// 的机器表示，所以「尚未赋值」天然就是 None。
struct IRGlobal {
    std::string name;
    TypePtr ty;
};

struct IRFunction {
    std::string name;
    std::vector<IRParam> params;
    TypePtr ret;
    // 局部槽位（含形参），下标即槽号。参数槽排在最前面，后端按这个
    // 顺序把寄存器 / 栈上的实参搬进来。
    std::vector<TypePtr> slot_ty;
    // 每个虚拟寄存器的类型，后端决定用 rax 还是 xmm 时查它
    std::vector<TypePtr> vreg_ty;
    std::vector<Block> blocks;
    // 有捕获的函数：入口把 %r10 里的闭包指针存进这个槽。
    int env_slot = -1;
    // 含 setjmp 的函数关闭槽位寄存器提升：longjmp 会把 callee-saved
    // 寄存器恢复成 setjmp 时刻的值，寄存器里的变量会回退。
    bool has_handlers = false;

    int new_slot(TypePtr ty) {
        slot_ty.push_back(std::move(ty));
        return static_cast<int>(slot_ty.size()) - 1;
    }
};

// 一个类的运行时形态。布局在降级阶段就完全确定：
//   vtable 是静态表，每个槽一个函数地址（未填的槽是 -1，正常不会出现）；
//   n_fields 含继承来的字段，对象大小 = 8 * (1 + n_fields)。
struct IRClass {
    std::string name;
    std::vector<int> vtable;  // 槽 → 函数下标
    int n_fields = 0;
    // 按字段偏移顺序（父类在前），1 表示该字段是堆类型，析构时要 dec。
    std::vector<uint8_t> field_heap;
};

struct IRModule {
    // 用 unique_ptr 持有：降级过程中会不断新增函数（嵌套 def），
    // vector<IRFunction> 扩容会让外面持有的指针失效。
    std::vector<std::unique_ptr<IRFunction>> funcs;
    std::vector<IRGlobal> globals;
    std::vector<std::string> strings;
    std::vector<IRClass> classes;
    int entry = -1;  // 模块顶层代码编译成的函数下标

    IRFunction* add_func(std::string name) {
        auto f = std::make_unique<IRFunction>();
        f->name = std::move(name);
        funcs.push_back(std::move(f));
        return funcs.back().get();
    }

    int intern_string(const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return static_cast<int>(i);
        strings.push_back(s);
        return static_cast<int>(strings.size()) - 1;
    }
};

// ── 类型到机器类的映射 ────────────────────────────────────────────

inline bool ir_is_float(const TypePtr& t) { return t && t->kind == TyKind::Float; }

// 引用计数管理的堆类型。unknown 故意不算：它的机器表示可能是整数，
// 按指针 dec 会直接段错误。宁可泄漏，不可误杀。
inline bool ir_is_heap(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Str:
        case TyKind::List:
        case TyKind::Tuple:
        case TyKind::Class:
        case TyKind::Func:  // 静态闭包永生，堆闭包引用计数
        case TyKind::Dict:
        case TyKind::Set:
            return true;
        case TyKind::Optional:
            return !t->sub.empty() && ir_is_heap(t->sub[0]);
        default:
            return false;
    }
}

// 指针类：需要按地址传递 / 取字段的类型
inline bool ir_is_ref(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Str:
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Dict:
        case TyKind::Tuple:
        case TyKind::Func:
        case TyKind::BoundMethod:
        case TyKind::Class:
            return true;
        default:
            return false;
    }
}

inline bool ir_is_int_like(const TypePtr& t) {
    if (!t) return true;
    switch (t->kind) {
        case TyKind::Bool:
        case TyKind::Int:
        case TyKind::None:
        case TyKind::Optional:
            return true;
        default:
            return false;
    }
}

void dump_ir(const IRModule& m, std::ostream& os);

}  // namespace py23
