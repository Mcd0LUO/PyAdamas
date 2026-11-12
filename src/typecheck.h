#pragma once
// typecheck.h —— 静态类型检查
//
// 设计原则：
//   1. 类型标注是约束，不是语法必需。没有标注时按使用推断；
//      推断不出来就退化成 Unknown，任何涉及 Unknown 的检查都放行。
//      —— 宁可漏报，不可误杀。歧义是静态检查最大的敌人。
//   2. 变量类型一旦确定就不可改变（这是与 CPython 的语义分歧点，
//      也是拿到性能的前提：每个变量在栈上有固定槽位和固定宽度）。
//   3. 遇到尚未实现的语句（try/with/import/yield/async）
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

namespace pyadamas {

// ════════════════════════════════════════════════════════════════════
//  类型表示
// ════════════════════════════════════════════════════════════════════

enum class TyKind : uint8_t {
    Unknown,  // 推断失败 / 放弃检查
    None,     // None
    Bool,
    Int,
    Float,
    Complex,
    Str,
    Bytes,  // b"..."：运行时和 str 同一套表示，类型上区分
    List,
    Range,   // range 对象：惰性序列，sub[0] 是元素类型（int）
    Set,
    Dict,
    Tuple,
    Optional,   // sub[0] 是内层类型；None 是合法值
    Func,
    BoundMethod,  // 尚未调用的方法，sub[0] 是接收者
    Class,        // 类实例，cls 指向编译期确定的布局
    Generator,    // sub[0] 是 yield 产生的元素类型
    Coroutine,    // sub[0] 是 await / asyncio.run 得到的结果类型
    Task,         // asyncio.create_task 返回的已调度协程；sub[0] 是结果类型
    AsyncGenerator, // async def + yield；sub[0] 是产出类型
    Future,       // asyncio.Future；sub[0] 是结果类型
    AsyncEvent,   // asyncio.Event
    AsyncLock,    // asyncio.Lock
    AsyncQueue,   // asyncio.Queue；sub[0] 是元素类型
    TypeObj,      // type(x) 的类型对象；sub[0] 是被表示的类型
    AsyncTimeout, // asyncio.timeout
    AsyncTaskGroup, // asyncio.TaskGroup
    AsyncSemaphore, // asyncio.Semaphore
    Exception,    // 异常值（type name 放在 Type::name）；args 是 message 列表
    File,         // open() 返回的文件对象
    Random,       // random.Random() 返回的随机数生成器
    CompletedProcess,  // subprocess.run() 返回的结果对象
    Popen,        // subprocess.Popen() 返回的进程句柄
    Regex,        // re.compile() 返回的 Pattern
    Match,        // re.search()/match() 返回的 Match
    DateTime,     // datetime.date / datetime.datetime
    Path,         // pathlib.Path
    Deque,        // collections.deque
};

struct Type;
using TypePtr = std::shared_ptr<Type>;
struct FuncInfo;
using FuncInfoPtr = std::shared_ptr<FuncInfo>;
struct ClassInfo;
using ClassInfoPtr = std::shared_ptr<ClassInfo>;
struct ModuleInfo;
using ModuleInfoPtr = std::shared_ptr<ModuleInfo>;
using ModuleLookup = std::function<ModuleInfoPtr(const std::string&)>;

// 默认参数的可序列化字面量。跨模块接口不能带 AST 指针，
// 只能保留这种最小的常量表示。
struct LiteralValue {
    enum class Kind : uint8_t { None, Bool, Int, Float, Str } kind = Kind::None;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

struct Type {
    TyKind kind = TyKind::Unknown;
    // List/Set: [elem]   Dict: [key, val]   Tuple: elems
    // BoundMethod: [recv]   Optional: [inner]
    std::vector<TypePtr> sub;
    FuncInfoPtr fn;    // kind == Func
    // kind == Class。裸指针：ClassInfo 由 TypeChecker 拥有，而 ClassInfo
    // 的字段类型又指回 Type —— 两边都用 shared_ptr 会成环，永远不会释放。
    ClassInfo* cls = nullptr;
    std::string name;  // BoundMethod 的方法名
    // 元素类型是否由标注固定。字面量推出来的容器是「开放」的：
    // d = {}; d['k'] = 1; d['k2'] = 's' 在 Python 里合法，
    // 只有 x: dict[str, int] 才应该拒绝 's'。
    bool locked = false;
    // Tuple 专用：长度不定（tuple(x)、切片、*args）。元素类型只有 sub[0]。
    // 不区分「长度不定」和「单元素元组」会造成 (1,) 与 tuple(x) 混淆，
    // `a, b = t[-2:]` 会被误报成「不能把 1 个值拆成 2 个」。
    bool variadic = false;
};

// 判断 needle 是否出现在 hay 的子树里。容器元素类型可能指向容器自身
// （d['k'] = d），直接写进 sub 会形成环，打印类型时无限递归。
inline bool type_contains(const TypePtr& hay, const TypePtr& needle, int depth = 0) {
    if (!hay || depth > 8) return false;
    if (hay == needle) return true;
    for (auto& s : hay->sub)
        if (type_contains(s, needle, depth + 1)) return true;
    return false;
}

inline void lock_type(const TypePtr& t) {
    if (!t) return;
    switch (t->kind) {
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Dict:
        case TyKind::Tuple:
        case TyKind::Optional:
            break;
        default:
            return;  // 标量类型是共享单例，不要改
    }
    if (t->locked) return;
    t->locked = true;
    for (auto& s : t->sub) lock_type(s);
}

// 一个形参。以前用 pnames/ptypes/has_default 三条平行数组，结果
// 关键字参数被算进了「位置参数个数」，`def f(a, *, b)` 会接受 f(1, 2)。
// 现在按参数种类分开存，调用检查不必再猜下标。
struct Param {
    std::string name;
    TypePtr ty;
    bool has_default = false;
    bool kwonly = false;
    // 默认值的 AST 节点。降级器在调用点内联填充；通过函数值调用时
    // 没有 FunctionDef 可查，只能靠签名里的这个指针。
    const Expr* default_expr = nullptr;
    // 跨模块接口用的字面量形式；本地函数也会填，导入函数只有它。
    std::optional<LiteralValue> default_lit;
    // ty 是从调用点实参回填来的（不是标注）。同一个函数被多种类型调用时
    // 要退回 unknown，不能当成类型错误。
    bool inferred = false;
    bool multi = false;
    // cimport 的 ptr/ptr[T] 形参：额外接受 list/bytes 缓冲区实参。
    bool cptr = false;
    CKind cptr_elem = CKind::None;  // ptr[T] 的 T；None 表示无类型 ptr
    // ptr[Struct]：不透明结构体指针，只接受地址/None，不接受 list/bytes。
    bool cptr_opaque = false;
};

struct FuncInfo {
    std::string name;
    std::vector<Param> pos;   // 位置参数，前 n_posonly 个只能按位置传
    size_t n_posonly = 0;
    std::vector<Param> kwonly;
    bool varargs = false;
    std::string vararg_name;
    TypePtr vararg_ty;        // *args 的元素类型
    bool kwargs = false;
    std::string kwarg_name;
    TypePtr kwarg_ty;         // **kwargs 的值类型
    TypePtr ret;              // nullptr 表示无返回值
    bool ret_declared = false;
    // 函数体尚未检查时，ret 还没推断出来。此时把它当 None 会让
    // 「先调用、后定义」的函数（互递归、typing.py 的辅助函数）返回 None，
    // 进而产生一堆 "None is not iterable" 的假错误。
    bool ret_known = false;
    bool unchecked = false;   // 内置函数：参数规则在 builtin_call 里按名字分派
    bool is_extern = false;   // cimport 外部函数：参数/返回值按 C ABI 编组
    // 函数体里有 yield：调用得到生成器对象，return 标注描述的是元素类型
    // 之外的东西。不能只看 ret 是不是 Generator——返回生成器的普通函数
    // （工厂）也会是 Generator，但它们本身不是生成器函数。
    bool is_generator = false;
    // async def：ret 存 Coroutine[结果]，async_result 是 return/await 看到的
    // 结果类型。两者分开是因为调用点需要前者、return 检查需要后者。
    bool is_async = false;
    TypePtr async_result;
    // async def 里还有 yield：调用得到异步生成器（只支持 async for）
    bool is_async_generator = false;
    // 未标注形参被调用点回填过。函数体如果已经查过，需要按新签名重查。
    bool sig_dirty = false;
    // 方法的 call_sig 是 sig 去掉 self 的副本。回填要写回 sig，否则方法体
    // 重查时看不到新类型。outer_off 是 call_sig->pos[0] 在 sig->pos 里的下标。
    FuncInfo* outer = nullptr;
    size_t outer_off = 0;
};

// ── 类 ────────────────────────────────────────────────────────────
//
// 布局在编译期定死：对象 = [vtable 指针][字段 0][字段 1]…，字段偏移
// 是 8 的倍数，属性访问编译成一条固定偏移的读，不查表。
//
// 单继承。子类的字段接在父类之后，vtable 前缀与父类完全一致，覆盖
// 只改槽里的函数地址，新增方法追加到末尾。这样「用父类类型持有子类
// 实例再调方法」能走对实现，而槽号在编译期就是常量。
struct FieldInfo {
    std::string name;
    TypePtr ty;
    bool has_default = false;
    Expr* default_expr = nullptr;  // 供降级阶段生成字段初始化
    // 从 `self.x = ...` 推断出来的字段（没写标注）。类型只是推测，
    // 赋别的类型时放宽成 join，不报错。
    bool implicit = false;
};

enum class MethodKind : uint8_t {
    Normal,
    Static,
    // @classmethod：首参 cls 不占运行时实参，方法体里静态绑定成所属类。
    ClassMethod,
    PropertyGetter,
    PropertySetter,
    PropertyDeleter,
};

struct MethodInfo {
    std::string name;
    MethodKind kind = MethodKind::Normal;
    FuncInfoPtr sig;          // 含 self 的完整签名（方法体内 self 的类型来自它）
    FuncInfoPtr call_sig;     // 去掉 self 的调用签名（外部 obj.m(...) 按它查）
    FunctionDef* def = nullptr;  // 供降级阶段编译方法体
    int slot = -1;
    // 自定义装饰器：类创建时把装饰器链的结果存成类属性，调用点按
    // decorated_ty 查参，降级时通过类属性做间接调用。
    bool decorated = false;
    TypePtr decorated_ty;     // 装饰器链的返回类型（含 self 位置）
    bool is_accessor() const {
        return kind == MethodKind::PropertyGetter || kind == MethodKind::PropertySetter ||
               kind == MethodKind::PropertyDeleter;
    }
};

// 属性描述符：访问器方法在 owner->methods 里的下标。getter 是
// @property，setter/deleter 由 @x.setter / @x.deleter 注册。
struct PropertyInfo {
    ClassInfo* owner = nullptr;
    int getter = -1;
    int setter = -1;
    int deleter = -1;
};

// 类体里的普通赋值（class C: x = 1）是类属性：所有实例共享同一份存储。
// pyadamas 没有运行期类对象，类属性在模块里占一个隐藏全局，按「声明它的类」
// 命名；子类读取时沿 MRO 找到声明者，读到的是同一份数据。
struct ClassAttrInfo {
    std::string name;
    TypePtr ty;
    Expr* init = nullptr;            // 类体里的初值表达式，降级时求值一次
    const ClassInfo* owner = nullptr;  // 声明它的类
};

struct ClassInfo {
    std::string name;
    // 基类用裸指针，所有权统一在 TypeChecker::all_classes_。
    // 类之间的继承是 DAG，但字段类型会指回类，用 shared_ptr 会成环。
    ClassInfo* base = nullptr;
    // 自定义异常类：继承内置异常名或另一个自定义异常类。实例值按
    // TyKind::Exception 处理，不参与对象布局/vtable。
    bool is_exception_class = false;
    std::string exc_parent;
    // 多重继承的附加基类：主基类提供字段布局，附加基类只能复用
    // 已有字段；方法按 C3 MRO 解析。
    std::vector<ClassInfo*> extra_bases;
    std::vector<FieldInfo> fields;    // 只含本类声明的字段
    std::vector<MethodInfo> methods;  // 只含本类声明的方法（含属性访问器）
    std::vector<ClassAttrInfo> class_attrs;  // 只含本类声明的类属性
    // 通过本类给「继承来的」类属性赋过值的名字。Python 里这会让本类从
    // 此拥有独立的一份；降级阶段据此分配独立存储和「已拥有」标志位。
    std::unordered_set<std::string> cowed_attrs;
    // class 体里的 `__match_args__ = ("x", "y")`，只服务类模式的位置子模式。
    std::vector<std::string> match_args;
    // C3 线性化，self 在首位。跨模块导入的类没有运行期 MRO 表，留空。
    std::vector<ClassInfo*> mro;
    std::unordered_map<std::string, PropertyInfo> properties;
    int base_fields = 0;              // 继承来的字段数

