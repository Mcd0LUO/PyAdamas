# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.10] - 2026-11-09

这一版补上**字典 / 集合**和 `in` 运算符。类型检查层早就有 `dict`/`set`
类型，这一轮把运行时表示、降级和 GC 接上。

### 新增

- **开放寻址哈希表**：`Py23Dict {len, cap, entries, order, key_kind,
  key_own, val_own}`，`order` 保留插入顺序；`len` 在偏移 0，`len()` 和
  真值判断都是一条读
- **key 类别**：int/bool、str（FNV-1a）、float（规范化 -0.0）、其他按
  机器字比较；字符串 key 用 `py23_str_eq` 判等
- **key/value 所有权**：`key_own` / `val_own` 区分非堆、堆、unknown；
  插入时降级器 retain，更新/销毁时运行时 dec；unknown 靠 GC 兜底
- **set 复用 dict**：`val_own = 0`、值恒 0；集合字面量、`in`、`for`
  都走同一张表
- **下标读写**：`d[k]` → `py23_dict_get`（缺失 panic KeyError），
  `d[k] = v` → `py23_dict_set`；key 按字典声明类型做数值转换
- **迭代**：`for k in d` / `for x in s` 按下标 `py23_dict_key_at` 取 key
- **`in`**：dict/set 用 `py23_dict_contains`；list/tuple 用
  `py23_list_contains`（int/str/float/raw 四种比较）；str 子串用
  `py23_str_contains`；支持 `not in`
- **GC**：`PY23_RC_DICT` 扫描条目 key/value，清扫时用 `dict_gc_release`
  只减可达对象的引用；`entries` / `order` 在 dtor/GC/退出时释放
- `tests/run/14_dict.py`；`tests/run/13_gc.py` 增加字典自环

### 修复

- **插入空槽的经典 bug**：`dict_probe` 查不到返回 -1，早期直接拿 -1
  当插入槽，写坏 `entries[-1]`；增加 `dict_empty_slot` 专门找空槽
- 字典/集合现在属于堆类型：槽、全局、字段、列表元素、闭包 cell 都按
  引用计数管理，GC 也能扫描/释放

### 验证

- `make test`：11 正例 + 34 反例 + 14 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：全部运行用例 + 字典/集合压力程序零报告
- 严格警告零报告
- 16MB `RLIMIT_AS`：10 万次字典创建、10 万次字典自环、10 万次集合、
  10 万次字典-列表创建，峰值 RSS 约 8–9MB

### 已知限制

- dict/set 的**方法**（`get`/`keys`/`items`/`add`/`update` 等）类型检查
  放行，但 `build` 仍拒绝；当前支持的是字面量、下标、`len`、`in`、`for`
- 空字典 `{}` 的 key 类型是 unknown；不同对象但内容相同的动态字符串
  key 按 unknown 的机器字比较，可能不相等。标注/推出来的 str key 没有
  这个问题
- 混合 value 类型的字典值会退化成 unknown（类型系统限制），此时再嵌套
  下标会按未知容器处理
- dict/set 相等比较仍是指针比较；没有深比较
- 键类型没有运行时标签，不能像 CPython 那样混用任意 hashable

## [0.9] - 2026-11-09

这一版补上两件事：**lambda** 和**循环泄漏收集器**。闭包从「无环才能回收」
变成「不可达的环也能回收」，`lambda` 直接复用同一套闭包机制。

### 新增

- **lambda 脱糖**：解析后把 `lambda` 展开成插在语句前的合成嵌套 `def`
  （名字形如 `__py23$lambda_N`），表达式位置替换成这个名字。类型检查、
  闭包捕获、cell、引用计数全部复用普通函数路径
- **签名携带默认表达式**：`Param::default_expr` 指向 AST 默认值，通过
  函数值 / lambda 调用时也能内联填充默认参数（以前只认 `def` 名字）
