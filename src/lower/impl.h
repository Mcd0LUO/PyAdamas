// lower/impl.h —— Lowerer 的实现细节（只给 lower_*.cpp 用）
//
// 降级器按职责拆成多个实现文件：驱动/预扫描、语句、表达式、调用、
// 生成器与协程。这里放共享的 FuncCtx/ClosureMeta 和 Lowerer 声明。
#pragma once

#include <algorithm>
#include <cstdlib>
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

#include "lower/lower.h"
#include "lower/func_ctx.h"
#include "lower/ops_map.h"
#include "ir/module.h"

namespace pyadamas {

// ────────────────────────── 小工具 ──────────────────────────

// 只认字面量常量。默认参数、assert 消息这些地方够用；
// 更复杂的常量表达式留给后续的常量折叠。


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
    // 0.580：模块级字面量常量（name → 值），用于折叠 Name/Attribute 默认值
    std::unordered_map<std::string, ConstVal> module_consts_;

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
    // `x: set[str] = set()` / `d: dict[str, int] = dict()`：构造调用自己
    // 推不出元素类型（实参为空），AnnAssign 把标注下的容器类型挂在这里，
    // 由 lower_builtin_container 取用。不传下去的话运行时拿到的是 unknown
    // 种类：str 键按指针比较（动态串查不回来/不去重），元素引用还会双释放（N17）。
    TypePtr pending_ctor_ty_;

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

    void err(int line, int col, std::string msg);

    void unsupported(const Stmt& s, const char* what);

    // ── 块与值 ──────────────────────────────────────────────────

    Block& cur() { return st_->fn->blocks[st_->blk]; }

    int new_block(std::string name);

    void switch_to(int b) { st_->blk = b; }

    // noreturn 调用之后把当前块收尾，避免后端把后续块当成顺序落下。
    void end_unreachable();
    // 从 (type, message) 临时槽重新抛出（RaiseOwn + store_slot_raw 清槽）。
        // 循环体有副作用时，把 cond 块里缓存的 `i < len(seq)` 改成每轮重读 len。
    // 返回是否打了补丁（stmt_for 与 comp 共用）。
    bool patch_cond_reload_len(int cond, size_t cond_cmp_pos, VReg seq,
                                const TypePtr& seq_ty, int seq_slot);
void emit_rethrow_from_slots(int ts, int msgs);

    VReg alloc(TypePtr ty);

    // 非负性判定与值来源规则（定义在 lower/idiom.cpp）。
    bool nn(VReg v) const;
    uint8_t nonneg_of(const Inst& in);
    // 非负提示窗口（定义在 lower/idiom.cpp）：入口擦目标槽旧提示、
    // 按条件打标；退出擦本层键。嵌套同名循环不得恢复外层提示。
    bool enter_nonneg_window(For& f, VReg start, int ctr, bool dyn_step, int64_t step,
                             int& target_slot);
    void exit_nonneg_window(bool nn_hint, int ctr, int target_slot);

    VReg def(TypePtr ty, Inst in);

    void emit(Inst in);

    bool terminated() const { return st_->fn->blocks[st_->blk].terminated(); }

    /* 按静态类型算 ListAppend 的 heap 参数：0 非堆 / 1 堆 / 2 unknown。 */
    VReg heap_arg(TypePtr t);

    VReg const_int(int64_t v);

    // 带静态类型的整数常量：定宽标注的字面量（类型检查已验值域）与
    // 无符号 64 位常量必须带自己的类型，否则 codegen 按 int 生成、
    // 后续正常化/无符号判定都会看不见目标宽度。
    VReg const_int_ty(int64_t v, TypePtr ty);

    VReg const_bool(bool v);

    VReg const_float(double v);

    VReg const_none();

    VReg const_str(const std::string& s);

    // bytes 字面量：与同内容的 str 字面量共用字节但 pub 不同，必须单独 intern。
    VReg const_bytes(const std::string& s);

    // P3：big 字面量 → 永生静态 BigInt 对象（与 str 字面量同口径：
    // 借用、不入临时栈；存储时 rc_inc 对永生哨兵是空操作）。
    VReg const_big(const std::string& s);

    VReg const_literal(const LiteralValue& v);

    int new_temp_slot(TypePtr ty) { return st_->fn->new_slot(std::move(ty)); }

    int new_borrow_slot(TypePtr ty);

    void store_borrow_slot(int slot, VReg v) { store_slot_raw(slot, v); }

    VReg load_slot(int slot, TypePtr ty);

    VReg load_global(int gi, TypePtr ty);

    void store_slot_raw(int slot, VReg v);

    void store_global_raw(int gi, VReg v);

    // 引用计数成员声明移到 lower/impl_rc.h（纯移动，原位展开）。
    #include "lower/impl_rc.h"
#include "lower/ops_map.h"

    // ── 名字解析 ────────────────────────────────────────────────

    int global_idx(const std::string& name);

    int global_idx_as(const std::string& name, TypePtr ty);

    // ── 闭包 ────────────────────────────────────────────────────

