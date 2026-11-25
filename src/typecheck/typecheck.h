#pragma once
// typecheck/typecheck.h —— 静态类型检查
//
// 设计原则：
//   1. 类型标注是约束，不是语法必需。没有标注时按使用推断；
//      推断不出来就退化成 Unknown，任何涉及 Unknown 的检查都放行。
//      —— 宁可漏报，不可误杀。歧义是静态检查最大的敌人。
//   2. 变量类型一旦确定就不可改变（这是与 CPython 的语义分歧点，
//      也是拿到性能的前提：每个变量在栈上有固定槽位和固定宽度）。
//   3. 遇到尚未建模或无法静态表达的构造（少数 async 边角、动态属性等）
//      不报错，只记入 unsupported 列表，并尽量把子树也检查一遍。
//      这样 check 命令对真实代码仍然可用。
#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <utility>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/ast.h"
#include "cimport/cimport.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include "typecheck/types.h"

namespace pyadamas {

// ════════════════════════════════════════════════════════════════════
//  诊断
// ════════════════════════════════════════════════════════════════════

struct Diag {
    int line = 1;
    int col = 1;
    std::string msg;
};

// ════════════════════════════════════════════════════════════════════
//  检查器
// ════════════════════════════════════════════════════════════════════

struct Symbol {
    TypePtr ty;
    int line = 1;
    bool annotated = false;  // 由标注确定，不允许再改类型
    bool is_global = false;
    // 名字由 def 绑定。函数名在 Python 里是可随时改绑的普通变量
    // （types.py 的 `_c = _c()` 就是拿调用结果覆盖函数名），
    // 所以它不像标注那样锁死类型。
    bool from_def = false;
    // class 语句绑定的名字：函数内定义的类也靠它和「类实例变量」区分。
    bool is_class_name = false;
    // 槽已退化成动态表示（见 stmt_for）：同一个名字被不同类型元素的 for
    // 先后绑定过，槽不能再收窄回某个具体类型 —— 前面循环的存储点会按新
    // 类型逐元素转换，而读取点仍按原类型解释（实测把 int 位模式当 double
    // 读）。置位后 declare 不再用后续赋值给槽定型。
    bool dynamic = false;
};

// from 内置模块 import 的函数：保留原始签名，调用点据此走模块专用规则。
struct NativeAlias {
    std::string module;
    std::string member;
    TypePtr sig;
};

struct Scope {
    std::unordered_map<std::string, Symbol> vars;
    // import m 绑定的模块别名；只用于属性/from 解析，不是运行时值。
    std::unordered_map<std::string, ModuleInfoPtr> modules;
    Scope* parent = nullptr;
    // 函数作用域上累积的「标注过的名字 → (类型, 首次声明的行)」。块作用域
    // 是临时的，光靠 vars 查不到已经结束的块里声明过什么；而 Python 的名字
    // 是函数作用域，块里的 `c: str` 与函数级的 `c: big` 指的是同一个槽。
    std::unordered_map<std::string, std::pair<TypePtr, int>> ann_seen;
    bool is_module = false;
    // 这个作用域属于哪个函数；模块 / 推导式 / 类作用域为 nullptr。
    // 闭包捕获靠它判断「名字是不是从外层函数来的」。
    const FunctionDef* fn_owner = nullptr;
};

// functools.partial 的绑定结果。sig 是返回的函数签名；bound_params 按
// 实参求值顺序记录被绑定形参的下标（pos 在前，kwonly 从 pos.size() 起），
// remaining 是仍暴露给调用方的原始形参下标，顺序与 sig 的形参一致。
struct PartialPlan {
    FuncInfoPtr sig;
    std::vector<int> bound_params;
    std::vector<int> remaining;
};


class TypeChecker {
public:
    explicit TypeChecker(std::string file) : file_(std::move(file)) {}
    // 析构时把本实例拥有的 ClassInfo/StructInfo 标记为失效。Type 里的
    // cls/st 是裸指针，一旦 TypeChecker 先于 Type 销毁，后续访问就是
    // use-after-free。alive 标志让这类误用在调试/ASan 构建里立刻暴露，
    // 而不是变成幽灵读取。（生产构建不读 alive，零开销。）
    ~TypeChecker();
    // 声明析构会抑制隐式移动构造/赋值；TypeChecker 里有 unique_ptr 容器
    // （pool_/all_structs_），必须显式恢复移动语义。
    TypeChecker(TypeChecker&&) = default;
    TypeChecker& operator=(TypeChecker&&) = default;

