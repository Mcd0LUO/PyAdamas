// ir/ops.h —— IR 算子与运行期函数编号
//
// 从 ir/ir.h 拆出：这一半是纯枚举与名字表，数据结构在 ir/types.h。
#pragma once

#include <cstdint>

namespace pyadamas {

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

#include "ir/rtfn.h"

// 字符串方法的操作码，序号必须与 runtime/rt_obj.c 的 pa_str_method 一致。
enum class StrOp : int64_t {
    Upper, Lower, Strip, LStrip, RStrip, StripChars, LStripChars, RStripChars,
    SplitWs, SplitSep, Join, Replace, Find, RFind, StartsWith, EndsWith, Count,
    ZFill, LJust, RJust, Center, Capitalize, Title, IsDigit, IsAlpha, IsSpace,
    IsUpper, IsLower, IsAlphaNum, RemovePrefix, RemoveSuffix, SplitLines,
    Encode, Decode,
    IsAscii,  // 0.572：只含 ASCII（C locale 语义，不动码点）
    RIndex,   // 0.573：rfind + 找不到就抛 ValueError
};

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
    GlobalAddr,  // dst = &globals[idx]（外部符号时是符号地址，不解引用）
    ClassAddr,   // dst = &classes[idx].vtable

    // ── 容器 ──────────────────────────────────────────────
    MakeList,   // dst = <ty>[](args...)
    MakeTuple,  // dst = (args...)
    IndexLoad,  // dst = a[b]
    IndexLoadUnchecked, // dst = a[b]，调用方已证明 0 <= b < len(a)，不检查
    IndexStore, // a[b] = c
    IndexStoreUnchecked, // a[b] = c，调用方已证明 0 <= b < len(a)，不检查
    Len,        // dst = len(a)

    // ── 对象 ──────────────────────────────────────────────
    ObjNew,     // dst = new <class imm>[n_fields]，头部写 vtable 指针
    FieldLoad,  // dst = a->field[imm]（imm 是字节偏移）
    FieldStore, // a->field[imm] = b
    PtrLoad,    // dst = *(a + b)；imm 是元素宽度（2/4/8 走比例寻址）
    PtrStore,   // *(a + b) = c；imm 是元素宽度
    VCall,      // dst = a->vtable[imm](a, args...)  第一个实参固定是 self
    VCallStatic,// dst = a->vtable[imm](args...)  静态方法不传 self
    SuperVCall, // dst = super_lookup(a, idx, imm)(a, args...)  沿运行期 MRO
    CallDirect, // dst = funcs[idx](args...)  直接调用裸函数（静态方法）
    CallPtr,    // dst = a(args...)  a 是裸代码地址（类对象 vtable 分派）

    // ── 值类型 ────────────────────────────────────────────
    StructNew,  // dst = pa_struct_new(imm)；imm 是 payload 字节数
    StructCopy, // dst = pa_struct_copy(a, imm)；按值语义复制一份 payload

    // ── 调用 ──────────────────────────────────────────────
    Call,    // dst = a(args...)（a 是函数指针）
    CallRt,  // dst = <RtFn(imm)>(args...)
    CallExtern, // dst = <externs[idx].symbol>(args...)；参数按 C ABI 编组
    LoadExtern, // dst = <extern_vars[idx].symbol>；按声明宽度读取 C 全局变量
    StoreExtern, // <extern_vars[idx].symbol> = a；按声明宽度写回
    Setjmp,  // dst = setjmp(a)（a 是 handler 记录，异常跳回时返回非 0）
    // ── 零开销异常（Z2-b2）：保护区 / landing pad 的位置标记 ───────
    // 都是「不产出机器码（只发标签）/ 只发 landing pad 入口序列」的标记
    // 指令，供 codegen 拼 .gcc_except_table 的 call-site 区间。它们没有
    // dst、没有副作用，不在 terminated() 里。
    EhBegin,  // 保护区起点：发 <begin> 标签
    EhEnd,    // 保护区终点（开区间）：发 <end> 标签
    EhLandingPad, // landing pad 入口：发 <lp> 标签；之后紧跟 CallRt EhLand
    ImportModule, // imm 是 imports 下标：确保模块已初始化
    SlotAddr,     // dst = &slots[idx]（栈槽或生成器帧槽的地址）

