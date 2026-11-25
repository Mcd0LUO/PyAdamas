// ir/types.h —— IR 数据结构与类型到机器类的映射
#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/ops.h"
#include "typecheck/typecheck.h"

namespace pyadamas {

using VReg = uint32_t;
inline constexpr VReg kNoReg = 0xFFFFFFFFu;

// 定义在文件末尾（依赖 TyKind 的完整列表）。
inline bool ir_is_heap(const TypePtr& t);


// ── 带 tag 的索引操作数 ─────────────────────────────────────────
//
// 原来 `Inst::idx` 是个裸 int32_t，含义随 op 变（槽号 / 全局号 /
// 字符串号 / 函数号 / 类号 / 外部符号号）。写降级代码时搞错 op 对应的
// 含义，编译器不会报错，只能靠测试和 dump 发现 —— 这是这套 IR 最容易
// 踩的坑。
//
// Operand 把「它是哪一种索引」显式记下来：
//   - 新增代码必须写明 kind（`Operand::slot(i)` 之类），语义自文档化；
//   - codegen / 优化遍可以在 debug 构建里断言 kind 与 op 匹配；
//   - 存量代码通过隐式转换继续工作（迁移期不强制一次改完）。
//
// 隐式转换是**刻意的过渡手段**：195 处使用点一次性改完风险太高，先让
// kind 能被记录和校验，再逐块收紧。等所有消费点都改成显式取 kind 后，
// 就可以删掉 operator int32_t()，那时漏写 kind 会变成编译错误。
struct Operand {
    enum Kind : uint8_t {
        None = 0,   // 未使用
        Slot,       // 局部槽号（LoadSlot/StoreSlot/SlotAddr）
        Global,     // 全局变量号（LoadGlobal/StoreGlobal/GlobalAddr）
        Str,        // 字符串池下标（ConstStr）
        Func,       // 函数号（FuncAddr/FuncCode/CallDirect）
        Class,      // 类号（ClassAddr）
        Extern,     // 外部符号号（CallExtern/LoadExtern/StoreExtern）
        Block,      // 基本块号（跳转目标，仅 target/target2 用）
        Field,      // 字段下标（FieldLoad/FieldStore）
        Big,        // big 字面量池下标（ConstBig，P3）
    };
    Kind kind = None;
    int32_t value = -1;

    Operand() = default;
    Operand(Kind k, int32_t v) : kind(k), value(v) {}
    // 迁移期隐式转换：让存量 `in.idx >= 0`、`in.idx = x` 继续工作。
    Operand& operator=(int32_t v) {
        value = v;
        return *this;
    }
    operator int32_t() const { return value; }

    static Operand slot(int32_t v) { return {Slot, v}; }
    static Operand global(int32_t v) { return {Global, v}; }
    static Operand str(int32_t v) { return {Str, v}; }
    static Operand func(int32_t v) { return {Func, v}; }
    static Operand cls(int32_t v) { return {Class, v}; }
    static Operand ext(int32_t v) { return {Extern, v}; }
    static Operand block(int32_t v) { return {Block, v}; }
    static Operand field(int32_t v) { return {Field, v}; }
    static Operand big(int32_t v) { return {Big, v}; }
};

}  // namespace pyadamas

// std::format 支持：直接打印数值，让存量的 `std::format("...{}", in.idx)`
// 继续工作（迁移期必需）。
template <>
struct std::formatter<pyadamas::Operand> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const pyadamas::Operand& o, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", o.value);
    }
};

namespace pyadamas {

struct Inst {
    // 稳定标识：在函数内唯一，且**不随块/指令重排而变**。
    //
    // 为什么需要它：优化遍（CSE / DCE / 版本化）要在指令之间建立引用
    // （use-def 边、可用表达式、跳转目标），而块内下标与块号都会被
    // vector::insert/erase 和新增块改变。用 id 做引用才不会失效。
    // 0 表示「未分配」（降级时通过 IRFunction::next_inst_id 分配）。
    uint32_t id = 0;
    IrOp op = IrOp::ConstNone;
    BinKind bop = BinKind::Add;
    UnKind uop = UnKind::Neg;
    CmpKind cop = CmpKind::Eq;

    VReg dst = kNoReg;
    VReg a = kNoReg, b = kNoReg, c = kNoReg;