- **保守标记-清扫收集器**：
  - RC 头扩成 64 字节：注册表链、dtor、GC release、负载大小、类型、标记位
  - 所有 `rc_alloc` 对象进堆注册表；分配量超过 2MB 自动触发一轮收集
  - 根：全局根表（生成代码在 `main` 里注册）、临时值栈、真实栈扫描、
    `setjmp` 捕获的 callee-saved 寄存器
  - 标记阶段保守扫描列表元素、对象字段、闭包 cell；清扫阶段先只对
    「不可达对象 → 可达对象」的引用做 rc 递减（`release` 回调），
    再统一释放，避免释放顺序问题
  - 入口函数退出时 `py23_gc_shutdown` 清空剩余堆对象（含环），
    LSan 零报告
- 生成代码：全局根表 `.Lgroots`、`py23_gc_init`、每个有堆字段的类多一个
  `.LgcrelN`（GC 用 release），对象分配传 `dtor + release`
- `tests/run/12_lambda.py`、`tests/run/13_gc.py`

### 修复

- **循环引用泄漏**：对象自环、相互递归的嵌套闭包、`a.next = a` 不再
  无限增长；16MB 限制下 20 万次环创建 / 30 万次闭包垃圾通过
- **unknown 容器泄漏**：`list[unknown]` 里的字符串/对象由 GC 扫到，
  不可达时回收（不再依赖 kind 2 的逐个 dec）
- **函数值默认参数**：`f = lambda x, y=3: ...`、把函数存进变量再调用
  默认参数不再报「不是字面量常量」
- **保守扫描与 ASan**：用真实 `%rsp` 而不是局部变量地址作为栈扫描起点，
  ASan 的 fake stack 不再被误扫

### 验证

- `make test`：11 正例 + 34 反例 + 13 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：编译器全量、全部运行用例、GC 压力程序零报告
- 严格警告零报告
- 16MB `RLIMIT_AS`：对象环 20 万次、相互递归闭包 20 万次、unknown 容器
  20 万次、闭包垃圾 30 万次，峰值 RSS 约 8MB；同时验证局部/全局存活列表
  和闭包跨 GC 不被误收
- 性能：纯整数路径不分配堆，GC 不触发，`fib(35)` / 2 亿循环与 0.8 持平

### 已知限制

- GC 是**保守**的：栈/寄存器里的整数如果恰好等于堆地址会被误认为根，
  只会延迟回收，不会提前释放
- unknown 容器里指向「仍有其它根」的对象的引用不会做 rc 递减；这些
  计数可能虚高，对象要等下一轮不可达后由 GC 回收
- 没有 `__del__`、弱引用、分代/增量；每次收集是 stop-the-world 全堆扫描
- 类体字段默认值里的 `lambda` 暂不脱糖（类体只放声明，`build` 会拒）
- `lambda` 在条件/循环表达式里会在包含它的语句前创建一次，而不是每次
  求值都新建；调用语义一致，函数对象身份/创建时机与 CPython 略有差异

## [0.8] - 2026-11-09

这一版加的是**闭包**：内层函数捕获外层局部变量，支持 `nonlocal`、
多实例（计数器工厂）、多层嵌套、自递归和方法里捕获 `self`。

### 新增

- **cell 模型**：被内层引用的局部变量在函数入口转成单元素 `list[T]`，
  cell 指针槽、闭包 env 和变量访问都复用现有列表/引用计数机制
- **`nonlocal`**：类型检查把声明预收集到函数级；赋值解析到最近的外层
  函数作用域，不再误当成本地变量
- **捕获分析**：类型检查在 `lookup` 命中祖先函数作用域时记录自由变量，
  降级期自底向上传播；子函数要的名字如果在本函数有绑定就生成 cell，
  否则继续往上要，多层嵌套也能拿到正确 cell
- **闭包对象**：负载 `[code, n_caps, cell…]`；无捕获函数用
  `.data.rel.ro` 静态闭包（rc = INT64_MIN），有捕获才运行时分配
- **函数值 ABI**：`Func` 变成堆类型；调用时从闭包 +0 取代码，闭包指针
  经 `%r10` 传给被调函数，入口存进 env 槽；方法调用显式清 `%r10`
- **自递归优化**：嵌套函数直接引用自己的名字时使用当前闭包/env，不生成
  self cell 环；`make_fact()` 这类递归工厂不再泄漏
- `tests/run/11_closure.py`、`tests/fail/f34_nonlocal.py`
- 运行时 `py23_closure_new` / 闭包析构器

