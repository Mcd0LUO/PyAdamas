// ir_types.h —— IR 数据结构与类型到机器类的映射
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "ir_ops.h"
#include "typecheck.h"

namespace pyadamas {

using VReg = uint32_t;
inline constexpr VReg kNoReg = 0xFFFFFFFFu;

// 定义在文件末尾（依赖 TyKind 的完整列表）。
inline bool ir_is_heap(const TypePtr& t);


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
    // 生成指令对应的源码行（调试信息 / DWARF .loc）。0 = 无行信息。
    int32_t line = 0;

    TypePtr ty;  // 结果类型；无结果时为 nullptr

    std::vector<VReg> args;  // MakeList/MakeTuple/Call 的操作数
    // CallExtern 每个实参的编组方式（CKind）。同一符号可能被不同类型实参
    // 调用（如 ptr 既接地址又接 list），所以按调用点记录；为空时回退到
    // IRExtern::params。
    std::vector<uint8_t> cargs;

    // FieldLoad/FieldStore 的访问宽度（字节）：1/4/8。类字段固定 8；
    // C 结构体的 int32/uint32 是 4，bool 是 1。
    uint8_t fwidth = 8;
    // 4 字节字段是否无符号：false 时 movslq 符号扩展，true 时 movl 零扩展。
    bool funsigned = false;

    std::string text() const;
};

struct Block {
    std::string name;
    std::vector<Inst> insts;

    bool terminated() const {
        return !insts.empty() && (insts.back().op == IrOp::Jump ||
                                  insts.back().op == IrOp::Branch ||
                                  insts.back().op == IrOp::Return ||
                                  insts.back().op == IrOp::GenYield ||
                                  insts.back().op == IrOp::GenAwait ||
                                  insts.back().op == IrOp::GenRethrow ||
                                  insts.back().op == IrOp::GenDone);
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
    // 编译期的链接符号；为空表示本地 .LgN。
    std::string symbol;
    // 外部模块的全局：只引用符号，不在这里分配存储，也不进根表。
    bool external = false;
    // 曾经以 unknown 值写入过（或被 unknown 类型重新标注）：旧值释放要
    // 走注册表判断，避免把整数当指针 dec。
    bool maybe_unknown = false;
};

struct IRFunction {
    std::string name;
    std::vector<IRParam> params;
    TypePtr ret;
    // 局部槽位（含形参），下标即槽号。参数槽排在最前面，后端按这个
    // 顺序把寄存器 / 栈上的实参搬进来。
    std::vector<TypePtr> slot_ty;
    // 槽是否持有堆引用（出口要 release、异常展开要按帧记录释放）。
    // 借用槽（只读形参、迭代临时槽、闭包环境槽）为 false。
    std::vector<bool> slot_owned;
    // 堆槽里可能装着非堆值（unknown 值流过静态堆类型槽）：释放要走
    // 注册表判断，不能直接按指针 dec。
    std::vector<bool> slot_unknown;
    // 每个虚拟寄存器的类型，后端决定用 rax 还是 xmm 时查它
    std::vector<TypePtr> vreg_ty;
    std::vector<Block> blocks;
    // 有捕获的函数：入口把 %r10 里的闭包指针存进这个槽。
    int env_slot = -1;
    // 含 setjmp 的函数关闭槽位寄存器提升：longjmp 会把 callee-saved
    // 寄存器恢复成 setjmp 时刻的值，寄存器里的变量会回退。
    bool has_handlers = false;
    // O2 内联候选：无捕获、无 handler、非生成器/异步、签名固定。
    bool can_inline = false;
    // 生成器 resume：槽位不在栈上，而在堆帧里（%r15 指向帧负载）。
    // 入口按帧里的 state 分发到续跑块，参数已由创建包装写进帧。
    bool is_generator = false;