    int64_t imm = 0;
    double fimm = 0.0;
    Operand idx;          // 带 tag 的索引（含义见 Operand::Kind）
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
    // 直接写进调用方缓冲区（注释热路径复用同一块缓冲，避免临时串）。
    void text_into(std::string& buf) const;
};

// 单条指令是否是终止指令（块必须以它结尾，且只能有一条）。
//
// 与 Block::terminated() 共用同一份口径 —— 校验器不能有第二套判断，
// 否则「lowering 认为是终止、校验器认为不是」会互相打架。
inline bool is_terminator(const Inst& in) {
    const IrOp op = in.op;
    if (op == IrOp::Jump || op == IrOp::Branch || op == IrOp::Return ||
        op == IrOp::GenYield || op == IrOp::GenAwait ||
        op == IrOp::GenRethrow || op == IrOp::GenDone)
        return true;
    // **不返回的运行时调用也算终止。**
    //
    // 漏掉这一类会让后续的收尾逻辑往它们后面追加指令 —— 那些指令永远
    // 执行不到，但**会被当成活代码保留下来**，其中的清理动作（比如
    // ExcDone）就静默丢失了。
    //
    // 实测 32_exception_value：`except ...: raise` 的裸 raise 生成
    // pa_reraise（_Noreturn），但 terminated() 不认识它，于是 lower_try
    // 又在后面补了 pa_exc_done + jump。ExcDone 永远执行不到，
    // pa_exc_accept_value 压的 PyAdamasExcSave 泄漏 16 字节。
    if (op == IrOp::CallRt) {
        switch (static_cast<RtFn>(in.imm)) {
            case RtFn::ExcReraise:
            case RtFn::Raise:
            case RtFn::RaiseValue:
            case RtFn::RaiseValueFrom:
            case RtFn::RaiseInstance:
            case RtFn::Reraise:
            case RtFn::EhRaise:
            case RtFn::EhRaiseValueFrom:
            case RtFn::EhReraise:
            case RtFn::EhRethrow:
                return true;
            default:
                return false;
        }
    }
    return false;
}

struct Block {
    // 稳定标识：与 Inst::id 同思路 —— 块在 vector 里的下标会因
    // insert/erase 改变，跨 pass 保存的引用必须用 id。
    // 0 = 未分配。
    uint32_t id = 0;
    std::string name;
    std::vector<Inst> insts;