    void check(Module& m);
    // 协议 dunder 的返回类型校验（在函数体推理完之后跑，见 typecheck/func.cpp）。
    void check_dunder_returns();
    // P4 块4（lang_gaps，0.346）：@dataclass 编译期展开——类体字段收集完后
    // 合成 __init__/__repr__/__eq__ 的 FunctionDef AST 注入类体，复用既有
    // 方法收集/槽分配/降级。门控关不调用；v1 范围与负面见
    // docs/lang-gaps-landing.md §8 块4。
    void expand_dataclass(ClassDef& c, ClassInfoPtr& cls);

    // 多模块编译时由驱动注入：按模块名取已经加载的接口。
    // check 模式：import 图解析失败/属性缺失按 unknown 处理，不判死。
    void set_lenient_imports(bool v) { lenient_imports_ = v; }
    // 模块级未定义名字：check 模式宽松（标准库基线需要），build/run 模式
    // 必须严格。否则变量名打错会被降级成一个初值为 0 的全局，运行期
    // 静默算出 0 而不是抛 NameError —— 这是最难发现的静默错误。
    void set_strict_module_names(bool v) { strict_module_names_ = v; }
    // P4 语言缺口开关（PYADAMAS_LANG_GAPS）：内建异常名在值位置得到
    // ClassObj[Exception] 类型（运行期类对象），而不是退化成 unknown/0。
    void set_lang_gaps(bool v) { lang_gaps_ = v; lang_gaps_enabled() = v; }
    bool lang_gaps() const { return lang_gaps_; }
    bool strict_module_names() const { return strict_module_names_; }
    bool lenient_imports() const { return lenient_imports_; }
    void set_module_lookup(ModuleLookup lookup) { module_lookup_ = std::move(lookup); }

    // cimport 的 .cpya 查找路径（驱动注入）。
    void set_cinclude_dirs(std::vector<std::string> dirs) {
        cinclude_dirs_ = std::move(dirs);
    }

    const CImportDecl* cmodule(const std::string& bind) const {
        auto it = cmodules_.find(bind);
        return it == cmodules_.end() ? nullptr : it->second.get();
    }

    const CExtern* extern_of(const FuncInfo* fn) const {
        auto it = extern_by_fn_.find(fn);
        return it == extern_by_fn_.end() ? nullptr : it->second;
    }

    const std::vector<Diag>& diags() const { return diags_; }

    // 名字是否是「内置可调用」。降级阶段用它把「能解析但没实现」的内置
    // 变成明确诊断，而不是退化成调用空指针。
    static bool is_builtin_name(const std::string& n) { return is_builtin_callable(n); }
    // 内置类型名：值位置是类型对象（type_name_value），标注位置按类型解析。
    static bool is_type_name(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "int", "float", "str", "bool", "list", "dict", "tuple", "set",
            "bytes", "bytearray", "object", "type", "frozenset", "complex",
        };
        return names.count(n) > 0;
    }

    // 表达式推断出的类型。IR 降级阶段要用它决定槽宽和指令选择，
    // 不能靠 ast 重新推一遍——两套推断迟早会分叉。
    // 降级层判断「联合里哪个成员能接住这个方法」用（0.529）。
    static bool type_has_method(const TypePtr& t, const std::string& m) { return has_method(t, m); }

    TypePtr type_of(const Expr* e) const {
        auto it = expr_types_.find(e);
        return it == expr_types_.end() ? t_unknown() : it->second;
    }

    // 模块作用域与各函数体的最终符号表。lower 直接查它拿「这个名字的
    // 最终类型」，避免自己再实现一遍 join/收窄。
    const Scope* module_scope() const { return module_; }
    const Scope* scope_of(const Stmt* fn) const {
        auto it = func_scopes_.find(fn);
        return it == func_scopes_.end() ? nullptr : it->second;
    }

    // 函数签名（含返回类型）。lower 需要它来判断默认参数、重排关键字参数。
    FuncInfoPtr func_info(const Stmt* fn) const {
        auto it = func_infos_.find(fn);
        return it == func_infos_.end() ? nullptr : it->second;
    }

    // 签名对应的源定义（含脱糖后的合成 lambda）。lower 用它看函数体形态
    // （如 sorted(key=恒等函数) 的键域推断），没有定义时返回 nullptr。
    const FunctionDef* signature_def(const FuncInfo* fn) const {
        auto it = signature_defs_.find(fn);
        return it == signature_defs_.end() ? nullptr : it->second;
    }

    // functools.partial 的绑定计划；lower 用它生成转发函数。
    std::shared_ptr<PartialPlan> partial_plan(const Call* c) const {
        auto it = partial_plans_.find(c);
        return it == partial_plans_.end() ? nullptr : it->second;
    }