    const ClosureMeta* closure() const { return st_ ? st_->closure : nullptr; }

    bool is_cell(const std::string& name) const;

    int comp_slot(const std::string& name) const;

    bool is_env(const std::string& name) const;

    TypePtr captured_ty(const std::string& name) const;

    VReg load_cell(const std::string& name);

    // cell 是单元素 list[T]；读出来是借用引用，写进去由 list_set 管旧值。
    VReg read_cell(const std::string& name);

    VReg read_env(const std::string& name);

    VReg cell_for_capture(const std::string& name);

    // 写回捕获变量：先按 cell 的元素类别 retain，list_set 负责 dec 旧值
    //（kind 1）；kind 2 只 retain 不 release，宁可泄漏。
    void write_cell(const std::string& name, VReg v, TypePtr vty);

    VReg zero_value(TypePtr T);

    // 被内层函数捕获的局部变量在入口转成单元素 list[T]（cell）。
    // 形参先用原始槽接实参，再把值 retain 进 cell。
    void create_cells();

    void bind_name_value(const std::string& name, VReg v, TypePtr vty);

    TypePtr name_ty(const std::string& name);

    bool is_local(const std::string& name) const;

    // 一个名字的存储类型：cell/env 看捕获类型，局部看槽，模块级看全局表。
    TypePtr storage_ty(const std::string& name);

    // 当前作用域里有没有这个名字（局部槽、捕获变量或 global 声明）。
    bool is_bound_here(const std::string& name) const;

    // 内置名可以被用户遮蔽：模块里定义了 print，它就不再是内置。
    bool is_builtin(const std::string& name) const;

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
    void bind_iter_pair(Expr& target, VReg a, TypePtr aty, VReg b, TypePtr bty);
    // for i, v in enumerate(xs) / for a, b in zip(xs, ys)：直接计数循环，
    // 不物化 [下标, 值] 的元组表。
    void lower_for_enumerate(For& f, Call& c);
    void lower_for_zip(For& f, Call& c);
    void lower_for_file(For& f);
    void lower_aug_assign(AugAssign& a);
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
    // st_->zero_cost_tries 集合（可多条）。规则见 lower/exc.cpp 的实现注释。
    void collect_zero_cost_tries(FunctionDef& f);
    // 零开销 try 的降级：发 EhBegin/EhEnd 保护区 + EhLandingPad；landing pad
    // 取回异常后直接跳进现有的 except 分发块（ExcMatches/ExcAcceptValue/
    // ExcDone 一行不改）。
    void lower_try_zero_cost(Try& t);
    // 零开销 try 体里的 raise：把老 RtFn 映射到 _Unwind 版本；其余地方原样
    // 返回（老路径一个字都不变）。
    RtFn raise_rt(RtFn f) const;
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
    // 多继承 adjustor thunk（0.509）：self += delta 后直调真实实现，拿自己的
    // fidx 填进子类 vtable 对应槽（定义在 lower/mi_thunk.cpp）。
    int build_mi_thunk(int target_fidx, int delta_bytes);
    void install_mi_thunks(const ClassInfoPtr& ci, int cid);

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
    int new_str_cursor();

    // 取 str 的第 idx 个码点（1 码点串）。带游标时顺序访问是 O(1)；
    // 非顺序访问回退到从头扫，与 pa_str_char_at 一致。
    VReg str_char_at(VReg seq, VReg idx, int cur_slot);
    // 生成器 resume 重进时 C 栈帧是新的：把跨挂起仍然活跃的 vreg
    // 在挂起前溢写到堆帧槽，恢复后再装回来（见 lower/gen.cpp）。
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
    bool json_desc(const TypePtr& t, std::string& out, int line, int col,
                   bool for_loads = false);
    // json.stringify 用：静态类型编码成描述符，unknown（以及运行期才能定型的
    // 位置）编码成 'u'，运行时按值的实际种类判型。永不失败。
    bool json_desc_dyn(const TypePtr& t, std::string& out, int line, int col);
    // CPython 桥（from python import）：参数类型描述符 + 调用降级。
    bool cpy_desc(const TypePtr& t, std::string& out, int line, int col);
    VReg lower_cpy_call(const std::string& proxy_name, const std::string& attr, Call& c);

    VReg lower_construct(Call& c, const ClassInfoPtr& cls);
    // A8-4：enum 按值反查成员（本模块 + 导入类共用）。
    VReg lower_enum_by_value(Call& c, const ClassInfo* cls);

