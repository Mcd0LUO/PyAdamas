# IR 参考

形态：**函数 → 基本块 → 线性三地址码 + 虚拟寄存器**。每个值有编号，每个变量有固定槽位，指令自带类型。

```
%7  = load.global g1        ; int
%8  = load.global g0        ; func(int) -> int
%9  = load.global g2        ; int
%10 = call %8(%9)           ; int
%11 = bin.add %7, %10       ; int
```

## 为什么不是 SSA

Python 语义里变量天生可变，循环里反复赋值是常态。一上来就构造 phi 会把 `for` / `while` / `break` 的降级搞得非常绕，而收益要等到真正的优化遍才兑现。

现在用「可变槽位 + vreg」：中间值有编号（不可变），变量有固定槽（可变）。后端按槽分配栈帧，规则简单、不会错。要加 SSA 是在这层之上再构造一次，不必回头改前端。

代价是同一个变量在循环里每轮走一次 `store.slot` / `load.slot`。等加了块内寄存器缓存，这些 load/store 大部分会被消掉。

## 数据结构

`src/ir.h`。

| 结构 | 说明 |
|---|---|
| `Inst` | 一条指令。`op` + 至多三个源操作数 `a/b/c` + `dst` + `imm/fimm/idx/target/target2` + `args` + `ty` |
| `Block` | 名字 + 指令序列，`terminated()` 判断末尾是不是跳转/分支/返回 |
| `IRFunction` | 名字、形参、返回类型、`slot_ty`（局部槽类型）、`vreg_ty`（每个 vreg 的类型）、基本块、`env_slot`（有捕获时存闭包指针的槽） |
| `IRGlobal` | 一个全局变量；数据段里就是一个 `.quad`，初值 0 同时也是 `None` |
| `IRClass` | 名字、vtable（槽 → 函数下标）、字段数（含继承）、`field_heap`（每个字段是否堆类型，供析构器使用）、`qualname`（模块限定类名）、`mro`（C3 类下标，self 在前）、`own_slots`（本类声明的方法槽） |
| `IRModule` | 函数表、全局表、字符串池、类表、`entry`（顶层代码编译成的函数下标） |

`Inst` 里有个容易踩的点：**`idx` 的含义随 `op` 变**——`const.str` 是字符串池下标，`load.slot` 是槽号，`load.global` 是全局号，`func.addr` 是函数号。类型上看不出区别，读指令时要先看 `op`。

`IRModule::add_func` 返回裸指针，函数表用 `vector<unique_ptr<IRFunction>>` 持有：降级过程中会不断新增函数（嵌套 `def`），`vector<IRFunction>` 扩容会让外部持有的指针失效。

## 指令集

`ty` 一律是结果类型；没有结果的指令（`store.*`、`call.rt` 到 void 函数、`jump`）`dst == kNoReg`。

