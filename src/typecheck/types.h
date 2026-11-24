// typecheck/types.h —— 类型模型（Type / FuncInfo / ClassInfo 与工厂）
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

#include "common/ast.h"

namespace pyadamas {

// ════════════════════════════════════════════════════════════════════
//  类型表示
// ════════════════════════════════════════════════════════════════════

enum class TyKind : uint8_t {
    Unknown,  // 推断失败 / 放弃检查
    None,     // None
    Bool,
    Int,  // 默认整数 = i64 机器字；定宽标注用下面的 I8..U64
    // ── 定宽整数（P1，docs/numeric-types-design.md §1）──
    // 机器表示统一 8 字节 i64 槽：有符号在槽内**符号扩展**，无符号**零扩展**。
    // 运算按宽度截断回绕（C 式），不是 CPython 无限精度；要无限精度用 big（P3）。
    // I64 不单列：Int 就是 i64（i64/long 标注解析成 Int）。
    I8,   // i8 / char（char = i8 别名，C 式字节）
    I16,  // i16 / short
    I32,  // i32
    U8,   // u8 / uchar
    U16,  // u16
    U32,  // u32
    U64,  // u64（无符号 64 位：存储与 Int 同为 i64 槽，语义无符号）
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
    DYLIB,         // dylib.load() 的动态库句柄；机器表示是 dlopen 返回的整数地址
    VThread,      // go f(args) 返回的虚拟线程 handle（vthread 先验）
    Chan,         // make_chan(T, cap) 通道；sub[0] 是元素类型
    // ── P3（docs/numeric-types-design.md §1/§4）──追加在尾部，既有编号不动。
    Big,          // big：任意精度整数，BigInt 堆对象（rt_big.c），BIG tag=12
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
    enum class Kind : uint8_t { None, Bool, Int, Float, Str, Bytes, Big } kind = Kind::None;
    bool b = false;
    int64_t i = 0;
    // Kind::Int 且来自 u64 范围十进制字面量（>INT64_MAX）：i 是无符号位
    // 模式，只有 u64 形参收得下。跨模块编码用独立 tag（旧解码器按「有默认
    // 但不可表示」降级，旧缓存文件的 tag 0..7 逐字不变）。
    bool u64 = false;
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
    // .cpya 定长数组字段的「地址视图」：非空表示这个地址还要再走
    // cdims.size() 层下标（每步 elem_size * prod(cdims[1:]) 字节）。
    // 1 维数组不设置（直接按 ptr[T] 语义处理 p[i]）；只有多维数组的
    // 中间层需要它区分「取地址」还是「读元素」。
    std::vector<int64_t> cdims;
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
    bool omitted_none = false;  // None default observed; cannot enter a float slot.
    // 显式 object/Any 标注（不是「没标注」也不是回填来的 unknown）：
    // 形参故意留在 unknown 域，不做调用点单态化；float 实参在调用点装箱。
    bool ann_unknown = false;
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
    // 函数名被当作值使用过（赋给变量、放进容器、当实参传、返回）。
    // 间接调用点上调用方看不到签名，只能按统一约定装填；此时形参若靠
    // 单态回填定 ABI，一旦实际实参类型与回填结果不符就会静默算错。
    bool value_used = false;
    // 函数体尚未检查时，ret 还没推断出来。此时把它当 None 会让
    // 「先调用、后定义」的函数（互递归、typing.py 的辅助函数）返回 None，
    // 进而产生一堆 "None is not iterable" 的假错误。
    bool ret_known = false;
    // 返回类型由调用点按实参特化（.pysig 的 @ret_specialized）：ret 只是占位，
    // 类型检查在调用点按实参推导（random.choice / Random.choice 这类）。
    bool ret_specialized = false;
    bool unchecked = false;   // 内置函数：参数规则在 builtin_call 里按名字分派
    bool is_extern = false;   // cimport 外部函数：参数/返回值按 C ABI 编组
    // 免写直调（from c import X / dylib 句柄属性调用）：没有 .cpya 声明，
    // 参数编组由调用点实参类型推断，返回类型由推断/标注决定。
    // c_symbol 是 C 侧符号名（别名导入时与 name 不同）。
    bool c_lazy = false;
    std::string c_symbol;
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
    // .cpya 定长数组字段：非空 = 数组（行优先，dims[0] 最外层）。
    // ty 是元素类型（标量 / Optional[int] 指针 / StructPtr），size 是数组总字节数。
    std::vector<int64_t> dims;
    // 数组元素是标量/ptr 时的 C 机器种类（用于地址视图与解引用宽度）。
    CKind c_elem = CKind::None;
    bool is_array() const { return !dims.empty(); }
};

// 值类型 struct 的布局。v1 只允许 int / float / bool 字段：
// 分别对应 C 的 int64_t / double / _Bool，布局与 C 完全一致。
struct StructInfo {
    std::string name;
    // 同 ClassInfo::alive：StructInfo 由 TypeChecker::all_structs_ 拥有。
    bool alive = true;
    std::vector<StructFieldInfo> fields;
    int64_t size = 0;
    int64_t align = 1;
    // .cpya 声明的 C 结构体：指针只是整数地址，没有 RC 头，不能当引用计数对象。
    bool c_struct = false;
    // @union 声明的联合体（所有字段偏移 0）。按值传给 C 的 ABI 分类不做，
    // 声明侧会拒绝；这里用于诊断与字段访问。
    bool c_union = false;
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
    // @abstractmethod（abc）：声明为抽象方法。含未实现抽象方法的类
    // 不能实例化（typecheck 在构造点报错）。
    bool is_abstract = false;
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


// enum 成员（class Color(Enum): RED = 1）：名字 + 字面量值。编译期登记，
// 降级阶段在类创建时生成成员对象（含 value/name 两个字段槽）。
struct EnumMember {
    std::string name;
    LiteralValue val;
    int line = 0;
};
struct ClassInfo {
    std::string name;
    // 所有权契约（调试期可检测）：ClassInfo 由 TypeChecker::all_classes_ 拥有，
    // Type 里的 cls 是裸指针。TypeChecker 析构时把 alive 清零；任何在这之后
    // 通过 Type::cls 访问都是 use-after-free，pa_type_cls() 会断言出来。
    // 生产构建不读它，零开销。
    bool alive = true;
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
    // 提供字段前缀的基类（0.504）：MRO 顺序里第一个有字段的基类。它的字段
    // 在子类里的偏移与它自己类里**完全一致**，所以它的方法体按声明类布局算
    // 偏移是对的 —— 「mixin 在前、状态基类在后」因此不需要 adjustor thunk。
    // null = 没有任何基类带字段（无前缀）。
    ClassInfo* layout_base = nullptr;
    // 多继承「布局实体化」（0.506）：非 layout base 的字段块被**追加**进
    // 子类布局，这里记「基类 → 该块在子类布局里的起始下标」。有了它，
    // `field()/field_index()` 能解析进基类块，方法体读 self.x 才不会落到
    // 遮蔽的隐式字段上（thunk 的 delta 也从这里取，见 docs/mi-layout.md §8）。
    std::unordered_map<const ClassInfo*, int> base_field_offsets;
    std::vector<FieldInfo> fields;    // 本类声明的字段 + 追加进布局的基类块
    std::vector<MethodInfo> methods;  // 只含本类声明的方法（含属性访问器）
    std::vector<ClassAttrInfo> class_attrs;  // 只含本类声明的类属性
    // 通过本类给「继承来的」类属性赋过值的名字。Python 里这会让本类从
    // 此拥有独立的一份；降级阶段据此分配独立存储和「已拥有」标志位。
    std::unordered_set<std::string> cowed_attrs;
    // class 体里的 `__match_args__ = ("x", "y")`，只服务类模式的位置子模式。
    std::vector<std::string> match_args;
    // 类体里的「方法别名」赋值语句（`__str__ = short`）：编译期已经把它变成
    // methods 里共享实现的别名条目，降级层不再按普通赋值执行这条语句。
    std::unordered_set<const Stmt*> alias_assign_stmts;
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
    // 带 @dataclass 装饰器：由装饰器在运行期合成 __init__，静态看不到。
    // 实例化时不能按「没有 __init__、不许带实参」判死。
    bool is_dataclass = false;
    // enum.Enum 子类：类体赋值是枚举成员（成员对象有 value/name 字段）。
    bool is_enum = false;
    std::vector<EnumMember> enum_members;
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

