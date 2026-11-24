# 代码生成

`src/codegen*.cpp`：IR → AT&T 语法的 GNU as 汇编，调用约定是 System V AMD64。

## 输出结构

```asm
        .section .data.rel.ro,"aw",@progbits   ; 字符串常量对象
        .quad 0                                ; next = NULL（不进 GC 注册表）
        .quad 0                                ; prev
        .quad 0x8000000000000000               ; rc = INT64_MIN，永生
        .quad 0                                ; dtor = NULL
        .quad 0                                ; release = NULL
        .quad 0                                ; size
        .quad 0                                ; kind
        .quad 0                                ; marked
.Lstr0: .quad 11                               ; 负载从 len 开始，布局与堆字符串一致
        .quad 0
        .quad .Lstr0_c
.Lstr0_c: .ascii "hello, PyAdamas\000"

        .bss                                   ; 全局变量
.Lg0:   .zero 8

        .section .data.rel.ro,"aw",@progbits   ; 每个类一张 vtable
.Lvt0:  .quad .Lf3
        .quad .Lf4

        .text
        .p2align 4
.Lf0:                                          ; 每个 IR 函数一个 label
        pushq %rbx                             ; 只 push 实际用到的 callee-saved
        subq  $80, %rsp                        ; 出参区 + 局部区；rsp 全程不动
        ...
        .globl main                            ; 真正的入口，只负责调用 .Lf<entry>
main:
        pushq %rbp
        movq  %rsp, %rbp
        call  .Lf0
        popq  %rbp
        ret
        .section .note.GNU-stack,"",@progbits  ; 不加会警告「需要可执行栈」
```

字符串对象是 `{len, hash, data}` 三字段，`data` 直接跟在后面；前面额外有
64 字节的 RC/GC 头。放在 `.data.rel.ro` 而不是 `.rodata`：对象
里有指向自身字符区的绝对地址，只读段会触发 PIE 重定位报错。vtable 同理
——表里是函数地址的 relocation。

每个有堆字段的类会生成两个回调：`.LdtorN`（按字段偏移调 `pa_rc_dec`，
正常引用计数归零时用）和 `.LgcrelN`（调 `pa_gc_unref_child`，只减掉
指向可达对象的引用，GC 清扫时用）。对象分配时把这两个地址传给
`pa_obj_new`。

全局变量在 `.data.rel.ro` 里还有一张 `.Lgroots` 根表（每个全局槽一个
`.quad .LgN`）。生成的 `main` 在调用入口前执行：

```asm
        leaq .Lgroots(%rip), %rdi
        movq $<全局数>, %rsi
        movq %rsp, %rdx              # 栈顶
        call pa_gc_init
```

循环收集器按全局根表、临时值栈、帧记录登记的地址（unknown 槽与含 unknown vreg 的临时区）和 callee-saved 寄存器找根；纯类型化帧不扫整段栈。

## 寄存器策略

IR 一行没改，全部优化都发生在这一层。下面这些机制叠起来，把「值在寄存器里」做实：

| 机制 | 触发条件 | 省掉什么 |
|---|---|---|
| 变量提升 | 按 `load.slot` + `store.slot` 次数排序，最热的 6 个变量整函数常驻寄存器 | 每次读写各一次内存访问 |
| 临时值别名 | `%v = load.slot x`，且 %v 只被读一次、同块 | 整条 load |
| 结果融合 | `%v = ...` 紧跟 `store.slot x, %v`，且 %v 只被读一次、同块 | 整条 store |
| 常量延迟 | `%v = const.int N`，只被读一次、同块、在 32 位有符号范围内 | 一条 mov + 一次内存往返 |
| 跨块常量立即数化 | O2；`const.int` 在 32 位范围内，整函数范围直接当立即数 | 常量不占寄存器、不落槽；`i += 1` 是 `addq $1` |
| 比较连分支 | `%v = cmp` 紧跟 `branch %v`，且 %v 只被读一次、同块 | setCC + movzbq + 槽往返 + cmpq |
| 拷贝合并 | 块内临时池里 `v = a op b`，且 `a` 的最后一次使用就是这条（GPR 限加减乘/位运算/移位，浮点限加减乘除） | 目标复用 `a` 的寄存器，原地算，省一条搬移 |
| 浮点寄存器搬运 | 寄存器之间复制浮点值 | `movapd` 整 128 位拷贝，避免 `movsd` 写合并的依赖链 |
| 块内临时寄存器 | 整块没有调用/占用 %r8–%r11 的指令；值定义和使用同块 | 临时值不落栈，线性扫描复用 %r8–%r11 |
| 跨块 vreg 提升 | O2；跨块整型/指针 vreg，整函数独占「局部变量没用到的」callee-saved 寄存器 | 循环不变量、合并结果等跨块值不落栈 |
| 无调用函数再扩池 | 整个函数没有任何会破坏 %r8–%r11 的指令时，再用至多两个 %r8–%r11 | 跨块值多两个常驻寄存器，剩余寄存器仍留给块内临时值 |
| 浮点跨块 vreg 提升 | O2；无调用函数里 xmm8–15 扣掉浮点槽还有空闲时，再按使用次数分给至多两个跨块浮点值 | 跨块浮点值不落栈（会发调用的 `//`、`%`、`**` 不算无调用） |

含 handler（`setjmp`）的函数关闭「变量提升」：longjmp 会把 callee-saved
寄存器恢复成 setjmp 时刻的值，寄存器里的变量会悄悄回退。这类函数的所有
变量走内存槽，其它函数完全不受影响。跨块提升用「整函数独占寄存器」而
不是活跃性复用，所以循环回边天然安全，不需要额外 spill 逻辑。浮点槽是
例外：xmm 是 caller-saved，只在真正有调用时按块级活跃性写回/读出——调用
后还会被读的槽才往返内存，循环里反复赋值、调用后不再读的槽直接跳过。
被取过地址的槽留在内存；类型被 unknown 吞掉但实际流着浮点值的槽也留在
内存，避免把 `movsd` 写进整数寄存器。

`while i < n: s += i * 2; i += 1` 的循环体，优化前后：

```asm
# 之前：21 条，每个临时值都先落一次栈再读回来（偏移只是示意）
.Lf1_b2:
        movq  72(%rsp), %rax         # %5 = load.slot s1
        movq  %rax, 40(%rsp)
        movq  64(%rsp), %rax         # %6 = load.slot s2
        movq  %rax, 48(%rsp)
        movq  $2, 56(%rsp)           # %7 = const.int 2
        movq  48(%rsp), %rax         # %8 = bin.mul %6, %7
        imulq 56(%rsp), %rax
        movq  %rax, 32(%rsp)
        movq  40(%rsp), %rax         # %9 = bin.add %5, %8
        addq  32(%rsp), %rax
        movq  %rax, 48(%rsp)
        movq  48(%rsp), %rax         # store.slot s1, %9
        movq  %rax, 72(%rsp)
        ...                          # i += 1 原样再来一遍，另外 7 条
```

