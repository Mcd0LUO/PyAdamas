// typecheck_types.h —— 类型模型（Type / FuncInfo / ClassInfo 与工厂）
#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.h"

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
    ClassObj,     // 类名当值：运行期类对象；sub[0] 是 mk_class(cls)
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
    TimeDelta,    // datetime.timedelta
    Path,         // pathlib.Path
    Deque,        // collections.deque
    Struct,       // 值类型 struct 的实例，st 指向编译期布局
    StructPtr,    // addr(struct) 得到的指针；机器表示与 Struct 相同
};

struct Type;
using TypePtr = std::shared_ptr<Type>;
struct FuncInfo;
using FuncInfoPtr = std::shared_ptr<FuncInfo>;
struct ClassInfo;
using ClassInfoPtr = std::shared_ptr<ClassInfo>;
struct StructInfo;
struct ModuleInfo;
using ModuleInfoPtr = std::shared_ptr<ModuleInfo>;
using ModuleLookup = std::function<ModuleInfoPtr(const std::string&)>;

// 默认参数的可序列化字面量。跨模块接口不能带 AST 指针，
// 只能保留这种最小的常量表示。
struct LiteralValue {
    enum class Kind : uint8_t { None, Bool, Int, Float, Str, Bytes } kind = Kind::None;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

// 常量表达式 → 可序列化字面量；非 Constant 返回 nullopt。
std::optional<LiteralValue> literal_of_expr(const Expr* e);

struct Type {
    TyKind kind = TyKind::Unknown;
    // List/Set: [elem]   Dict: [key, val]   Tuple: elems
    // BoundMethod: [recv]   Optional: [inner]
    std::vector<TypePtr> sub;
    FuncInfoPtr fn;    // kind == Func
    // kind == Class。裸指针：ClassInfo 由 TypeChecker 拥有，而 ClassInfo
    // 的字段类型又指回 Type —— 两边都用 shared_ptr 会成环，永远不会释放。
    ClassInfo* cls = nullptr;
    // kind == Struct / StructPtr。所有权在 TypeChecker::all_structs_。
    StructInfo* st = nullptr;
    std::string name;  // BoundMethod 的方法名
    // 元素类型是否由标注固定。字面量推出来的容器是「开放」的：
    // d = {}; d['k'] = 1; d['k2'] = 's' 在 Python 里合法，
    // 只有 x: dict[str, int] 才应该拒绝 's'。
    bool locked = false;
    // Tuple 专用：长度不定（tuple(x)、切片、*args）。元素类型只有 sub[0]。
    // 不区分「长度不定」和「单元素元组」会造成 (1,) 与 tuple(x) 混淆，
    // `a, b = t[-2:]` 会被误报成「不能把 1 个值拆成 2 个」。
    bool variadic = false;
    // kind == Optional 且内层是 int 的 C 指针（ptr[T]）：T 的机器种类。
    // 用于 `p[i]` 解引用；None 表示不透明指针（ptr / void*）。
    CKind celem = CKind::None;
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
    // 跨模块接口用的字面量形式；非字面量初值只有 default_expr。
    std::optional<LiteralValue> default_lit;
    // 从 `self.x = ...` 推断出来的字段（没写标注）。类型只是推测，
    // 赋别的类型时放宽成 join，不报错。
    bool implicit = false;
};

// 值类型 struct 的一个字段。偏移/大小按 C 结构体规则在编译期算好，
// 降级阶段直接按偏移读写，不生成访问器。
struct StructFieldInfo {
    std::string name;
    TypePtr ty;
    int64_t offset = 0;
    int64_t size = 0;
    int64_t align = 1;
    // C 结构体的 uint32 字段：读取时零扩展而不是符号扩展。
    bool c_unsigned = false;
};

// 值类型 struct 的布局。v1 只允许 int / float / bool 字段：
// 分别对应 C 的 int64_t / double / _Bool，布局与 C 完全一致。
struct StructInfo {
    std::string name;
    std::vector<StructFieldInfo> fields;
    int64_t size = 0;
    int64_t align = 1;
    // .cpya 声明的 C 结构体：指针只是整数地址，没有 RC 头，不能当引用计数对象。
    bool c_struct = false;
    const StructFieldInfo* field(const std::string& n) const {
        for (auto& f : fields)
            if (f.name == n) return &f;
        return nullptr;
    }
    int index_of(const std::string& n) const {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == n) return static_cast<int>(i);
        return -1;
    }
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

// 方法槽的全局键：访问器按名字前缀区分，普通/静态/类方法共用方法名。
inline std::string method_slot_key(const std::string& name, MethodKind kind) {
    switch (kind) {
        case MethodKind::PropertyGetter: return "@get:" + name;
        case MethodKind::PropertySetter: return "@set:" + name;
        case MethodKind::PropertyDeleter: return "@del:" + name;
        default: return name;
    }
}

struct MethodInfo {
    std::string name;
    MethodKind kind = MethodKind::Normal;
    FuncInfoPtr sig;          // 含 self 的完整签名（方法体内 self 的类型来自它）
    FuncInfoPtr call_sig;     // 去掉 self 的调用签名（外部 obj.m(...) 按它查）
    FunctionDef* def = nullptr;  // 供降级阶段编译方法体
    int slot = -1;
    // 同一 (name, kind) 在不同模块里可能占了不同槽；导入类的接口
    // 会带完整别名表，vtable 填充时所有槽写同一个实现。
    std::vector<int> aliases;
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
// 类属性在模块里占一个隐藏全局，按「声明它的类」命名；子类读取时沿 MRO
// 找到声明者，读到的是同一份数据。类名当值时另有运行期类对象单例。
struct ClassAttrInfo {
    std::string name;
    TypePtr ty;
    Expr* init = nullptr;            // 类体里的初值表达式，降级时求值一次
    const ClassInfo* owner = nullptr;  // 声明它的类
};

// 跨模块祖先描述：导入类携带的完整 MRO 链（不含自身）。方法槽与类编号
// 都只在模块内有效，导入方靠这三项生成 mirror 元数据并引用导出的
// vtable/own_slots 符号。
struct ClassAncestor {
    std::string module;
    std::string name;
    std::string qualname;
};

struct ClassInfo {
    std::string name;
    // 类名在值位置被用过（赋值、传参、类装饰器）：降级时要生成一个
    // 运行期类对象单例。
    bool value_used = false;
    // 基类用裸指针，所有权统一在 TypeChecker::all_classes_。
    // 类之间的继承是 DAG，但字段类型会指回类，用 shared_ptr 会成环。
    ClassInfo* base = nullptr;
    // 自定义异常类：继承内置异常名或另一个自定义异常类。
    bool is_exception_class = false;
    // 富异常：定义了字段或方法（含 __init__/__str__）。这类异常实例是
    // 真正的 vtable 对象，except as e 拿到的是实例；否则（pass-only）
    // 仍是扁平 Exception 值，e.args 可用。
    bool is_rich_exception = false;
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
    // 跨模块导入的类的来源：定义模块、定义模块发出的限定名、完整祖先链
    // （不含自身，MRO 顺序）。本模块定义的类这三项为空。
    std::string origin_module;
    std::string origin_qualname;
    std::vector<ClassAncestor> ext_ancestors;
    bool imported() const { return !origin_module.empty(); }
    // 定义模块里的类名：导入时绑定名可能被改过（别名），符号必须用原名。
    std::string external_name() const {
        if (origin_qualname.empty()) return name;
        auto pos = origin_qualname.rfind('.');
        return pos == std::string::npos ? name : origin_qualname.substr(pos + 1);
    }
    std::unordered_map<std::string, PropertyInfo> properties;
    int base_fields = 0;              // 继承来的字段数