    // 类表：名字 → 编译期布局。lower 靠它把属性访问变成固定偏移。
    const std::unordered_map<std::string, ClassInfoPtr>& classes() const { return classes_; }

    // 值类型 struct 表：名字 → 编译期布局（字段偏移/大小/总大小）。
    StructInfo* struct_info(const std::string& n) const {
        auto it = structs_.find(n);
        return it == structs_.end() ? nullptr : it->second;
    }

    // match 捕获名在 case 结束后的合并类型；lower 用它在函数帧里补局部槽。
    const std::unordered_map<std::string, TypePtr>* match_captures(const Match* m) const {
        auto it = match_capture_types_.find(m);
        return it == match_capture_types_.end() ? nullptr : &it->second;
    }

    // 模块内方法槽总数。所有类的 vtable 等长，槽位按方法名/访问器全局分配。
    int method_slot_count() const { return method_slot_count_; }

    // 函数体直接引用、且解析到外层函数作用域的名字。降级期用它算
    // cell / env；不含通过嵌套函数传递的引用（那部分由 lower 自底向上传播）。
    const std::unordered_set<std::string>& free_refs(const Stmt* fn) const {
        static const std::unordered_set<std::string> empty;
        auto it = free_refs_.find(fn);
        return it == free_refs_.end() ? empty : it->second;
    }

    // 顶层符号快照，供 `types` 子命令展示
    std::vector<std::pair<std::string, std::string>> top_symbols() const;
    // 异常名判断，供降级阶段生成 except / raise 类型字符串
    static bool is_exception_type(const std::string& n);
    std::vector<std::string> unsupported;
    size_t n_stmt = 0, n_expr = 0, n_func = 0;
    // PYADAMAS_TYPE_STATS=1：统计仍为 unknown 的类型与来源（默认关，零成本）。
    bool type_stats_on_ = false;
    std::unordered_map<std::string, long> type_stats_;
    std::unordered_map<std::string, std::vector<std::string>> type_stats_examples_;
    void type_stat(const std::string& cat, const std::string& example);

