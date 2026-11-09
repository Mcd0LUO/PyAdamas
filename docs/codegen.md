# 代码生成

`src/codegen.cpp`：IR → AT&T 语法的 GNU as 汇编，调用约定是 System V AMD64。

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
.Lstr0_c: .ascii "hello, py23\000"

        .bss                                   ; 全局变量
.Lg0:   .zero 8

        .section .data.rel.ro,"aw",@progbits   ; 每个类一张 vtable
.Lvt0:  .quad .Lf3
        .quad .Lf4

        .text
.Lf0:                                          ; 每个 IR 函数一个 label
        pushq %rbp
        movq  %rsp, %rbp
        subq  $48, %rsp
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

每个有堆字段的类会生成两个回调：`.LdtorN`（按字段偏移调 `py23_rc_dec`，
正常引用计数归零时用）和 `.LgcrelN`（调 `py23_gc_unref_child`，只减掉
指向可达对象的引用，GC 清扫时用）。对象分配时把这两个地址传给
`py23_obj_new`。

全局变量在 `.data.rel.ro` 里还有一张 `.Lgroots` 根表（每个全局槽一个
`.quad .LgN`）。生成的 `main` 在调用入口前执行：

```asm
        leaq .Lgroots(%rip), %rdi
        movq $<全局数>, %rsi
        movq %rsp, %rdx              # 栈顶
        call py23_gc_init
```

循环收集器按全局表、临时值栈、真实栈和 callee-saved 寄存器找根。

## 寄存器策略

IR 一行没改，全部优化都发生在这一层。五件事叠起来，把「值在寄存器里」做实：

| 机制 | 触发条件 | 省掉什么 |
|---|---|---|
| 变量提升 | 按 `load.slot` + `store.slot` 次数排序，最热的 5 个变量整函数常驻寄存器 | 每次读写各一次内存访问 |
| 临时值别名 | `%v = load.slot x`，且 %v 只被读一次、同块 | 整条 load |
| 结果融合 | `%v = ...` 紧跟 `store.slot x, %v`，且 %v 只被读一次、同块 | 整条 store |
| 常量延迟 | `%v = const.int N`，只被读一次、同块、在 32 位有符号范围内 | 一条 mov + 一次内存往返 |
| 比较连分支 | `%v = cmp` 紧跟 `branch %v`，且 %v 只被读一次、同块 | setCC + movzbq + 槽往返 + cmpq |

`while i < n: s += i * 2; i += 1` 的循环体，优化前后：

```asm
# 之前：21 条，每个临时值都先落一次栈再读回来
.Lf1_b2:
        movq  -16(%rbp), %rax        # %5 = load.slot s1
        movq  %rax, -48(%rbp)
        movq  -24(%rbp), %rax        # %6 = load.slot s2
        movq  %rax, -40(%rbp)
        movq  $2, -32(%rbp)          # %7 = const.int 2
        movq  -40(%rbp), %rax        # %8 = bin.mul %6, %7
        imulq -32(%rbp), %rax
        movq  %rax, -56(%rbp)
        movq  -48(%rbp), %rax        # %9 = bin.add %5, %8
        addq  -56(%rbp), %rax
        movq  %rax, -40(%rbp)
        movq  -40(%rbp), %rax        # store.slot s1, %9
        movq  %rax, -16(%rbp)
        ...                          # i += 1 原样再来一遍，另外 7 条
```

```asm
# 现在：5 条，两个变量全程待在寄存器里
.Lf1_b2:
        movq  %rbx, %rax             # %8 = bin.mul %6, %7（%6 就是 %rbx）
        imulq $2, %rax               # 常量直接当立即数
        movq  %rax, -80(%rbp)        # %8 只被用一次，落一次栈
        addq  -80(%rbp), %r12        # %9 就地写进 s1 的寄存器
        addq  $1, %rbx               # i += 1 一条搞定
```

### 为什么不做全局寄存器分配