    // 构造签名就是 __init__ 的调用签名（已去掉 self）。没有 __init__ 时
    // 只允许零实参构造。
    bool has_init = false;
    bool has_field_defaults = false;  // 整条继承链上是否有字段初值
    // 实例需要动态属性槽（反射访问了静态字段没有的名字，或 setattr 过一个
    // 非类字面量名字）。降级时给实例末尾留一个 dict 指针槽。
    bool needs_dict = false;
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
    // 同一类的判断：本模块内的类比指针；导入类在接口里被拍平后可能
    // 来自不同 ModuleInfo 副本，按定义模块 + 类名比较。
    static bool same_class(const ClassInfo* a, const ClassInfo* b) {
        if (a == b) return true;
        if (!a || !b || !a->imported() || !b->imported()) return false;
        return a->origin_module == b->origin_module &&
               a->external_name() == b->external_name();
    }
    bool is_subclass_of(const ClassInfo* b) const {
        if (same_class(this, b)) return true;
        // 导入类的祖先链可能比 mro 更长（接口里拍平过）。
        if (b && b->imported()) {
            for (auto& a : ext_ancestors)
                if (a.module == b->origin_module && a.name == b->external_name())
                    return true;
        }
        if (!mro.empty()) {
            for (const ClassInfo* c : mro) {
                if (same_class(c, b)) return true;
                if (!c->imported()) continue;
                // 导入类的祖先链不在 mro 里，按接口记录展开比较。
                for (auto& a : c->ext_ancestors)
                    if (b && b->imported() && a.module == b->origin_module &&
                        a.name == b->external_name())
                        return true;
            }
            return false;
        }
        for (const ClassInfo* p = this; p; p = p->base) {
            if (same_class(p, b)) return true;
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
inline TypePtr mk_deque(TypePtr e) {
    auto t = new_ty(TyKind::Deque);
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

inline TypePtr mk_struct(StructInfo* s) {
    auto t = new_ty(TyKind::Struct);
    t->st = s;
    return t;
}
inline TypePtr mk_struct_ptr(StructInfo* s) {
    auto t = new_ty(TyKind::StructPtr);
    t->st = s;
    return t;
}

// .cpya 的 C 类型 → PyAdamas 类型。指针统一当作 Optional[int]：
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
        // 按值结构体需要作用域里的 StructInfo，调用方在结构体表命中时
        // 已提前返回 mk_struct，这里只作兜底。
        case CKind::Struct:
        case CKind::Callback: return t_unknown();
    }
    return t_int();
}
inline TypePtr cptr_elem_type(CKind k) {
    switch (k) {
        case CKind::Float: return t_float();
        case CKind::Bool: return t_bool();
        default: return t_int();
    }
}

inline TypePtr mk_bound(TypePtr recv, std::string method) {
    auto t = new_ty(TyKind::BoundMethod);
    t->sub.push_back(std::move(recv));
    t->name = std::move(method);
    return t;
}
// 方法值的类型：把 `obj.m` / `C.m` 当成普通函数值。
// class_recv 为真表示接收者是类本身（`C.m`），普通方法按 Python 3 保持
// 未绑定（self 仍是第一个形参）；实例上取到的是绑定方法，self 去掉。
inline TypePtr mk_method_value(const MethodInfo& m, bool class_recv) {
    FuncInfoPtr src;
    bool drop_first = false;
    switch (m.kind) {
        case MethodKind::Static:
        case MethodKind::ClassMethod:
            src = m.call_sig;  // 静态方法没有 self；类方法已绑定到类
            break;
        case MethodKind::Normal:
            if (m.decorated && m.decorated_ty && m.decorated_ty->kind == TyKind::Func) {
                src = m.decorated_ty->fn;  // 包装签名含 self
                drop_first = !class_recv;
            } else {
                src = class_recv ? m.sig : m.call_sig;
            }
            break;
        default:
            return t_unknown();
    }
    if (!src) return t_unknown();
    auto fn = std::make_shared<FuncInfo>(*src);
    fn->outer = nullptr;
    fn->sig_dirty = false;
    if (drop_first && !fn->pos.empty()) fn->pos.erase(fn->pos.begin());
    auto t = new_ty(TyKind::Func);
    t->fn = std::move(fn);
    return t;
}
inline TypePtr mk_class(ClassInfo* c) {
    auto t = new_ty(TyKind::Class);
    t->cls = c;
    return t;
}
// Generator[Y, S, R]：sub[0]=yield 类型，sub[1]=send 值类型，
// sub[2]=return 值类型（StopIteration.value，目前只做记录）。
inline TypePtr mk_generator(TypePtr elem, TypePtr send = nullptr, TypePtr result = nullptr) {
    auto t = new_ty(TyKind::Generator);
    t->sub.push_back(std::move(elem));
    if (send || result) t->sub.push_back(std::move(send));
    if (result) t->sub.push_back(std::move(result));
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
inline TypePtr mk_class_obj(TypePtr cls_ty) {
    auto t = new_ty(TyKind::ClassObj);
    t->sub.push_back(std::move(cls_ty));
    return t;
}
// 类对象类型里的 ClassInfo；不是类对象返回 nullptr。
inline ClassInfo* class_obj_cls(const TypePtr& t) {
    if (!t || t->kind != TyKind::ClassObj || t->sub.empty() || !t->sub[0]) return nullptr;
    return t->sub[0]->kind == TyKind::Class ? t->sub[0]->cls : nullptr;
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
    if (a->kind == TyKind::Struct || a->kind == TyKind::StructPtr) return a->st == b->st;
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
        case TyKind::Struct:
            return t->st ? t->st->name : "struct";
        case TyKind::StructPtr:
            return t->st ? std::format("ptr[{}]", t->st->name) : "ptr";
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
        case TyKind::ClassObj:
            return t->sub.empty() ? "type"
                                  : std::format("type[{}]", type_name_impl(t->sub[0], depth + 1));
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
        case TyKind::TimeDelta: return "timedelta";
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
        return mk_generator(
            join(a->sub.empty() ? t_unknown() : a->sub[0],
                 b->sub.empty() ? t_unknown() : b->sub[0]),
            (a->sub.size() > 1 && b->sub.size() > 1) ? join(a->sub[1], b->sub[1]) : nullptr,
            (a->sub.size() > 2 && b->sub.size() > 2) ? join(a->sub[2], b->sub[2]) : nullptr);
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
    if (a->kind == TyKind::ClassObj && b->kind == TyKind::ClassObj) {
        // 两个不同的类对象合并后没法静态表示，只能退 unknown。
        return t_unknown();
    }
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
    if (dst->kind == TyKind::ClassObj && src->kind == TyKind::ClassObj) {
        ClassInfo* dc = class_obj_cls(dst);
        ClassInfo* sc = class_obj_cls(src);
        if (!dc || !sc) return true;
        return sc->is_subclass_of(dc);
    }
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


}  // namespace pyadamas