    int new_slot(TypePtr ty) {
        slot_owned.push_back(ir_is_heap(ty));
        slot_unknown.push_back(false);
        slot_ty.push_back(std::move(ty));
        return static_cast<int>(slot_ty.size()) - 1;
    }
};

// 跨模块祖先：子类元数据里为它生成一个本地 mirror，指向定义模块导出的
// vtable / own_slots 表。mirror 的类编号固定 -1，只参与限定名比较。
struct IRClassExtBase {
    std::string vtable_symbol;
    std::string own_slots_symbol;
    std::string qualname;
};

// 一个类的运行时形态。布局在降级阶段就完全确定：
//   vtable 是静态表，每个槽一个函数地址（未填的槽是 -1，正常不会出现）；
//   n_fields 含继承来的字段，对象大小 = 8 * (1 + n_fields)。
struct IRClass {
    std::string name;
    std::vector<int> vtable;  // 槽 → 函数下标
    // 与 vtable 平行：非空表示该槽来自导入基类，内容是外部符号表达式
    // （如 pyadamas$base$vt$Base + 24）。
    std::vector<std::string> vtable_ext;
    int n_fields = 0;
    // 按字段偏移顺序（父类在前），1 表示该字段是堆类型，析构时要 dec。
    std::vector<uint8_t> field_heap;
    // 运行期类元数据：模块限定类名、C3 MRO（类下标，self 在前）和
    // 本类自己声明的方法槽。isinstance / super() 都靠这张表。
    std::string qualname;
    // C3 MRO，self 在前。>=0 是本地类下标；<0 表示 -(ext_bases 下标 + 1)，
    // 允许本地类与导入祖先按真实 MRO 顺序交错。
    std::vector<int> mro;
    // 导入祖先：mirror 元数据描述。
    std::vector<IRClassExtBase> ext_bases;
    std::vector<int> own_slots;
    // 动态属性槽在实例里的字节偏移；0 表示这个类没有动态属性槽。
    // 槽放在字段之后，实例大小 = 8 * (1 + n_fields) + (dict_off ? 8 : 0)。
    // 运行时通过 vtable 元数据的第 6 个字拿到它，所以派生类的偏移可以不同。
    int64_t dict_off = 0;
};

// 一个生成器的运行时形态。帧负载布局与运行时 rt.c 一致：
//   [state][status][sent][out][slot0..slotN-1]
// resume 是状态机函数；slot_heap 决定帧销毁时要 dec 哪些槽。
struct IRGenerator {
    int resume = -1;
    std::vector<uint8_t> slot_heap;
    int n_slots = 0;
    // 0 生成器 / 1 协程 / 2 异步生成器
    int frame_kind = 0;
};

// 按值结构体在 SysV x86-64 下的分类结果：每 8 字节一个 chunk，
// chunk_fp[i] == 1 表示该 chunk 走 XMM（只含 double 字段），否则走整数寄存器/栈。
struct IRStructABI {
    int64_t size = 0;
    std::vector<uint8_t> chunk_fp;
};

// C 回调参数的签名；codegen 为每个签名生成一个跳板 + 一个闭包槽。
struct IRCallbackSig {
    std::vector<CKind> params;
    CKind ret = CKind::None;
};

struct IRExtern {
    std::string symbol;
    std::vector<CKind> params;
    CKind ret = CKind::None;
    // 与 params 等长；非结构体参数对应 size == 0 的空项。
    std::vector<IRStructABI> param_abi;
    IRStructABI ret_abi;
    // 与 params 等长；回调参数记录 cb_sigs 下标，其余为 -1。
    std::vector<int> param_cb;
    // C 可变参数函数：调用前要设置 %al（使用的 xmm 寄存器数）。
    bool varargs = false;
};

// .cpya 的 extern 变量：只记录符号与加载宽度。
struct IRExternVar {
    std::string symbol;
    CKind kind = CKind::Int;
};

struct IRModule {
    // 用 unique_ptr 持有：降级过程中会不断新增函数（嵌套 def），
    // vector<IRFunction> 扩容会让外面持有的指针失效。
    std::vector<std::unique_ptr<IRFunction>> funcs;
    std::vector<IRGlobal> globals;
    std::vector<std::string> strings;
    std::vector<IRClass> classes;
    std::vector<IRGenerator> generators;
    // 模块编译：符号前缀、运行期 __name__、源路径、是否入口。
    std::string module_symbol;
    std::string runtime_name = "__main__";
    std::string source_path;
    bool is_entry = true;
    // 生成代码优化等级（0/1/2），由 LowerOptions 传入 codegen。
    int opt_level = 2;
    // import.module 指令引用的模块名（按出现顺序去重）。
    std::vector<std::string> imports;
    // cimport 外部函数表：CallExtern 的 idx 指向这里。
    std::vector<IRExtern> externs;
    // cimport extern 变量表：LoadExtern 的 idx 指向这里。
    std::vector<IRExternVar> extern_vars;
    // cimport 回调签名表：生成的跳板/闭包槽按这里的下标命名。
    std::vector<IRCallbackSig> cb_sigs;
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
        case TyKind::Bytes:
        case TyKind::List:
        case TyKind::Tuple:
        case TyKind::Class:
        case TyKind::Func:  // 静态闭包永生，堆闭包引用计数
        case TyKind::Dict:
        case TyKind::Set:
        case TyKind::Generator:
        case TyKind::Coroutine:
        case TyKind::Task:
        case TyKind::AsyncGenerator:
        case TyKind::Future:
        case TyKind::AsyncEvent:
        case TyKind::AsyncLock:
        case TyKind::AsyncQueue:
        case TyKind::TypeObj:
        case TyKind::ClassObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:  // 异常值：堆对象，args 列表归它所有
        case TyKind::Range:  // range 对象：堆上的惰性序列
        case TyKind::Complex:
        case TyKind::File:
        case TyKind::Random:
        case TyKind::CompletedProcess:
        case TyKind::Popen:
        case TyKind::Regex:
        case TyKind::Match:
        case TyKind::DateTime:
        case TyKind::TimeDelta:
        case TyKind::Path:
        case TyKind::Deque:
        case TyKind::Struct:
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
        case TyKind::Bytes:
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Dict:
        case TyKind::Tuple:
        case TyKind::Func:
        case TyKind::BoundMethod:
        case TyKind::Class:
        // 值类型结构体（包括 .cpya 声明的 C 结构体）都由 pa_struct_new
        // 分配，带 RC 头；只有 StructPtr 才可能是没有 RC 头的 C 指针。
        case TyKind::Struct: return true;
        case TyKind::StructPtr: return t->st && !t->st->c_struct;
        case TyKind::Generator:
        case TyKind::Coroutine:
        case TyKind::Task:
        case TyKind::AsyncGenerator:
        case TyKind::Future:
        case TyKind::AsyncEvent:
        case TyKind::AsyncLock:
        case TyKind::AsyncQueue:
        case TyKind::TypeObj:
        case TyKind::ClassObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:
        case TyKind::Range:
        case TyKind::Complex:
        case TyKind::File:
        case TyKind::Random:
        case TyKind::CompletedProcess:
        case TyKind::Popen:
        case TyKind::Regex:
        case TyKind::Match:
        case TyKind::DateTime:
        case TyKind::TimeDelta:
        case TyKind::Path:
        case TyKind::Deque:
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

// 程序是否需要「存活堆索引」：unknown 值的保守 retain 靠它判断一个机器字
// 是不是堆指针。没有任何 unknown 类型、没有编译器发出的 RcIncUnknown、
// 也不涉及 asyncio/生成器运行时的程序用不到索引，可以在入口关掉维护，
// 省掉每个堆对象分配/释放时的哈希表插入和删除。
bool ir_type_contains_unknown(const TypePtr& t);
bool ir_module_needs_unknown_index(const IRModule& m);

}  // namespace pyadamas