    // 构造签名就是 __init__ 的调用签名（已去掉 self）。没有 __init__ 时
    // 只允许零实参构造。
    bool has_init = false;
    bool has_field_defaults = false;  // 整条继承链上是否有字段初值
    // 有「看不见的父类」：基类是外部导入的类型（unittest.TestCase、
    // OrderedDict……）或者内建 object。这类类的成员表必然不全，查不到
    // 属性/方法时只能认输返回 unknown —— 否则标准库里每个测试类都会被
    // assertEqual、addCleanup 这类继承来的方法判死，属于误杀。
    bool open = false;
    // 顶层预扫描会先建一个只有名字的骨架，好让「函数签名里引用后面
    // 才定义的类」能解析。骨架的字段/方法是空的，方法体检查都发生在
    // classdef 填充之后。
    bool complete = false;

    int n_fields() const { return base_fields + static_cast<int>(fields.size()); }
    int64_t obj_size() const { return 8 * static_cast<int64_t>(n_fields() + 1); }

    // 整条继承链上只要有一层看不全，就都看不全
    bool is_open() const {
        for (const ClassInfo* p = this; p; p = p->base) {
            if (p->open) return true;
            for (const ClassInfo* e : p->extra_bases)
                if (e->is_open()) return true;
        }
        return false;
    }