    // 声明式原生签名文件（stdlib/sigs/*.pysig）用的注解入口：只转发到
    // parse_annotation，语义与 typecheck 内部完全一致（类型语法只有一份）。
    // 见 docs/native-stdlib-cookbook.md。
    TypePtr parse_annotation_for_decl(Expr* e, int line) { return parse_annotation(e, line); }

private:
    std::string file_;
    ModuleLookup module_lookup_;
    bool lenient_imports_ = false;
    bool strict_module_names_ = false;
    bool lang_gaps_ = false;
    std::vector<std::string> cinclude_dirs_;
    // cimport 绑定名 → 声明；extern_by_fn_ 把类型系统里的 FuncInfo 指回
    // .cpya 声明，供降级阶段做 C ABI 编组。
    std::unordered_map<std::string, std::shared_ptr<CImportDecl>> cmodules_;
    std::unordered_map<const FuncInfo*, const CExtern*> extern_by_fn_;
    std::unordered_map<const CExtern*, FuncInfoPtr> extern_fns_;
    std::vector<std::unique_ptr<Scope>> pool_;
    Scope* cur_ = nullptr;
    Scope* module_ = nullptr;
    std::vector<Diag> diags_;
    FuncInfoPtr cur_fn_;
    const FunctionDef* cur_fn_def_ = nullptr;
    Scope* cur_fn_scope_ = nullptr;
    std::unordered_set<std::string> global_decl_;
    std::unordered_set<std::string> nonlocal_decl_;
    // 每个函数直接引用的外层函数变量；值在 lookup 命中祖先函数作用域时记录。
    std::unordered_map<const Stmt*, std::unordered_set<std::string>> free_refs_;
    struct RetInfo {
        TypePtr ty;
        int line = 1;
        int col = 1;
    };
    // 指向当前函数的 return 收集器。嵌套函数必须有自己的，
    // 否则内层 def 会把外层的 return 记录清掉。
    std::vector<RetInfo>* rets_ = nullptr;
    // 当前函数里每个 yield 产生的值类型，用来推断 Generator[T] 的 T。
    std::vector<TypePtr>* yields_ = nullptr;
    int loop_depth_ = 0;
    int expr_depth_ = 0;
    bool saw_yield_ = false;
    bool star_import_ = false;
    // 模块作用域里装的是类本身（不是实例）的名字：类定义、from ... import
    // 进来的类、import * 带进来的类。区分「C(...) 构造」和「实例 __call__」
    // 只能看绑定的来源，类型上两者都是 Class。
    std::unordered_set<std::string> class_obj_names_;
    // from asyncio import ...：保留成员名与原始泛型签名，调用点仍走专用规则。
    std::unordered_map<std::string, NativeAlias> native_aliases_;
    // 模块里有 globals()[...] = ... 这类动态注入（inspect/turtle 都这么干），
    // 之后模块级名字集合在静态上就不可知了，未定义名一律放行。
    bool dynamic_globals_ = false;
    std::unordered_set<std::string> globals_aliases_;
    std::unordered_map<const Expr*, TypePtr> expr_types_;
    // u64 大字面量（>INT64_MAX 的十进制）：词法已解析成位模式，这里登记
    // 待消费节点；只有 u64 上下文（coerce/提升后 U64 的运算）能消费。
    // check_once 末尾未被消费的照旧报 too large for int64（默认 int 不变）。
    std::unordered_set<const Expr*> u64_lit_pending_;
    std::unordered_set<const Expr*> u64_lit_ok_;
    std::unordered_map<const Call*, std::shared_ptr<PartialPlan>> partial_plans_;
    std::unordered_map<const Stmt*, Scope*> func_scopes_;
    std::unordered_map<const Stmt*, FuncInfoPtr> func_infos_;
    // 检查过的函数定义（含方法），供形参回填后的重查用。
    std::vector<FunctionDef*> func_defs_;
    // 嵌套的生成器表达式合成函数：重查时要回到定义时的词法父作用域，
    // 否则形参回填后函数体里的自由变量会解析到模块级。
    std::unordered_map<const FunctionDef*, Scope*> nested_recheck_parent_;
    // 调用合成生成器函数的位置：重查后返回类型从 Generator[unknown] 变成
    // Generator[T]，这里要同步刷新调用点缓存的表达式类型。
    std::unordered_map<const Expr*, FuncInfoPtr> genexpr_call_sites_;
    // 合成生成器函数在形参回填后同步重查一次（见 typecheck/call.cpp）。
    void recheck_synth_gen(const FuncInfoPtr& fn);
    struct CallResult {
        const FunctionDef* def;
        TypePtr ty;
    };
    // Hints survive a clean replay; observed calls and signatures do not.
    std::unordered_map<const Call*, CallResult> result_hints_;
    std::unordered_map<const Call*, CallResult> observed_results_;
    std::unordered_map<const FuncInfo*, const FunctionDef*> signature_defs_;
    void check_once(Module& m);
    bool collect_result_hints();
    TypePtr observe_call_result(Call& c, const FuncInfoPtr& fn, TypePtr result);
    std::unordered_map<const FunctionDef*, ClassInfoPtr> func_owners_;
    std::unordered_set<const FunctionDef*> methods_with_float_defaults_;
    void check_inferred_default_overrides();
    std::unordered_map<std::string, ClassInfoPtr> classes_;
    // 正在检查方法体的类。`self.x = ...` 里 x 没声明过时，就地补成字段——
    // Python 的实例属性本来就是在 __init__ 里长出来的，要求先写标注会把
    // 标准库里绝大多数类挡在门外。
    ClassInfo* cur_class_ = nullptr;
    // 类体命名空间：class 体里先定义的类属性对后面的语句可见（`y = x + 1`），
    // 但方法体里看不见（那里必须写 self.x / C.x）。只在检查类体语句时非空。
    std::unordered_map<std::string, TypePtr> class_body_attrs_;
    // 类体里**已经执行过**的 def：名字 → cls->methods 里的下标。CPython 的
    // 类体是顺序求值，def 当场绑定，所以 `__str__ = short` 这种「方法别名」
    // 只看得到前面的定义。别名在降级层映射成同一实现的两个方法槽。
    std::unordered_map<std::string, int> class_body_methods_;
    // 类体的顺序求值（定义在 typecheck/class_alias.cpp）：def 绑定名字、
    // `NAME = 前面的方法名` 建成方法别名。
    void class_body_register_def(const ClassInfoPtr& cls, FunctionDef& f);
    bool class_body_method_alias(const ClassInfoPtr& cls, Stmt& s);
    // 方法槽按「名字」在模块内全局分配：多继承的两个无关基类可能给同名
    // 方法分配过不同槽，全局唯一后 vtable 才能同时满足两边的调用点。
    std::unordered_map<std::string, int> method_slots_;
    // 槽号 → (name, kind) 键：用于发现不同方法名占用同一槽。
    std::unordered_map<int, std::string> method_slot_owner_;
    int method_slot_count_ = 0;
    std::unordered_map<const Match*, std::unordered_map<std::string, TypePtr>> match_capture_types_;