变量提升是**函数级、固定**的：一个变量要么整函数在寄存器里，要么整函数在栈上。不做活跃性分析，因此不产生 spill。

代价是寄存器被长期占着。收益是分支汇合、循环回边、异常路径全都天然正确——不存在「这条边上值在寄存器、那条边上在栈里」这种需要插 phi 或 reload 的状态。Python 的局部变量本来就不多，五个够用。

真要做图着色，IR 的形状已经准备好了（vreg 是 SSA 风格的，每个只定义一次），但那要等有优化遍需要它的时候。

### 寄存器池

`%rbx` / `%r12` / `%r13` / `%r14` / `%r15`，全部是 **callee-saved**：被调用方保证返回时原样恢复，所以跨函数调用不需要把值挪回栈，也不需要在调用点保存。

浮点值不进寄存器。`xmm` 全是 caller-saved，跨调用要自己 save/restore，而浮点循环远没有整数循环常见——收益抵不上复杂度。

## 栈帧布局

```
       高地址
    ┌──────────────┐
    │  调用者的栈   │
    ├──────────────┤
    │  返回地址     │  ↑ 调用者的 rsp
    ├──────────────┤
    │  保存的 rbp   │  ← rbp 指向这里
    ├──────────────┤
    │  保存的 rbx   │  -8(%rbp)      只 push 实际用到的，最多 5 个
    │  保存的 r12   │  -16(%rbp)
    │  ...         │
    ├──────────────┤
    │  slot 0      │  -8*(k+1)(%rbp)   k = push 的寄存器个数
    │  slot 1      │
    │  ...         │
    │  vreg 槽 0   │  -8*(k+n_slots+1)(%rbp)
    │  vreg 槽 1   │
    │  ...         │
    └──────────────┘  ← rsp（16 字节对齐）
       低地址
```

- 局部区从 `-8*(k+1)(%rbp)` 开始，**必须让开 push 的 k 个寄存器**。让不开就会把保存的 `%rbx` 当成变量槽写掉，函数返回时 `pop` 回一个垃圾值，调用方的变量凭空变了。这个 bug 差分测试抓到过一次，现象是「调完一个函数，调用方的循环变量变成了 -19」。
- 整个帧（push 的 + `subq` 的）合起来对齐到 16 的倍数。push 个数是奇数时 `subq` 的量要相应减 8，否则 `call` 时 `rsp` 不是 16 对齐，任何用到 `movaps` 的被调用方（包括 libc 的 `printf`）都会段错误。
- 栈传的参数在读的时候是 `16 + off(%rbp)`（返回地址 + 保存的 rbp 各 8 字节）。

函数末尾：

```asm
        leaq  -8*k(%rbp), %rsp     # 退到 push 的寄存器下面
        popq  %r15 ... popq %rbx    # 逆序弹回
        popq  %rbp
        ret
```

**不能用 `movq %rbp, %rsp`**——那会跳过 push 的寄存器，等于不恢复。

### 非参数变量清零

序言里对每个非参数变量发一条 `movq $0, ...`。`x: int` 这种纯声明在 lower 里不生成任何指令，靠的就是「槽初值是 0」；栈帧里的内存是脏的，不显式清掉就会读到上一个函数留下的垃圾。

## vreg 槽位复用

如果每个 vreg 独占一个槽，2000 行的函数会生成 100KB 栈帧，递归几层就爆。所以做一次**线性扫描**：给每个 vreg 算 `[定义, 最后使用]` 区间，不重叠的共用槽位。

关键限制：**只有定义和使用都在同一个基本块里的值才能按区间复用**。

跨块的值（尤其循环头里每一轮都要读的）在静态上「已经用完」之后，下一轮迭代还会被读到。线性区间会把它的槽分给别人，然后循环第二次迭代读到垃圾。这类值一律独占槽位、区间拉满整个函数。

> 这个坑真的踩过：`for` 循环里读的列表在第 2 轮变成别的值，`py23_list_get` 段错误。修法就是上面这条规则，写在 `assign_vreg_slots()` 的注释里。

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