    bool terminated() const {
        return !insts.empty() && is_terminator(insts.back());
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
    // P4 块3（lang_gaps，0.346）：traceback 显示名。门控开时 lowerer 填
    // （入口 <module>、方法去掉类前缀的裸方法名）；空 = 沿用 codegen 的
    // debug_display_name(name) 旧口径——门控关时恒空，asm 逐字节不变。
    std::string debug_name;
    std::vector<IRParam> params;
    TypePtr ret;
    // 多继承 adjustor thunk（0.509）：self 已由 thunk 自己调整过，直调点不要再
    // 加一次 delta（否则双重偏移）。
    bool mi_thunk = false;
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
    // 每个 vreg 是否可证非负（降级期按值来源打标：非负常量 / len /
    // 非负槽的 load / 非负操作数的 and/shr/mod 等）。常数除数的除法/
    // 取模据此走无符号魔数路径。优化遍新建的 vreg 不打标（越界读按
    // false 处理），保守正确。
    std::vector<uint8_t> vreg_nonneg;
    std::vector<Block> blocks;

    // ── 稳定 ID 与 use-def 索引 ────────────────────────────────────
    // 指令 id 的分配器。只增不减（删指令不回收 id），保证任何时刻
    // 拿到的 id 都还能在 inst_by_id 里查到是否已删。
    uint32_t next_inst_id = 1;
    // 块 id 的分配器，同 inst_id 语义。
    uint32_t next_block_id = 1;
    // 块 id -> 当前块下标。rebuild_use_def() 时一并重建。
    std::unordered_map<uint32_t, int> block_by_id;

    // 给一个块分配 id（若尚未分配）。
    uint32_t ensure_block_id(Block& b) {
        if (b.id == 0) b.id = next_block_id++;
        return b.id;
    }
    // 按 id 找当前下标；找不到返回 -1（块已被删）。
    int block_index(uint32_t id) const {
        auto it = block_by_id.find(id);
        return it == block_by_id.end() ? -1 : it->second;
    }
    // 重新扫描整个函数，重建 block_by_id。
    // 在块结构改动后调用（内联、循环版本化等），代价 O(块数)。
    //
    // 历史：0.205 起这里还重建 inst_by_id / def_of / uses_of 三张
    // vreg/指令级 unordered_map。0.3xx 的 DCE 改成自己线性计数后
    // （见 CHANGELOG 0.335 优化 3），这三张表已经**没有任何读者**，
    // 但重建仍是「逐指令哈希插入」——perf 显示 _M_insert_unique_node +
    // emplace_uniq + 配套 malloc/free 合计 ~20% 编译时间（大函数上）。
    // 编译提速时把死索引去掉：唯一仍被读的 block_by_id 继续重建。
    // 若将来要恢复指令级索引，应改成访问时惰性重建的显式入口。
    void rebuild_use_def();
    // 给一条指令分配 id（若尚未分配）。
    uint32_t ensure_inst_id(Inst& in) {
        if (in.id == 0) in.id = next_inst_id++;
        return in.id;
    }
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
    // 零开销异常的 EH 区域（可多条：一份 LSDA 含多条 call-site 记录）；
    // 为空表示本函数走 setjmp 路径。
    // 注意：codegen 与「不可达块裁剪」一律以 EhBegin/EhEnd/EhLandingPad
    // 三条标记指令为准（内联等 pass 会重建块表，这里的块号会过期，仅作
    // 记录/诊断用，不要拿它当索引）。
    struct EhRegion {
        // 用**块 id**而不是块号：内联、循环版本化等 pass 会重建块表，
        // 块号随之过期（这正是 0.203 边界版本化失败的根因）。
        // codegen 仍以 EhBegin/EhEnd/EhLandingPad 标记指令为准，这里
        // 只作诊断记录 —— 但记录本身也不该是过期数据。
        uint32_t begin_block = 0;
        uint32_t end_block = 0;
        uint32_t lp_block = 0;
    };
    std::vector<EhRegion> eh_regions;

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
    // dunder 槽号（MRO 上找到实现才算，-1 = 没有）。codegen 把它们写进一张
    // 常量表，vtable 元数据第 7 个字指向该表（[eq, lt, hash, -1]）；unknown
    // 域据此按「实例 → vtable → 槽」分派 __eq__/__lt__/__hash__
    // （typed 域仍走编译期槽号的直接 VCall，不受影响）。
    int eq_slot = -1;
    int lt_slot = -1;
    int hash_slot = -1;
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
    // 同一个 8 字节 chunk 里既有浮点又有整数字段：SysV 要求拆成一个 SSE
    // 寄存器 + 一个整数寄存器，当前按 chunk 的分类表达不了，按值传递时报错。
    uint8_t mixed = 0;
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
    // 免写直调的绑定方式：0 = 链接期符号（cimport，call symbol）；
    // 1 = dlsym(RTLD_DEFAULT)（from c import，首次调用解析并缓存）；
    // 2 = dlsym(句柄)（dylib 句柄属性调用，句柄在 Inst::a，每次解析）。
    int lazy = 0;
    // lazy != 0 时指向 IRModule::c_lazy_syms 的符号名字符串下标。
    int sym_name = -1;
};

// .cpya 的 extern 变量：只记录符号与加载宽度。
struct IRExternVar {
    std::string symbol;
    CKind kind = CKind::Int;
};

// 字符串字面量：kind=0 是 str（pub=码点数），kind=1 是 bytes（pub=字节数）。
// 必须按 (kind, 内容) 去重——"中" 与 b"\xe4\xb8\xad" 字节相同但 pub 不同，
// 共用一个静态对象必然有一方错。
struct IRStringLit {
    std::string text;
    bool is_bytes = false;
    int64_t pub = 0;   // str: 码点数；bytes: 字节数
};

struct IRModule {
    // 用 unique_ptr 持有：降级过程中会不断新增函数（嵌套 def），
    // vector<IRFunction> 扩容会让外面持有的指针失效。
    std::vector<std::unique_ptr<IRFunction>> funcs;
    std::vector<IRGlobal> globals;
    std::vector<IRStringLit> strings;
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
    // 免写直调的符号名表：IRExtern::sym_name 指向这里，codegen 为每个
    // 名字发射 .Lcsymstr<N>（asciz）与 .Lcsym<N>（解析缓存槽）。
    std::vector<std::string> c_lazy_syms;
    int entry = -1;  // 模块顶层代码编译成的函数下标

    IRFunction* add_func(std::string name) {
        auto f = std::make_unique<IRFunction>();
        f->name = std::move(name);
        funcs.push_back(std::move(f));
        return funcs.back().get();
    }