    // ── 生成器 ────────────────────────────────────────────────
    GenNew,     // dst = 新生成器帧；imm 是 generators 下标
    GenSetSlot, // 创建期把参数写进帧槽；imm 是槽号，args = {gen, value}
    GenGetState,// dst = 帧 state
    GenClosing, // dst = 帧 closing 标志（close/finalize 期间非 0）
    GenSent,    // dst = 恢复时传入的值
    GenYield,   // 把 a 写进 out、state=imm，然后返回（终止指令）
    GenAwait,   // 把 a 写进 out、state=imm、status=3，然后返回（终止指令）
    GenGetExc,  // dst = 帧里待注入的异常指针
    GenTakeExc, // dst = 帧里待注入的异常指针，并把帧槽清零（所有权转移出去）
    GenRethrow, // 在 resume 帧内抛出注入的异常（终止指令）
    GenDone,    // status=1；a 非空时先把返回值写进 out（终止指令）

    // ── 控制流 ────────────────────────────────────────────
    Jump,    // -> target
    Branch,  // a ? target : target2
    Return,  // return a（a == kNoReg 表示 None）

    // ── P3 big ──────────────────────────────────────────────
    ConstBig, // dst = bigs[idx]（永生 BigInt 静态对象，十进制串编译期转 limb）
};

inline const char* irop_name(IrOp op) {
    switch (op) {
        case IrOp::ConstInt: return "const.int";
        case IrOp::ConstFloat: return "const.float";
        case IrOp::ConstStr: return "const.str";
        case IrOp::ConstNone: return "const.none";
        case IrOp::ConstBig: return "const.big";
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
        case IrOp::GlobalAddr: return "global.addr";
        case IrOp::ClassAddr: return "class.addr";
        case IrOp::MakeList: return "list.new";
        case IrOp::MakeTuple: return "tuple.new";
        case IrOp::IndexLoad: return "index.get";
        case IrOp::IndexLoadUnchecked: return "index.get.unchecked";
        case IrOp::IndexStore: return "index.set";
        case IrOp::IndexStoreUnchecked: return "index.set.unchecked";
        case IrOp::Len: return "len";
        case IrOp::ObjNew: return "obj.new";
        case IrOp::FieldLoad: return "field.get";
        case IrOp::FieldStore: return "field.set";
        case IrOp::PtrLoad: return "ptr.load";
        case IrOp::PtrStore: return "ptr.store";
        case IrOp::VCall: return "vcall";
        case IrOp::VCallStatic: return "vcall.static";
        case IrOp::SuperVCall: return "vcall.super";
        case IrOp::CallDirect: return "call.direct";
        case IrOp::CallPtr: return "call.ptr";
        case IrOp::StructNew: return "struct.new";
        case IrOp::StructCopy: return "struct.copy";
        case IrOp::Call: return "call";
        case IrOp::CallRt: return "call.rt";
        case IrOp::CallExtern: return "call.extern";
        case IrOp::LoadExtern: return "load.extern";
        case IrOp::StoreExtern: return "store.extern";
        case IrOp::Setjmp: return "setjmp";
        case IrOp::EhBegin: return "eh.begin";
        case IrOp::EhEnd: return "eh.end";
        case IrOp::EhLandingPad: return "eh.landing.pad";
        case IrOp::ImportModule: return "import.module";
        case IrOp::SlotAddr: return "slot.addr";
        case IrOp::GenNew: return "gen.new";
        case IrOp::GenSetSlot: return "gen.set";
        case IrOp::GenGetState: return "gen.state";
        case IrOp::GenClosing: return "gen.closing";
        case IrOp::GenSent: return "gen.sent";
        case IrOp::GenYield: return "gen.yield";
        case IrOp::GenAwait: return "gen.await";
        case IrOp::GenGetExc: return "gen.exc";
        case IrOp::GenTakeExc: return "gen.take.exc";
        case IrOp::GenRethrow: return "gen.rethrow";
        case IrOp::GenDone: return "gen.done";
        case IrOp::Jump: return "jump";
        case IrOp::Branch: return "branch";
        case IrOp::Return: return "return";
    }
    return "?";
}


}  // namespace pyadamas