    // 3.9：本类的方法体里读过/写过 self.<字段>（含继承来的字段）。多继承下
    // 非主基类的字段偏移会平移，而方法体按**声明类**的布局算偏移 → 静默
    // 读错字段。这个标记让「class C(B, A)」在类定义处被拒绝，而不是算错值。
    // 纯 mixin（方法不碰 self.<字段>）不受影响。
    bool methods_access_own_fields = false;

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
        if (base)
            if (int i = base->field_index(n); i >= 0) return i;
        // 字段前缀来自 layout base（无字段时退回主基类，行为与 0.503 一致）。
        if (layout_base && layout_base != base) return layout_base->field_index(n);
        return -1;
    }
    const FieldInfo* field(const std::string& n) const {
        for (auto& f : fields)
            if (f.name == n) return &f;
        if (base)
            if (const FieldInfo* f = base->field(n)) return f;
        // layout base 的字段也在本类布局里（非主基类提供前缀时）。
        if (layout_base && layout_base != base) return layout_base->field(n);
        return nullptr;
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
// 抽象方法检查：按「自身 → 基类」顺序找每个抽象方法最先定义它的那一层，
// 若那一层是 @abstractmethod 就是未实现。看不全的类（open：外部导入的基类
// 成员表缺失）不判，避免误杀 unittest.TestCase 这类。
inline std::string unimplemented_abstract(const ClassInfo* cls) {
    if (!cls) return {};
    std::vector<const ClassInfo*> chain;
    for (const ClassInfo* p = cls; p; p = p->base) {
        chain.push_back(p);
        for (const ClassInfo* e : p->extra_bases) chain.push_back(e);
    }
    for (const ClassInfo* p : chain)
        if (p->open) return {};
    std::unordered_set<std::string> seen;
    for (const ClassInfo* p : chain) {
        for (auto& m : p->methods) {
            if (m.is_accessor()) continue;
            if (!seen.insert(m.name).second) continue;  // 更派生的层已定义
            if (m.is_abstract) return m.name;
        }
    }
    return {};
}


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
// 定宽整数单例。标注解析、提升结果、cast 目标都用它们，
// 这样同一种宽度/符号在世界里只有一个 Type 节点（type_eq 走指针等值）。
inline const TypePtr& t_i8() {
    static TypePtr v = new_ty(TyKind::I8);
    return v;
}
inline const TypePtr& t_i16() {
    static TypePtr v = new_ty(TyKind::I16);
    return v;
}
inline const TypePtr& t_i32() {
    static TypePtr v = new_ty(TyKind::I32);
    return v;
}
inline const TypePtr& t_u8() {
    static TypePtr v = new_ty(TyKind::U8);
    return v;
}
inline const TypePtr& t_u16() {
    static TypePtr v = new_ty(TyKind::U16);
    return v;
}
inline const TypePtr& t_u32() {
    static TypePtr v = new_ty(TyKind::U32);
    return v;
}
inline const TypePtr& t_u64() {
    static TypePtr v = new_ty(TyKind::U64);
    return v;
}
// P3 big 单例（t_big 定义在后面，先前向声明）。
inline const TypePtr& t_big();
// 显式转换函数名（cast_i8 / cast_u8 / ...）→ 目标类型。不是 cast 名返回 nullptr。
inline TypePtr cast_target_of(std::string_view n) {
    if (n == "cast_i8") return t_i8();
    if (n == "cast_i16") return t_i16();
    if (n == "cast_i32") return t_i32();
    if (n == "cast_i64" || n == "cast_int") return t_int();
    if (n == "cast_u8") return t_u8();
    if (n == "cast_u16") return t_u16();
    if (n == "cast_u32") return t_u32();
    if (n == "cast_u64") return t_u64();
    // P3：cast_big（int/定宽/bool/float/str → big）。
    if (n == "cast_big") return t_big();
    return nullptr;
}

// 按 TyKind 取定宽单例；不是整数返回 nullptr。
inline TypePtr int_ty_of(TyKind k) {
    switch (k) {
        case TyKind::I8: return t_i8();
        case TyKind::I16: return t_i16();
        case TyKind::I32: return t_i32();
        case TyKind::Int: return t_int();
        case TyKind::U8: return t_u8();
        case TyKind::U16: return t_u16();
        case TyKind::U32: return t_u32();
        case TyKind::U64: return t_u64();
        default: return nullptr;
    }
}
inline const TypePtr& t_float() {
    static TypePtr v = new_ty(TyKind::Float);
    return v;
}
// P3：big 单例（与定宽单例同口径，type_eq 走指针等值）。
inline const TypePtr& t_big() {
    static TypePtr v = new_ty(TyKind::Big);
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
inline const TypePtr& t_dylib() {
    static TypePtr v = new_ty(TyKind::DYLIB);
    return v;
}

// 可变的类型洞：容器元素类型未知时用它占位，后续插入时原地填上
inline TypePtr fresh_hole() { return new_ty(TyKind::Unknown); }

// ── 联合类型（0.488 修 Bug A）────────────────────────────────────
// `int | str` 这类「成员互不可赋值」的联合，以前在 parse_annotation 里落到
// join() 的兜底 t_unknown() 单例，于是**任何值都能赋进去**（`x: int | str = 1.5`
// 静默通过，float 的 double 位模式一路流到运行期）。
//
// 表示法：TyKind::Unknown + 非空 sub（成员表）。刻意复用 Unknown：
//   * 运行期表示、降级、coercion 全部沿用 unknown 的 8 字节字，零代码生成改动；
//   * 所有 `kind == Unknown` 的宽松路径照旧（不引入新的误杀面）。
// 只有两处要认它：assignable（目标侧按成员逐个判）与 type_name（打印）。
// 洞（fresh_hole）与联合的区别就是 sub 是否为空。
inline bool is_union_ty(const TypePtr& t) {
    return t && t->kind == TyKind::Unknown && !t->sub.empty() && t.get() != t_unknown().get();
}


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
        case CKind::Float:
        case CKind::Float32: return t_float();
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
        // P1 收尾：窄/无符号 C 类型映射到定宽 TyKind（边界按宽度符号/零
        // 扩展）。既有 int/int32/uint32 仍映射默认 int，提升语义不动。
        case CKind::Int8: return t_i8();
        case CKind::Int16: return t_i16();
        case CKind::UInt8: return t_u8();
        case CKind::UInt16: return t_u16();
        case CKind::UInt64: return t_u64();
        // 按值结构体需要作用域里的 StructInfo，调用方在结构体表命中时
        // 已提前返回 mk_struct，这里只作兜底。
        case CKind::Struct:
        case CKind::Callback: return t_unknown();
    }
    return t_int();
}
inline TypePtr cptr_elem_type(CKind k) {
    switch (k) {
        case CKind::Float:
        case CKind::Float32: return t_float();
        case CKind::Bool: return t_bool();
        case CKind::Int8: return t_i8();
        case CKind::Int16: return t_i16();
        case CKind::UInt8: return t_u8();
        case CKind::UInt16: return t_u16();
        case CKind::UInt64: return t_u64();
        default: return t_int();
    }
}

// .cpya 定长数组字段的「地址视图」类型：Optional[int] 地址 + 元素信息。
//  - 1 维标量/指针数组：celem 有值，按既有 ptr[T] 语义处理 p[i]（读元素/写回）；
//    整个数组当 ptr 传给 C 也走同一条路。
//  - struct/union 元素数组：st 有值，下标先得到元素地址（StructPtr）。
//  - 多维数组：cdims 记录剩余维数，中间层下标返回「地址视图」而不是读元素。
inline TypePtr c_array_view_type(const StructFieldInfo& f) {
    if (!f.is_array()) return f.ty;
    TypePtr v = mk_optional(t_int());
    v->cdims = f.dims;
    if (f.ty && (f.ty->kind == TyKind::Struct || f.ty->kind == TyKind::StructPtr))
        v->st = f.ty->st;
    else
        v->celem = f.c_elem;
    return v;
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
inline TypePtr mk_vthread() { return new_ty(TyKind::VThread); }
inline TypePtr mk_chan(TypePtr elem) {
    auto t = new_ty(TyKind::Chan);
    t->sub.push_back(std::move(elem));
    return t;
}


}  // namespace pyadamas

// 以下三个头是本文件的组成部分（按原顺序包含，声明顺序不变）：
// 提升规则 / 判定与显示 / 合并与赋值兼容。
#include "typecheck/promote.h"
#include "typecheck/query.h"
#include "typecheck/join.h"
