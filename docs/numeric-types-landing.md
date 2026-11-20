# P1 定宽整数 —— 落地与闸门（0.341 预留，fixint1 分支）

> 范围：docs/numeric-types-design.md §1 的 P1——显式定宽/无符号整数
> i8/i16(short)/i32/i64(long)/u8/u16/u32/u64 + char=i8 别名。基线 master
> ee68d5c / 0.340（含 P2 类型标签），独立 worktree /tmp/wt-fixint1（分支
> fixint1），不推 master、不动主树。**无门控**：定宽是 opt-in 标注，
> 默认 int 的代码路径逐字不变。
>
> **结论（先说）**：P1 落地。定宽标注、C 式提升、宽度回绕、字面量 coerce
> 范围检查、显式 cast、u64 无符号除法/移位/比较/输出全部可用；**现有
> 程序（无定宽标注）的生成 asm 逐字节不变**（bench 120 个文件 cmp 全同，
> 其中 117 个成功编译，与 P2 口径一致）。明确未做：f32/big（P3）、
> u64 大整数字面量、定宽容器元素的隐式字面量 coerce、u64 列表排序、
> SIMD 通道化（见 §6）。

## 1. 类型与标注

| 标注 | TyKind | 宽度 | 槽内规范值 | 溢出 |
|---|---|---|---|---|
| i8 / char | I8 | 8 | 符号扩展（−128..127） | 2^7 回绕 |
| i16 / short | I16 | 16 | 符号扩展 | 2^15 回绕 |
| i32 | I32 | 32 | 符号扩展 | 2^31 回绕 |
| i64 / long / int（默认） | Int | 64 | i64（现状） | 2^63 回绕 |
| u8 / uchar | U8 | 8 | 零扩展（0..255） | 2^8 回绕 |
| u16 / ushort | U16 | 16 | 零扩展 | 2^16 回绕 |
| u32 / uint | U32 | 32 | 零扩展 | 2^32 回绕 |
| u64 / ulong | U64 | 64 | 位模式（语义无符号） | 2^64 回绕 |
| bool（已有） | Bool | 1 | 0/1 | — |

- **char = i8 别名**（C 式字节，x86 char 默认 signed），不是 Unicode 码点
  （码点用 i32/u32；str 内部已是码点序列）。
- i64 不新增 TyKind：Int 就是 i64（int/i64/long 解析到同一类型）。
- 存储统一 8 字节 i64 槽（符号/零扩展），**不改帧布局、不改 RC 头、
  不改 ABI**：一个定宽形参与 int 形参占用同一种寄存器/栈槽。
- 跨模块接口按新编码 17..23 序列化（0..16 逐字沿用，旧 modcache 解码不变）。

## 2. 提升规则（§1.2，定死；实现只有一处）

typecheck_types.h 的 int_promote(l, r) 是**唯一**实现：类型检查的
binop/compare、降级期两侧转换、常量折叠都调用它。规则：

1. 任一 Float/F32 → float（F32 未实现，全 f64）。
2. 纯整数取**较宽**宽度；**同宽 signed+unsigned → unsigned**（C 口径，
   所以 i64 + u64 → u64）。默认 int(i64) 与定宽混时 i64 最宽。
3. Bool 参与 → 提升为对方的整数类型；bool+bool → int（CPython True+True==2）。
4. 位运算（& | ^）同 §1.2 提升（i8 & u8 → u8）；bool & bool 仍是 bool。
5. 移位结果类型**跟随左操作数**（u8 << 3 是 u8，计数只要求整数）。
6. 比较：先按 §1.2 提升，两侧**转换到提升后的类型**再比，结果 bool。
   u32 与 i32 混比按 unsigned：i32(-1) > u32(1) 为 True。
7. 非数值域（unknown/Optional/None 混进来）→ int（旧行为，保守）。

**降级期两侧必须转换到提升后的类型**（lower_expr_ops.cpp 的 binop 与
compare）：i32 −1 与 u32 1 若各按自己的机器字比，符号扩展后的 −1 会给出
相反结论。这是本轮第一个静默算错风险点，已由 332/333 的混符号用例钉住。

## 3. 机器表示不变量（codegen 简化的前提）

**每个定宽值在槽里恒为规范形式**：有符号 = 低 w 位符号扩展，无符号 =
低 w 位零扩展。产生/消费点如下，任何一处漏正常化都是静默算错：