    // 字段的全局序号；-1 表示不存在
    int field_index(const std::string& n) const {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == n) return base_fields + static_cast<int>(i);
        return base ? base->field_index(n) : -1;
    }
    const FieldInfo* field(const std::string& n) const {
        for (auto& f : fields)
            if (f.name == n) return &f;
        return base ? base->field(n) : nullptr;
    }
    // 类属性沿 MRO 解析：子类读到的就是声明者那一份，没有拷贝。
    const ClassAttrInfo* class_attr(const std::string& n) const {
        for (const ClassInfo* c : mro)
            for (auto& a : c->class_attrs)
                if (a.name == n) return &a;
        if (!mro.empty()) return nullptr;
        for (auto& a : class_attrs)
            if (a.name == n) return &a;
        if (base) return base->class_attr(n);
        for (const ClassInfo* e : extra_bases)
            if (auto* a = e->class_attr(n)) return a;
        return nullptr;
    }
    // 自己声明的方法；后定义的同名方法胜出。属性访问器默认不参与
    // 普通方法查找（它们只通过属性语法触发）。
    const MethodInfo* own_method(const std::string& n, bool accessors = false) const {
        for (auto it = methods.rbegin(); it != methods.rend(); ++it) {
            if (it->name != n) continue;
            if (!accessors && it->is_accessor()) continue;
            return &*it;
        }
        return nullptr;
    }
    const MethodInfo* method(const std::string& n) const {
        for (const ClassInfo* c : mro)
            if (auto* m = c->own_method(n)) return m;
        if (!mro.empty()) return nullptr;
        // 导入类/骨架没有 C3 表，退回逐层查找
        if (auto* m = own_method(n)) return m;
        if (base) {
            if (auto* m = base->method(n)) return m;
        }
        for (const ClassInfo* e : extra_bases)
            if (auto* m = e->method(n)) return m;
        return nullptr;
    }
    const ClassInfo* method_owner(const std::string& n) const {
        for (const ClassInfo* c : mro)
            if (c->own_method(n)) return c;
        if (!mro.empty()) return nullptr;
        if (own_method(n)) return this;
        if (base) {
            if (auto* o = base->method_owner(n)) return o;
        }
        for (const ClassInfo* e : extra_bases)
            if (auto* o = e->method_owner(n)) return o;
        return nullptr;
    }
    const PropertyInfo* property(const std::string& n) const {
        for (const ClassInfo* c : mro) {
            auto it = c->properties.find(n);
            if (it != c->properties.end()) return &it->second;
        }
        if (!mro.empty()) return nullptr;
        auto it = properties.find(n);
        if (it != properties.end()) return &it->second;
        if (base) {
            if (auto* p = base->property(n)) return p;
        }
        for (const ClassInfo* e : extra_bases)
            if (auto* p = e->property(n)) return p;
        return nullptr;
    }
    // __match_args__ 按普通类属性语义沿 MRO 查找。
    const std::vector<std::string>* match_args_of() const {
        for (const ClassInfo* c : mro)
            if (!c->match_args.empty()) return &c->match_args;
        if (!mro.empty()) return nullptr;
        if (!match_args.empty()) return &match_args;
        if (base) {
            if (auto* m = base->match_args_of()) return m;
        }
        for (const ClassInfo* e : extra_bases)
            if (auto* m = e->match_args_of()) return m;
        return nullptr;
    }
    int slot_of(const std::string& n) const {
        const MethodInfo* m = method(n);
        return m ? m->slot : -1;
    }
    bool is_subclass_of(const ClassInfo* b) const {
        if (this == b) return true;
        if (!mro.empty()) {
            for (const ClassInfo* c : mro)
                if (c == b) return true;
            return false;
        }
        for (const ClassInfo* p = this; p; p = p->base) {
            if (p == b) return true;
            for (const ClassInfo* e : p->extra_bases)
                if (e->is_subclass_of(b)) return true;
        }
        return false;
    }
};

// ── 工厂 ──────────────────────────────────────────────────────────

inline TypePtr new_ty(TyKind k) {
    auto t = std::make_shared<Type>();
    t->kind = k;
    return t;
}

inline const TypePtr& t_unknown() {
    static TypePtr v = new_ty(TyKind::Unknown);
    return v;
}
inline const TypePtr& t_none() {
    static TypePtr v = new_ty(TyKind::None);
    return v;
}
inline const TypePtr& t_bool() {
    static TypePtr v = new_ty(TyKind::Bool);
    return v;
}
inline const TypePtr& t_int() {
    static TypePtr v = new_ty(TyKind::Int);
    return v;
}
inline const TypePtr& t_float() {
    static TypePtr v = new_ty(TyKind::Float);
    return v;
}
inline const TypePtr& t_complex() {
    static TypePtr v = new_ty(TyKind::Complex);
    return v;
}
inline const TypePtr& t_str() {
    static TypePtr v = new_ty(TyKind::Str);
    return v;
}
inline const TypePtr& t_bytes() {
    static TypePtr v = new_ty(TyKind::Bytes);
    return v;
}
inline const TypePtr& t_file() {
    static TypePtr v = new_ty(TyKind::File);
    return v;
}
inline const TypePtr& t_random() {
    static TypePtr v = new_ty(TyKind::Random);
    return v;
}
inline const TypePtr& t_completed() {
    static TypePtr v = new_ty(TyKind::CompletedProcess);
    return v;
}
inline const TypePtr& t_popen() {
    static TypePtr v = new_ty(TyKind::Popen);
    return v;
}
inline const TypePtr& t_regex() {
    static TypePtr v = new_ty(TyKind::Regex);
    return v;
}
inline const TypePtr& t_match() {
    static TypePtr v = new_ty(TyKind::Match);
    return v;
}

// 可变的类型洞：容器元素类型未知时用它占位，后续插入时原地填上
inline TypePtr fresh_hole() { return new_ty(TyKind::Unknown); }

inline TypePtr mk_list(TypePtr e) {
    auto t = new_ty(TyKind::List);
    t->sub.push_back(std::move(e));
    return t;
}
inline TypePtr mk_range(TypePtr e) {
    auto t = new_ty(TyKind::Range);
    t->sub.push_back(std::move(e));
    return t;
}
inline TypePtr mk_set(TypePtr e) {
    auto t = new_ty(TyKind::Set);
    t->sub.push_back(std::move(e));
    return t;
}
inline TypePtr mk_dict(TypePtr k, TypePtr v) {
    auto t = new_ty(TyKind::Dict);
    t->sub.push_back(std::move(k));
    t->sub.push_back(std::move(v));
    return t;
}
inline TypePtr mk_tuple(std::vector<TypePtr> elems) {
    auto t = new_ty(TyKind::Tuple);
    t->sub = std::move(elems);
    return t;
}
// 长度未知、元素同构的元组：tuple(x)、t[a:b]、*args
inline TypePtr mk_tuple_rest(TypePtr elem) {
    auto t = mk_tuple({std::move(elem)});
    t->variadic = true;
    return t;
}
inline TypePtr mk_optional(TypePtr inner) {
    if (!inner || inner->kind == TyKind::Unknown || inner->kind == TyKind::None) return t_unknown();
    auto t = new_ty(TyKind::Optional);
    t->sub.push_back(std::move(inner));
    return t;
}

// .cpya 的 C 类型 → py23 类型。指针统一当作 Optional[int]：
// 0 表示 NULL，运行时不做解引用。
inline TypePtr ckind_type(CKind k) {
    switch (k) {
        case CKind::None: return t_none();
        case CKind::Float: return t_float();
        case CKind::Bool: return t_bool();
        case CKind::Str: return t_str();
        case CKind::OptStr: return mk_optional(t_str());
        case CKind::Ptr: return mk_optional(t_int());
        case CKind::BufF64:
        case CKind::BufI64:
        case CKind::BufBytes:
        case CKind::Int:
        case CKind::Int32:
        case CKind::UInt32: return t_int();
    }
    return t_int();
}
inline TypePtr mk_bound(TypePtr recv, std::string method) {
    auto t = new_ty(TyKind::BoundMethod);
    t->sub.push_back(std::move(recv));
    t->name = std::move(method);
    return t;
}
inline TypePtr mk_class(ClassInfo* c) {
    auto t = new_ty(TyKind::Class);
    t->cls = c;
    return t;
}
inline TypePtr mk_generator(TypePtr elem) {
    auto t = new_ty(TyKind::Generator);
    t->sub.push_back(std::move(elem));
    return t;
}
inline TypePtr mk_coroutine(TypePtr result) {
    auto t = new_ty(TyKind::Coroutine);
    t->sub.push_back(std::move(result));
    return t;
}
inline TypePtr mk_task(TypePtr result) {
    auto t = new_ty(TyKind::Task);
    t->sub.push_back(std::move(result));
    return t;
}
inline TypePtr mk_async_generator(TypePtr elem) {
    auto t = new_ty(TyKind::AsyncGenerator);
    t->sub.push_back(std::move(elem));
    return t;
}
inline TypePtr mk_future(TypePtr result) {
    auto t = new_ty(TyKind::Future);
    t->sub.push_back(std::move(result));
    return t;
}
inline TypePtr mk_typeobj(TypePtr inner) {
    auto t = new_ty(TyKind::TypeObj);
    t->sub.push_back(std::move(inner));
    return t;
}
inline TypePtr mk_async_timeout() { return new_ty(TyKind::AsyncTimeout); }
inline TypePtr mk_async_taskgroup() { return new_ty(TyKind::AsyncTaskGroup); }
inline TypePtr mk_async_semaphore() { return new_ty(TyKind::AsyncSemaphore); }
inline TypePtr mk_async_event() { return new_ty(TyKind::AsyncEvent); }
inline TypePtr mk_async_lock() { return new_ty(TyKind::AsyncLock); }
inline TypePtr mk_async_queue(TypePtr elem) {
    auto t = new_ty(TyKind::AsyncQueue);
    t->sub.push_back(std::move(elem));
    return t;
}
inline TypePtr mk_exception(const std::string& name) {
    auto t = new_ty(TyKind::Exception);
    t->name = name;
    return t;
}

// ── 判定与显示 ────────────────────────────────────────────────────

inline bool is_num(const TypePtr& t) {
    return t && (t->kind == TyKind::Int || t->kind == TyKind::Float || t->kind == TyKind::Bool);
}
inline bool is_unknown(const TypePtr& t) { return !t || t->kind == TyKind::Unknown; }

inline bool type_eq(const TypePtr& a, const TypePtr& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == TyKind::Func) {
        if (!a->fn || !b->fn) return a->fn == b->fn;
        if (a->fn->pos.size() != b->fn->pos.size() ||
            a->fn->kwonly.size() != b->fn->kwonly.size())
            return false;
        for (size_t i = 0; i < a->fn->pos.size(); ++i)
            if (!type_eq(a->fn->pos[i].ty, b->fn->pos[i].ty)) return false;
        for (size_t i = 0; i < a->fn->kwonly.size(); ++i)
            if (!type_eq(a->fn->kwonly[i].ty, b->fn->kwonly[i].ty)) return false;
        return type_eq(a->fn->ret, b->fn->ret);
    }
    if (a->kind == TyKind::BoundMethod) return a->name == b->name && type_eq(a->sub[0], b->sub[0]);
    if (a->kind == TyKind::Class) return a->cls == b->cls;
    if (a->kind == TyKind::Exception) return true;  // 异常值统一按 Exception 处理
    if (a->kind == TyKind::Tuple && a->variadic != b->variadic) return false;
    if (a->sub.size() != b->sub.size()) return false;
    for (size_t i = 0; i < a->sub.size(); ++i)
        if (!type_eq(a->sub[i], b->sub[i])) return false;
    return true;
}