    int intern_method_slot(const std::string& key) {
        auto it = method_slots_.find(key);
        if (it != method_slots_.end()) return it->second;
        int s = method_slot_count_++;
        method_slots_.emplace(key, s);
        method_slot_owner_.emplace(s, key);
        return s;
    }
    // 继承导入类时把它的方法槽并进本模块的槽空间。同名不同槽作为
    // aliases 允许（vtable 填多份），不同名同槽无法同时满足两边调用点，
    // 这里直接报错。
    bool reserve_imported_method_slots(const ClassInfo* cls, int line, int col);
    // 当前方法第一个参数的名字（通常就是 self）。`self.x = 1` 只有认得出
    // 这里，才能把它当成字段声明而不是外部对象的属性写入。
    std::string cur_self_name_;
    // @classmethod 里首参的名字（cls）。它不是 self：`cls.x` 读的是类属性。
    std::string cur_cls_name_;
    // 所有建过的类，含被同名重定义顶掉的旧类。Type 里存的是裸指针，
    // 这里必须保证它们在 typecheck 期间一直有效。
    std::vector<ClassInfoPtr> all_classes_;
    // 重查轮留下的上一代 ClassInfo（3.13）。check() 的每一轮都会重建整个类
    // 世界，而旧一代的 ClassInfo 可能仍被某些 Type 的裸 cls 指针引用（跨轮
    // 缓存、接口快照）；move 赋值会把它们释放掉 → 悬空指针。旧一代统一留到
    // 本 TypeChecker 析构，代价是每轮一点内存。
    std::vector<ClassInfoPtr> retired_classes_;

    // 值类型 struct：名字 → 布局；vector 保证 Type::st 的裸指针长期有效。
    std::unordered_map<std::string, StructInfo*> structs_;
    std::vector<std::unique_ptr<StructInfo>> all_structs_;

    // 模块成员不存在的统一报错：先查「成员级拒绝」（@reject）表，命中就报原因，
    // 否则报 module 'x' has no attribute 'y'。五个使用点都走这里，消息只有一份。
    void err_member(const std::string& module, const std::string& name, int line, int col);

    void err(int line, int col, std::string msg) {
        diags_.push_back({line, col, std::move(msg)});
    }

    // ── 作用域 ──────────────────────────────────────────────────

    Symbol* lookup(const std::string& name);
    ModuleInfoPtr lookup_module_alias(const std::string& name) const;

    Scope* assign_target_scope(const std::string& name);

    void declare(const std::string& name, TypePtr ty, int line, int col = 1, bool annotated = false);

    // ── 标注解析 ────────────────────────────────────────────────

    TypePtr parse_annotation(Expr* e, int line, int depth = 0);

    TypePtr parse_annotation_impl(Expr* e, int line, int depth);

    TypePtr parse_annotation_text(const std::string& text, int line, int depth = 0);

    // ── 顶层收集：让函数可以前向调用（含互递归） ────────────────

    void collect_toplevel(std::vector<StmtPtr>& body);
    // 计算 struct 的 C 兼容布局并登记到模块作用域。
    void build_struct(StructDef& s);
    // .cpya 里带字段的 struct：布局已在解析期算好，这里转成类型系统布局。
    StructInfo* build_cstruct(const CStruct& cs);

    // 模块级绑定可能出现在函数定义之后（条件 import、延迟赋值），
    // Python 的全局名在函数调用时才解析，所以要先整体登记一遍。
    void hoist_module_bindings(std::vector<StmtPtr>& body);

    // 顶层 import 绑定预登记：签名在 collect_toplevel 里构建，早于导入
    // 语句执行，注解里的 `mod.Cls` 需要提前可见。
    void hoist_import_bindings(std::vector<StmtPtr>& body);

    // 函数里的 `global x` 也会在模块作用域造名字，先整体登记
    void hoist_global_decls(std::vector<StmtPtr>& body);

    // 函数体同样需要预登记局部名字：内层函数可以引用外层稍后才赋值的
    // 变量（Python 的名字在调用时解析）。
    void hoist_scope_bindings(std::vector<StmtPtr>& body, Scope* sc);

    // 预收集函数体内（不含嵌套函数/类）的 global / nonlocal 声明。
    // 两者都作用于整个函数，不能等按文本顺序执行到语句才生效。
    void collect_fn_decls(std::vector<StmtPtr>& body,
                          std::unordered_set<std::string>& globals,
                          std::unordered_set<std::string>& nonlocals);

    void bind_target(Expr& target, Scope* sc);

    // 赋值目标链上是否出现 globals()（globals()[k] = v、globals()[k].x = v），
    // 或者出现 globals() 的别名（d = globals(); d[k] = v）。
    bool target_touches_globals(Expr& target) const;