### 修复

- **`global` / `nonlocal` 预收集**：两者都作用于整个函数，先收集再 hoist；
  避免局部预声明盖住外层绑定
- **函数值 RC**：函数值不再当成裸代码地址，存储/传参/返回/容器都按堆值
  参与引用计数；静态闭包永生，运行时跳过 inc/dec
- **`list[T]` 下标赋值漏 retain**：0.7 里 `l[i] = v` 只在运行时 dec 旧值，
  没有 retain 新值；存字符串/函数值时会悬空或提前释放。补上 retain
- IR dump 的 `func.addr` / `func.code` 打印 `imm` 而不是未使用的 `idx`

### 验证

- `make test`：11 正例 + 33 反例 + 11 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan：编译器全量 + 全部运行用例零报告
- 严格警告零报告
- 16MB `RLIMIT_AS` 下 20 万次闭包工厂创建（含字符串捕获和自递归）
  峰值 RSS 约 8MB
- 性能与 0.7 持平：`fib(35)` 约 0.06s（同机 CPython 3.13 约 0.6–1.0s）

### 已知限制

- `lambda` 仍未实现（语法能解析，`build` 拒绝）
- 相互递归的嵌套闭包会形成 cell 环，环不回收
- 嵌套函数直接引用自己的名字按「调用当前闭包」处理；父作用域之后重绑
  这个名字不会影响它（与 CPython 的晚绑定略有差异）
- 闭包捕获的是函数作用域变量；类作用域、生成器作用域不参与捕获

## [0.7] - 2026-11-08

这一版加的是**内存管理**：引用计数。所有权由静态类型决定，表达式临时值
进运行时临时值栈，语句 / 循环 / 函数边界按 mark 清理，不需要做活跃性分析。

### 新增

- **隐藏 RC 头**：每个堆对象前面 16 字节 `{rc, dtor}`，负载指针和所有字段
  偏移保持不变（字符串 `len` 仍在 +0，对象 vtable 仍在 +0）
- **字面量永生**：`.data.rel.ro` 里的字符串前置 `rc = INT64_MIN`，inc/dec 跳过
- **临时值栈**：`py23_rc_mark` / `py23_rc_temp` / `py23_rc_cleanup`；语句、
  `while` / `for`、函数返回值都按水位清理
- **所有权规则**：槽 / 全局 / 字段 / kind 1 列表元素持有引用；load /
  `field.get` / `list.get` 借用；形参入口 retain、出口释放；返回值 retain
- **容器元素类别**：`py23_list_new(cap, elem_kind)`；kind 1 的元素在覆盖、
  截断、销毁时由运行时 dec；kind 2（unknown / 混合）只 retain 不 release
- **对象析构器**：codegen 按 `IRClass::field_heap` 生成 `.LdtorN`，只 dec
  堆字段；`py23_obj_new(size, dtor)`
- **RC 脚手架消除**：语句 / 循环 / 函数完全没有临时堆值时，mark 和 cleanup
  在降级期删掉；纯整数代码的生成结果与 0.6.1 完全相同
- `tests/run/10_rc.py`：覆盖别名、字段、列表、返回值、字符串迭代、
  `continue` 跳过清理等场景
- 入口函数返回前释放全局变量并 `py23_rc_shutdown`，保持 ASan 零泄漏基线

### 修复

- **`list[str]` 下标被当成字符串下标**：结果类型都是 `str`，误判后把借用
  元素入临时栈，ASan 报 heap-use-after-free；改为看容器类型
- **`and`/`or` 在 unknown 类型下泄漏**：内部临时槽 retain 了堆值却永不释放；
  这类表达式内部槽改成纯借用，生命周期交给临时栈
- **`continue` 跳过语句清理**：`if ...: continue` 会把条件临时值留到下一轮；
  `while` / `for` 都加循环水位，body / incr / 退出路径兜底
- **遍历字符串泄漏单字节串**：`for ch in "abc"` 的下标是 `str_char_at`
  新建的串，之前按借用处理；现在入临时栈
- `-Wshadow`：类字段继承链的循环变量重名

### 验证

- `make test`：11 正例 + 32 反例 + 10 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan：编译器全量检查 + 全部运行用例（含生成代码走 ASan 运行时）
  零报告
