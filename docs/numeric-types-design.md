# 统一数值类型系统 + 运行期类型标签 + 语言缺口 设计

> 状态：设计稿（lead 起草，待评审）。动机：用户要求显式定宽/无符号整数（i32/u64/bool/char/short）
> + 任意精度 big；stdlib-b3 取证暴露 unknown 运行期类型信息不足（pprint/isinstance/repr 卡住）。
> 参照 Mojo（Int8/UInt8/.../Int64/Bool/Float32）/ Codon（int/uint）模型。

## 0. 现状（代码取证）

- **TyKind**（typecheck/types.h:25）：Unknown/None/Bool/**Int**/Float/Complex/Str/Bytes/List/.../File/.../Struct/Chan 等 47 种。
  **Int 是单一类型 = i64 机器字**（ir/types.h int64_t；asm addq/imulq）；无定宽/无符号/big。Float=f64，无 f32。Bool 已有。
- **整数溢出**：i64 二进制补码回绕（2**63→-2**63，2**64→0），**非 CPython 无限精度**。无 BigInt。
- **P3 shadow tag**（backend/impl.h:334 PaTag / rt_any.c:376）：UNK0/INT1/FRAW2/FBOX3/STR4/HEAP5。
  用途=RC/算术免探针优化，**不是完整运行期类型信息**：INT 不区分 bool/None/宽度；HEAP 不区分 list/dict/tuple/set。
- **unknown 表示**：i64 机器字（None=0/bool=0,1/float 位模式/堆地址），探针 rc_index_has 判堆。
- **isinstance/repr 缺口**（stdlib-b3 实测）：unknown 值 int/bool/None 分不清（True≡1、None≡0）、tuple 打印成指针、set 认成 dict、bytes/str 混同 → pprint 不可能正确。
- **File TyKind 已存在**（open() 返回），但 file 协议未泛化（StringIO 不通）；无 traceback 对象/sys.exc_info；无注解反射/动态类。

## 1. 统一数值类型系统

> **P1 已落地（0.341 预留，fixint1 分支）**：i8/i16/i32/i64 + u8/u16/u32/u64
> + char=i8/short=i16/long=i64 别名、C 式提升、宽度回绕、字面量 coerce
> 范围检查、cast_* 全部可用；默认 int 的 asm 逐字节不变。落地细节/边界/
> 负面结论见 **docs/numeric-types-landing.md**（§1.3 的宽度选择落地为
> 「64 位域计算 + 结果正常化」，见该文档 §8）。

### 1.1 类型阶梯（新增 TyKind）

| 标注 | TyKind | 宽度 | 机器表示 | 溢出 |
|---|---|---|---|---|
| `i8` | I8 | 8 | i64 槽符号扩展 | 2^7 回绕 |
| `i16` / `short` | I16 | 16 | i64 槽符号扩展 | 2^15 回绕 |
| `i32` | I32 | 32 | i64 槽符号扩展 | 2^31 回绕 |
| `i64` / `int`（默认） | Int | 64 | i64 | 2^63 回绕（现状） |
| `u8` | U8 | 8 | i64 槽零扩展 | 2^8 回绕 |
| `u16` | U16 | 16 | i64 槽零扩展 | 2^16 回绕 |
| `u32` | U32 | 32 | i64 槽零扩展 | 2^32 回绕 |
| `u64` | U64 | 64 | i64 | 2^64 回绕 |
| `bool` | Bool（已有） | 1 | i64 (0/1) | — |
| `char` | Char=i8 别名（见 1.5） | 8 | i64 | 2^7 |
| `float32` | F32 | 32 | xmm 低 32 / f64 槽 | IEEE |
| `float64` / `float`（默认） | Float | 64 | xmm | IEEE |
| `big` | Big | 任意 | BigInt 堆对象 | 无（无限精度） |

- 定宽整数**存储统一 i64 槽**（符号/零扩展），运算按宽度选指令 + 回绕掩码。避免改帧布局/RC 头。
- `int`/`float` 裸标注 = i64/f64（向后兼容，现状不变）。