    FuncInfoPtr build_sig(FunctionDef& f);

    // ── 语句 ────────────────────────────────────────────────────

    void stmts(std::vector<StmtPtr>& body);

    // 这个语句块是否必然离开（return/raise/break/continue 或所有分支都离开）。
    // 用来识别「提前退出」的 None 守卫：if x is None: return → 之后 x 不是 None。
    static bool block_exits(const std::vector<StmtPtr>& body);

    void stmt(Stmt& s);
    // 按语句形态拆开（typecheck/stmt_forms.cpp）。
    void stmt_assign(Assign& a);
    void stmt_aug_assign(AugAssign& a);
    void stmt_ann_assign(AnnAssign& a);
    // 免写直调（from c import / dylib 句柄调用）：调用点推断返回 ABI。
    TypePtr check_c_lazy(Call& c, const std::string& sym);
    bool is_c_lazy_call(const Expr* e) const;
    void stmt_return(Return& a);
    void stmt_if(If& a);
    void stmt_while(While& a);
    void stmt_for(For& a);
    void stmt_try(Try& a);
    void stmt_with(With& a);
    void stmt_raise(Raise& a);
    void stmt_assert(Assert& a);
    void stmt_import(Import& a);
    void stmt_cimport(CImport& a);
    void stmt_import_from(ImportFrom& a);
    void stmt_delete(Delete& a);

    // match/case 模式：在当前位置绑定捕获名。
    void match_pattern(Pattern& p, TypePtr subject,
                       const std::unordered_set<std::string>* or_names = nullptr);

    // 当前方法里 zero-arg super() 的下一实现（按定义类的 C3 MRO）。
    const MethodInfo* super_method(const std::string& name) const;
    // 定义类的 MRO 里有没有跨模块导入的基类（其方法 check 阶段不可见）。
    bool imported_base_in_mro(const ClassInfo* c) const;


    // `if x is not None:` 这类守卫之后的简单类型收窄。
    // 不做完整的数据流分析，只处理 Name 与 None 的比较。
    struct Narrowing {
        Symbol* sym = nullptr;
        TypePtr old;
        TypePtr now;  // 收窄后的类型；块内被重新赋值过就不该再恢复
        // 属性收窄（0.498）：`if self.x is not None:` 之后 `self.x.foo()`。
        // 键是「基名.属性」，值存在 attr_refine_ 里（没有 Symbol 可改）。
        std::string akey;
        bool had_old = false;
    };
    std::unordered_map<std::string, TypePtr> attr_refine_;
    Narrowing narrow_on(Expr& cond, bool truthy);
    // 条件里「可窄化的名字」：裸名字或 walrus 的赋值目标（定义在 stmt.cpp）。
    Name* narrow_name_of(Expr* e);
    // isinstance 窄化的类型计算（定义在 stmt.cpp）：判不准返回 nullptr。
    TypePtr narrow_isinstance(const TypePtr& t, const ClassInfo* cls, bool truthy);
    // 同一条件的全部窄化（`a is not None and b is not None` 有两个）。
    std::vector<Narrowing> narrow_all(Expr& cond, bool truthy);
    // 未判空就使用 T | None（0.498 窄化配套）：报错并返回 true。
    bool maybe_none(const Expr* e, const TypePtr& t, int line, int col);
    void restore_narrowing(const Narrowing& n);

    static Tok aug_to_bin(Tok t);

    void funcdef(FunctionDef& f);

    // owner 非空时按方法处理：不把方法名绑到外层作用域，
    // 并且第一个形参（self）的类型就是这个类。
    void funcdef_impl(FunctionDef& f, const ClassInfoPtr& owner);

    // ── 类 ──────────────────────────────────────────────────────

    void classdef(ClassDef& c);

    // 这个属性访问的目标是不是当前方法的 self（`self.x`）
    bool is_self_attr(const Attribute& a) const;
    // 3.9（docs/mi-layout.md）：方法体碰 self.<字段> 的记录与多继承布局护栏。
    // 实现放在 typecheck/mi_guard.cpp（文件规模限制）。
    void mark_self_field(const Attribute& a);
    void check_mi_layout(const ClassDef& c, const ClassInfo* self, ClassInfo* base,
                         const std::vector<ClassInfo*>& extra_bases, bool open);
    // 选出 layout base（MRO 顺序第一个有字段的基类）并设置 base_fields。
    void set_layout_base(ClassInfo& cls, ClassInfo* base,
                         const std::vector<ClassInfo*>& extra_bases);
    // 把非 layout base 的字段块追加进子类布局，并记 base_field_offsets（0.506）。
    void materialize_base_fields(ClassInfo& cls, ClassInfo* base,
                                 const std::vector<ClassInfo*>& extra_bases);
    // PYADAMAS_DUMP_LAYOUT=1 时打印类布局（阶段验证用，默认关闭、零开销）。
    void dump_layout(const ClassDef& c, const ClassInfo& cls);
    // 接收者是不是类本身（`C.x` / @classmethod 里的 `cls.x`），而不是实例
    bool is_class_receiver(const Attribute& a);
    // 裸名字是不是类本身（构造）而不是装着实例的变量
    bool name_is_class(const Name& n);
    // 把 `self.x` 补成隐式字段；已存在就按 join 放宽，返回是否新建
    bool add_implicit_field(const Attribute& a, TypePtr ty);