- 严格警告零报告
- 16MB `RLIMIT_AS` 下完成 100 万次拼接、100 万次对象分配、30 万次带字符串
  字段的对象分配、30 万次列表构造，峰值 RSS 约 8MB
- 性能（同机容器）：整数 `fib(35)` 与 2 亿次循环与 0.6.1 持平；2000 万次
  方法调用因 self 形参 inc/dec 慢约 30%

### 已知限制

- **循环引用不回收**：`a.next = a` 会一直活着，没有标记-清扫
- **unknown 值泄漏**：unknown 标量、`list[unknown]`、混合类型元组元素不参与
  计数
- 入口函数退出时会释放全局变量；程序中途退出（panic）不保证清理

## [0.6.1] - 2026-11-08

对象模型落地后跑标准库基线，发现类的检查把真实代码判死了一大片（35693 个语法通过的文件里 19064 个类型报错）。这一版全是**放宽**：按「宁可漏报，不可误杀」把静态上看不见的东西降级成 unknown。

### 修复

- **`class A(object)` 被判死**：`object` 在内置表里是函数（`object()` 造实例），不是 `ClassInfo`。基类是内建类型名时按根类处理
- **外部基类的方法全丢**：`class T(unittest.TestCase)` 之后 `self.assertEqual(...)` 报「没有此方法」。`ClassInfo` 新增 `open` 标记（整条继承链生效），查不到的成员按 unknown 放过
- **`self.x = ...` 要求先写标注**：现在就地补成隐式字段，类型按首次赋值推断；`None` 与 unknown 不参与推断（否则后面每次 `self.x.foo()` 都被判死）
- **类体里的普通语句被拒**：`x = 1` / `if` / `for` 改成标记「没建模」，并把类标成 open（类属性查不到）；`build` 时才拒绝
- **多继承被拒**：改成标记「没建模」+ `build` 拒绝。标准库里的测试类大量是 `class T(Mixin, TestCase)`
- **`self.method()` 拼错也报错 → 现在放过**（mixin 组合出来的成员静态看不见），类外的 `p.m()` 仍然检查
- **骨架类被当成空类**：类定义在引用点之后时（`def f(): return C(1)` 写在 `class C` 前面），骨架的 `complete == false`，构造参数和成员查找一律放宽
- **嵌套函数里的 `self`**：`def last(): self.x` 这类闭包捕获，之前因为 `cur_self_name_` 被 `std::move` 移空而失效
- **隐式字段的类型被后来的赋值覆盖**：`self.x = f()` 后 `self.x = None` 把 unknown 改成了 None，`join` 语义修正为 unknown 吸收
- **`@staticmethod` 的零参方法**报「缺 self」：带装饰器的方法不检查 self，也不给调用签名（实参不检查）
- **`MyList[int]` 泛型下标**被报「不可下标」：类对象下标返回 unknown
- **`duplicate method`**：`@property` 的 getter / setter 是同名两个 `def`，Python 里合法（后者覆盖）。现在两者都检查、共用一个槽，vtable 里后者胜出
- **`self.x = v` 撞上同名方法**：可能是 `@property` 的 setter，静态分不清，放过并标记；类外的 `p.x = v` 仍然报错
- **`c.__annotations__ = d`**：给 dunder 属性赋值不再报「不能给方法赋值」
- **类实例的运算符 / 解包 / 比较 / `len()`**：`Fraction - Fraction`、`enum.FlagBoundary` 解包这类全被误判。类可能重载了 dunder 协议（重载还可能在外部基类里），我们没建模就一律放过
- **`set[a] < set[b]`**：集合子集比较放行
- `@property` 返回的数值参与一元运算（`bad operand type for unary operator`）同样放过

### 新增

- `tests/unsupported/` 测试类：`check` 必须打标记、`build` 必须拒绝，防止「标了没建模却悄悄编译过去」
- `tests/run/09_class.py` 增加无标注字段的用例

## [0.6] - 2026-11-08

这一版加的是**对象模型**：类、字段、方法、单继承、虚调用。类在编译期定死布局，属性访问退化成一条固定偏移的读。

### 新增