```asm
# 现在：5 条，两个变量全程待在寄存器里
.Lf1_b2:
        movq  %rbx, %rax             # %8 = bin.mul %6, %7（%6 就是 %rbx）
        imulq $2, %rax               # 常量直接当立即数
        movq  %rax, 8(%rsp)          # %8 只被用一次，落一次栈
        addq  8(%rsp), %r12          # %9 就地写进 s1 的寄存器
        addq  $1, %rbx               # i += 1 一条搞定
```

### 为什么不做图着色寄存器分配
变量提升按**活跃区间线性扫描**（0.203–0.204，见 [regalloc.md](regalloc.md)）：整型槽用 callee-saved 寄存器 + 无调用区间借用 r10/r11，浮点槽也走线性扫描。不做图着色/活跃性复用，因此不产生复杂 spill。

代价是寄存器利用率不如图着色。收益是分支汇合、循环回边、异常路径全都天然正确——不存在「这条边上值在寄存器、那条边上在栈里」这种需要插 phi 或 reload 的状态。实测性能收益为零，根因是 `__pa_main` 调用密度过高（见 regalloc.md），不是分配算法本身。

真要做图着色，需要先解决模块顶层函数边界，并考虑在 vreg 上构造 SSA（IR 本身不是 SSA：vreg 可有多个定义，如生成器续跑）；决策记录见 [ir.md](ir.md#那为什么不干脆上-ssa0205208-的取舍)。

### 寄存器池

`%rbx` / `%r12` / `%r13` / `%r14` / `%r15` / `%rbp`，全部是 **callee-saved**：被调用方保证返回时原样恢复，所以跨函数调用不需要把值挪回栈，也不需要在调用点保存。0.163 之前 `%rbp` 被帧指针占着，现在帧相对 `%rsp`，它也能拿来存变量。

浮点槽用 `xmm8`–`xmm15`（`xmm0`–`xmm7` 留给参数和临时），全部是 caller-saved，调用点要自己写回/读出，因此只在有调用的函数里按活跃性 spill；跨块浮点 vreg 只分给无调用函数，调用会破坏寄存器的形状（`//`、`%`、`**` 走 libm）同样排除。

## 栈帧布局

0.163 起生成函数**没有帧指针**：槽位、帧记录、栈传实参一律相对 `%rsp`，
而 `rsp` 在函数体里从头到尾不动（出参区一次性留够，调用点不再
`subq`/`addq`），所以偏移是常量，`setjmp`/`longjmp` 回到的也是同一个 `rsp`。

```
       高地址
    ┌──────────────┐
    │  调用者的栈   │
    ├──────────────┤
    │  返回地址     │  ↑ 调用者的 rsp
    ├──────────────┤
    │  保存的 rbx   │  frame_bytes_-8(%rsp)  只 push 实际用到的，最多 6 个
    │  保存的 r12   │                        （rbx/r12/r13/r14/r15/rbp）
    │  ...         │
    ├──────────────┤
    │  帧记录       │  4+n 个字：GC 根与异常展开用
    ├──────────────┤
    │  vreg 槽 ...  │
    │  slot 1      │  槽号小的在低地址，偏移落在 disp8 里
    │  slot 0      │  out_bytes_(%rsp)
    ├──────────────┤
    │  出参区       │  0(%rsp) 起：第 7 个以后的整数实参、第 9 个以后的浮点实参
    └──────────────┘  ← rsp（16 字节对齐）
       低地址
```

- 帧总长 `frame_bytes_ = 8*k + frame_pad_`，而 `frame_pad_ = out_bytes_ + locals_` 是 `subq` 的立即数。布局在 `layout_frame()` 里一次算好：局部区先取 `8 * 槽数`，再把 padding 补到「push + subq 的总量 ≡ 8 (mod 16)」——入口 `rsp ≡ 8`，补完之后 `call` 时正好 16 对齐。
- 出参区按整函数最大的那个调用点预留（`outgoing_bytes()` 扫一遍 IR 估上界，结构体按 ABI 尺寸算），调用点只写 `{stack_off}(%rsp)`，不碰 `rsp`。
- 槽位区从 `out_bytes_(%rsp)` 往上排，形参和最常用的局部（slot 号小）落在低地址；帧记录放在最上面（每函数只碰一两次）。
- 栈传的实参读 `frame_bytes_ + 8 + off(%rsp)`（返回地址上面）。
- 帧记录、槽地址这些**地址**在函数体内是常量，GC 和异常展开直接按地址读写。

函数末尾：

```asm
        addq  $frame_pad_, %rsp     # 退掉出参区和局部区
        popq  %r15 ... popq %rbx    # 逆序弹回
        ret
```

`%rbp` 因此空出来，并进了变量寄存器池（`kSlotRegs` 现在 6 个）。

### 对齐

函数入口发 `.p2align 4`，循环头（回边目标）也按 16 字节对齐。热点循环
对「入口落在哪条缓存行」很敏感：0.163 去掉帧指针后 nqueens 一度慢了 10%，
把函数入口对齐就回到原水平（44.2ms vs 44.5ms），matmul/spectral_norm 同理。

### 非参数变量清零

序言里对每个非参数变量发一条 `movq $0, ...`。`x: int` 这种纯声明在 lower 里不生成任何指令，靠的就是「槽初值是 0」；栈帧里的内存是脏的，不显式清掉就会读到上一个函数留下的垃圾。

## vreg 槽位复用

如果每个 vreg 独占一个槽，2000 行的函数会生成 100KB 栈帧，递归几层就爆。所以做一次**线性扫描**：给每个 vreg 算 `[定义, 最后使用]` 区间，不重叠的共用槽位。

关键限制：**只有定义和使用都在同一个基本块里的值才能按区间复用**。

跨块的值（尤其循环头里每一轮都要读的）在静态上「已经用完」之后，下一轮迭代还会被读到。线性区间会把它的槽分给别人，然后循环第二次迭代读到垃圾。这类值一律独占槽位、区间拉满整个函数。

> 这个坑真的踩过：`for` 循环里读的列表在第 2 轮变成别的值，`pa_list_get` 段错误。修法就是上面这条规则，写在 `assign_vreg_slots()` 的注释里。

提升到寄存器的变量仍然占着一个槽位（不会被读写），这是有意的：`vreg_off()` 的算法因此保持简单，槽位编号不会因为提升与否而错位。

实测收益：`big.py`（2000 行赋值）栈帧从 112032 字节降到 48 字节。

## 调用约定

| 项目 | 规则 |
|---|---|
| 整数/指针参数 | `%rdi, %rsi, %rdx, %rcx, %r8, %r9`（最多 6 个） |
| 浮点参数 | `%xmm0` … `%xmm7`（最多 8 个） |
| 两类计数 | 独立。第 7 个整数参数压栈不妨碍第 1 个浮点参数用 `%xmm0` |
| 超出部分 | 按出现顺序压栈，`stack_off = 序号 * 8` |
| 栈对齐 | 压栈字节数向上取整到 16 的倍数再 `subq` |
| 返回值 | `%rax`（整数/指针）、`%xmm0`（浮点） |
| 闭包环境 | `%r10`（隐藏参数，不属于 SysV 参数序列）；方法调用清零 |
| 调用前 | `xorl %eax, %eax`（变参 ABI 约定，表示没有 vector 参数） |

被调用者只保存 `%rbx` / `%r12`–`%r15` / `%rbp` 里**实际用到的那几个**，序言 push、尾声逆序 pop。
其余寄存器（`%rax`、`%rsi`、`%rdi`、`%r8`–`%r11`、全部 `xmm`）是 caller-saved，可以随便写。

`*args` / `**kwargs` 不引入新的调用约定：它们就是参数列表末尾的一个 list /
dict 指针，调用点打包好按普通参数传；整数寄存器不够时照常压栈。装饰器
调用、`with` 的 `__enter__` / `__exit__` 都是普通的 `call` / `vcall`。
`__init__` / `__enter__` / `__exit__` / `__aenter__` / `__aexit__` /
`__call__` 被装饰时 vtable 槽是空的，调用点改走类属性里的包装函数：
先按名字取类属性，再用 `call` 把实例作为第一个实参传进去。

方法值 `obj.m` 不是运行期的描述符对象，而是在取值的编译点合成一个闭包：
接收者存进单元素 cell 作为捕获，闭包体按方法种类发出 `vcall` /
`vcall_static` / `call_direct`，或在装饰方法上先取类属性再 `call`。
捕获布局与普通闭包一致（环境槽 16 起是 cell），因此方法值可以像函数值
一样存变量、进容器、当实参。

调用一个函数值的序列：

```asm
        movq  40(%rsp), %r11         # callee 先取到 r11，避开参数寄存器
        movq  72(%rsp), %r10         # 第 7 个实参：直接写进出参区
        movq  %r10, 0(%rsp)
        movq  64(%rsp), %rdi         # 寄存器参数
        xorl  %eax, %eax
        call  *%r11
```

出参区在函数帧里预留，调用点**不动 `%rsp`**：栈传实参直接写
`{stack_off}(%rsp)`（偏移相对出参区起点）。

## C 互操作（cimport）

`call.extern` 不走 PyAdamas 自己的调用约定，直接按 SysV C ABI 调外部符号：

- 实参按调用点的 `CKind` 编组：`long`/`int`/`_Bool`/指针进整数寄存器，`double` 进 `xmm`；`Int32`/`UInt32` 在寄存器里仍占 8 字节，写 C 结构体字段时才截到 4 字节。
- `ptr[T]` 接 `list[T]` / `bytes` 时编译成取 `data`（偏移 16），空容器传 NULL；`BufF64` / `BufI64` / `BufBytes` 只由 lower 在调用点选择，`.cpya` 不能声明。
- 带字段的 `struct` 按 SysV 分类：INTEGER / SSE 各占寄存器，超过两个 eightbyte 或含非规整字段时走 MEMORY（调用方分配临时区、传隐藏指针）。
- 可变参数函数在 `call` 前把用到的 `%xmm` 数写进 `%al`。
- 回调参数（`Callback`）生成跳板：C 侧拿到裸函数指针，跳板把 C 实参搬进合成闭包的 `fn(...)` 并调用捕获的函数值；闭包槽由 `cb_sigs` 下标命名。
- `load.extern` / `store.extern` 按声明宽度访问 C 全局；`.cpya` 的 `link <lib>` 追加 `-l<lib>` 到链接命令。

`cdecl` 只解析头文件子集：不展开宏函数/条件编译，`float`、union 按值与定长数组不生成。

`struct` 值类型由 `struct.new` / `struct.copy` 分配 `[RC 头][payload]`；字段读写按指令的 `fwidth` 发 `movb` / `movl` / `movq`，4 字节字段按 `funsigned` 选 `movslq` 或 `movl` 零扩展。

## 闭包

函数值现在一律指向**闭包对象**，负载 `[code, n_caps, cell…]`。无捕获函数
用 `.data.rel.ro` 里的静态闭包：

```asm
        .section .data.rel.ro
        .align 8
        .quad 0                    # next
        .quad 0                    # prev
        .quad 0x8000000000000000   # rc = INT64_MIN
        .quad 0                    # dtor = NULL
        .quad 0                    # release = NULL
        .quad 0                    # size
        .quad 0                    # kind
        .quad 0                    # marked
.Lclos7:
        .quad .Lf7                 # code
        .quad 0                    # n_caps
```

有捕获的嵌套函数在运行时分配闭包（`pa_closure_new(code, n)`），析构器
逐个 dec cell。调用序列：

```asm
        movq funcval, %r11
        movq (%r11), %r11          # +0 是代码地址
        ... 装实参 ...
        movq funcval, %r10         # 闭包指针作为隐藏环境参数
        xorl %eax, %eax
        call *%r11
```

被调函数如果有捕获，入口执行 `movq %r10, env_slot`；之后的变量访问是
「env + 16 + 8i 取 cell → cell[0] 读值」。方法调用在 `call` 前把 `%r10`
清零，因为方法没有闭包环境。嵌套函数直接引用自己的名字时：有 env 就
用 env 里的闭包指针，没有就是静态闭包，因此不会为自递归生成 self cell 环。

被捕获变量是单元素 `list[T]`（cell），读写复用 `index.get/index.set` 和
列表元素的引用计数；这样闭包、列表、RC 三套机制不需要各自实现一份。

## 异常展开

**现状说明**：本节描述的是 **setjmp/longjmp 路径**。0.202 起非可挂起函数默认走 Itanium 表驱动零开销异常（personality + LSDA，见 [zero-cost-exceptions.md](zero-cost-exceptions.md)，`--no-zero-cost-exc` 可回退）；setjmp 仍用于可挂起函数（生成器/协程）、`with` 在 `try` 内、Win64。下文按旧路径叙述，不重写整节。

异常用 **setjmp/longjmp**，不占用返回值寄存器，也不改调用约定：

- `pa_handler_push` 压入一条 `{jmp_buf, prev, temp_mark}` 记录，返回的
  指针直接作为 `setjmp` 的参数（`jmp_buf` 是记录的第一个字段）。
- 生成代码在 try/with/finally 入口 `call setjmp`，0 走正常体，非 0 走
  landing pad；`raise` 在运行时设置全局异常后 `longjmp` 到最内层记录。
- landing pad 先 `pa_handler_leave`：弹出记录并把临时值栈清理到
  记录水位；然后分派 `except` / `__exit__` / finalbody。
- 挂起恢复后若还要 `handler_leave`（`async for` 的 StopAsyncIteration
  保护、异步推导式），到手的堆值必须在 leave 之后才 `pa_rc_temp`：
  leave 会按复查时的水位清理临时栈，先入栈会被当场释放。
  `suspend_on_awaitable` 的 `push_result_temp=false` 就是给这类调用点用。
- 含 handler 的函数关闭槽位寄存器提升；被异常跳过的普通函数帧不执行
  `release_slots`，局部引用交给保守 GC。
- `return` / `break` / `continue` 不 longjmp，走编译期 cleanup 栈：
  `handler_discard` + `__exit__` / finalbody 直接内联发射。
  `with` / `async with` 的 `FuncCtx::Cleanup` 记录 `exit_cls`（退出方法被
  装饰时的类），正常、跳转、异常展开三条路径都按它从类属性分派。

## 生成器

含 `yield` 的函数编译成两个入口：

- **创建包装**：普通函数，按原签名收参，`gen.new` 分配帧，把实参 retain
  进帧槽，再把 `%r10` 里的闭包指针存进环境槽，返回生成器对象。函数值
  一律指向它。
- **resume**：`IRFunction::is_generator` 为真，`%r15` 固定指向帧负载；
  槽位读写走 `8*(6+slot)(%r15)`，不占栈帧，也不参与槽位寄存器提升。
  入口按 `state` 分发到续跑块，`gen.yield` 把值写进 out、state，然后
  直接返回。

vreg 的栈槽不跨挂起保存。降级完成后对整个 resume 函数做一次 vreg
活跃性分析：续跑块的 live-in 就是挂起期间还得活着的值，挂起前 retain
一份写进堆帧槽（`pa_rc_inc` / `pa_rc_inc_unknown`），恢复后在续跑块
开头装回 vreg 并把所有权交给临时栈（`pa_rc_temp` /
`pa_rc_temp_unknown`）。所以 `yield` / `await` 可以出现在任意子表达式
里，复合表达式不用逐个改；一个 vreg 因此可能有多个定义，寄存器分配
按「定义多于一次」直接独占栈槽。槽位寄存器提升整体关闭。`gen.done`
先由 lower 发射 `release_slots`，再置 status=1；帧析构由运行时的
finalize 路径按挂起点快照跑清理。

跨挂起的 `try` / `with` / `finally`：yield 前用
`pa_handler_suspend` 把记录摘下，恢复时分发块先
`pa_handler_rearm` 再重新 `setjmp`，非 0 直接跳回原 landing pad。
close 分发按挂起点快照重放 `__exit__` / finalbody，并
`pa_handler_free` 释放摘下的记录。可挂起函数里的 finally 体只编译
一份：正常路径与异常 landing 都跳到同一个 `finally.fin` 块，体内挂起点
因此只有一套状态机；异常路径在 land 里用 `pa_exc_take_value` 把 pending
转成异常值存进帧槽（`Exception` 堆槽），finalbody 结束后
`pa_exc_reraise_value_if` 非空就重抛，所以体内再 raise / 嵌套 try 不会
覆盖待重抛异常。close 重放到 finalbody 里的挂起点时按 CPython 报
`RuntimeError`（`generator ignored GeneratorExit`）。

协程复用同一套寄存器/槽位约定：帧头多出 kind/waiter/exc/cur 四个字，
`gen.await` 写 status=3；恢复入口先看 closing，再看 `gen.exc`，有注入
异常就按 state 进入 throw 分发——重挂 handler 并重新 `setjmp` 后
`gen.rethrow`，让 `try/except` 能抓到跨 await 的异常。返回值由
`gen.done a` 写进 out。普通生成器的 `.throw()` 复用同一条分发：挂起点不在
`yield from` 委托中就直接 rethrow，在委托中则用 `gen.take.exc` 取走异常、
存进帧槽并跳回委托循环的 cond，由它把异常转发给子生成器。
`asyncio.run/sleep/gather/create_task` 与异步生成器的 `__anext__`
都是普通 `call.rt`；事件循环、Task 取消和驱动推进都在运行时里。

## 模块编译

每个模块单独汇编成 `.o`，符号统一加前缀：

- 全局槽 `pyadamas$m$g$name`（非入口模块用 `.globl` 导出）；模块函数就是
  这类全局槽里的闭包值。
- 入口顶层函数 `pyadamas$m$init`；`__entry__` 模块额外生成 `main`。
  `main` 在栈上保存 `argc`/`argv`，`gc_init` 之后调用
  `pa_sys_set_args` 初始化 `sys.argv`。
- 模块初始化标志 `pyadamas$m$ready`；根表 `pyadamas$m$roots`。
- 模块 init 开始先 `pa_gc_add_roots(roots, n)`，再执行顶层代码；
  入口 `main` 用 `pa_gc_init` 注册入口模块的根表并调用顶层函数。
- `import.module` 发射
  `pa_import_module(&ready, init)`；`ready` 置位后函数直接返回。
- 外部全局（`IRGlobal::external`）只引用符号，不分配存储、不进根表，
  入口退出时也不释放；堆对象由 GC shutdown 统一回收。

含 handler 的函数关闭变量提升；模块 init 也一样（它可能 import 别的模块，
不直接含 handler，但仍走普通函数帧）。

## 对象与虚调用

对象是一块连续内存，字段偏移在编译期就定了。对象指针指向负载，RC/GC 头
在负载前面 64 字节，所以下面这些偏移一个都没变：

```
-16   rc（隐藏）
-8    dtor（隐藏）
+0    vtable 指针       ← 对象指针
+8    字段 0
+16   字段 1
...
```

所以 `p.x` 是一条 `movq 8(%rax), %rax`——没有哈希、没有查表、没有类型标签。

多继承的布局约束：主基类的字段和 vtable 前缀保持不变（覆盖沿用相同槽号，新增追加到末尾）；附加基类只能复用主基类已有的字段，不能引入新的字段布局。于是「基类类型的变量装子类实例」天然安全：

```
a: Animal = Dog()
a.speak()   →   a->vtable[0](a)   →   Dog::speak
```

构造：

```asm
        movq  $24, %rdi          # 8 * (1 + 2 个字段)
        leaq  .Ldtor0(%rip), %rsi
        leaq  .Lgcrel0(%rip), %rdx
        call  pa_obj_new       # rc_alloc，全零，rc = 1
        leaq  .Lvt0(%rip), %rcx
        movq  %rcx, (%rax)       # 对象头写 vtable
```

分配全零是有意的：意味着「声明了但没赋值的字段」就是 `None` / `0` / 假，和栈槽的约定完全一致。

虚调用：

```asm
        movq  64(%rsp), %r10     # self
        movq  (%r10), %r11       # vtable
        movq  8(%r11), %r11      # 第 1 号槽（覆盖过的就是子类实现）
        movq  64(%rsp), %rdi     # self 固定占第一个整型参数
        movq  72(%rsp), %rsi     # 其余实参照常分类
        call  *%r11
```

槽号是编译期常量，运行时只有两次解引用加一次间接跳转。

**终态类直接调用**：跨模块继承不支持，所以一个类的所有子类都在本模块
可见。没有本模块类继承的类就是终态——方法调用点把 vtable 槽直接解析成
函数下标，发 `call.direct fN`（构造点的 `__init__` 调用同理：对象是刚
用这个类 `obj.new` 出来的，类型确定）。O2 内联器的调用图与内联点识别
同时接受 `call` 和 `call.direct`，直接化之后再跑一轮，所以 `a.add(i)`
这类小方法会真正摊平进调用点；递归边（含 `call.direct` 自递归）不内联。
只有「静态类型存在子类」的接收者才走上面的 vtable 间接调用。

`Optional[Class]` 上的字段访问和方法调用会先生成一次空指针检查（`branch` + `panic`）：类型收窄做不到逐条路径成立，而段错误不带任何上下文——`PyAdamas: panic: NoneType object has no attribute 'x'` 有用得多。

## 指令选择要点

### 整数除法：先挡信号

除数是运行期值时，`idiv` 在除数为 0 和 `INT64_MIN / -1` 时都触发 `#DE`，那个信号带不出任何上下文。所以通用路径先生成守卫：

```asm
        testq %rcx, %rcx
        jne   1f
        call  pa_panic_div0
1:      cmpq  $-1, %rcx
        jne   2f
        movabsq $0x8000000000000000, %r8
        cmpq  %r8, %rax
        jne   2f
        call  pa_panic_overflow
2:
```

### 整除与取模：符号修正

除数是运行期值时，`idiv` 向零取整，Python 的 `//` 向下取整；余数也是 `idiv` 跟被除数同号，Python 跟除数同号。两者都在「余数非零且两操作数异号」时差一步：

```asm
        cqto
        idivq %rcx
        testq %rdx, %rdx
        je    done
        movq  %rdx, %r8
        xorq  %rcx, %r8
        jns   done
        decq  %rax        ; 整除：商减一
        addq  %rcx, %rdx  ; 取模：余数加除数
done:
```

浮点的 `//` 是 `divsd` + `call floor`。浮点的 `%` 用 `fmod` 再修符号，另外余数为 0 时 Python 取**除数**的符号（`-0.0 % 3.0` 是 `0.0`），所以零的情况要单独处理：

```asm
        call  fmod
        movsd b, %xmm1               # xmm1 是调用者保存的，回来要重装
        xorpd %xmm2, %xmm2
        ucomisd %xmm2, %xmm0
        jp    done                   # NaN 原样返回
        jne   nonzero
        movq  %xmm1, %rdx            # 零：取除数的符号位
        movabsq $0x8000000000000000, %rax
        andq  %rdx, %rax
        movq  %rax, %xmm0
        jmp   done
nonzero:
        movq  %xmm0, %rax
        movq  %xmm1, %rdx
        xorq  %rdx, %rax
        jns   done                   # 同号，不用修
        addsd %xmm1, %xmm0
done:
```

### 常数除数：魔数乘法

上面的整数除法和取模走的是「除数是运行期值」的通用路径。除数是编译期常量时，`gen_bin` 先试
`gen_div_const`（`src/backend/ops.cpp`）：`operand_const_imm` 认出 `src_op` 给出的
`$N` 形态立即数，`pow2_log2` 判断是不是 2 的幂，`udiv_magic` 求魔数，
`gen_div_const_emit` 负责发指令。`/`、`//`、`%` 三个都走这套，不再发
`cqto` + `idivq`。

为什么值得：x86-64 的 64 位 `idivq` 在 Cascade Lake 上吞吐约 24 周期，而魔数法
是十来条一周期指令加一次 3 周期的 `imul`。实测 `idivq` 在 queue_sim 的
`simulate` 里占该函数 47.6% 的采样（`t % 3`、`t % 7` 各一处），在 str_build 的
`build_line` 里占 71.9%（`i % 10`）——全是循环变量对常数取模。

算法（Granlund-Montgomery 的 round-up 魔数）：对 `d > 1` 且非 2 的幂，取
`L = ceil(log2 d)`、`M = floor(2^(64+L) / d) + 1`。`M` 一定 `> 2^64`（因为
`d < 2^L`），写成 `M = 2^64 + m'`，于是

    floor(x * M / 2^(64+L)) == (mulhu(x, m') + x) >> L

—— 65 位魔数拆成「高位那 1 加上 m'」：`x * 2^64` 那部分恰好整块进到高 64 位，
所以一条 `mulq` 拿到 `mulhu(x, m')`，再 `addq` 一次被乘数、右移 `L` 位就完事。
有符号不必另找魔数：`trunc(x/d) = sign(x) · floor(|x|/d)`，`|x|` 用
`(x ^ sign) - sign` 在无符号域算（`INT64_MIN` 得到 2^63，正好），顺手把
`INT64_MIN / d` 的上溢一起解决；算完再按符号还原。除数为负时先按 `|d|` 算、
最后 `negq`。

2 的幂必须单独走，不能套魔数：`d = 2^L` 时 `M` 退化成 `2^63 + 1`，代入等式
不成立。它另有更便宜的写法（`trunc` 语义）：

```asm
        movq %rax, %rcx
        sarq $63, %rcx
        andq $7, %rcx        # d-1；只有负 x 这一项非零
        addq %rcx, %rax
        sarq $3, %rax        # x / 8
```

`//` 和 `%` 由商反算余数：`r = x - q*d`（`imulq $d`，`d` 在 32 位立即数范围内），
`r` 与除数异号时 `//` 商减一、`%` 余数加一个 `d`——通用路径里 `idivq` 之后那段
符号修正，在这里并进了余数计算。

不是所有常量除数都能走：`d == 0` 落回通用路径（`pa_panic_div0` 要给出可读
报错）、`d == -1` 也落回（`INT64_MIN / -1` 的上溢保护省不得，魔数法算不出那个
上溢，会安静地返回 `INT64_MIN`）、`d` 超出 `[INT32_MIN, INT32_MAX]` 同理（32 位
立即数放不下，`d == INT64_MIN` 是其中一例）。`d == 1` 再特化：`/` 和 `//` 直接
`movq`，`%` 直接给 0。

正确性对拍：30,583,433 组 `(x, d)` 零失败——覆盖 `d ∈ [-4000, 4000] ∪
{2^k±1, 2^k} ∪ 500 个随机 31 位除数`，`x` 含 0/±1/±d/±(d±1)/±2d/`INT64_MIN`/
`INT64_MAX`，小除数上再密集跑满 `[0, 2^18)`。

### 浮点比较：NaN

`ucomisd` 对无序（NaN）把 CF/ZF/PF 全置 1。规则分两条路径：

- **比较 + 分支融合**（`cmp` 紧跟 `branch`，结果不真正写出布尔值）：
  `<` / `<=` 交换操作数后分别用 `ja` / `jae`，`>` / `>=` 直接用 `ja` /
  `jae`——这四种有序比较在 NaN 时 CF=1，条件跳转自然不成立，落到假分支，
  **不再需要额外的 `jp`**（0.157）。`==` 在无序时 ZF=1，与相等区分不开，
  仍先 `jp` 到假分支。`!=` 反过来：NaN 应为真，所以先把无序跳到真分支，
  再按 ZF 用 `jne`/`je` 分派（0.158）。
- **真正写出布尔值**（`setcc` 路径）：`<` / `<=` / `==` 额外 `setnp` 再与，
  `!=` 额外 `setp` 再或，`>` / `>=` 用 `seta` / `setae`。

### 真值测试

| 类型 | 做法 |
|---|---|
| int / bool | `cmpq $0` + `setne` |
| float | `ucomisd 0` + `setne` + `setp`（NaN 算真） |
| str / list / tuple / dict / set | 读对象头的 `len`（偏移 0），`cmpq $0` + `setne` |

`len` 在偏移 0 是刻意设计的：`len(x)` 一条指令，真值判断也是一条。

字典/集合的构造、读写、`in`、遍历、方法和切片都走 `call.rt`
（`pa_dict_*`、`pa_list_contains`、`pa_str_contains`、
`pa_list_slice`、`pa_str_slice`），没有新增汇编指令；它们的
`len` 也放在偏移 0。切片缺省边界编码成 `INT64_MIN` 立即数传给运行时。

### 字符串比较

`==` / `!=` 先比指针：字符串（和 bytes）不可变，同一个对象必然相等，所以
`cmpq` 相等就直接出结果（`==` 给 1、`!=` 给 0），不等才 `call pa_str_eq`。
`!=` 的慢路径也要取反——这里曾经只改了快路径，非等指针时静默返回了 `==` 的
结果。指针相等能命中，靠的是单字节 ASCII 字面量和 `s[i]` 共用同一批对象：
这类字面量不生成 `.Lstr` 常量对象，而是直接取运行时驻留表

```asm
        movq pa_char_tab+384(%rip), %rax   # "0"：0x30 * 8
```

`pa_char_tab` 是 256 项载荷指针数组，指向运行时那批永生单字节串（`pa_char_str`
返回的也是这些对象）；`pa_static_strs_init()` 在 `main` / 模块 init 的序言里
把它填好——放在 `pa_register_literals` 之后，因为它依赖
`pa_disable_unknown_index` 的结果（驻留对象要不要登记进 RC 索引）。只有
`0x00–0x7F` 是合法单字节 str，生成代码也只对 `< 0x80` 的字节取驻留对象；
`0x80–0xFF` 那些槽只为让 `pa_char_str` 的越界调用有个确定对象可返回。于是 `s[i] == "0"` 这种逐字符比较整个跳过
`pa_str_eq`（str_build 里每轮内层循环 60 次）；bytes 字面量不参与（`pub` 语义
不同）。

大小比较走 `pa_str_cmp`（三路比较，返回 -1/0/1）。

> 这里曾经有个静默 bug：大小比较只生成了一行注释、什么指令都没有，于是 `a < b` 拿到栈槽里的残留值，恒为 `False`。发现它的方式是对着 CPython 跑差分测试。

## 汇编里的坑

| 坑 | 现象 | 处理 |
|---|---|---|
| 字符串常量放 `.rodata` | 链接器报 `relocation in read-only section` / `DT_TEXTREL` | 改 `.data.rel.ro` |
| 浮点常量不能直接进内存 | 没有 `movsd $imm, mem` 这种指令 | `movabsq` → `rax` → `movq` → `xmm0` → `movsd` |
| `neg` 一个 double | `0 - x` 多一条依赖 | 翻符号位：`xorpd` 一个 `0x8000...` 掩码 |
| 缺 `.note.GNU-stack` | 链接器警告「missing .note.GNU-stack section implies executable stack」 | 文末补一行 |
| `%al` 没清零 | 变参 ABI 要求 `%al` 是使用的 vector 寄存器个数 | 调用前 `xorl %eax, %eax` |
| 局部区压到 push 的寄存器上 | 覆盖保存的 callee-saved，返回时把垃圾弹回调用方 | 局部区从 `out_bytes_(%rsp)` 起往上排，push 的寄存器在它上面 |
| push 奇数个寄存器 | `call` 时 `rsp` 不是 16 对齐，被调用方一用 `movaps` 就段错误 | 帧大小把 push 的也算进去一起对齐 |

表里最后一行的背后是一个不变量：`call` 那一刻 `rsp` 必须 ≡ 0 (mod 16)。这条
不变量还在手写汇编里踩过两处更隐蔽的违规：C 回调跳板自己再 `call` 时漏了补那
8 字节；closure shim 的 `subq tmp → addq spill → call` 序列里 tmp 的公式只保证
了 `addq` 之前对齐（spill 是 8 的奇数倍时 `addq` 之后又歪回去）。gcc 14 恰好没在
对齐敏感的位置用 SSE，所以一直潜伏；gcc 15 在运行时函数里用 `movaps` 溢出 xmm，
才变成 SIGSEGV / SIGILL。两处都修掉后，普通运行时构建里的 `-mstackrealign` 缓解
已撤掉（只有 ASan 插桩那份运行时对象还带着这个开关）；另外加了
`PYADAMAS_ALIGN_CHECK=1` 自检——每个生成函数入口检查
`rsp % 16 == 8`，不满足直接 `ud2`，把「崩在运行时别处」变成「精确停在违规函数
的入口」。调查过程见 [stack-alignment.md](stack-alignment.md)。

## 调试

```bash
./build/dev/pyadamas asm script.py            # 看生成的汇编，带 IR 注释
PYADAMAS_KEEP_ASM=1 ./build/dev/pyadamas run x.py # 保留中间 .s
./build/dev/pyadamas build x.py -o x && gdb ./x
objdump -d --no-show-raw-insn ./x | less
```

汇编里每条指令前都有对应的 IR（`# %3 = bin.add %1, %2`），对照 `pyadamas ir` 的输出就能定位到是哪条 IR 生成错了。

### DWARF 行号

`build` 默认在汇编里发一行 `.file 1 "<源路径>"`，并在函数体内按源码行发
`.loc 1 <line>`；GNU as 据此生成 `.debug_line`。每个函数还有一个可读
符号 `pyadamas_fn_<序号>_<名字>`（内部跳转仍用 `.LfN`）。

```bash
addr2line -e prog -f 0x5d1b          # 地址 → 函数 + 源文件:行
readelf --debug-dump=decodedline prog
nm prog | grep pyadamas_fn
```

只有行号，没有 `.debug_info`：`gdb` 能定位源码行，但看不到变量名和类型。
未捕获异常另有一张 codegen 发出的 PC → (函数, 行号) 表：模块入口注册，
异常路径用 libgcc 的 `_Unwind_Backtrace` 逐帧取返回地址、查表命中才打印
（0.163；0.160 时是手走 rbp 链，因此整个项目都得保留帧指针）。生成器/协程的
堆帧不在调用栈上，`coro_step` / `gen_run` / 异步生成器驱动会在异常逃出时
把 resume 帧挂到异常上，未捕获时追加打印（仅函数名，0.161）；优化内联后
内联体归到调用者。优化遍复制/内联指令保留行号。

### CFI（栈展开规则）

`.debug_line` 只回答「这条 PC 属于第几行」，不回答「上一层帧在哪」。后者是
DWARF 的调用帧信息（CFI）：每个函数在 `.eh_frame` 里有一段 FDE，描述当前
PC 下 CFA 是哪个寄存器加多少、被保存的寄存器各在哪个槽。gdb 的 DWARF
展开器和 libgcc 的 `_Unwind_Backtrace` 都只读这张表，既不解析 prologue 也不
跟 rbp 链，所以生成代码缺 CFI 时它们走到第一个生成帧就断。0.162 起每个
生成函数都带 CFI。0.163 的帧里不再有 rbp，CFA 全程由 rsp 定义：

| 位置 | 发出的规则 |
|---|---|
| 函数入口 | `.cfi_startproc`（CFA = rsp+8） |
| 每压一个 callee-saved | `.cfi_def_cfa_offset <累计>` + `.cfi_offset <reg>, -<累计>`（生成器压 `%r15`） |
| `subq` 留出参区和局部区 | `.cfi_def_cfa_offset frame_bytes_+8` |
| 尾声 | `.cfi_remember_state` → `addq` 退局部区（更新偏移）→ 每个 `popq` 后 `.cfi_restore` + 更新偏移 → `ret` → `.cfi_restore_state` |

`remember_state` / `restore_state` 让一个函数里的多条 `return` 各描述各的：
`ret` 之后 CFI 状态退回函数体，后面的块（异常落地、另一条返回）继续按体
规则展开。函数符号同时补了 `.type ..., @function` 和 `.size`。手写的那几段
汇编（类析构器、GC release、生成器帧释放、C 回调跳板、模块 init 桩、`main`）
也各自包了 `.cfi_startproc` / `.cfi_endproc`；C 回调跳板还要自己再发一次间接
调用，`call` 前先 `subq $8` 把 `rsp` 补到 16 对齐、返回后 `addq $8`，CFI 的
`def_cfa_offset` 也跟着在 8 / 16 之间切换（其余几段手写汇编不动 `rsp`，只用到
入口规则）。

```bash
readelf --debug-dump=frames prog | grep -c FDE   # 每个函数一段
gdb -batch -ex 'break *pyadamas_fn_1fib_1+4' -ex run -ex 'bt' prog
```

`bt` 默认在 `main` 停下（gdb 的 `set backtrace past-main off`），要看 libc 的
启动帧得加 `set backtrace past-main on`。帧里已经没有 rbp，能从函数体
任意 PC 走回调用方，只可能是 CFI 起的作用。运行时的 traceback 用的是同一个
展开器，所以 `objcopy --remove-section .eh_frame` 之后连回溯也一起没了——
链接时显式带 `-Wl,--eh-frame-hdr`，保证 `.eh_frame_hdr` 索引在。
`tests/cfi/` 用 `_Unwind_Backtrace` 数帧做回归：探针只认 `.eh_frame`，
CFI 一断深度就从 7 掉回 2。

还没做的：`.debug_info`（变量、类型、内联帧的 `DW_TAG_inlined_subroutine`）
和列区间。

## 引用计数在汇编层的代价

RC 不新增 IR 指令，在 IR 里仍以 `call.rt` 的形式出现；不过 `RcInc` / `RcDec` /
`RcMark` / `RcCleanup` 四个记账入口已经由生成端内联成快路径（见本节末
「记账调用的内联」），不再每次都 `call`。RC 在汇编层的其余表现：

- 字面量 rc = `INT64_MIN`，inc/dec 碰到永生对象直接跳过；
- 有堆字段的类才有 `.LdtorN` / `.LgcrelN`，没有堆字段的类传 `NULL`；
- 分配量超过阈值才触发循环收集；阈值是「上次收集后的存活堆字节数」，
  下限 2MB——存活堆翻倍才扫一次，总扫描量是 O(live)，垃圾多时回落到
  2MB 下限（扫描本身也便宜）；`PYADAMAS_GC_DISABLE=1` 可整机关闭循环收集
  （只留下引用计数，会漏检环）。纯整数代码不分配堆，完全不受影响；
- 存活堆索引只在程序可能用到 unknown 时维护：IR 里没有 unknown 类型、
  没有 `RcIncUnknown`、没有 `super()` / `isinstance` 这类运行期类型检查、
  也不涉及 asyncio/生成器时，入口调用 `pa_disable_unknown_index`，
  此后分配/释放不再做索引哈希表的插入和删除（`PYADAMAS_FORCE_UNKNOWN_INDEX=1`
  可强制保留，用于 A/B）；
- 同一条件（exact-RC）下，**字符串和没有堆字段的类实例不进 GC 堆链**：
  它们没有出边、不可能成环，RC 归零即释放；垃圾容器持有的这类叶子在
  清扫时按正常引用释放。剩余链上对象数在分配/释放时增量维护，收集时
  不用再遍历计数；地址表和工作栈跨收集复用，不再每轮 calloc/free；
- exact-RC（整程序无 unknown）时，容器与闭包的子节点按静态类型直接标记
  （`gc_mark_direct`，不查堆表），纯数值元素的容器完全不标记；minor 也
  不再扫帧记录和寄存器——所有引用都归槽/临时值/容器所有。
  `PYADAMAS_GC_TIMING=1` 可打印每轮的分段耗时；
- 不含临时堆值的语句 / 循环 / 函数在降级期把 mark/cleanup 删掉。
  所以整数循环和递归生成的汇编与 0.6.1 完全一样；
- **只读形参免 RC**：函数降级完成后扫描——形参槽在整个函数里从未被
  覆盖、也没被取址时，入口的 retain 和所有出口的 release 都被撤掉。
  形参本来就是按值借用的：调用期间实参的引用在调用方手里，函数体只读
  就无需自己再持有一份。方法调用的 `self` 绝大多数属于这一类；
- 调用结果没人读时（表达式语句里的调用），返回值不再搬回栈槽。

### 记账调用的内联

`gen_call_rt`（`src/backend/call.cpp`）对 `RcMark` / `RcCleanup` / `RcInc` /
`RcDec` 四个 RtFn 有专用发射：快路径就地展开，没走快路径时才落回运行时
（`pa_rc_inc_slow` / `pa_rc_dec_slow`；`RcCleanup` 落回的是 `pa_rc_cleanup` 本体，
它没有单独的 slow 变体；`RcMark` 干脆没有慢路径）。动因是 perf 采样：
queue_sim / str_build 里 `pa_rc_cleanup` 占 8.2% / 11.4%，`pa_rc_inc` 占
4.0% / 3.0%，`pa_rc_dec` 占 4.2% / 3.7%——这几个函数体本身只有 3~6 条指令，
call/ret 加被调用者保存的开销比它们干的活还大。

- `RcMark`：编译成一条 `movq temp_len(%rip), %rax`。临时栈水位就是个全局
  变量，为一次 load 发一次调用不划算。
- `RcCleanup`：只内联最常见的快速出口——`cmpq temp_len(%rip), <mark>` 相等
  就直接跳过（绝大多数语句边界上水位根本没动过）；不等才整体交给
  `pa_rc_cleanup`，由它逐个弹出并 dec。
- `RcInc`：`p == NULL`、`rc <= 0` 落回慢路径；`rc == INT64_MIN`（字面量等
  永生对象）直接跳过；否则把 `rc` 加一写回。`rc <= 0` 交给慢路径是为了报
  `refcount increment on a freed object`，不是正常路径。
- `RcDec`：比 inc 多一道 `pa_gc_phase_fast` 判断——收集进行中只登记边、绝不
  重入释放，所以整个落回慢路径；`rc == INT64_MIN` 跳过；`rc <= 1` 也落回
  慢路径，真正的析构、池归还在那里；只有「减完仍 > 0」才在生成代码里就地
  减一。
- `RcIncUnknown` / `RcDecUnknown` 没有内联：它们要先查堆索引确认机器字是不是
  堆指针，仍然直接 `call`。

慢路径**必须**先 `spill_fp_slots`、回来再 `reload_fp_slots`：xmm 全是
caller-saved，慢路径会调运行时、可能一路走到析构函数，提升过的浮点值不落盘
就被冲掉了。快路径不产生调用，浮点槽不必往返内存。

## 性能现状

同一台机器、`gcc:14` 容器、直接运行编译产物，对手 CPython 3.13.5：

| 程序 | 相对 CPython 3.13 |
|---|---|
| `fib(35)`（递归 + 调用） | 约 16× |
| 2 亿次 `while` 累加 | 约 33× |
| 2000 万次方法调用 | 约 19× |

0.156/0.157 后 `bench/` 的浮点用例明显变快：mandelbrot 同机 release
26.4ms → 12.0ms（约 CPython 23.3×），spectral_norm 34.1→30.0ms，其余
用例持平。

绝对值随机器和负载浮动，倍数基本稳定。RC 之前同机基线（同一轮测量）
是 0.044s / 0.289s / 0.047s，RC 之后是 0.039s / 0.294s / 0.062s：
整数路径不变，方法调用因为每个 self 参数的 inc/dec 慢了约 30%。

比的是「PyAdamas 出的汇编 + `cc -O0`」对「CPython 的解释器」。

寄存器分配之前这两项是 0.08s / 0.34s：`fib` 快 1.4 倍（它全是调用，省不下多少），整数循环快 5.7 倍。现在有跨块 CSE、循环旋转和拷贝合并，仍然没有做强度削减、通用向量化和全局寄存器分配，`cc` 也没开优化；赢在「静态类型 + 直接出机器码 + 热变量在寄存器里」这三点。

## 0.271–0.300 新增快路径与融合（补记）

- **RC inc/dec 内联热路径缩短（0.291）**：普通堆值把 `INT64_MIN` 永生哨兵判断移到冷分支——`RcInc` 在非正计数上用 `negq/jo` 识别哨兵，`RcDec` 用 `subq $1` 同时做有符号比较与减法，只有原计数大于 1 才写回；`str`/`bytes` 保留优先判断永生值（统一推迟的实验让 str_build 慢 4.7%）。
- **元组分配内联（0.280）**：kind-0 元组（全非堆元素、n≤4）在池命中且 `gc_need_check==0`、diag 关闭时，直接在生成代码里弹池、初始化；任一守卫不满足落回 `pa_tuple_new_filled` 原路径。
- **list.append 内联（0.282）**：无扩容（len<cap）、无位图（heap_bits==NULL）、非 kind2 时内联成 `data[len++]=v` 一条 store；三个守卫任一不满足落回原调用；RC retain 仍在 append 前单独发射。
- **str ASCII 下标内联（0.287）**：`gen_str_index_inline`——`0<=i<pub` 且 `pub==len`（等号当且仅当全 ASCII）且字节 <0x80 时直接查 `pa_char_tab`；负索引/越界/非 ASCII 落回 `pa_str_char_at`（归一化、IndexError、UTF-8 码点语义不变）。
- **临时栈 push/take 内联（0.286）**：永生 push/take 仅 5 条指令；非永生 push 12 条，stats/diag/扩容守卫落回原函数；take 内联栈顶命中 + 从后往前扫描 + swap-remove，与运行时逐位一致。
- **str 单字节相等快路径（0.288）**：指针不等后双方 len==1 直接比 data[0]，不等直接出结果（Eq→0 / Ne→1）；len!=1 才落回 `pa_str_eq`。
- **Bin+StoreGlobal 融合（0.289）**：整数 Bin（Add/Sub/Mul/位运算/移位）+ StoreGlobal，结果块内只用一次且不跨块 → 结果留在 `%rax`，紧跟一条 `movq %rax, global`，省掉栈槽往返。
- **守卫整数叶子虚方法内联（0.294，`src/backend/vcall_inline.cpp`）**：O2 无额外实参、返回整数的普通 `VCall`，至多为本模块 4 个实际虚表生成守卫，比较实际虚表地址（继承同一方法的类共用内联体）；只接受整数常量返回、读 self 整数实例字段或两个此类字段加减乘的叶子方法；命不中、候选过多或带副作用时保留原间接调用。
- **守卫 VCall→Add→StoreGlobal 融合（0.297）**：只融合相邻的守卫零参数整数链，调用与加法结果均须块内单用；命中体和间接回退都把返回值留在 `%rax`，合流后加上调用前已求值的 LHS 快照再写全局——回退可能改写全局或抛错，不重新读取左值。
- **RcDec 条件全局转发（0.296）**：匹配同块「读旧全局、发布新值、RcDec、再读同一全局」；只有没有运行时调用的减计数快路才转发已发布值，一旦走慢调用，返回后重新读全局；不撤掉 0.295 的重入屏障。
- **浮点 IndexStore 快路（0.298）**：O1+ 的 checked 浮点下标写入保留边界检查和负下标归一化，通过后仅在运行时 `elem_kind==0` 时直接写入，不读旧值也不释放；非零种类走原路径（不用静态 `list[float]` 代替所有权证明）。
- **函数入口与循环头 32B 对齐（0.275/0.282）**：`.p2align 4` 改 `5`，对齐 DSB uop 缓存窗口粒度——代码尺寸变化可能把热循环挤出 DSB 窗口、落回 MITE 传统解码，产生 ±10% 的远端效应；用 `idq.dsb_uops` / `idq.mite_uops` 诊断。