| 点 | 处理 |
|---|---|
| 字面量 | 类型检查已验值域（在范围内），常量直接按目标类型生成 |
| cast_* / 赋值 coerce | norm_int_rax：8 位 movsbq/movzbq、16 位 shl 48+sar/shr 48、32 位 movslq/movl |
| 二元运算结果 | gen_bin_int_fixed 末尾 norm_int_rax（加减乘/位运算/除法/取模/幂） |
| 一元 − / ~ | gen_un 按结果类型正常化（−(−128) → −128，~u8 200 → 55） |
| 常量折叠（opt.cpp） | Bin/Un/Convert 折叠结果用 int_narrow_value 按结果类型回绕 |
| 函数实参/返回 | 调用点与返回点的 coerce 已发转换 |
| 容器元素/字段 | 写入点 coerce（元素类型是定宽时，右侧必须是 cast 或字面量——见 §6.3） |

**推论（本轮最重要的简化）**：宽度 < 64 的无符号值在 64 位槽里恒非负，
所以 sarq 与 shrq、jl/jg 与 jb/ja、idivq 与 divq、signed 魔数除法在这些
宽度上**逐例等价**。真正需要无符号指令的只有 **u64**：

- 除法/取模/整除：u64 走 divq（gen_udiv64，先 xor %rdx，余数在 %rdx）；
  且除零检查**跳过 INT64_MIN/−1 溢出检查**（无符号域里那是
  2^63/(2^64−1)=0，不是溢出；有符号检查会误报 panic）。宽度 < 64 的魔数
  常数除法照旧。
- 右移：u64 用 shrq，其余用 sarq（等价）。
- 比较：u64 用 setb/setbe/seta/setae（分支形用 jb/jbe/ja/jae），其余 jl/jg。
- 常量折叠：u64 按 uint64 比较、u64→double 按无符号折算。

## 4. 字面量 coerce 与显式 cast

- 裸 5 默认 int(i64)。
- **定宽目标 + 字面量**：范围内隐式 coerce（类型检查把字面量的静态类型
  改写成目标类型，降级按它生成常量），**超范围编译报错**：
  "literal 200 is out of range for 'i8' (allowed -128..127)"。
  覆盖赋值（含类字段 c.n = 127）、带标注的声明、函数实参、默认参数、
  return 五类位置。
- **宽→窄必须显式 cast**（设计 §1.2）：c: i8 = n（n:int）编译报错并
  提示 cast_i8(x)；调用点实参同理。这是「静默截断」的唯一闸门。
- 复合赋值按 C 口径隐式转换回目标类型：u: u8 = 250; u += 10 → 4
  （回绕确定，不是 UB）。
- cast_i8/…/cast_u64（另有 cast_i64/cast_int 别名）：C 式截断 +
  pa 回绕，**超范围不报错**（这正是 cast 的意义），与字面量严格相反。
- abs/sum 对定宽元素按 C 的**整数提升**返回 int（abs(i8 -128) = 128，
  不在 i8 里回绕成 −128）。

## 5. u64 的 I/O

u64 的 ≥ 2^63 值必须按无符号输出，否则全是负号。新增运行期入口
（追加在 RtFn 末尾，不改既有编号）：
pa_str_from_u64（str/repr/f-string/print 到文件）、pa_print_u64、
pa_fmt_u64（format()/f-string 格式说明符）、pa_uint_to_base
（hex/bin/oct）。pa_fmt_i64 拆出公共尾段 fmt_int_mag，pa_fmt_u64 复用
（格式化语义逐字一致）。printf 风格 %d/%x/%o 新增描述符 'u'（无符号
量值、不带负号），其余定宽仍用 'i'（槽内值已是正确值）。

定宽 → float：窄/短宽度值非负或已符号扩展，cvtsi2sdq 直接可用；
**u64 → f64** 用 gcc 的无符号折算 (x>>1)|(x&1) 再乘 2，避免 ≥2^63 转成
负数。

## 6. 边界（v1 明确不做 / 不可做）

1. **定宽过 unknown 边界擦宽度提升 i64**（设计 §6.5 的保守 v1）：
   ident(x: object) 收到 u64 max 后按 i64 位模式解释成 −1；回来必须
   cast_u64(x)。这不是 bug，是定死的 v1 边界（要宽度信息得等带宽度
   的运行期 tag，属 P2 扩展）。窄宽度（i8/u8/…/u32）过 unknown 后值与
   int 一致，无观察差异。
2. **u64 大整数字面量不可写**：词法把 INT 字面量解析进 int64，
   18446744073709551615 会报 "integer literal too large for int64"。
   用 cast_u64(-1) 等构造。要支持需改词法/常量表示，属后续。