被调用者只保存 `%rbx` / `%r12`–`%r15` 里**实际用到的那几个**，序言 push、尾声逆序 pop。
其余寄存器（`%rax`、`%rsi`、`%rdi`、`%r8`–`%r11`、全部 `xmm`）是 caller-saved，可以随便写。

调用一个函数值的序列：

```asm
        movq  -24(%rbp), %r11        # callee 先取到 r11，避开参数寄存器
        subq  $16, %rsp              # 有栈参数时
        movq  -32(%rbp), %r10        # 栈参数
        movq  %r10, 0(%rsp)
        movq  -40(%rbp), %rdi        # 寄存器参数
        xorl  %eax, %eax
        call  *%r11
        addq  $16, %rsp
```

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

有捕获的嵌套函数在运行时分配闭包（`py23_closure_new(code, n)`），析构器
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

单继承的布局约束：子类字段接在父类字段之后，vtable 前缀与父类完全一致（覆盖沿用相同槽号，新增追加到末尾）。于是「父类类型的变量装子类实例」天然安全：

```
a: Animal = Dog()
a.speak()   →   a->vtable[0](a)   →   Dog::speak
```

构造：

```asm
        movq  $24, %rdi          # 8 * (1 + 2 个字段)
        leaq  .Ldtor0(%rip), %rsi
        leaq  .Lgcrel0(%rip), %rdx
        call  py23_obj_new       # rc_alloc，全零，rc = 1
        leaq  .Lvt0(%rip), %rcx
        movq  %rcx, (%rax)       # 对象头写 vtable
```

分配全零是有意的：意味着「声明了但没赋值的字段」就是 `None` / `0` / 假，和栈槽的约定完全一致。

虚调用：

```asm
        movq  -16(%rbp), %r10    # self
        movq  (%r10), %r11       # vtable
        movq  8(%r11), %r11      # 第 1 号槽（覆盖过的就是子类实现）
        movq  -16(%rbp), %rdi    # self 固定占第一个整型参数
        movq  -24(%rbp), %rsi    # 其余实参照常分类
        call  *%r11
```

槽号是编译期常量，运行时只有两次解引用加一次间接跳转。代价是不能内联（还没有去虚化遍），换来的是覆盖语义天然正确。

`Optional[Class]` 上的字段访问和方法调用会先生成一次空指针检查（`branch` + `panic`）：类型收窄做不到逐条路径成立，而段错误不带任何上下文——`py23: panic: NoneType object has no attribute 'x'` 有用得多。

## 指令选择要点

### 整数除法：先挡信号

`idiv` 在除数为 0 和 `INT64_MIN / -1` 时都触发 `#DE`，那个信号带不出任何上下文。所以先生成守卫：

```asm
        testq %rcx, %rcx
        jne   1f
        call  py23_panic_div0
1:      cmpq  $-1, %rcx
        jne   2f
        movabsq $0x8000000000000000, %r8
        cmpq  %r8, %rax
        jne   2f
        call  py23_panic_overflow
2:
```

### 整除与取模：符号修正

`idiv` 向零取整，Python 的 `//` 向下取整；余数也是 `idiv` 跟被除数同号，Python 跟除数同号。两者都在「余数非零且两操作数异号」时差一步：

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

### 浮点比较：NaN

`ucomisd` 遇到 NaN 会把 ZF/PF/CF 全置 1，直接 `setb` 会得到错误的真值。规则是：`<` `<=` `==` 额外 `setnp` 与一下，`!=` 额外 `setp` 或上，`>` `>=` 用 `seta` / `setae`（这两个对手性本身就把无序排除了）。

### 真值测试

| 类型 | 做法 |
|---|---|
| int / bool | `cmpq $0` + `setne` |
| float | `ucomisd 0` + `setne` + `setp`（NaN 算真） |
| str / list / tuple | 读对象头的 `len`（偏移 0），`cmpq $0` + `setne` |