    // 类的字段访问：返回字段类型，不是字段返回 nullptr
    TypePtr class_field_type(const TypePtr& recv, const std::string& name);

    // ── 赋值目标 ────────────────────────────────────────────────

    void assign(Expr& target, TypePtr value, int line, int col);

    // ── 定宽整数字面量 coerce（P1）────────────────────────────────
    //
    // 整数字面量（含一元负号）的编译期值。不是字面量返回 false。
    static bool literal_int_value(const Expr* e, int64_t& out);
    // 目标类型是定宽整数、值表达式是字面量时：
    //   范围内 → 把字面量的静态类型改写成目标类型并返回 true（调用方按
    //   已兼容处理，降级按该类型生成常量，不会截断）；
    //   超范围 → 报编译错误并返回 true（别再叠加一条通用类型错误）。
    // 不是字面量/目标不是定宽整数返回 false（走普通 assignable 判断）。
    bool accept_literal_into(const TypePtr& want, Expr* value);
    // 定宽元素的容器字面量（list/set/dict）：逐元素按元素类型做字面量
    // coerce（范围内隐式、超范围报错，与标量赋值同口径），全部位置都是
    // 字面量时把容器字面量的静态类型改写成目标类型并返回 true；否则一个
    // 都不改写，返回 false 走普通 assignable 判断。空字面量返回 false
    // （保留既有的洞回填路径）。
    bool coerce_container_literal(const TypePtr& want, Expr* value);
    // u64 大字面量的登记/消费/收尾检查（见 expr_types_ 旁的成员注释）。
    void note_u64_literal(const Expr* e);
    // 运算/比较提升到 U64 时把 u64 字面量操作数的静态类型改写成 U64。
    void consume_u64_literal(const TypePtr& rt, Expr* e);
    void check_u64_literals();