- `class` 语句：方法定义 + 带标注的字段声明 + `pass` + 文档字符串
- 字段偏移在编译期确定：对象 = `[vtable 指针][字段…]`，`p.x` 是 `movq 8(%rax), %rax`
- 单继承：子类字段接在父类之后，vtable 槽位前缀与父类一致；覆盖沿用同一槽
- 虚调用 `vcall`：从对象头取 vtable，按常量槽号间接跳转，父类类型能调到子类实现
- 构造：`Cls(...)` = 分配 + 初始化带初值的字段 + 调 `__init__`；支持默认参数与关键字参数
- 字段初值（`x: int = 0`）在每次构造时求值，实例之间互不影响
- 类型系统新增 `TyKind::Class`：子类可赋给父类、`join` 取较浅的公共上界、`isinstance` 之外的多态检查
- `Optional[Cls]` 上的字段访问与方法调用带运行时空指针检查
- `print(obj)` 输出 `<Point object>`
- IR 新指令：`obj.new` / `field.get` / `field.set` / `vcall`；`IRModule::classes`
- 运行时 `py23_obj_new(size)`（`calloc`，全零）
- `tests/09_class.py`（类型正例）、`tests/run/09_class.py`（运行用例）、6 个类相关反例

### 修复

- **类型系统与类信息的 shared_ptr 环**：`ClassInfo` 的字段类型指回 `Type`，`Type` 又指回 `ClassInfo`，ASan 报 2 万字节泄漏。`Type::cls` 与 `ClassInfo::base` 改成裸指针，所有权归 `TypeChecker::all_classes_`
- 顶层函数签名引用「后面才定义的类」时解析成 `unknown`：`collect_toplevel` 现在先给每个类建骨架，`classdef` 再往同一个对象里填
- `Optional[Cls]` 上的属性赋值（`n.next.next = x`）被误报成「没有可赋值的属性」

### 验证

- `make test`：11 正例 + 33 反例 + 9 个运行用例
- 差分测试 8 个种子、1600 个程序，零差异
- ASan + UBSan 零报告（含零泄漏）
- 严格警告（`-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual`）零报告
- 性能：2000 万次方法调用 0.09s（CPython 3.88s）

## [0.5] - 2026-11-08

这一版只做一件事：**把值从栈上搬进寄存器**。IR 一行没改，全部在 `codegen.cpp`。

### 新增

- 变量提升：按 `load.slot` + `store.slot` 次数排序，最热的 5 个局部变量整函数常驻 `rbx` / `r12`–`r15`（callee-saved，跨调用不需要 spill）
- 块内别名：`%v = load.slot x` 且 %v 只被读一次时，%v 直接指向 x 的寄存器，load 整条消失
- 结果融合：`%v = ...` 紧跟 `store.slot x, %v` 时，%v 就地写进 x 的寄存器，store 整条消失
- 常量延迟：`%v = const.int N` 只被读一次时不开指令，使用时直接当立即数
- 比较连分支：`%v = cmp` 紧跟 `branch %v` 时直接发条件跳转，省掉 `setCC` + `movzbq` + 一次内存往返
- 非参数变量在序言清零（`x: int` 这种纯声明原先会读到栈上的垃圾）
- `tests/run/08_regalloc.py`：钉住 callee-saved 保护、多变量溢出、多参数、六种比较、嵌套循环

### 修复

- **局部区起点压住了 callee-saved 保存槽**：变量槽从 `-8(%rbp)` 开始，正好是被 `push` 下来的 `%rbx`；函数返回时把它弹回调用方，调用方的变量凭空变成被调函数的中间值。差分测试抓到的现象是「调完 `f0` 之后，`v3` 从 0 变成 -19」
- **融合目标与 LoadSlot 别名冲突**：`store.slot s, %v` 被融合吞掉时，如果 %v 走别名路径，变量寄存器根本不会被写，赋值静默丢失。`gen_load_slot` 现在在 `dst_loc_` 非空时强制真的写过去
- `cmp` 出现 mem-to-mem 操作数（x86 不允许），`cvtsi2sd` 拿到立即数（x86 不允许）
- push 奇数个 callee-saved 时 `call` 前 `rsp` 不是 16 对齐

### 性能