### 1.2 提升规则（关键，C 式但简化，定死防静默算错）

二元运算结果类型 = 按格提升：
1. 任一 Big → Big。
2. 任一 Float/F32 → 浮点（F32+F32=F32，含 F64 则 F64）；整数+浮点→浮点。
3. 纯整数：取较宽宽度；**同宽 signed+unsigned → unsigned**（C 口径）；bool 参与→提升为对方整数类型。
4. 默认 int(i64) 与定宽混：i64 最宽（除 u64 同宽→u64）。
- 比较运算：提升后比较，结果 bool。unsigned 用 jb/ja，signed 用 jl/jg。
- 赋值 coerce：宽→窄需显式 cast（`a: i8 = cast_i8(x)`）或字面量范围内隐式；超范围字面量编译报错。
- **字面量定型**：裸 `5` 默认 i64；`a: u8 = 5` 字面量 coerce（范围检查）；`a: i8 = 200` 编译报错（超 127）。

### 1.3 codegen 映射

- 宽度选指令：i8/u8→addb/subb/movb（+掩码 0xFF），i16→addw（0xFFFF），i32/u32→addl（u32 零扩展）,i64/u64→addq。
- 符号：除法 idiv(signed)/div(unsigned)，右移 sar/shr，比较 jl/jg vs jb/ja。
- 回绕：窄宽度运算后掩码截断（i8: &0xFF 再符号扩展；u32: movl 自动零扩展）。
- **SIMD 协同**：显式 u8/i16/i32 数组 → SSE/AVX 通道直接对应（u8×16/i32×4），i32 用 pmulld 免 pmuludq 守卫舞。
- big：调运行时 BigInt 函数（pa_big_add/sub/mul/div/mod/pow/cmp），堆对象 RC 管理。

### 1.4 unknown / P3 表示

- 定宽整数存 unknown i64 槽：值符号/零扩展到 i64，**tag 需带宽度/符号**（见 §2）或运算由静态类型驱动（unknown 边界擦宽度→提升 i64）。
- 简化 v1：定宽整数过 unknown 边界**提升为 i64/int**（擦宽度，保守），回来需 cast。big 过 unknown 是 HEAP/BIG tag。
- C FFI 对齐：cimport 已有 C 类型（cimport/cimport.h CK Int=i64/Bool/Float），扩 char/short/unsigned 映射到原生定宽。

### 1.5 char 定义

- **char = i8 别名**（C 式字节，x86 char 默认 signed）；`uchar`/`u8` 无符号字节。
- Unicode 码点不是 char——用 i32/u32 表示码点（str 内部已是码点序列）。Python 本无 char，这是 C 式扩展。
- short=i16 别名，long=i64 别名（C 口径，x86-64 long=64）。

## 2. unknown 运行期类型标签（解锁 pprint/isinstance/repr）

### 2.1 问题
P3 tag（UNK/INT/FRAW/FBOX/STR/HEAP）为 RC/算术优化，**语义类型信息不足**：
- INT1 覆盖 int+bool+None（bool=0,1 / None=0 与 int 碰撞）→ isinstance(x,bool)/repr 分不清。
- HEAP5 不区分 list/dict/tuple/set/obj → repr/set 识别错。

### 2.2 方案：扩展 tag + 堆 kind 内省
- **扩 PaTag 标量区分**：{UNK, NONE, BOOL, INT, FRAW, FBOX, STR, HEAP, BIG}。None/Bool/Int 各自 tag（不再碰撞）。
  传播规则沿用 P3（tag 跟值走，UNK 回退探针）；新增 NONE/BOOL tag 在常量/bool 运算/None 产生点发射。