    void unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col);

    // 解包目标各自拿到的类型（不写符号表）。规则与 unpack_assign 的各个
    // 分支一致；quiet=true 时只算类型、不报诊断（调用方随后还会走 assign
    // 报错，避免重复诊断）。返回 false 表示解包本身不合法。
    bool unpack_types(const std::vector<ExprPtr>& targets, TypePtr value, int line, int col,
                      std::vector<TypePtr>& out, bool quiet);

    // for 目标里每个名字在**循环体内**应有的类型（逐层解包，规则同上）。
    // 用于把「本轮元素类型」与「跨轮合并后的槽类型」分开（见 stmt_for）。
    void collect_target_types(Expr& target, TypePtr value,
                              std::vector<std::pair<std::string, TypePtr>>& out);

    // ── 表达式 ──────────────────────────────────────────────────

    TypePtr expr(Expr& e);

    // 真正的推断逻辑。外层 expr() 只负责把结果登记进 expr_types_，
    // 保证「任何一个被检查过的表达式」都能反查类型。
    TypePtr expr_impl(Expr& e);

    // 推导式：先绑定前面的 for 子句，再求 elt 的类型
    TypePtr comp_elem(Expr& self, std::vector<ExprPtr>& gens, ExprPtr& elt);

    void enter_comp_scope(std::vector<ExprPtr>& gens);

    void exit_comp_scope() { cur_ = cur_->parent; }

    // ── 运算规则 ────────────────────────────────────────────────

    TypePtr binop_type(Tok op, TypePtr l, TypePtr r, int line, int col,
                       const Expr* le = nullptr, const Expr* re = nullptr);
    // 类实例的算术 dunder：有可用实现时返回方法声明的返回类型，
    // 否则 nullptr。
    TypePtr dunder_binop_ret(Tok op, TypePtr l, TypePtr r);
    // 增量赋值优先看 __iadd__ 这类原地版本。
    TypePtr dunder_aug_ret(Tok op, TypePtr l, TypePtr r);
    // 单参数 dunder（self 之外无形参）的声明返回类型：一元运算和 abs()
    // 都按它定结果类型，没有实现时返回 nullptr。
    TypePtr dunder_unary_ret(const char* name, TypePtr t);
    // 大小比较的 dunder 是否存在（含右操作数镜像版）。
    bool dunder_cmp_ok(CmpOp op, TypePtr l, TypePtr r);

    // §1.2 提升规则集中在 typecheck/types.h 的 int_promote，类型检查/降级/
    // codegen 共用同一份判断（定宽整数的 signed/unsigned 坑只允许有一处定义）。
    static TypePtr promote(const TypePtr& l, const TypePtr& r) {
        return int_promote(l, r);
    }

    void compare_type(CmpOp op, TypePtr l, TypePtr r, int line, int col);

    TypePtr subscript_type(TypePtr obj, TypePtr idx, int line, int col);
    // 属性表达式的类型检查（typecheck/expr_attr.cpp，从 expr 的 Attribute
    // case 逐字拆出，文件规模硬限制）。
    TypePtr expr_attribute(Attribute& a);

    TypePtr iter_elem(TypePtr it, int line, int col);

    // ── 调用 ────────────────────────────────────────────────────

    TypePtr call(Call& c);
    // 按被调表达式形态拆开（typecheck/call_forms.cpp）。
    TypePtr call_name(Name& n, Call& c);
    TypePtr call_attr(Attribute& a, Call& c);
    TypePtr call_value(Call& c);

    TypePtr check_args(const FuncInfoPtr& fn, Call& c);

    // ── 内置函数 ────────────────────────────────────────────────

    // 内置异常类型：让 raise / except 按名字解析并按继承关系匹配
    static bool is_exception_name(const std::string& n);

    // 内置「值」与内置「可调用对象」必须分开：
    // __name__ 是 str，NotImplemented/Ellipsis 是哨兵值，都不是函数。
    // 以前混在一个集合里，导致 __name__ + '.io' 报「func + str」。
    static TypePtr builtin_value_type(const std::string& n);

    static bool is_builtin_callable(const std::string& n);

    TypePtr builtin_type(const std::string& n);

    TypePtr builtin_call(const std::string& n, Call& c);
    // asyncio 的 run/sleep/gather 需要按实参类型推结果，静态签名表达不了。
    std::optional<TypePtr> asyncio_call(const std::string& member, Call& c);
    // random.choice/shuffle 同理：choice 的返回类型是序列的元素类型。
    std::optional<TypePtr> random_call(const std::string& member, Call& c);
    // enum / abc 的编译期类语义（typecheck/enum.cpp）。
    void mark_abstract_methods(const FunctionDef& f, MethodInfo& m);
    void enum_member_assign(ClassInfo* cls, const std::string& name, const Expr* value,
                            int line, int col);
    void finalize_enum(const ClassDef& c, ClassInfo* cls);
    // itertools/collections 没有泛型，按调用点的实参类型特化。
    std::optional<TypePtr> itertools_call(const std::string& member, Call& c);
    // operator 的包装函数没有泛型：按调用点实参类型给出结果类型。
    std::optional<TypePtr> operator_call(const std::string& member, Call& c);
    std::optional<TypePtr> collections_call(const std::string& member, Call& c);
    std::optional<TypePtr> functools_call(const std::string& member, Call& c);
    // @functools.lru_cache / @lru_cache(...)：类型上按恒等装饰器处理。
    bool is_lru_cache_expr(const Expr* e);
    // json.loads 的目标类型由第二个「样例值」的静态类型给出。
    std::optional<TypePtr> json_call(const std::string& member, Call& c);

    // ── 方法 ────────────────────────────────────────────────────

public:
    // 降级层 hasattr() 的静态成员判定（方法或属性都算）。
    static bool type_has_member(const TypePtr& t, const std::string& m) {
        return has_method(t, m) || is_property_name(m);
    }

private:
    static bool has_method(const TypePtr& t, const std::string& m);

    // 名字是属性而非方法的成员（int.numerator 等）。取到的是值，
    // 后面跟 () 才该报「不可调用」。
    static bool is_property_name(const std::string& m) {
        static const std::unordered_set<std::string> s = {
            "numerator", "denominator", "real", "imag",
        };
        return s.count(m) > 0;
    }

    TypePtr method_call(TypePtr recv, const std::string& name, Call& c,
                        bool self_recv = false);
};


// 未填的类型洞归一化为全局 unknown 单例（定义见 typecheck/typecheck.cpp）。
void normalize_type_holes(TypePtr& t, int depth);
// 容器/元组/生成器类型里是否含 unknown 元素（类型统计用）。
bool type_contains_unknown_elem(const TypePtr& t);

}  // namespace pyadamas