| 指令 | 操作数 | 语义 | 后端 |
|---|---|---|---|
| `const.int dst, imm` | `imm: i64` | `dst = imm` | `movq $imm` |
| `const.float dst, fimm` | `fimm: double` | `dst = fimm` | `movabsq` + `movq` + `movsd` |
| `const.str dst, idx` | `idx: 字符串池` | `dst = &str_obj[idx]` | `leaq .LstrN(%rip)` |
| `const.none dst` | — | `dst = 0` | `xorl` 或直接存 0 |
| `func.addr dst, idx` | `idx: 函数号` | `dst = &static_closure`（无捕获函数值） | `leaq .LclosN(%rip)` |
| `func.code dst, idx` | `idx: 函数号` | `dst = 裸代码地址`（只给 `closure.new` 用） | `leaq .LfN(%rip)` |
| `mov dst, a` | — | `dst = a` | 两条 `movq`。**当前没有降级路径产生它**，留着给后续优化遍用 |
| `bin dst, a, b` | `bop` | 按 `bop` 做二元运算 | 整数 `addq` 系 / 浮点 `addsd` 系 |
| `un dst, a` | `uop` | `neg/pos/bitnot/not` | `negq` / 空 / `notq` / 真值取反 |
| `cmp dst, a, b` | `cop` | 比较，结果 bool | `cmpq`+`setcc` / `ucomisd`+`setcc` |
| `convert dst, a` | `ty` 是目标类型 | 数值/字符串转换 | `cvtsi2sdq` / `cvttsd2siq` / `call py23_str_from_*` |
| `load.slot dst, idx` | `idx: 槽号` | 读局部槽 | `movq off(%rbp)` |
| `store.slot idx, a` | — | 写局部槽 | `movq %rax, off(%rbp)` |
| `load.global dst, idx` | `idx: 全局号` | 读全局 | `movq .LgN(%rip)` |
| `store.global idx, a` | — | 写全局 | `movq %rax, .LgN(%rip)` |
| `list.new dst, args...` | `imm: 元素 kind` | 按 `args` 构造列表（0 非堆 / 1 堆 / 2 unknown 或混合） | 逐个 `py23_list_append` |
| `tuple.new dst, args...` | `imm: 元素 kind` | 按 `args` 构造元组 | 同上（目前与 list 同表示） |
| `index.get dst, a, b` | — | `dst = a[b]` | list/tuple 内联快路径，str 走 `py23_str_char_at` |
| `index.get.unchecked dst, a, b` | — | 同 `index.get`，但调用方已证明 `0 <= b < len(a)` | 直接读 `data`，无边界分支 |
| `index.set a, b, c` | — | `a[b] = c` | `py23_list_set` |
| `len dst, a` | — | `dst = len(a)` | 一条 `movq (%rax), %rax` |
| `obj.new dst, imm` | `imm: 类下标` | 分配对象并写入 vtable 指针；析构器取自 `IRClass::field_heap` | `call py23_obj_new` + `leaq .LvtN`；`.LvtN` 前有 5 字类元数据 |
| `field.get dst, a, imm` | `imm: 字节偏移` | `dst = a->field` | 一条 `movq off(%rax), %rax` |
| `field.set a, imm, b` | `imm: 字节偏移` | `a->field = b` | `movq %rcx, off(%rax)` |
| `vcall dst, a, args...` | `imm: vtable 槽` | `dst = a->vtable[imm](a, args...)` | 取表、取槽、间接跳转 |
| `vcall.static dst, a, args...` | `imm: vtable 槽` | 静态方法：不传 self | 同 vcall，参数序列少一个 self |
| `vcall.super dst, a, args...` | `imm: 槽`，`idx: after 类下标`，`b: 方法名` | 按运行期 MRO 找下一实现 | 先 `py23_super_lookup`，再照 vcall 传 self |
| `call.direct dst, imm, args...` | `imm: 函数下标` | 直接调用裸函数（类名上的静态方法） | `leaq .LfN(%rip)` + `call` |
| `call dst, a, args...` | `a: 函数值` | 间接调用 | `call *%r11` |
| `call.rt args..., imm` | `imm: RtFn` | 调运行时函数 | `call py23_xxx` |
| `setjmp dst, a` | `a: handler 记录` | `dst = setjmp(a)`；异常 longjmp 回来时为非 0 | `call setjmp` |
| `import.module imm` | `imm: imports 下标` | 确保模块已初始化 | `py23_import_module(ready, init)` |
| `slot.addr dst, idx` | `idx: 槽号` | `dst = &slot`（栈槽或生成器帧槽） | `leaq off(%rbp/%r15)` |
| `gen.new dst, imm` | `imm: generators 下标` | 分配生成器帧，写入 resume 代码和 GC release | `call py23_gen_new` |
| `gen.set gen, s, v` | `imm: 槽号` | 创建期把实参/闭包写进帧槽 | `movq v, 8*(6+s)(gen)` |
| `gen.state dst` | — | 读帧 state | `movq 0(%r15)` |
| `gen.closing dst` | — | 读帧 closing 标志 | `movq 40(%r15)` |
| `gen.exc dst` | — | 读待注入异常 | `movq 64(%r15)` |
| `gen.sent dst` | — | 读本次 resume 传入的值 | `movq 16(%r15)` |
| `gen.yield a, state` | `a == kNoReg` 表示 None | 写 out、存 state、返回（终止指令） | 写 `24(%r15)` / `0(%r15)` + 尾声 |
| `gen.await a, state` | — | 写 out、state，status=3（awaiting），返回（终止指令） | 同上 + 写 status |
| `gen.rethrow` | — | 在 resume 帧内抛出注入的异常（终止指令，noreturn） | `call py23_coro_rethrow` |
| `gen.done [a]` | `a` 非空时写返回值 | status=1、返回（终止指令） | 写 `24(%r15)` / `8(%r15)` + 尾声 |
| `jump target` | — | 无条件跳转 | `jmp` |
| `branch a, target, target2` | — | `a` 为真跳 `target` | 按类型测试真值 |
| `return a` | `a == kNoReg` 表示 `None` | 返回 | `rax` / `xmm0` |