3. **定宽容器元素的插入需要显式 cast**：l: list[i8] 的 l.append(5)
   报 "cannot store 'int' into 'list[i8]'"，写 l.append(cast_i8(5))。
   容器元素位置没有「字面量表达式」上下文（merge_elem 只拿到类型），
   所以不开口子——宁可要求 cast，也不静默窄化。
4. **u64 列表排序**：sorted()/sort() 的 int kind 比较在运行时按 int64
   有符号比，u64 ≥ 2^63 会排错。min/max 走 IR Cmp（已无符号）正常。
   SIMD 通道化（u8×16/i32×4、i32 的 pmulld）留后续（本轮只保证标量语义）。
5. **移位计数沿用 pa 既有行为**：x86 按 64 取模（u8 << 8 在 8 位回绕后
   为 0），负数计数不报 ValueError（与默认 int 的现状一致）。
6. **类型名/repr 仍是 int**：type(x).__name__ == 'int'、
   isinstance(x, int) 为 True（定宽在 Python 语义里是 int 的子类）。
7. format() 对定宽值支持十进制/进制/对齐等整数格式说明符（复用
   i64/u64 格式化）；precision 等非法说明符按原语义报 ValueError。

## 7. 闸门（实测）

- **默认 int 程序的 asm 逐字节不变**：bench 全量 120 个 .py 文件
  pyadamas asm 输出与基线 ee68d5c 逐字节 cmp **120/120 identical**
  （其中 3 个本来就编译失败，空输出同样相同；117 个有效文件）。
- **difftest（O0 vs O2）**：308 用例，**0 不一致**（新增 332-334 在内）。
- **全套件 normal**：**326 ran ok EXIT=0**（基线 323；+332/333/334）。
  反例组 +3（f67/f68/f69）。
- **全套件 ASan（JOBS=1）**：**326 ran ok EXIT=0**（见 §8 记录）。
- **O0/O1/O2 一致**：332/333/334 三个用例三档输出逐字节相同。
- **CPython 对拍**：
  - 332（取值不溢出宽度，定宽在 Python 语义里仍是 int）：与 CPython
    **逐字节一致**（用 from __future__ import annotations + cast 桩）。
  - 334（I/O/unknown 边界）：CPython 模型（ctypes 式 wrap 桩）**23 行里
    22 行一致**，唯一差异是 §6.1 的 u64 过 unknown → −1（预期边界）。
  - 333（回绕/混符号）**刻意不与 CPython 对拍**：pin 的是 pa 的 C 式
    回绕（127+1 → −128、u8 255+1 → 0、i8(−1)<u8(1) 为 False）。
- **check_size**：新增文件 src/codegen_int_fixed.cpp（134 行）/
  src/codegen_div.h（19 行）都在限内；把 codegen_ops.cpp 拆到 1184 行、
  typecheck_expr.cpp 压回 1200 行、codegen_impl.h 压回 1200 行，
  **没有新增超限文件**（基线本就超限的 codegen_regalloc/driver_build/
  ir_ops.h/lower_impl.h/typecheck_types.h 维持原状；typecheck_types.h 因
  本轮新增 ~244 行从 1337 涨到 1581，仍是既有的超限文件，拆文件留后续）。

## 8. 负面/残余

- 提升规则没有「更安全」的替代：本轮按设计 §1.2 的 C 口径定死
  （同宽 signed+unsigned → unsigned）。这意味着 i8(-1) < u8(1) 为 False、
  i32(-1) + u32(2) 为 1。**这是 C 的坑，但对定宽类型是可预期语义**；
  用例 333 把每一条都钉在期望输出里，本文档 §2 写明规则。
- 「宽度选择」的落地方式与设计 §1.3 的字面描述不同：没有发
  addb/addw/addl，而是**64 位域计算 + 结果正常化**（宽度只在正常化/符号
  判定上体现）。理由：(a) 现有寄存器分配/操作数层是 64 位 MOperand，
  byte/word 指令写内存操作数会破坏槽内规范值；(b) 加/减/乘/位运算在
  mod 2^64 与 mod 2^w 下逐位等价（2^w 整除 2^64），结果相同；(c) 避免为
  窄宽度再写一套发射路径。u64 的无符号除法/移位/比较按要求用了
  divq/shrq/jb-ja。
- 常量折叠必须同步回绕，否则会出现「u8 的 250+10 折叠成 260 存进 u8
  槽」这类静默算错（本轮实测抓到：u += 10 在全局常量传播后被折成 260；
  修复 = opt.cpp 的 Bin/Un/Convert/Cmp 折叠按类型回绕/无符号）。
- 环境噪声：/tmp 下有 root 所有的旧日志文件，工作日志一律写 build/ 下。