| 程序 | 0.4 | 0.5 | CPython 3.11 |
|---|---|---|---|
| `fib(35)` | 0.08s | 0.06s | 1.00s |
| 2 亿次 `while` 累加 | 0.34s | 0.06s | 7.00s |

`fib` 只快 1.4 倍——它几乎全是函数调用，热变量本来就只有 `n` 一个。整数循环快 5.7 倍，因为循环变量和累加器现在整轮都待在寄存器里。

### 验证

- `make test`：10 正例 + 27 反例 + 8 个运行用例
- 差分测试 10 个种子、2000 个程序，零差异
- ASan + UBSan + `-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual` 零报告

## [0.4] - 2026-11-08

这一版的主线是**把 IR 到可执行文件这条路走通并验证**，同时补齐文档体系。

### 新增

- IR 层：基本块 + 三地址码 + 虚拟寄存器，类型挂在指令上（`src/ir.h`、`src/ir.cpp`）
- 降级器：`src/lower.cpp`，覆盖标量、字符串、列表、元组、函数、控制流、`print`、f-string
- 代码生成：`src/codegen.cpp`，System V AMD64 调用约定，栈帧 + vreg 槽位线性扫描复用
- 运行时：`runtime/rt.c`，字符串/列表/输出/panic，纯 C11
- CLI：`ir` / `asm` / `build` / `run` 四个模式
- `print` 的 `sep=` / `end=`（降级期展开，支持任意字符串表达式）
- 字符串大小比较（`<` `<=` `>` `>=`，按 UTF-8 字节序，与码点序一致）
- 标注的类型强制：`x: int = 1` 之后再 `x = "s"` 会报错
- 数值隐式转换：int → float 在赋值、传参、返回、容器字面量四处生效
- `tests/run/` 运行用例（7 个）、`tests/diff/` 差分测试（4 个生成器 + 统一 runner）
- `docs/` 文档体系（架构、语言、IR、后端、运行时、兼容性、开发、路线图）

### 修复

- 浮点 `%` 的符号：`-7.5 % 2` 得到 `-1.5`（`fmod` 语义），应为 `0.5`；零的符号也要取除数的
- 浮点转字符串：`10.0` 被排成 `1e+01`（`%g` 的切换阈值与 CPython 不同）
- `"a" < "b"` 生成空指令、恒为 `False`
- `x: float = 1` 把整数位模式当 double 读，打印出 `5e-324`
- 预扫描先占位的符号吃掉标注标志，导致标注在后续赋值中失效
- `Constant::boolean/none/ellipsis` 没有行列号，诊断指向 1:1
- 内置调用的关键字值被求值两次，同一个未定义名报两遍
- 复合赋值 `*=` 被降级成 `+=`（`Tok` 的两套记号没映射）
- f-string 里的表达式从未被类型检查
- vreg 槽位复用忽略控制流回边，循环第二轮读到垃圾（段错误）
- 整数除零与 `INT64_MIN / -1` 触发 `SIGFPE`，改为带消息的 panic
- `py23 run` 在源码旁边留下可执行文件，改为临时文件、跑完即删
- 字符串常量放 `.rodata` 触发 PIE 重定位警告，改 `.data.rel.ro`

### 移除

- 死指令 `IrOp::Print`（降级器从不产出，codegen 里是空 `case`）
- 无人调用的 `py23_pow_f64` / `py23_fmod_f64` 包装

## [0.3] - 阶段

- 增量构建：源码拆成 6 个翻译单元，`-MMD -MP` 依赖跟踪
- 前端大规模质检：标准库 + site-packages 38582 个文件作为基线
- 类型检查补强：分支合并、`is None` 收窄、`try` 体类型处理、容器元素扩宽

## [0.2] - 阶段

- 类型检查器：标注解析、推断、`unknown` 放行策略、内置函数与方法签名表
- 反例测试集（`tests/fail/`）

## [0.1] - 阶段

- 缩进敏感的词法分析器，覆盖 Python 3 全部关键字与运算符
- 递归下降解析器 + 完整 AST + 缩进式 dump
- PEP 701 f-string 解析（同引号嵌套、自文档化、spec 嵌套）
- 正例测试集（`tests/`）