    // 按签名重排实参：位置、关键字、默认值、数值转换。
    std::vector<VReg> bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn);

    // 容器元素跨数值域（list[int] ↔ list[float]）需要逐元素转换，
    // 现阶段一律报错，避免按错误的机器表示读槽位。
    bool bad_elem(const TypePtr& from, const TypePtr& to);

    // extern 变量：登记到模块表并返回下标。同一个 C 符号只登记一次。
    int extern_var_idx(const CVar& v);

    // extern 变量在 PyAdamas 侧的类型；ptr[Struct] 保留结构体指针身份。
    TypePtr cvar_type(const CVar& v);

    // C 结构体字段的地址（数组字段、取元素地址时用）：obj + off 的整数地址。
    VReg field_addr(VReg obj, int64_t off);

    void field_store(VReg obj, int off, VReg v, uint8_t width = 8, bool unsig = false);

    VReg field_load(VReg obj, int off, TypePtr ty, uint8_t width = 8, bool unsig = false);

    // 值类型 struct：分配一块 payload（带 RC 头，但头在 payload 之前，
    // 交给 C 的地址就是 payload 地址），以及按值复制一份。
    VReg struct_new(StructInfo* s);

    VReg struct_copy(VReg v, StructInfo* s);

    // struct 的特殊形式与构造。
    VReg lower_struct_builtin(const std::string& name, Call& c);
    VReg lower_struct_construct(Call& c, StructInfo* s);

    // Optional[T] 上的属性访问必须先确认不是 None。类型收窄做不到
    // 逐条路径成立，这里用一次运行时检查兜底：解引用空指针的段错误
    // 不带任何上下文，比一条 panic 难查得多。
    VReg guard_not_none(VReg v, const std::string& what);

    // 剥掉 Optional 外壳，返回内层类型；不是 Optional 就原样返回。
    static TypePtr strip_optional(const TypePtr& t);

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
    // A8-3：类属性声明者那份的存储槽（导入类是定义模块的外部全局）。
    int class_attr_decl_slot(const ClassAttrInfo* ca);
    ClassAttrStorage class_attr_storage(const ClassInfo* recv, const ClassAttrInfo* ca);
    int class_attr_slot(const ClassInfo* recv, const ClassAttrInfo* ca);
    VReg class_attr_load(const ClassInfo* recv, const ClassAttrInfo* ca);
    // enum 成员表物化成 list（list(Color) / for c in Color）。
    VReg enum_member_list(const ClassInfo* cls);
    VReg class_attr_load_dyn(VReg obj, const ClassInfo* stat, const ClassAttrInfo* ca);
    void class_attr_store(const ClassInfo* recv, const ClassAttrInfo* ca, VReg v, TypePtr vty);
    // 接收者是类本身（`C.x` / @classmethod 的 `cls.x`）而不是实例
    bool is_class_receiver(const Attribute& a);
    const ClassInfo* class_receiver_cls(const Attribute& a, TypePtr ot);

    // ── 表达式 ──────────────────────────────────────────────────

    VReg expr(Expr& e);

    VReg lower_constant(Constant& c);
    VReg lower_binop(BinOp& b);
    VReg lower_big_binary(VReg l, TypePtr lt, VReg r, TypePtr rt, Tok op, int line, int col);
    VReg lower_any_big_binary(VReg l, VReg r, Tok op);
    VReg lower_percent_format(BinOp& b, TypePtr lt, TypePtr ty);
    VReg lower_unary(UnaryOp& u);
    VReg lower_boolop(BoolOp& b);
    VReg lower_compare(Compare& c);
    // 单步比较的公共降级（P3：链式比较也按操作数静态类型分派）。
    VReg lower_cmp_single(VReg l, TypePtr lt, VReg r, TypePtr rt, CmpOp op,
                          int line, int col);
    // 容器相等：list/tuple 逐元素比，dict 按键值对（顺序无关），set 按键。
    // 元素类型在编译期已知，所以比较方式也能定下来；不适用时返回 kNoReg。
    VReg lower_container_eq(VReg l, TypePtr lt, VReg r, TypePtr rt);
    // 容器元素的比较链：每个字符说明该层元素怎么比（i/s/f/x/l/d/t）。
    void append_elem_chain(TypePtr container, std::string& chain);
    // 排序描述符（0.312）：自定界前缀树，定长异构元组逐位置比。
    // i/f/s/x 标量；l+子描述符 list/变长元组；t+arity 字节+逐位置描述符。
    // 超出深度/长度预算返回 false，调用方整体退机器字。
    bool append_sort_desc(TypePtr t, std::string& d, int depth);
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
    // B1-min（0.535）：绑定方法的接收者类型判据。
    static bool bm_receiver_ok(const TypePtr& t);
    // 按接收者类型拆开的方法调用降级（lower_method_*.cpp）。
    VReg lower_method_native(Attribute& at, Call& c, VReg recv, TypePtr recv_ty, bool opt);
    // vthread 先验（lower/vthread.cpp）
    VReg lower_go(Expr& e);
    VReg lower_builtin_vthread(const std::string& name, Call& c);
    VReg lower_method_chan(Attribute& at, Call& c, VReg recv, TypePtr recv_ty);
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
    TypePtr vreg_ty(VReg v) const;

    VReg call_runtime(RtFn f, TypePtr ret, std::vector<VReg> args);

    // 返回 void 的运行时函数不分配结果寄存器，省掉一条无用的 movq
    void call_void(RtFn f, std::vector<VReg> args);

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

    void push_frame(IRFunction* fn, const Scope* scope);

    void pop_frame();
};


}  // namespace pyadamas