    // 数码点：非续字节（(b & 0xC0) != 0x80）的个数。与运行时的
    // str_count_cp 同一口径（对非法字节也安全）。
    static int64_t count_cp(const std::string& s) {
        int64_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    // P3 big 字面量：十进制串（可带负号），按内容去重。
    std::vector<std::string> bigs;
    int intern_big(const std::string& s) {
        for (size_t i = 0; i < bigs.size(); ++i)
            if (bigs[i] == s) return static_cast<int>(i);
        bigs.push_back(s);
        return static_cast<int>(bigs.size()) - 1;
    }

    int intern_string(const std::string& s) { return intern_lit(s, false); }

    int intern_bytes(const std::string& s) { return intern_lit(s, true); }

    int intern_lit(const std::string& s, bool is_bytes) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i].is_bytes == is_bytes && strings[i].text == s)
                return static_cast<int>(i);
        IRStringLit lit;
        lit.text = s;
        lit.is_bytes = is_bytes;
        lit.pub = is_bytes ? static_cast<int64_t>(s.size()) : count_cp(s);
        strings.push_back(std::move(lit));
        return static_cast<int>(strings.size()) - 1;
    }
};

// ── 类型到机器类的映射 ────────────────────────────────────────────

inline bool ir_is_float(const TypePtr& t) { return t && t->kind == TyKind::Float; }

/* ── 闭包的统一调用约定（间接调用） ─────────────────────────────
   函数值（闭包）在间接调用点的实参类型常常是 unknown，调用方无法按真实
   签名装填寄存器；而被调函数是按自己的签名编的 ABI。两边一旦对不上，
   浮点实参/返回值就会被当成整数位模式传递（静默算错）。

   办法：凡是签名里含浮点的函数，额外生成一个 shim 入口 —— shim 用「全部
   实参都是原始 64 位整数寄存器 + 返回值放 %rax」的统一约定接收，再按真实
   签名重排到 %rdi/%xmm 等位置。闭包 +0 指向 shim，于是所有间接调用都走
   统一约定，无需运行期分派。签名不含浮点的函数不需要 shim（统一约定与
   它自身的 ABI 恰好一致）。

   限制：只处理「参数个数 <= 6 且不含栈上实参」的签名；超出时退回真实入口
   （与改动前行为相同，不会更差）。生成器/协程的入口签名不同，一律不 shim。 */
inline int closure_shim_nparams(const IRFunction& f) {
    return static_cast<int>(f.params.size());
}
inline bool closure_needs_shim(const IRFunction& f) {
    // 统一约定对**所有**间接调用的目标都成立：调用点不知道被调签名，
    // 一律按「第 i 个实参进第 i 个整型寄存器、返回值在 %rax」装填与取值。
    // 所以凡是可能被当作函数值使用的函数都需要 shim —— 不能只给「签名含
    // 浮点」的做：实参类型由调用点决定，而调用点可能把浮点传给一个签名
    // 里没有浮点的函数（例如 float 经 unknown 传给 int 函数）。
    //
    // 生成器/协程的入口签名不同（参数由创建包装写进堆帧），不 shim。
    //
    // 参数超过 6 个的签名**也要** shim。原先这里直接 return false 退回
    // 「不生成 shim、让真实 ABI 直接生效」，但那是错的：统一约定把第 i 个
    // 实参一律放进第 i 个**整型**寄存器，而真实 SysV ABI 有 8 个浮点寄存器，
    // 两者在「参数 > 6 且含浮点」时不一致 —— 实测 10 个 float 参数的函数在
    // -O0/-O1 下第 9、10 个参数丢失（输出 8.8 而非 59.6），-O2 因为内联了
    // 函数才碰巧正确。
    //
    // 现在 shim 会处理栈上实参：把统一约定的溢出区读回来，按真实签名重排
    // 到寄存器 + 栈的正确位置。
    if (f.is_generator) return false;
    return true;
}

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
        case TyKind::StatResult:
        case TyKind::StructTime:
        case TyKind::Popen:
        case TyKind::Regex:
        case TyKind::Match:
        case TyKind::DateTime:
        case TyKind::TimeDelta:
        case TyKind::Path:
        case TyKind::Deque:
        case TyKind::Struct:
        case TyKind::VThread:
        case TyKind::Chan:
        case TyKind::Big:   // P3：BigInt 堆对象（RC 管理，BIG tag=12）
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
        case TyKind::StatResult:
        case TyKind::StructTime:
        case TyKind::Popen:
        case TyKind::Regex:
        case TyKind::Match:
        case TyKind::DateTime:
        case TyKind::TimeDelta:
        case TyKind::Path:
        case TyKind::Deque:
        case TyKind::VThread:
        case TyKind::Chan:
        case TyKind::Big:   // P3：按地址传递（BigInt 堆指针）
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
bool ir_module_can_skip_index_maint(const IRModule& m);
// 模块是否用到 from c import（dlsym RTLD_DEFAULT，lazy==1）：符号名是运行期
// 字符串，链接期静态可达性无法覆盖，整程序须保守关闭 --gc-sections。
bool ir_module_uses_c_sym_default(const IRModule& m);

}  // namespace pyadamas