- **堆 kind 内省**：PyAdamasRc.kind 已区分 STR/LIST/DICT/TUPLE/SET/OBJ/CPYFLOAT 等——HEAP tag 时读头 kind 得具体类型（isinstance/repr 用）。
- **isinstance/repr 运行期**：pa_any_type(v,tag)→类型码（NONE/BOOL/INT/FLOAT/STR/BYTES/LIST/DICT/TUPLE/SET/OBJ/BIG）；repr 按类型码分派。
- 异常类当值：ClassObj/TypeObj 过 unknown 带 HEAP+kind=CLASSOBJ tag，不再退化成 0 → 解锁 contextlib.suppress/异常类比较。

### 2.3 解锁
pprint（递归 repr 按类型）、isinstance 全类型、动态分派、异常类传递、contextlib.suppress/closing。

## 3. 语言缺口（stdlib-b3 取证）

| 缺口 | 现状 | 方案 |
|---|---|---|
| **file IO 协议** | File TyKind 有，但 csv.writer 收 `f:file` 静态类型，StringIO 不通 | 泛化 file 协议（FileLike trait：read/write/seek/close），io.StringIO/BytesIO 实现该协议 |
| **traceback 对象** | 无 traceback/sys.exc_info | 异常值挂 traceback 帧链（运行时已有帧记录），sys.exc_info() 返回当前异常 |
| **注解反射/动态类** | 类布局编译期固定 | dataclasses 需编译期注解处理（@dataclass 展开为 __init__/__repr__，编译期生成，非运行期反射）|
| **os.urandom** | secrets 用内建 random（强度弱）| getrandom syscall 胶水 |

## 4. 分阶段路径

| 阶段 | 内容 | 工程量 | 收益 |
|---|---|---|---|
| **P1 定宽整数** | I8-I64/U8-U64/bool/char/short TyKind + 提升规则 + codegen 宽度选择 + 字面量 coerce | 中（机械 codegen + 提升规则要小心）| SIMD 干净/紧凑数组/C FFI。**0.341 落地，见 docs/numeric-types-landing.md** |
| **P2 unknown 类型标签** | 扩 PaTag NONE/BOOL/堆子类 + pa_any_type/repr/isinstance + 跨模块 tag 侧信道 + stdlib/pprint | 中 | 解锁 pprint/isinstance；**0.340 落地，见 docs/type-tags-landing.md（异常类传递未做）** |
| **P3 big BigInt + f32** | BigInt 运行时（任意精度算术 + RC）+ F32 codegen | 重 | CPython 无限精度兼容/精确金融密码学 |
| **P4 file 协议/traceback/反射** | FileLike 泛化 + io.StringIO + traceback 对象 + dataclass 编译期展开 | 重 | 解锁 tarfile/traceback/dataclasses/io |

## 5. 风险与闸门

- **提升规则是静默算错重灾区**（C 的 signed/unsigned 坑）：规则定死 + 大量边界回归（混合宽度/符号/溢出/coerce）。
- **定宽回绕语义**与 CPython 无限精度不同——文档明确「定宽是 C 式回绕，要无限精度用 big」。
- **P3 tag 扩展**改 unknown 表示核心：flag-off asm 逐字节不变（门控），全套件 + ASan + 语料 rc 对拍 + 13/174 钉子。
- **BigInt** 是新堆对象类型：RC/GC/探针/P3 tag 全链路协同，ASan leak/UAF 命门。
- 每阶段独立 worktree/门控默认关/版本递增/全套闸门（difftest/套件 normal+ASan/CPython 对拍/语料/leak/26 asm/check_size）。

## 6. 待用户决策

1. char 定义：i8 别名（C 式）还是单独码点类型？（建议 i8 别名）
2. 提升规则：C 式（同宽 signed+unsigned→unsigned）还是更安全（混合→i64）？
3. 优先级：P1 定宽（助 SIMD）先，还是 P2 unknown 标签（解锁 pprint）先？
4. big BigInt 是否本期立项（重工程）还是后续？
5. 定宽过 unknown 边界：擦宽度提升 i64（保守 v1）还是带宽度 tag（完整但复杂）？