`len` 在偏移 0 是刻意设计的：`len(x)` 一条指令，真值判断也是一条。

### 字符串比较

`==` / `!=` 走 `py23_str_eq`，大小比较走 `py23_str_cmp`（三路比较，返回 -1/0/1）。

> 这里曾经有个静默 bug：大小比较只生成了一行注释、什么指令都没有，于是 `a < b` 拿到栈槽里的残留值，恒为 `False`。发现它的方式是对着 CPython 跑差分测试。

## 汇编里的坑

| 坑 | 现象 | 处理 |
|---|---|---|
| 字符串常量放 `.rodata` | 链接器报 `relocation in read-only section` / `DT_TEXTREL` | 改 `.data.rel.ro` |
| 浮点常量不能直接进内存 | 没有 `movsd $imm, mem` 这种指令 | `movabsq` → `rax` → `movq` → `xmm0` → `movsd` |
| `neg` 一个 double | `0 - x` 多一条依赖 | 翻符号位：`xorpd` 一个 `0x8000...` 掩码 |
| 缺 `.note.GNU-stack` | 链接器警告「missing .note.GNU-stack section implies executable stack」 | 文末补一行 |
| `%al` 没清零 | 变参 ABI 要求 `%al` 是使用的 vector 寄存器个数 | 调用前 `xorl %eax, %eax` |
| 局部区从 `-8(%rbp)` 开始 | 覆盖 push 下来的 callee-saved，返回时把垃圾弹回调用方 | 局部区从 `-8*(k+1)(%rbp)` 开始 |
| push 奇数个寄存器 | `call` 时 `rsp` 不是 16 对齐，被调用方一用 `movaps` 就段错误 | 帧大小把 push 的也算进去一起对齐 |

## 调试

```bash
./build/dev/py23 asm script.py            # 看生成的汇编，带 IR 注释
PY23_KEEP_ASM=1 ./build/dev/py23 run x.py # 保留中间 .s
./build/dev/py23 build x.py -o x && gdb ./x
objdump -d --no-show-raw-insn ./x | less
```

汇编里每条指令前都有对应的 IR（`# %3 = bin.add %1, %2`），对照 `py23 ir` 的输出就能定位到是哪条 IR 生成错了。

## 引用计数在汇编层的代价

RC 不新增 IR 指令，全部通过 `call.rt` 调运行时：

- 字面量 rc = `INT64_MIN`，inc/dec 在运行时直接跳过；
- 有堆字段的类才有 `.LdtorN` / `.LgcrelN`，没有堆字段的类传 `NULL`；
- 分配量超过 2MB 才触发循环收集；纯整数代码不分配堆，完全不受影响；
- 不含临时堆值的语句 / 循环 / 函数在降级期把 mark/cleanup 删掉。
  所以整数循环和递归生成的汇编与 0.6.1 完全一样，性能差异只在
  「方法调用的 self 形参 inc/dec」这类真正的堆值传递上。

## 性能现状

同一台机器、`gcc:14` 容器、直接运行编译产物，对手 CPython 3.13.5：

| 程序 | 相对 CPython 3.13 |
|---|---|
| `fib(35)`（递归 + 调用） | 约 16× |
| 2 亿次 `while` 累加 | 约 33× |
| 2000 万次方法调用 | 约 19× |

绝对值随机器和负载浮动，倍数基本稳定。RC 之前同机基线（同一轮测量）
是 0.044s / 0.289s / 0.047s，RC 之后是 0.039s / 0.294s / 0.062s：
整数路径不变，方法调用因为每个 self 参数的 inc/dec 慢了约 30%。

比的是「py23 出的汇编 + `cc -O0`」对「CPython 的解释器」。

寄存器分配之前这两项是 0.08s / 0.34s：`fib` 快 1.4 倍（它全是调用，省不下多少），整数循环快 5.7 倍。仍然没有做常量折叠、CSE、死代码消除，`cc` 也没开优化；赢在「静态类型 + 直接出机器码 + 热变量在寄存器里」这三点。