inline std::string type_name_impl(const TypePtr& t, int depth) {
    if (!t) return "unknown";
    if (depth > 12) return "...";
    switch (t->kind) {
        case TyKind::Unknown: return "unknown";
        case TyKind::None: return "None";
        case TyKind::Bool: return "bool";
        case TyKind::Int: return "int";
        case TyKind::Float: return "float";
        case TyKind::Complex: return "complex";
        case TyKind::Str: return "str";
        case TyKind::Bytes: return "bytes";
        case TyKind::List:
            return t->sub.empty() ? "list"
                                  : std::format("list[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Range:
            return t->sub.empty() ? "range"
                                  : std::format("range[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Set:
            return t->sub.empty() ? "set"
                                  : std::format("set[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Dict:
            return t->sub.size() < 2
                       ? "dict"
                       : std::format("dict[{}, {}]", type_name_impl(t->sub[0], depth + 1),
                                     type_name_impl(t->sub[1], depth + 1));
        case TyKind::Optional:
            return std::format("{} | None",
                               t->sub.empty() ? "unknown" : type_name_impl(t->sub[0], depth + 1));
        case TyKind::Tuple: {
            if (t->variadic)
                return std::format("tuple[{}, ...]",
                                   t->sub.empty() ? "unknown" : type_name_impl(t->sub[0], depth + 1));
            std::string s = "tuple[";
            for (size_t i = 0; i < t->sub.size(); ++i) {
                if (i) s += ", ";
                s += type_name_impl(t->sub[i], depth + 1);
            }
            return s + "]";
        }
        case TyKind::Func: {
            if (!t->fn) return "func";
            std::string s = "func(";
            bool first = true;
            for (const auto& p : t->fn->pos) {
                if (!first) s += ", ";
                first = false;
                s += type_name_impl(p.ty, depth + 1);
                if (p.has_default) s += "=?";
            }
            if (t->fn->varargs) {
                if (!first) s += ", ";
                first = false;
                s += "*" + type_name_impl(t->fn->vararg_ty, depth + 1);
            }
            if (!t->fn->kwonly.empty() && !t->fn->varargs) {
                if (!first) s += ", ";
                s += "*";
                first = false;
            }
            for (const auto& p : t->fn->kwonly) {
                if (!first) s += ", ";
                first = false;
                s += type_name_impl(p.ty, depth + 1);
                if (p.has_default) s += "=?";
            }
            if (t->fn->kwargs) {
                if (!first) s += ", ";
                s += "**" + type_name_impl(t->fn->kwarg_ty, depth + 1);
            }
            s += ") -> ";
            s += t->fn->ret ? type_name_impl(t->fn->ret, depth + 1) : "None";
            return s;
        }
        case TyKind::BoundMethod:
            return std::format("method '{}' of {}", t->name,
                               t->sub.empty() ? "?" : type_name_impl(t->sub[0], depth + 1));
        case TyKind::Class:
            return t->cls ? t->cls->name : "class";
        case TyKind::Generator:
            return t->sub.empty()
                       ? "generator"
                       : std::format("generator[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Coroutine:
            return t->sub.empty()
                       ? "coroutine"
                       : std::format("coroutine[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Task:
            return t->sub.empty() ? "task"
                                  : std::format("task[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::AsyncGenerator:
            return t->sub.empty()
                       ? "asyncgen"
                       : std::format("asyncgen[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Future:
            return t->sub.empty() ? "future"
                                  : std::format("future[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::TypeObj: return "type";
        case TyKind::AsyncTimeout: return "asyncio.Timeout";
        case TyKind::AsyncTaskGroup: return "asyncio.TaskGroup";
        case TyKind::AsyncSemaphore: return "asyncio.Semaphore";
        case TyKind::AsyncEvent: return "asyncio.Event";
        case TyKind::AsyncLock: return "asyncio.Lock";
        case TyKind::AsyncQueue:
            return t->sub.empty()
                       ? "asyncio.Queue"
                       : std::format("asyncio.Queue[{}]",
                                     type_name_impl(t->sub[0], depth + 1));
        case TyKind::Exception:
            return t->name.empty() ? "Exception" : t->name;
        case TyKind::File: return "file";
        case TyKind::Random: return "random.Random";
        case TyKind::CompletedProcess: return "subprocess.CompletedProcess";
        case TyKind::Popen: return "subprocess.Popen";
        case TyKind::Regex: return "re.Pattern";
        case TyKind::Match: return "re.Match";
        case TyKind::DateTime: return "datetime";
        case TyKind::Path: return "pathlib.Path";
        case TyKind::Deque: return "collections.deque";
    }
    return "?";
}

inline std::string type_name(const TypePtr& t) { return type_name_impl(t, 0); }

// 错误消息里的类型名不带参数：CPython 说 'tuple'，不说 'tuple[int]'。
inline std::string bare_type_name(const TypePtr& t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TyKind::None: return "NoneType";
        case TyKind::Bool: return "bool";
        case TyKind::Int: return "int";
        case TyKind::Float: return "float";
        case TyKind::Complex: return "complex";
        case TyKind::Str: return "str";
        case TyKind::Bytes: return "bytes";
        case TyKind::List: return "list";
        case TyKind::Range: return "range";
        case TyKind::Set: return "set";
        case TyKind::Dict: return "dict";
        case TyKind::Tuple: return "tuple";
        case TyKind::Generator: return "generator";
        default: return type_name_impl(t, 0);
    }
}

// ── 类型合并 ──────────────────────────────────────────────────────
// 两个分支的类型不同时取公共上界；同一数值域内做提升，其余情况
// 只要有一边是 Unknown 就取另一边，实在合不了就退化成 Unknown。

inline TypePtr join(const TypePtr& a, const TypePtr& b);

inline void fill_hole(const TypePtr& hole, const TypePtr& with) {
    if (!hole || hole->kind != TyKind::Unknown) return;
    if (!with || with->kind == TyKind::Unknown) return;
    // t_unknown() 是全程序共享的单例，不是「洞」。原地写它会把所有
    // unknown 一起改掉（曾把全程序的 unknown 变成某个函数类型）。
    if (hole.get() == t_unknown().get()) return;
    if (type_contains(with, hole)) return;  // 会形成环，保持 unknown
    *hole = *with;
}

inline TypePtr join_into(const TypePtr& a, const TypePtr& b) {
    if (!a) return b;
    if (!b) return a;
    // 一边是 unknown 说明信息已经丢了；合并结果只能是 unknown。
    // 以前返回具体类型，会让「先像 int、后来又赋了类实例」的变量一直
    // 被当成 int，后续属性访问全部误报。
    if (is_unknown(a) || is_unknown(b)) return t_unknown();
    if (type_eq(a, b)) return a;
    if (is_num(a) && is_num(b)) return t_float();
    if (a->kind == TyKind::List && b->kind == TyKind::List)
        return mk_list(join(a->sub[0], b->sub[0]));
    if (a->kind == TyKind::Range && b->kind == TyKind::Range)
        return mk_range(join(a->sub.empty() ? t_unknown() : a->sub[0],
                             b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Set && b->kind == TyKind::Set)
        return mk_set(join(a->sub[0], b->sub[0]));
    if (a->kind == TyKind::Dict && b->kind == TyKind::Dict)
        return mk_dict(join(a->sub[0], b->sub[0]), join(a->sub[1], b->sub[1]));
    if (a->kind == TyKind::Generator && b->kind == TyKind::Generator)
        return mk_generator(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                 b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Coroutine && b->kind == TyKind::Coroutine)
        return mk_coroutine(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                 b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Task && b->kind == TyKind::Task)
        return mk_task(join(a->sub.empty() ? t_unknown() : a->sub[0],
                            b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::AsyncGenerator && b->kind == TyKind::AsyncGenerator)
        return mk_async_generator(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                       b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::Future && b->kind == TyKind::Future)
        return mk_future(join(a->sub.empty() ? t_unknown() : a->sub[0],
                              b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::TypeObj && b->kind == TyKind::TypeObj)
        return mk_typeobj(join(a->sub.empty() ? t_unknown() : a->sub[0],
                               b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::AsyncQueue && b->kind == TyKind::AsyncQueue)
        return mk_async_queue(join(a->sub.empty() ? t_unknown() : a->sub[0],
                                   b->sub.empty() ? t_unknown() : b->sub[0]));
    if (a->kind == TyKind::AsyncTimeout && b->kind == TyKind::AsyncTimeout) return a;
    if (a->kind == TyKind::AsyncTaskGroup && b->kind == TyKind::AsyncTaskGroup) return a;
    if (a->kind == TyKind::AsyncSemaphore && b->kind == TyKind::AsyncSemaphore) return a;
    if (a->kind == TyKind::AsyncEvent && b->kind == TyKind::AsyncEvent) return a;
    if (a->kind == TyKind::AsyncLock && b->kind == TyKind::AsyncLock) return a;
    if (a->kind == TyKind::Exception && b->kind == TyKind::Exception) return a;
    if (a->kind == TyKind::Complex && b->kind == TyKind::Complex) return a;
    if (a->kind == TyKind::Tuple && b->kind == TyKind::Tuple) {
        if (a->variadic || b->variadic) {
            // 有一边长度不定，结果也只能是长度不定：元素取公共上界
            TypePtr acc = nullptr;
            for (auto& e : a->sub) acc = acc ? join(acc, e) : e;
            for (auto& e : b->sub) acc = acc ? join(acc, e) : e;
            return mk_tuple_rest(acc ? acc : t_unknown());
        }
        if (a->sub.size() == b->sub.size()) {
            std::vector<TypePtr> elems;
            for (size_t i = 0; i < a->sub.size(); ++i) elems.push_back(join(a->sub[i], b->sub[i]));
            return mk_tuple(std::move(elems));
        }
    }
    if (a->kind == TyKind::Optional) {
        if (b->kind == TyKind::None) return a;
        return mk_optional(join(a->sub.empty() ? t_unknown() : a->sub[0], b));
    }
    if (a->kind == TyKind::Class && b->kind == TyKind::Class) {
        // 单继承下两个类实例的公共上界就是较浅的那个；不同分支合并只能放弃
        if (!a->cls || !b->cls) return t_unknown();
        if (a->cls->is_subclass_of(b->cls)) return b;
        if (b->cls->is_subclass_of(a->cls)) return a;
        return t_unknown();
    }
    if (b->kind == TyKind::Optional) {
        if (a->kind == TyKind::None) return b;
        return mk_optional(join(a, b->sub.empty() ? t_unknown() : b->sub[0]));
    }
    if (a->kind == TyKind::None || b->kind == TyKind::None) return t_unknown();
    return t_unknown();
}

inline TypePtr join(const TypePtr& a, const TypePtr& b) { return join_into(a, b); }

// 赋值的兼容性：允许 int -> float 的隐式提升，Unknown 与任何类型兼容
inline bool assignable(const TypePtr& dst, const TypePtr& src) {
    if (is_unknown(dst) || is_unknown(src)) return true;
    if (type_eq(dst, src)) return true;
    if (dst->kind == TyKind::Float && src->kind == TyKind::Int) return true;
    // bool 是 int 的子类型（Python 里 True + 1 == 2）
    if (dst->kind == TyKind::Int && src->kind == TyKind::Bool) return true;
    if (dst->kind == TyKind::Optional)
        return src->kind == TyKind::None ||
               (dst->sub.empty() ? true : assignable(dst->sub[0], src));
    // 子类实例可以放进父类类型的槽（布局前缀相同，方法槽位相同）
    if (dst->kind == TyKind::Class && src->kind == TyKind::Class)
        return dst->cls && src->cls && src->cls->is_subclass_of(dst->cls);
    // 反向放宽：Optional[T] 当作 T 用不报错（类型收窄需要数据流分析，
    // 在这个阶段做了只会误杀）
    if (src->kind == TyKind::Optional)
        return src->sub.empty() ? true : assignable(dst, src->sub[0]);
    if (dst->kind == TyKind::List && src->kind == TyKind::List)
        return assignable(dst->sub[0], src->sub[0]);
    // range 不是 list：不能互相赋值（CPython 也是两回事）。
    if (dst->kind == TyKind::Range && src->kind == TyKind::Range)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    // 协程/生成器是只读产出，元素类型按协变放宽（coroutine[int] 可传给
    // coroutine[unknown] 的 asyncio.gather/run）。
    if (dst->kind == TyKind::Coroutine && src->kind == TyKind::Coroutine)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Generator && src->kind == TyKind::Generator)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Task && src->kind == TyKind::Task)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::AsyncGenerator && src->kind == TyKind::AsyncGenerator)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Future && src->kind == TyKind::Future)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::TypeObj && src->kind == TyKind::TypeObj) return true;
    if (dst->kind == TyKind::AsyncTimeout && src->kind == TyKind::AsyncTimeout) return true;
    if (dst->kind == TyKind::AsyncTaskGroup && src->kind == TyKind::AsyncTaskGroup)
        return true;
    if (dst->kind == TyKind::AsyncSemaphore && src->kind == TyKind::AsyncSemaphore)
        return true;
    if (dst->kind == TyKind::AsyncEvent && src->kind == TyKind::AsyncEvent) return true;
    if (dst->kind == TyKind::AsyncLock && src->kind == TyKind::AsyncLock) return true;
    if (dst->kind == TyKind::AsyncQueue && src->kind == TyKind::AsyncQueue)
        return dst->sub.empty() || src->sub.empty() || assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Exception && src->kind == TyKind::Exception) return true;
    // int/float 可以隐式提升为 complex
    if (dst->kind == TyKind::Complex && src->kind == TyKind::Complex) return true;
    if (dst->kind == TyKind::Complex && is_num(src)) return true;
    if (dst->kind == TyKind::Set && src->kind == TyKind::Set)
        return assignable(dst->sub[0], src->sub[0]);
    if (dst->kind == TyKind::Dict && src->kind == TyKind::Dict)
        return assignable(dst->sub[0], src->sub[0]) && assignable(dst->sub[1], src->sub[1]);
    if (dst->kind == TyKind::Tuple && src->kind == TyKind::Tuple) {
        if (dst->variadic && src->variadic)
            return dst->sub.empty() || src->sub.empty() ||
                   assignable(dst->sub[0], src->sub[0]);
        // 长度不定的一端按元素类型逐个核对，不因为「长度不同」直接报错
        if (dst->variadic) {
            for (auto& s : src->sub)
                if (!assignable(dst->sub[0], s)) return false;
            return true;
        }
        if (src->variadic) {
            for (auto& d : dst->sub)
                if (!assignable(d, src->sub[0])) return false;
            return true;
        }
        if (dst->sub.size() == src->sub.size()) {
            for (size_t i = 0; i < dst->sub.size(); ++i)
                if (!assignable(dst->sub[i], src->sub[i])) return false;
            return true;
        }
    }
    // 容器字面量的空类型洞
    if (dst->kind == TyKind::Tuple && src->kind == TyKind::Tuple && src->sub.empty()) return true;
    return false;
}

// 开放容器扩宽元素类型。若扩宽结果指回容器自身（x[0] = x），
// 就退化成 unknown，避免形成类型环。
inline void widen_slot(TypePtr& slot, const TypePtr& container, const TypePtr& add) {
    TypePtr merged = join(slot, add);
    if (type_contains(merged, container)) {
        slot = t_unknown();
        return;
    }
    slot = std::move(merged);
}

// 把一个值并进容器的元素槽位。返回值 true 表示兼容（或已经扩宽），
// false 表示不兼容且容器被标注锁住，由调用方报错。
//
// 关键一条：来源是 unknown 时也要把槽位退化成 unknown。否则
// `iters = [None] * n; iters[0] = gs[i]().__next__`（值未知）之后，
// 槽位还停在 None 上，下一句 iters[i]() 就报「None 不可调用」。
inline bool merge_elem(TypePtr& slot, const TypePtr& container, const TypePtr& value, bool locked) {
    if (is_unknown(value)) {
        if (!locked && !is_unknown(slot)) slot = t_unknown();
        return true;
    }
    if (assignable(slot, value)) {
        if (is_unknown(slot)) fill_hole(slot, value);
        return true;
    }
    if (!locked) widen_slot(slot, container, value);
    return !locked;  // 扩宽过就算兼容，只有标注锁住时才是真错误
}

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

    void check(Module& m);

    // 多模块编译时由驱动注入：按模块名取已经加载的接口。
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
    std::unordered_map<FunctionDef*, ClassInfoPtr> func_owners_;
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
    int method_slot_count_ = 0;
    std::unordered_map<const Match*, std::unordered_map<std::string, TypePtr>> match_capture_types_;

    int intern_method_slot(const std::string& key) {
        auto it = method_slots_.find(key);
        if (it != method_slots_.end()) return it->second;
        int s = method_slot_count_++;
        method_slots_.emplace(key, s);
        return s;
    }
    // 当前方法第一个参数的名字（通常就是 self）。`self.x = 1` 只有认得出
    // 这里，才能把它当成字段声明而不是外部对象的属性写入。
    std::string cur_self_name_;
    // @classmethod 里首参的名字（cls）。它不是 self：`cls.x` 读的是类属性。
    std::string cur_cls_name_;
    // 所有建过的类，含被同名重定义顶掉的旧类。Type 里存的是裸指针，
    // 这里必须保证它们在 typecheck 期间一直有效。
    std::vector<ClassInfoPtr> all_classes_;

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

    // match/case 模式：在当前位置绑定捕获名。
    void match_pattern(Pattern& p, TypePtr subject,
                       const std::unordered_set<std::string>* or_names = nullptr);

    // 当前方法里 zero-arg super() 的下一实现（按定义类的 C3 MRO）。
    const MethodInfo* super_method(const std::string& name) const;

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

    TypePtr check_args(const FuncInfoPtr& fn, Call& c);

    // ── 内置函数 ────────────────────────────────────────────────

    // 内置异常类型：异常体系尚未实现，先让这些名字可解析
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
    std::optional<TypePtr> collections_call(const std::string& member, Call& c);
    std::optional<TypePtr> functools_call(const std::string& member, Call& c);
    // @functools.lru_cache / @lru_cache(...)：类型上按恒等装饰器处理。
    bool is_lru_cache_expr(const Expr* e);
    // json.loads 的目标类型由第二个「样例值」的静态类型给出。
    std::optional<TypePtr> json_call(const std::string& member, Call& c);

    // ── 方法 ────────────────────────────────────────────────────

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