### 运行时函数表

`RtFn` 是**枚举**，后端按枚举直接生成调用，不经过符号查找，用户代码也不能引用。清单见 [runtime.md](runtime.md#函数表)。

引用计数不走 IR 新指令，全部是 `call.rt`：`py23_rc_inc` / `py23_rc_dec`
管堆值，`py23_rc_inc_unknown` 对类型不可见的值按堆注册表保守 retain，
`py23_rc_temp` / `py23_rc_mark` / `py23_rc_cleanup` 管表达式临时值栈，
`py23_rc_shutdown` 在入口函数退出时收尾。循环收集器也是 `call.rt`：
`py23_gc_shutdown` 在入口退出时清空剩余堆；周期性收集由 `rc_alloc` 在
运行时内部按 2MB 阈值触发，IR 看不到。

闭包对象用 `py23_closure_new(code, n_caps)` 分配，负载是
`[code, n_caps, cell…]`；`Func` 是堆类型，静态闭包 rc 为永生。
`func.code` 只在创建堆闭包时取裸代码地址，`func.addr` 一律指静态闭包。

异常同样走 `call.rt` + 新指令 `setjmp`：`py23_handler_push` 压入
`{jmp_buf, prev, temp_mark}`，`setjmp` 指令把 handler 记录交给 libc；
`raise` 设置全局异常后 longjmp 回来，landing pad 用
`py23_handler_leave` 清理临时栈，再用 `py23_exc_matches` /
`py23_exc_accept` / `py23_exc_done` / `py23_exc_reraise` 分派。
`has_handlers` 的函数关闭槽位寄存器提升，避免 longjmp 回滚变量。

生成器帧的布局由 `IRGenerator` 描述：`resume` 是状态机函数下标，
`slot_heap` 标记哪些帧槽在销毁时需要 `rc_dec`，`frame_kind` 决定帧里
的 kind 标记（0 生成器 / 1 协程 / 2 异步生成器）。帧负载的前 10 个字是
`[state, status, sent, out, resume, closing, kind, waiter, exc, cur]`，槽位从
下标 10 开始；`gen.new` 只出现在创建包装里，resume 函数里用固定偏移访问
头部。协程恢复时 `exc` 非空就走异常注入分发：按 state 重挂 handler 后
`gen.rethrow`。Task 没有新指令，全部走 `asyncio.*` 的 `call.rt`。

另有三类符号不走 `RtFn`，由后端直接按名字调用：`py23_pow_i64`、`py23_str_eq`、`py23_str_cmp`（以及 libm 的 `floor` / `fmod` / `pow`）。这类是「参数形态固定、没有枚举必要」的辅助函数。

## IR 优化遍

O2 在降级完成后运行 `optimize_ir`（O0/O1 不运行）：

- **常量折叠**：整数/浮点 Add/Sub/Mul、位运算、比较、一元 Neg/BitNot/Not、
  数值 Convert 直接折成常量。Div/Mod/Pow、字符串/容器操作和可能抛异常的
  指令不折叠。
- **分支化简**：常量条件的 `branch` 改成 `jump`；不可达块保留但不再执行。
- **DCE**：纯标量指令（常量、局部/全局 load、安全算术、比较、数值转换、
  `len`、`index.get.unchecked` 等）在结果无人使用时删除，迭代到不动点。
  可能抛异常的 checked index、容器分配和调用不删除。
- **静态小函数内联**：调用点的函数值来自 `func.addr`（或入口模块里只赋值
  一次的模块全局）且 callee 无捕获、无 handler、非生成器、非可变参、非递归时，用
  callee 的 IR 克隆替换调用。只读形参的 `load.slot` 直接指向调用点实参；
  单 `return` 直接写调用结果 vreg；克隆块紧挨调用点插入，单块直返函数
  整段就地插入，入口/返回跳转由 O2 下落消除吃掉。每函数上限 24 个内联点
  / 256 条指令增量。
- **静态调用直接化**：内联之后，能静态解析（`func.addr` / 单次赋值的模块
  全局）且 callee 不读闭包环境的剩余调用改成 `call.direct`，直接
  `call .LfN`，不再装载全局闭包指针。
- **二次折叠**：内联和直接化之后再跑一遍常量折叠 + DCE，折掉内联带进来的
  实参常量，并清掉失效的 `load.global` / `func.addr`。
- **循环不变量外提（LICM）**：按支配关系找自然循环，把循环内不抛异常的
  不变量搬进新增前置块（循环外前驱先跳到它，它再跳循环头，只在进入循环
  时执行一次）。可搬常量、`mov`、数值 `un`/`cmp`/安全 `bin`/数值 `convert`、
  循环内没被写过的槽装载、循环内无调用且没被写过的全局装载。检查型下标、
  `div/mod/pow`、字符串/容器运算不搬；含 handler 的函数和生成器帧跳过。
- **检查型下标外提（循环版本化）**：`index.get` 的 base 和 index 都是
  循环不变量、循环内又没有调用和下标写入时，在循环入口检一次
  `0 <= idx < len(base)`：通过执行无检查的循环副本（`index.get.unchecked`），
  不通过（负下标/越界）走原循环。两版共用槽，只有一版执行。

## 后端的局部优化

优化按 `-O0/-O1/-O2` 分档（默认 O2），O0 关闭下列大部分项目，O1 打开
P0/P1/P2，O2 再加控制流与小函数优化：

- **O2 SIMD 归约**：int/bool 的 `sum`、int 的 `min/max` 降级为运行时
  AVX2 内核；float `sum` 只在 `--fast-math` 时使用 SIMD（分组求和会改
  舍入）。
- **O2 控制流**：跳到下一块的 `jmp` 省略；条件分支一边是 fallthrough 时
  发反向条件跳转；非变参调用不再清 `%eax`；序言用 must 数据流只清零
  可能读到未赋值值的槽（取地址槽、handler、生成器帧保守清零）。

- **list/tuple 下标内联**：`IndexLoad` / `IndexStore` 不再无条件调
  `py23_list_get/set`。快路径直接读 `len`/`data` 并用无符号比较挡越界；
  负索引归一化也在内联路径里。慢路径只处理越界，调用运行时抛异常，
  不会返回。浮点元素直接走 `movsd`，跳过 GPR 往返。
- **store.slot 结果融合**：`v = expr` 且 `v` 块内只用一次时，`expr` 的
  结果直接写变量位置（提升寄存器或栈槽），省掉临时 vreg 槽。
- **LoadSlot 内存别名**：加载结果块内只用一次、且变量在“读之后、第一次
  使用之前”没有再被赋值时，vreg 直接指向变量槽/寄存器，加载指令消失。
- **浮点槽提升**：按 Load/Store 热度把 float 局部提升到 xmm8–15
  （xmm0–7 留给参数和临时）。所有会正常返回的 call 前后统一
  `spill_fp_slots` / `reload_fp_slots`；含 handler 的函数、生成器帧
  不提升。
- **免检查下标**：`for x in list/tuple`、推导式、`sum`/`min`/`max`/
  `any`/`all`/`print(*seq)` 的元素装载在能证明 `0 <= i < len` 时改成
  `index.get.unchecked`。普通 for/推导式用降级期副作用计数判断循环体
  是否会调用、`del`、切片赋值或触发属性/with 等可能改变长度的操作；
  生成器帧与异步推导式不参与。
  另外，`for k in range(len(a))`（含 `range(0, len(a))`、步长 1）在循环体
  没有副作用、也没给 `a`/`k` 赋值时，体内形如 `a[k]` 的读下标同样改成
  `index.get.unchecked`；一旦有人写 `a` 或 `k` 的槽，提示立即作废。
- **浮点结果就地算 / 临时融合**：目标已经是 xmm 时直接
  `addsd/sub/mul/div` 到目标；结果只被下一条浮点二元指令使用时先留在
  `%xmm1`，省一次临时槽写入和读回。

## 不变量

降级器必须保证，后端可以依赖：

1. 每个基本块以 `jump` / `branch` / `return` / `gen.yield` / `gen.await` / `gen.rethrow` / `gen.done` 结尾。（codegen 有兜底，但那条路径不该被触发。）
2. 指令的 `dst` 类型等于 `vreg_ty[dst]`；`def()` 是唯一分配 vreg 的入口，会自动登记类型。
3. `store.slot` / `store.global` 的值类型与目标槽类型一致——由 `coerce()` 在写入前补 `convert` 保证。
4. 形参槽排在最前面，槽号等于形参下标（后端按这个顺序搬寄存器）。
5. `entry` 一定指向「模块顶层代码」编译出的那个函数，名字是 `__py23_main`。
6. 同一条指令的 `idx` 只有一个含义，但含义由 `op` 决定（见上表）。
7. `vcall` 的 `a` 是接收者（self），`args` 里**不含** self——后端把它装进第一个整型参数位。
8. 类表在降级开始时就按 AST 顺序建好（父类在前），`obj.new` 的 `imm` 是 `IRModule::classes` 的下标；vtable 里 `-1` 表示该槽没填上，正常不会出现（方法编译失败时整个编译已经报错）。

## 降级规则

`src/lower*.cpp`。

### 语句

| AST | 降级 |
|---|---|
| `Expr` | 求值后丢弃结果 |
| `Assign` | 求值右侧一次，按目标分配（名字 / 解包 / 下标） |
| `AugAssign` | `target = target <op> value`，浮点目标先做统一提升 |
| `AnnAssign` | 有初值时同 `Assign`；纯声明不生成代码（槽本来就是 0） |
| `If` / `While` | `br cond, body, else`；`else` 块单独成块，`while` 的 `else` 在条件为假时进入 |
| `For(range)` | 特化：独立的计数器槽 + 用户变量槽，每轮把计数器写进用户变量 |
| `For(序列)` | 通用：`len` + 下标 + 计数器；元素类型来自序列的元素类型 |
| `Return` | `return a`；有声明返回类型时先 `coerce` |
| `Break` / `Continue` | 跳到循环的 `end` / `incr` 块；先按逆序发射跨过的 with / finally / handler 清理 |
| `With` | 每个项求值一次，调 `__enter__` 并绑定 `as`；每层一个 handler + setjmp。正常/跳转路径按逆序调 `__exit__(None,None,None)`，异常 landing 调 `__exit__(type,message,None)`，返回真值则 `exc_clear` 抑制 |
| `Try` | 外层 handler + setjmp；正常路径 `handler_leave` 后进 `else`；异常 landing 清理临时值栈，按 handler 顺序匹配，命中后 `exc_accept` 绑定 `as e`，处理完 `exc_done`；没有匹配则 `exc_reraise` |
| `Try(finally)` | 再用一层 handler 包住核心 try；正常、异常和提前退出路径都执行 finalbody，异常路径最后 `exc_reraise` |
| `Raise` | `raise X` / `raise X(msg)` 调 `py23_raise`；bare `raise` 调 `py23_reraise`；只接受内置异常名 |
| `Import` / `ImportFrom` | 发 `import.module` 确保模块初始化；`import m` 只登记编译期别名，`from m import x` 加载外部全局并快照到本地/全局槽 |
| `Global` | 只影响名字解析（该名字走全局读写） |
| `Assert` | `br cond, ok, fail`；`fail` 里拼消息后 `raise AssertionError` |
| `ClassDef` | 类表在正式降级前预扫描建好；这里只编译本类方法并填 vtable |

### 表达式

| AST | 降级 |
|---|---|
| 常量 | 直接产出 `const.*`；字符串进池 |
| `Name` | 局部 `load.slot`，否则 `load.global` |
| `BinOp` | 字符串 `+` 走 `py23_str_concat`；数值走 `bin` |
| `BoolOp` | 短路：先求左值，按真值分支，两条路径都把结果写进同一个临时槽 |
| `Compare` | 单次比较一条 `cmp`；链式比较把中间值存槽，逐个比较并短路 |
| `Call` | 内置特化（`print` / `len` / `int` / `float` / `str` / `bool` / `range`）→ 否则按 `FuncInfo` 重排实参 → `call`；`*` 打包成 list、`**` 打包成 dict 作为末尾形参传入 |
| `Subscript` | `index.get`；切片读走 `list_slice` / `str_slice`；切片赋值走 `list_slice_assign` |
| `Attribute` | 字段读 → `field.get`（偏移编译期确定）；方法名不加 `()` 报错 |
| `Call(类名)` | `obj.new` → 逐个字段初值 `field.set` → 有 `__init__` 时 `vcall` |
| `Call(对象方法)` | `vcall`，`self` 放在 `a`，`args` 只放其余实参 |
| 列表/元组字面量 | `list.new` / `tuple.new`，元素按声明的元素类型 `coerce` |
| 推导式 | 结果容器 + 逐层计数器循环 + 条件分支；循环变量放在降级器自己的作用域槽里；含 `async for` 的推导式把累加器/循环状态落帧槽，每个异步子句插一层 `StopAsyncIteration` handler |
| `YieldFrom` | 委托生成器：`gen.send_opt` 循环 + `gen.yield`；序列委托：计数器 + 下标循环；子生成器放帧槽，close 时随槽释放 |
| `IfExp` | 三块 + 临时槽 |
| `JoinedStr` | `str_concat` 链，每个 `{...}` 先 `convert` 到 `str` |
| `NamedExpr` | 求值后 `assign_to`，结果就是该值 |
| `Lambda` | 普通 lambda 在解析后已脱糖成嵌套 `def`；只有类体字段默认值里的 lambda 会走到这里报错 |

### 几处细节

**循环变量私有**：`for i in range(n)` 用独立计数器槽，每轮把计数器写进 `i`。Python 里在循环体给 `i` 重新赋值不影响迭代，两者必须分开。

**参数重排**：按 `FuncInfo` 把位置参数和关键字参数绑到形参槽上，缺省的用字面量默认值填。签名不可知时（函数值调用）只接受纯位置参数，且不能带 `*` / `**`。`*args` / `**kwargs` 形参接在普通形参之后，调用点把多出来的位置实参装进 `list.new`、关键字实参装进 `dict.new`，再按普通实参传入。`*` 右侧是定长 tuple 时静态展开成逐元素下标读。绑完之后按形参类型逐个 `coerce`。

**函数装饰器**：`def f` 正常编译并把原始函数值绑定到 `f`，然后按逆序对每个装饰器发一次普通闭包调用 `d(f)`，结果重新绑定到 `f`；没有新增 IR 指令。

**异常展开**：`try` / `with` / `finally` 各自发射 `handler_push` → `setjmp` → `branch`；landing pad 先 `handler_leave`（弹记录 + `rc_cleanup` 到记录水位），再运行 `except` 匹配 / `__exit__` / finalbody。`return` / `break` / `continue` 不经过 longjmp，而是由统一 cleanup 栈直接发射 `handler_discard`、`__exit__` 和 finalbody，因此正常退出路径仍然精确释放槽位。

**模块**：模块全局带链接符号 `py23$m$g$x`；外部模块全局是 `external = true` 的 `IRGlobal`，不分配存储、不进根表、出口也不释放。`import.module` 只带模块下标，符号命名由后端按模块名推导。模块顶层函数成为导出符号 `py23$m$init`，返回 `None` 且不执行 `GcShutdown` / `RcShutdown`。

**`print` 不是函数**：就地展开成一串 `call.rt`，`sep` / `end` 求值后交给 `py23_print_str` 原样写出。

**`coerce` 的边界**：只处理数值域之间的转换（int/bool ↔ float）。指针类之间的差异（`list[int]` → `list[float]`）需要逐元素转换并重新分配，现阶段直接报错，不允许静默产生错误的位模式。

**类在降级前先建表**：`Lowerer::run` 里先扫一遍语句块（不进函数体）把每个 `ClassDef` 变成一条 `IRClass`，vtable 槽先填 -1。这样方法体里构造另一个类时，类下标和对象大小都已经确定。方法槽在模块内按名字全局分配，所有 vtable 等长。真正编译 `ClassDef` 时先按 C3 MRO 从前向后填继承来的槽（每个槽只由最先声明它的类占用），再编译本类方法覆盖对应槽。`field_heap` 也在这时按父类到子类的布局顺序填好，codegen 据此生成析构器。

**lambda 的位置**：解析后、类型检查前，`lambda` 被展开成插在包含它的
语句之前的合成 `def`，表达式位置换成名字。这样类型检查和降级只需要
处理 `FunctionDef`，捕获/引用计数规则不重复。

**闭包的降级**：

- 类型检查在 `lookup` 命中祖先函数作用域时记录自由变量；降级器自底向上
  传播，得到每个函数的 `env`（从祖先捕获）和 `cells`（被子孙捕获的局部）
- 函数的捕获局部在入口变成单元素 `list[T]` cell；读写成
  `index.get/index.set`，引用计数沿用列表元素规则
- 嵌套 `def`：无捕获用 `func.addr` 静态闭包；有捕获用
  `closure.new` + 逐个 `field.set` 填 cell 指针
- 被调函数入口从 `%r10` 把闭包指针存进 `env_slot`；捕获变量读成
  `field.get env[16+8i]` 再 `index.get cell[0]`
- 嵌套函数直接引用自己的名字时走当前闭包（有 env 用 env，否则静态闭包），
  避免 self cell 环

**字典 / 集合不新增 IR 指令**：

- 字面量 → `py23_dict_new` + 逐对 `py23_dict_set`（set 用 `val_own = 0`）；
- `d[k]` → `py23_dict_get`，`d[k] = v` → `py23_dict_set`；
- `k in d` / `k in s` → `py23_dict_contains`；list/tuple 用
  `py23_list_contains`，str 用 `py23_str_contains`；
- `for k in d` / `for x in s` → `len` + `py23_dict_key_at`；
- 方法：`get(k[,d])` → `py23_dict_get` / `py23_dict_get_or`；
  `keys()/values()/items()` → `py23_dict_keys/values/items`（新 list）；
  `update` → `py23_dict_update`；`set.add/update` 复用 dict_set/update；
- 切片读 → `py23_list_slice` / `py23_str_slice`，缺省边界传 `INT64_MIN`
  哨兵；结果是新堆对象，需要 `rc_temp` 入栈；
- `len` 仍是一条 `Len`，真值判断仍走 `branch`（字典/集合的 `len` 在
  偏移 0）。

key/value 的 retain 在写入前由降级器发出；运行时负责更新/销毁时 dec，
unknown 类别不 dec，由 GC 兜底。

**引用计数插桩的边界**：

- 新堆值由 `def_heap()` 统一登记，产生后立刻 `py23_rc_temp` 入栈；
- 写入槽 / 全局 / 字段 / 容器前，源类型已知是堆才 `rc_inc`；
- 覆盖旧值前先读出来，写完再 `rc_dec`；
- 函数入口对堆形参 `rc_inc`，`return` 前对返回值 `rc_inc`，然后清理临时
  栈并释放所有堆槽；
- 语句、循环、函数都有 mark；完全没有临时堆值的层级会在降级期把
  mark/cleanup 删掉，纯整数代码不产生任何 RC 指令。

**字段初值放在构造点**：`x: int = 5` 不是类属性，而是「每次 `Cls(...)` 都求值一次并写进新对象」。这样父类字段的初值在子类覆盖 `__init__` 时也不会丢，且 `items: list[int] = []` 每个实例都是独立列表。

## dump 怎么看

```bash
./build/dev/py23 ir script.py
```

```
; globals: 3
global g0 twice : func(int) -> int
global g1 total : int
class c0 Point (2 fields, vtable: f3)

func f0 __py23_main() -> int {
  slots: s0=int
bb0 (entry):
  %0 = func.addr f-1  ; func
  store.global g0, %0
  %1 = const.int 0  ; int
  store.global g1, %1
```

- 头部：全局变量表（名字 + 类型）、字符串池、类表（字段数 + vtable 各槽的函数下标）
- 每个函数：名字、返回类型、槽位表（含类型）
- 基本块以 `bbN (名字)` 开头，块名是人可读的提示（`for.cond` / `for.body` / `if.then` …），不参与语义
- 每条指令后面的 `; 类型` 是 `ty` 或操作数类型
- `_ = ` 表示这条指令没有结果

## 加一条新指令要动哪里

1. `ir.h`：`IrOp` 加枚举 + `irop_name` 加名字
2. `ir.cpp`：`Inst::text()` 加打印
3. `lower*.cpp`：产出这条指令（别忘了 `def()` 里登记类型）
4. `codegen_ops.cpp`：`gen_inst` 加 `case`（`-Wswitch` 会强制你加）
5. `docs/ir.md`：指令表加一行，`docs/codegen.md` 说明生成策略

漏掉第 4 步编译不过，这正是当初把 `op` 做成 `enum class` 而不是字符串的原因。
