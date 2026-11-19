#pragma once
// typecheck.h —— 静态类型检查
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

#include "ast.h"
#include "cimport.h"
#include "lexer.h"
#include "parser.h"

#include "typecheck_types.h"

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
    // 协议 dunder 的返回类型校验（在函数体推理完之后跑，见 typecheck_func.cpp）。
    void check_dunder_returns();

    // 多模块编译时由驱动注入：按模块名取已经加载的接口。
    // check 模式：import 图解析失败/属性缺失按 unknown 处理，不判死。
    void set_lenient_imports(bool v) { lenient_imports_ = v; }
    // 模块级未定义名字：check 模式宽松（标准库基线需要），build/run 模式
    // 必须严格。否则变量名打错会被降级成一个初值为 0 的全局，运行期
    // 静默算出 0 而不是抛 NameError —— 这是最难发现的静默错误。
    void set_strict_module_names(bool v) { strict_module_names_ = v; }
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

    // 表达式推断出的类型。IR 降级阶段要用它决定槽宽和指令选择，
    // 不能靠 ast 重新推一遍——两套推断迟早会分叉。
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

private:
    std::string file_;
    ModuleLookup module_lookup_;
    bool lenient_imports_ = false;
    bool strict_module_names_ = false;
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

    // 值类型 struct：名字 → 布局；vector 保证 Type::st 的裸指针长期有效。
    std::unordered_map<std::string, StructInfo*> structs_;
    std::vector<std::unique_ptr<StructInfo>> all_structs_;

    // 内置类型名：作为值使用时（str.capitalize、int.bit_length）没有承载物
    static bool is_type_name(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "int", "float", "str", "bool", "list", "dict", "tuple", "set",
            "bytes", "bytearray", "object", "type", "frozenset", "complex",
        };
        return names.count(n) > 0;
    }

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
    // 按语句形态拆开（typecheck_stmt_forms.cpp）。
    void stmt_assign(Assign& a);
    void stmt_aug_assign(AugAssign& a);
    void stmt_ann_assign(AnnAssign& a);
    // 免写直调（from c import / cdll 句柄调用）：调用点推断返回 ABI。
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
    };
    Narrowing narrow_on(Expr& cond, bool truthy);
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

    void unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col);

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

    TypePtr binop_type(Tok op, TypePtr l, TypePtr r, int line, int col);
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

    static TypePtr promote(const TypePtr& l, const TypePtr& r) {
        if (l->kind == TyKind::Float || r->kind == TyKind::Float) return t_float();
        return t_int();
    }

    void compare_type(CmpOp op, TypePtr l, TypePtr r, int line, int col);

    TypePtr subscript_type(TypePtr obj, TypePtr idx, int line, int col);

    TypePtr iter_elem(TypePtr it, int line, int col);

    // ── 调用 ────────────────────────────────────────────────────

    TypePtr call(Call& c);
    // 按被调表达式形态拆开（typecheck_call_forms.cpp）。
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


}  // namespace pyadamas
