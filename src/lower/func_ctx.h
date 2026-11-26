#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/ir.h"
#include "lower/ops_map.h"

namespace pyadamas {

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
    // B1-min（0.535）：`push = xs.append` 这类**局部变量**绑定方法。只记
    // 编译期的「接收者表达式 + 方法名」，调用点还原成一次普通方法调用 ——
    // 不造运行期对象，所以存进容器/返回/传参仍然按老规矩报错。
    struct BmBinding {
        Attribute* at = nullptr;
        std::string recv;   // 接收者名字（不是 Name 时为空 → 只按版本号 0 处理）
        int version = 0;
    };
    std::unordered_map<std::string, BmBinding> bm_names;
    // 名字 / 字段的写版本号：绑定点记一份，调用点比对（B1-min 健全性）。
    std::unordered_map<std::string, int> name_version;
    std::unordered_map<std::string, int> field_version;
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
    // 类体求值表（分层）：类体/函数体各一层，避免同名嵌套串帧。
    std::vector<std::unordered_map<std::string, VReg>> class_body_vals;
    // 有类体求值结果的字段：ctor 直接用这份值，不再重算表达式
    std::unordered_set<std::string> class_body_globals;
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
        enum class Kind { With, AsyncWith, Finally, Handler, ExcDone, FileClose, LockRelease, TimeoutExit, TaskGroupCancel, SemRelease, ZeroCostEh, FinallyBody } kind;
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
    // 早退路径（return/break/continue 穿过零开销 try）内联的 finally 体所在块。
    // 这些块的 raise 必须物理上不在本函数 EH 区间内（LSDA 覆盖是代码位置），
    // 所以不原地发射，而是先跳到独立块、函数降级结束后统一移到函数末尾。
    std::vector<int> early_fin_blocks;
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
    // 显式 object/Any 标注的槽（局部 sym.annotated / 形参 ann_unknown）：
    // 禁止 store_slot 的 unknown→typed 升级。标注就是「故意留在 unknown
    // 域」，升级成 int 后后续 unknown 写入既不走 RC、装载又按 int 读回
    // （函数内累加器野指针形态，0.327 基线已存在）。
    std::unordered_set<int> ann_unknown_slots;
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

}  // namespace pyadamas
