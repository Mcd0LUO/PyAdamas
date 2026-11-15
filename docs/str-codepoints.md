# 字符串码点语义设计（UTF-8 字节存储 + nchars 缓存）

- 状态：**设计稿，未实现**（本文件是施工图，不含代码改动）
- 定案前提（[roadmap.md](roadmap.md:141)）：内部继续按 UTF-8 字节存储（不做 CPython 的
  PEP 393 定宽 1/2/4 字节表示），对外语义按码点：`len("中文") == 2`，下标/切片/迭代
  都按码点；`bytes` 类型语义不变（`len(b"中文") == 6`）。
- 本文覆盖：现状盘点（全部假设「字节 == 字符」的点，带 file:line）、表示方案、API 面、
  S1/S2/S3 分阶段落地、兼容性影响、性能估算、Win64 与 bytes/str 互转。
- 不在本文范围：CPython 的定宽表示、Unicode 大小写/字符类别全表、`re` 的 Unicode
  属性类（`\p{...}`）、异常/GC 语义改动。

---

## 0. 一页结论

1. **布局**：`PyAdamasStr` 从 24 字节扩到 32 字节，把「对外长度」放在**偏移 0**：
   `{ int64_t pub; int64_t len; int64_t hash; const char* data; }`，其中
   `pub` 对 `str` 是码点数（nchars）、对 `bytes` 是字节数；`len` 字段保持
   「UTF-8 字节数」语义不变。这样 `IrOp::Len` 的 `movq (%rax), %rax`、
   `if s:` 的 `cmpq $0,(%rax)` **一行都不用改**，而且 `unknown` 类型的值
   `len()` 也自动正确（str/bytes 两种对象偏移 0 都是各自的外长度）。
2. **字面量**：码点数在**编译期算好**直接写进静态对象（指针指向的段 RELRO 只读，运行期
   不可能懒缓存，必须编译期算）。这是收益最大的部分：代码里字符串常量占多数字符串操作数。
3. **API**：`len(s)` 返回码点数（对齐 CPython，O(1)）；字节长度用**新内置
   `len_bytes(s)`**（O(1)，推荐），不建议双参 `len(s, unit)`，`s.nbytes` 属性
   留作后续（见 §3.2）。
4. **bytes 不需要新的运行时种类**：`str` 与 `bytes` 继续共用同一结构体，差异全部
   由**静态类型**在降级期决定（共享的运行时入口加 `is_bytes` 参数）；只有
   `encode()` / `decode()` 需要真正换算公共长度（§7.2）。
5. **最大风险不是 len，而是迭代**：`s[i]` 从 O(1) 变 O(i)，`for ch in s` 的朴素
   实现会退化成 O(n²)。S2 必须同批上「顺序访问游标」优化，否则字符串迭代基准会出现
   两位数倍数的回归（§6.3）。稀疏索引表本轮不做（S4，看基准再定，§2.8）。
6. **工作量粗估**：S1 约 5–7 人日，S2 约 5–8 人日，S3 约 7–10 人日（含测试与文档），
   合计正好是任务里说的 2–4 周量级；S1 不能单独发布（见 §4.5）。

---

## 1. 现状盘点：所有假设「字节 == 字符」的地方

盘点方法：把运行时里所有吃 `PyAdamasStr*` 的入口、所有产出字符串的构造点、以及
降级/后端里对字符串的专门处理全部列一遍。逐条标注**改成码点后语义怎么变**。

### 1.1 表示与构造

| 位置 | 现状（字节 == 字符） | 改成码点后 |
|---|---|---|
| `runtime/rt_internal.h:217-221` | `PyAdamasStr { int64_t len; int64_t hash; const char* data; }`，24 字节负载 | 扩成 32 字节，加 `pub` 并把 `pub` 放偏移 0（§2.2） |
| `runtime/rt_core.c:704-722` | `str_alloc(len)`：`len` 是字节数；≤7 字节走 SSO（`hash` 字段存内联字符，`data` 指向它） | `str_alloc` 增加 `pub` 参数或拆成 `str_alloc_raw` + `str_seal`；SSO 条件仍按**字节** ≤7，内联区从 +8 挪到 +16（§2.2） |
| `runtime/rt_core.c:724-728` | `str_new(p,len)` 原样拷贝，不知道 nchars | 默认**扫一遍**数码点（安全默认）；另加 `str_new_ascii`（O(1)）与 `str_new_bytes`（bytes 语义，`pub=len`）（§2.4） |
| `runtime/rt_core.c:730-738` | `pa_str_from_cstr` / `_opt`：`strlen` 当字节长度 | 复制后扫一遍算 nchars（C 侧返回的 `char*` 假定是合法 UTF-8）；bytes 无对应入口 |
| `runtime/rt_core.c:740-805` | `char_tab[256]` 是 256 个**单字节**驻留串；`True/False/None` 也走静态对象 | 0–127 仍是合法单码点串，可保留；0x80–0xFF 的单字节对象**不再是合法 str**（非法 UTF-8），`pa_char_str` 只服务 ASCII；新增 `pa_cp_str(uint32_t)` 产 1–4 字节的码点串（§2.5） |
| `runtime/rt_core.c:279-313` | 短串 freelist 按**负载字节数** 8 字节分档（≤128） | 结构体 +8 后档位整体后移一档，逻辑不变；`rc_alloc` 的 `memset` 会把新增字段清零，所以**每个构造点必须显式写 `pub`**（§2.3 不变量） |
| `runtime/rt_core.c:533-567` | `rc_alloc(size,...)`；STR 是叶子（`rt_core.c:346` 判定），GC 不扫负载 | 负载 +8 只影响 `h->size` 记账与池档位，GC/标记逻辑不动 |
| `src/codegen.cpp:25-40` | `str_literal_hash`：对字面量字节算 FNV，长串编译期把哈希写进 `hash` | 不变（哈希仍按字节） |
| `src/codegen.cpp:60-87` | 字面量在 `.data.rel.ro` 里按 `{len, hash, data}` 三字 + 字符区构造，`rc = INT64_MIN` | 改成 `{pub, len, hash, data}` 四字；`pub` 由编译器数码点；RELRO 只读意味着**不能懒算**（§2.6） |
| `src/ir_types.h:236-242` | `intern_string(s)` 只按内容去重 | 必须按 `(kind, content)` 去重：`"中"` 与 `b"\xe4\xb8\xad"` 字节相同但 `pub` 不同（§2.6） |
| `src/lexer.h:34-50,55-139,141-152` | `\u`/`\U`/八进制转义走 `append_utf8`；`\xNN` **原样塞一个字节** | str 字面量的 `\xNN` 必须改成「U+00NN 再编成 UTF-8」（否则 str 可能不是合法 UTF-8）；bytes 字面量反过来要保持原始字节（§2.3、§7.4） |
| `src/lexer.h:666-679` | 普通字符串「先收原文再统一解码」，不区分 str/bytes | `decode_escapes` 要带 `is_bytes`（f-string 的字面量段永远是 str） |

注：运行时字符串构造点一共 **168 行匹配**（其中 2 行是函数定义，实际调用点 165；`grep -rn 'str_alloc(\|str_new(' runtime/*.c | wc -l`），
降级器里的字符串常量 **123 处**（`grep -rn 'const_str(' src/*.cpp | wc -l`）。这两类点
的责任划分见 §2.4。

### 1.2 长度、真值、比较、哈希

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `src/codegen_ops.cpp:82-86` | `IrOp::Len` 无条件发 `movq (%rax), %rax`（读偏移 0）；list/tuple/dict/set/str/bytes 都靠它 | **布局按 §2.2 后一行不用改**：偏移 0 对 str 是码点数、对 bytes 仍是字节数 |
| `src/codegen_ops.cpp:513-520` | `if s:` 也是读偏移 0 判非零 | 不变（空 ⇔ pub==0，与字节数是否为零等价） |
| `src/lower_builtin_seq.cpp:6-33` | `len()`：`IrOp::Len`；range/deque 走运行时；类走 `__len__` | 语义直接变成码点数，**大量迭代/`range(len(s))` 代码自动跟着变**；`len(unknown)` 也自动正确（§2.2 的收益） |
| `runtime/rt_str.c:245-249` | `pa_str_eq`：先比字节长度再 `memcmp` | 不变（字节相等 ⇔ 码点相等） |
| `runtime/rt_str.c:251-259` | `pa_str_cmp`：`memcmp` 三路比较；注释已指出 UTF-8 字节序与码点序一致 | 不变（可继续用，是少有的「不用改」点） |
| `runtime/rt_dict.c:22-48` | 字典字符串 key 哈希 = 字节 FNV-1a + mix64；`s->len > 7` 时把哈希缓存进 `hash` | 不变（`len` 仍是字节数，阈值条件不用改）；hash 值与 CPython 本来就不同（compatibility.md） |
| `runtime/rt_dict.c:59-68` | key 判等走 `pa_str_eq` | 不变 |

### 1.3 下标、切片、迭代

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `runtime/rt_str.c:60-64` | `pa_str_char_at(s,i)`：`i += s->len` 后用**字节下标**取 `s->data[i]`，返回 `pa_char_str(byte)` 单字节串 | 负索引改用 nchars；把码点下标映射成字节偏移（扫描），返回该码点的 1–4 字节串（ASCII 仍走驻留表） |
| `runtime/rt_slice.c:75-94` | `pa_str_slice`：`slice_bounds(len=字节数)`，步长 1 直接 `memcpy`；遇到 `count==1` 走 `pa_char_str` 单字节 | 边界按码点归一化；需先求码点→字节偏移表（最多扫两遍）；`count==1` 返回码点串；**bytes 切片必须分家**：新增 `pa_bytes_slice`（字节语义），降级按静态类型二选一（`src/lower_expr_container.cpp:22-31`） |
| `runtime/rt_slice.c:169-174` | `pa_str_to_list`：按字节拆 list[str]，每个字节一个串 | 按码点拆；内部用字节游标顺序推进（O(n)），不要逐个 `pa_str_char_at`（否则 O(n²)） |
| `runtime/rt_slice.c:177-181` | `pa_bytes_to_list`：按字节拆 list[int] | 不变（bytes 迭代仍是 int） |
| `src/lower_expr_container.cpp:106-118` | str 下标 → `IrOp::IndexLoad`，`def_heap`；bytes 下标走 `RtFn::BytesAt`（`:60-64`） | 不变（`pa_str_char_at` 语义变，调用点不变） |
| `src/codegen_call.cpp:24-34` | str 的 `IndexLoad` 发 `call pa_str_char_at` | 函数名不变、实现变 |
| `src/codegen_regalloc.cpp:413-418` | 字符串 `IndexLoad` 不借临时寄存器（「会真的调 pa_str_char_at」） | 不变，且更该如此（函数变贵） |
| `src/lower_stmt.cpp:1015-1026,1063-1108` | `for ch in s`：`Len` 定上界 + 每轮 `IndexLoad`（str 走 `def_heap`） | 语义自动跟着 Len/char_at 变，但**每轮 O(i) 扫描 ⇒ O(n²)**；需在 S2 加顺序游标（§6.3） |
| `src/lower_stmt.cpp:668-670,733-737` | `for i, v in enumerate(s)`：直接计数 + `IndexLoad` | 同上（游标优化要覆盖 enumerate） |
| `src/lower_stmt.cpp:796-798,895-910` | `for a, b in zip(s, t)`：双计数 + `IndexLoad` | 同上（zip 每个序列一个游标） |
| `src/lower_comp.cpp:288-299,378-384,412-421` | 推导式把 str 当索引序列；`IndexLoadUnchecked` 只对 list/tuple 打补丁 | 同上；补丁逻辑不要用到 str 上（str 没有免检查路径，见 `:412-421` 的条件） |
| `src/lower_aggregate.cpp:169-182,404-433,561-580` | `min/max/sum/any/all` 单实参时按 `Len` + `IndexLoad` 消费 str | 语义跟着变；性能同上，且这些内置在长串上会 O(n²)，S2/S3 用游标或 `pa_str_to_list` 一次物化 |
| `src/lower_call.cpp:93-102,637-650`、`lower_import_native.cpp:112-115,293-297`、`lower_import_stdlib.cpp:410-416`、`lower_method_misc.cpp:60-66`、`lower_expr_assign.cpp:120-126` | 6 处把 str `StrToList` 后按 list 消费（`*args` 展开、`list/tuple/sorted/reversed/enumerate/zip`、`deque(str)`、`random.choice(str)`、切片赋值的字符串右值） | 这 6 处**自动正确**（`pa_str_to_list` 改码点后即可）；前提是 `pa_str_to_list` 内部 O(n) |
| `runtime/rt_slice.c:204-209` | `pa_str_wrap_list`（textwrap 单元素列表） | 不变 |
| `src/typecheck_stmt.cpp:355-417` | str 的下标/切片赋值在类型检查期直接拒绝（`:370-374,412-415`） | 不变（str 不可变，不需要写路径的码点换算） |

### 1.4 字符串方法（`pa_str_method`，操作码见 `src/ir_ops.h:1044-1050`）

分派入口 `runtime/rt_str.c:352-687`，`bytes` 的方法复用同一份实现
（`src/lower_method_str.cpp:188-298`）。逐 op 标注：

| op / 行号 | 现状 | 改成码点后 |
|---|---|---|
| upper/lower `:352-360` | 逐字节 ASCII 大小写 | 输出码点数 = 输入码点数（ASCII 映射不改字节数）⇒ pub O(1) 继承；非 ASCII 大小写仍不支持（文档化差异，可留 S4） |
| strip/lstrip/rstrip `:362-386` | `chars` 当**字节集合**（`sm_char_in_set` `:288-292`）剥；默认空白是 ASCII 六种 | `chars` 必须按**码点集合**匹配（否则 `strip("中")` 永远剥不掉）；剥离必须整码点前进；返回串 pub = 原 nchars − 剥掉码点数。默认空白仍只认 ASCII（CPython 剥 Unicode 空白 ⇒ 保留为已知差异） |
| split() `:387-407` | 按 ASCII 空白字节切 | 切点都是 ASCII，字节边界天然是码点边界；每段 pub 需要数（切完每段各扫一次 = 总 O(n)） |
| split(sep) `:408-426` | `pa_find_bytes` 找 sep；maxsplit 计数 | 匹配本身不用改（见 §1.5 的 UTF-8 自同步结论）；每段 pub 同样要数 |
| join `:427-452` | 先累加字节总长，再 memcpy | pub = Σ 各元素 nchars + sep.pub×(k−1)，O(1) 合并；字节拷贝不动 |
| replace `:453-517` | 非空 old 用字节 find；old 为空时**逐字节**插入 new | 非空 old 匹配不用改；pub = nchars + hits×(new.pub−old.pub)；**空 old 必须改成逐码点插入**（`:478-486` 的 `for k <= s->len` 是字节循环） |
| find `:518-520`（`sm_find_from` `:312-320`） | start/end 按字节夹取；返回字节下标 | start/end 是**码点**下标（CPython 语义）⇒ 先换算成字节偏移；返回值再换算回码点下标（O(n)，find 本身也是 O(n)）；空子串 `start <= end` 的比较也要换成码点 |
| rfind `:521-533` | 从字节尾部往前 `memcmp` | 同上；「往前」要按码点回退（从 lead byte 定位） |
| startswith/endswith `:534-544` | 字节前缀/后缀比较 | 不变（合法 UTF-8 的字节前缀 ⇔ 码点前缀） |
| count `:545-556` | 不重叠字节 find 计数；空 sub 返回 `s->len+1` | 计数结果不变；空 sub 返回 nchars+1 |
| zfill `:557-572` | 宽度按字节；`pad = width - s->len` | 宽度按码点：`pad = width - pub`，输出 pub = width；符号位是 ASCII，首字节判断不变 |
| ljust/rjust/center `:573-595` | 宽度按字节；fill 只检查非空取首字节 | 宽度按码点；fill 必须**恰好 1 个码点**（CPython 报 TypeError），取 fill 的码点而不是首字节 |
| capitalize `:596-602` | 第 0 个**字节**大写、其余字节小写 | 第 0 个**码点**大写、其余码点小写；pub 不变 |
| title `:603-613` | 逐字节；`word = !is_alpha(byte)` | 必须逐码点：按字节时非 ASCII 的 continuation 字节会被当成「非字母」反复重置词边界 ⇒ 多字节字符会被拆着改；pub 不变 |
| isdigit/isalpha/isspace/isupper/islower/isalnum `:614-651` | 逐字节 ASCII 判定 | 逐码点；ASCII 语义不变，非 ASCII 码点按「非字母/非数字/非空白」处理（`"中".isalpha()` 仍 False，属已知差异；S4 可接 Unicode 表） |
| removeprefix/removesuffix `:652-665` | 字节前缀/后缀比较后切片 | 比较不变；结果 pub = pub − affix.pub（或原样） |
| splitlines `:666-682` | 只认 `\n`/`\r` 字节 | 不变（0x0A/0x0D 不可能出现在 UTF-8 多字节序列里）；CPython 还认 `\v \f \x1c-\x1e \u2028 \u2029`，差异保持 |
| capwords `:324-350`（string.capwords 的 C 实现） | 逐字节 ASCII 切词/改大小写 | 逐码点；输出 pub 一边写一边数 |
| `pa_str_repeat` `runtime/rt_stdlib.c:198-207` | `str_alloc(len*n)` 字节复制 | pub = pub×n，O(1)；**bytes 重复要分家**（降级 `src/lower_expr_ops.cpp:144-165` 的 `seq_ty` 里根本没有 Bytes，而类型层允许 `bytes*int`（`typecheck_expr.cpp:922-925`）——这是现存的降级缺口，S2 顺手补 `pa_bytes_repeat`） |

### 1.5 查找、包含、比较（内容级操作）

**关键结论**：对合法 UTF-8，任何「合法 UTF-8 的 needle」在字节层面能匹配到的位置**必然在
码点边界上**（多字节序列的首字节不会是 0x80–0xBF，ASCII 也不会出现在续字节里）。所以
`in`、`find/rfind/count/replace/split` 的**匹配本身可以继续保持字节级**，只需要
把「对外暴露/接收的下标」在码点与字节之间换算。这条结论把 S2/S3 的工作量砍掉一大块，
也解释了为什么 `re` 只有点号/字符类/量词那部分必须重写（§1.7）。

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `runtime/rt_slice.c:4-8` | `pa_str_contains`：`pa_find_bytes(...) >= 0` | 不变（返回 bool） |
| `runtime/rt_str.c:296-310` | `pa_find_bytes`：memchr + memcmp | 不变（内部工具，返回字节下标；调用方负责换算） |
| `src/lower_expr_ops.cpp:500-540` | `in` 对 str/bytes 都发 `pa_str_contains` | 不变 |

### 1.6 格式化、对齐宽度、f-string

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `runtime/rt_format.c:16-58` | `fmt_parse` 解析 spec（宽度/精度按十进制数） | 不变（spec 本身是 ASCII） |
| `runtime/rt_format.c:60-107` | `fmt_pad` 用 `strlen(body)` 当宽度单位 | 宽度 = 码点数：body 需要带 pub 进来（改签名）；padding 数量 = width − pub |
| `runtime/rt_format.c:137-223` | int/bool 的 body 全是 ASCII | pub = body 字节数，O(1) |
| `runtime/rt_format.c:225-239` | `pa_fmt_str`：`n = v->len`，按**字节**截断精度 | 精度按码点截断：先按 pub 夹取精度，再把「精度个码点」换成字节偏移后 memcpy；宽度按 pub |
| `runtime/rt_format.c:241-252` | 复数 body 是 ASCII | 同 int/float |
| `runtime/rt_format.c:290-497` | printf 风格百分号格式化：宽度/精度按字节；`%c` 只接受 0–255 / 单字节串（`:433-449`）；`%s` 走 `fmt_str`（`:451-482`）；`is_bytes` 参数**当前未使用**（`:328-329`） | 宽度/精度按码点；`%c` 接受 0–0x10FFFF 的码点或 1 个码点串；`is_bytes` 参数是现成的分流开关：`str` 语义走码点、`bytes` 语义走字节 |
| `src/lower_expr_dunder.cpp:223-330` | f-string：字面量段 `const_str`，替换段按类型调 `pa_fmt_*` | 字面量段自动带编译期 pub；替换段要把 spec 宽度传给上面改过的 `pa_fmt_*`。f-string 永远产 str（不会产 bytes） |

### 1.7 `re`（整条按字节）

`runtime/rt_re.c` 是自带回溯 VM，模式、文本、捕获位置全部按字节。要点：

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `:997-1005` | `is_word`/`fold_eq` 是 ASCII 字节判定 | 保持（`\w`、`I` 标志继续只认 ASCII，文档化差异） |
| `:1008-1100` | VM：`RE_ANY` 消费 1 **字节**；`RE_CLASS` 查 256 位 bitmap；`RE_CHAR` 比 1 字节 | `RE_ANY` 要消费 1 **码点**（读首字节算序列长度）；`RE_CLASS` 对非 ASCII 码点按「不在任何 ASCII 类里」处理，但对**否定类**要整体消费 1 码点；`RE_CHAR` 多字节模式串仍可逐字节比（自同步） |
| `:1156-1170` | `find_from` 从 `start..text->len` 逐**字节**试起点 | 起点只能落在码点边界（否则 `findall(".", "中文")` 会给出 6 个乱匹配）；起点推进改为按码点前进 |
| `:1216-1227` | `match_new` 保存 `caps`（字节偏移） | caps 内部继续字节，**只有对外 `start/end/span/group` 时换算**（`:1353-1361`） |
| `:1290-1310` | `group_str` 用字节区间 `str_new`；命名组按字节比名字 | 区间必在码点边界；结果 pub 数一遍；命名组比较不变（ASCII） |
| `:1382-1450` | `finditer/findall` 逐个匹配、逐个暴露字节位置 | 若逐个把字节下标换算成码点下标，1MB 文本 + 多匹配会 O(n²)；**一次扫全文建 byte→cp 映射**，或按匹配位置有序单遍推进 |
| `:1528-1600` | `sub/sub_fn/expand`：替换模板反斜杠数字引用与文本拼接都按字节 | 匹配/拼接不变；`Match.start()` 在替换函数回调里出现时要换算 |
| `:1637-1700` | `split` 按捕获组字节区间切 | 切点必在码点边界；每段 pub 数一遍 |
| `:1702-1713` | `re.escape` 只转义 ASCII 元字符，其余字节原样 | 不变（输出与 CPython 一致；非 ASCII 字节原样通过是正确行为） |

结论：`re` 需要的不是「整条重写成码点引擎」，而是三件事：① 点号/字符类/起点按码点步进；
② 对外下标换算；③ `finditer/findall` 的映射批量化。Unicode 属性类/全量大小写
折叠继续不支持（与现状一致）。

### 1.8 `repr`、`str()`、`print`、`ord`、`chr`

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `runtime/rt_format.c:254-288` | `pa_repr_str`：逐字节；只转义反斜杠/引号/换行/制表/回车，其余字节原样 | 输出字节不变（非 ASCII 多字节原样通过合法）；pub 一边写一边数；CPython 对不可打印 Unicode 会转 `\uNNNN`，差异保持 |
| `runtime/rt_str.c:21-51` | 同构元组 repr 逐元素拼 | 不变（元素 repr 各自处理） |
| `runtime/rt_math.c:307` | `pa_print_str`：`fwrite(data,1,len)` | 不变（写字节即可） |
| `runtime/rt_math.c:310-315` | `pa_bytes_at`：字节下标 | 不变（bytes 语义） |
| `runtime/rt_str.c:183-187` | `pa_ord`：要求 `len==1`（字节），返回 0–255 | 要求 pub==1，返回**码点**（0–0x10FFFF）；错误消息对齐 CPython |
| `runtime/rt_str.c:189-193` | `pa_chr`：只接受 0–255 | 接受 0–0x10FFFF；代理区（0xD800–0xDFFF）与不合法码点抛 ValueError（我们的 str 必须是合法 UTF-8，见 §7.4） |
| `runtime/rt_str.c:84-94` | `pa_str_any`（unknown 的 `str()`） | 不变；注意 unknown 若是 bytes 会原样返回（既有类型谎言，不扩大） |

### 1.9 文件 I/O

`runtime/rt_file.c` 内部已经把任意编码解码成 **UTF-8 字节**再交给 `PyAdamasStr`
（`:16-28,313-372`），所以内容层面不用改；要改的是**长度单位暴露的地方**：

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `:49` | `text_off` = 文本模式逻辑位置，单位是**解码后 UTF-8 字节数** | 内部还得留字节计数（`file_sync_raw :552-567` 的 `raw_base + dpos` 换算靠它）；另外维护 `text_chars` 供 `tell/seek/read(n)` 用 |
| `:374-378` | `file_takec` 每读 1 字节 `++text_off` | 每消费 1 码点 `++text_chars`（读首字节判序列长度） |
| `:380-430` | `pa_file_read(f,n)`：n 是字节数 | CPython 文本模式 n 是**字符数**：读到 n 个码点为止（内部仍拷贝字节）；`pa_file_read(f,-1)` 不变 |
| `:432-472` | `pa_file_readline(f,size)`：size 字节上限 | size 按码点上限 |
| `:474-486` | `readlines(hint)`：hint 用字节累计 | hint 按码点累计（CPython 也是字符数） |
| `:569-580` | `write(s)` 返回字节数 | CPython 返回字符数：返回 pub；`text_off` 仍加字节数 |
| `:603-652` | `seek/tell` 文本模式用解码字节偏移 | 归还/接受码点位置；`file_text_skip` 改成「跳 n 个码点」 |
| `:136-370` | `newline` 翻译与 iconv 都按字节 | 不变（回车换行是 ASCII，不会切到多字节序列中间） |
| `runtime/rt_datetime.c:572-578` | `Path.read_bytes`：`str_new(raw,len)` 却标成 bytes 类型 | 必须用 `str_new_bytes`（pub = len），否则 `len(Path.read_bytes())` 会数成码点 |
| `runtime/rt_file.c:380-407` 二进制模式 | `open(...,'rb').read()` 仍按 str 类型返回原始字节 | 不变量例外：内容可能不是合法 UTF-8。建议保持现状并文档化（§7.4），或后续让 rb 模式返回 bytes 类型（超出本设计） |

### 1.10 `cimport` 传字符串给 C 的边界

| 位置 | 现状 | 改成码点后 |
|---|---|---|
| `src/codegen_call.cpp:595-609` | 传 `str` 给 C 的 `char*`：从 `PyAdamasStr` **偏移 16** 取 `data` | 偏移改成 **24**（§2.2）；传出去的仍是 UTF-8 字节，C 侧无感 |
| `src/codegen_call.cpp:610-617` | `BufBytes` 也从偏移 16 取 data | 同上改 24 |
| `src/codegen_call.cpp:771-783` | C 返回 `char*` → `pa_str_from_cstr(_opt)` | 运行时扫描算 pub（§2.4）；C 返回的必须是 UTF-8，否则同 §7.4 |
| `src/codegen_call.cpp:824-833` | `extern char*` 变量同样 `pa_str_from_cstr` | 同上 |
| 嵌入 NUL | `strlen` 截断（既有行为） | 不变（`str->len` 仍按真实字节数；C 侧拿 NUL 结尾串是接口限制） |

### 1.11 `bytes` 与 `str` 共用表示的隐性依赖

| 位置 | 现状 | 影响 |
|---|---|---|
| `runtime/rt_internal.h:217-221` | 一个 `PyAdamasStr` 结构体同时表示 str 和 bytes（compatibility.md:287 明说「bytes 没有独立运行期种类」） | 码点语义下两者「对外长度」不同 ⇒ 所有**共享入口**必须带静态类型参数（§2.9） |
| `src/lower_method_str.cpp:188-298` | bytes 方法借用 `StrMethod` 同一套 op | 加 `is_bytes` 参数（§2.9） |
| `src/lower_expr_container.cpp:22-31` | str 和 bytes 切片都发 `RtFn::StrSlice` | 拆成 `StrSlice` / `BytesSlice` |
| `src/codegen_ops.cpp:252-266` | `gen_bin` 对 Str/Bytes 都发 `pa_str_concat` | 按 `in.ty` 分 `pa_str_concat` / `pa_bytes_concat` |
| `src/lower_expr_ops.cpp:28-68` | 百分号格式化的 `is_bytes` 已传但运行时丢弃 | 运行时用起来（§1.6） |
| `src/lower_expr_ops.cpp:19-20`、`lower_call.cpp:221`、`lower_impl_util.cpp:198` | 3 处「先当 str 常量、再 `retype` 成 bytes」 | 改成真正的 bytes 字面量（§2.6 的 intern kind） |
| `src/lower_method_str.cpp:25-29,189-194` | `encode/decode` 只是改标类型（零成本） | 码点语义下必须真正换算公共长度（§7.2） |
| `runtime/rt_str.c:352` | `pa_str_method` 无类型参数 | 加 `is_bytes`（放在第 6 个整型实参，`RtFn` 调用支持 6 参） |

### 1.12 后端/降级里对字符串下标的特化（汇总）

- `IrOp::Len` 读偏移 0：`src/codegen_ops.cpp:82-86`（布局方案的关键，§2.2）。
- 真值读偏移 0：`src/codegen_ops.cpp:513-520`。
- 下标：`src/codegen_call.cpp:24-34`（str → `pa_str_char_at`）。
- 比较：`src/codegen_ops.cpp:552-580`（str/bytes 都走 `pa_str_eq/cmp`，不用改）。
- 拼接：`src/codegen_ops.cpp:252-266`（要按 str/bytes 分家）。
- 字面量：`src/codegen.cpp:60-87`（加 `pub`、按 kind 去重）。
- 临时寄存器：`src/codegen_regalloc.cpp:403`（`Len` 纯）、`:413-418`（str 下标不许借寄存器）。
- 常量折叠：`src/opt_fold.cpp` 整份只有「纯指令判定 + DCE」，**没有任何 `len`/字符串常量折叠**；
  `src/opt_loop.cpp:220` 会把 `ConstStr` 提出循环（对 pub 无影响）。
- 跨模块 ABI：`src/module.cpp` 只序列化 `TyKind`，不涉及字符串内存布局 ⇒ 不用改。

---

## 2. 表示方案

### 2.1 现状与约束

- 负载 24 字节 `{len, hash, data}`；RC 头固定 64 字节且在负载**前面**
  （`rt_internal.h:298-311`，`static_assert(sizeof(PyAdamasRc)==64)`）。
- SSO：字节长度 `len<=7` 时字符内联在 `hash` 的 8 字节里，`data` 指向它
  （`rt_core.c:699-722`）；`hash` 的另一用途是长串字典哈希缓存
  （`rt_dict.c:22-48` 与 `codegen.cpp:76-79` 的编译期哈希）。
- 字面量在 `.data.rel.ro`（RELRO 后只读），运行期**不能**在对象上做懒缓存
  （`docs/runtime.md:83-85` 已写明这条约束）。
- `len` 在偏移 0 是刻意设计（`docs/runtime.md:63`）：`len(s)` 编译成一条
  `movq`，`if s:` 只是读一个字段。任何方案都要保留这两个指令形态。

### 2.2 候选布局与推荐

记 `P` = 对外长度（str=nchars，bytes=nbytes），`B` = UTF-8 字节长度。

**O2（推荐）**：把对外长度放偏移 0，字节长度留在第二字：

    typedef struct PyAdamasStr {
        int64_t pub;      /* +0  str: 码点数；bytes: 字节数 */
        int64_t len;      /* +8  UTF-8 字节数（既有字段语义不变） */
        int64_t hash;     /* +16 <=7 字节时内联字符；否则字典哈希缓存（0=未算） */
        const char* data; /* +24 字符区；SSO 时指向 &hash */
    } PyAdamasStr;        /* 负载 32 字节（原 24） */

不变量：

- str：`pub == nchars`、`len == nbytes`、字符区是**合法 UTF-8**；
  `0 <= pub <= len`，等号当且仅当全 ASCII。
- bytes：`pub == len == nbytes`。
- 字节 `len <= 7` 走 SSO（按字节判断，因为内联区装的是字节）；`data` 指向
  `&s->hash`（+16），8 字节里放 ≤7 字节 + NUL。长串 `data` 指向结构体后面的
  字符区（尾部补 NUL，只为调试方便）。
- `hash` 缓存规则不变：`len > 7` 且非 0 才有效（`rt_dict.c:28,36`；
  `codegen.cpp:76-79` 的 `s.size() > 7` 也对应字节长度，都不用改）。

**为什么是 O2 而不是「在尾部追加」**：`IrOp::Len`（`codegen_ops.cpp:82-86`）和真值
判断（`:513-520`）都是**无语义地读偏移 0**，对 list/tuple/dict/set/str/bytes 通用。把 P 放
偏移 0 后：

- 这两条指令**一行都不改**，str 自动得到码点数、bytes 自动得到字节数；
- `len(x)` 在 `x` 是 `unknown` 时也自动正确——这是 O2 相对「尾部追加」的决定性
  优势：追加方案里 `len(unknown_str)` 会静默退回字节数，而脚本语言里 unknown 太常见；
- 现有运行时 C 代码里读 `s->len` 的地方（字节语义）**字段名和含义都不变**，几乎零改动
  （真正需要改的是构造点与语义查询点，见 §1）;
- 代价：指针从 +16 挪到 +24，所有**编译器侧硬编码偏移**只有两处
  （`codegen_call.cpp:607` cimport 取 data、`:615` 取 BufBytes data），加上字面量布局。

对比其它候选：

| 方案 | 结构 | 优点 | 致命/明显缺点 |
|---|---|---|---|
| **O2（推荐）** | `{pub, len, hash, data}` 32B | 偏移 0 通用、unknown 正确、C 侧字节长度零改动 | 每个字符串多 8 字节；指针偏移变一次 |
| O1 尾部追加 | `{len, hash, data, nchars}` 32B | 同样只 +8；字面量/SSO 布局少动 | `Len` 要按类型选偏移（读 +24 还是 +0），`unknown` 通道静默算成字节数；代码要多一套类型分派 |
| O3 丢哈希缓存 | `{pub, len, data}` 24B | 不涨内存 | 字典字符串 key 每次查找重扫整串 FNV；`s->len>7` 的缓存逻辑全要删；长 key 密集场景明显变慢 |
| O4 位打包 | `{packed32+32, hash, data}` 24B | 不涨内存、保留缓存 | 单串上限 4GiB；每次访问多移位/掩码；`len` 字段含义被改，全部访问点要改成 accessor；调试困难 |

**8 字节的成本可以接受**：短串总分配从 64+24=88 变 64+32=96（+9%），长串固定 +8；
字符串对象在堆上的占比最大的是字符区本身，结构体那 8 字节是常数。用
`tests/bench/cases/bench_rc_strings.py` 可以量出来（§6.4）。

### 2.3 不变量与调试保障

`rc_alloc` 会把负载 `memset` 成 0（`rt_core.c:565`），所以**忘记写 `pub` 的
构造点会静默得到「长度 0」**——这是本设计最容易被写错的地方。缓解：

1. 构造 API 收敛（§2.4）：绝大多数点用 `str_new`（默认扫描，天然正确）。
2. 先 `str_alloc` 后手工填充的 30 处，统一改成 `str_alloc_raw` + `str_seal(s)`：
   `str_alloc_raw` 把 `pub` 置 `-1`；`str_seal` 扫描字符区写 `pub`（或调用
   `str_seal_n(s, pub)` 给已知值）。正常/测试构建在 `pub == -1` 被读到时 panic
   （`PYADAMAS_STR_DEBUG=1` 或 debug preset 默认开），release 构建退化为扫描兜底。
3. 字面量：编译期直接写 `pub`（§2.6），永远不为 -1。
4. 加一个内部校验函数 `str_check(s)`（仅 debug）：`0<=pub<=len`、
   `pub==len` 当且仅当 ASCII（可选，O(n) 只在调试用）。

### 2.4 谁负责算 nchars：构造点分类

**API 约定（新增，放在 `rt_internal.h` 与 `rt_core.c`）**：

| API | 语义 | 复杂度 | 用在哪 |
|---|---|---|---|
| `str_new(p,n)` | 复制并**扫描**算 nchars | O(n) | 默认；135 处现有 `str_new` 调用点不用改，语义自动正确 |
| `str_new_ascii(p,n)` | 断言全 ASCII，`pub=n` | O(1) | 已知 ASCII 的缓冲区：数字/日期/十六进制/路径拼接等（§下表） |
| `str_new_bytes(p,n)` | bytes 语义：`pub=len=n` | O(1) | 所有 bytes 产出点（§下表） |
| `str_alloc_raw(n)` + `str_seal(s)` / `str_seal_n(s,pub)` | 先分配后填充 | seal 扫描 O(n) 或给定值 O(1) | 现有 30 处「先 alloc 后逐字节写」 |
| `str_concat(a,b)` | `pub=a.pub+b.pub` | O(1) 计长 | `rt_str.c:52-58` |
| `str_repeat(s,n)` | `pub*=n` | O(1) 计长 | `rt_stdlib.c:198-207`；bytes 版走 `str_new_bytes` |
| `pa_bytes_concat/repeat/slice/method` | bytes 语义包装 | 同上 | §2.9 |

**责任划分表**（按「nchars 从哪来」分组）：

| 类别 | 代表位置 | nchars 来源 |
|---|---|---|
| 编译期常量（最大收益） | `src/codegen.cpp:60-87`（全部 `const_str` 字面量、f-string 字面量段、错误消息） | 编译器数（O(字节数)，只在编译期付一次） |
| O(1) 合并 | `rt_str.c:52-58` 拼接、`rt_stdlib.c:198-207` 重复、`rt_str.c:427-452` join、`rt_format.c:60-107,137-252` 数字/填充、`rt_str.c:352-360,596-613` 大小写 | 由操作数 pub 直接算 |
| 派生自原串（长度可控） | `rt_str.c:362-386` strip、`rt_str.c:652-665` removeprefix/suffix、`rt_slice.c:75-94` 切片（count=hi−lo 已知，但要先求字节偏移） | O(被剥/被取部分) 或 O(1) |
| 必须扫一遍 | `rt_core.c:724-728` `str_new`、`rt_core.c:730-738` from_cstr、`rt_file.c:380-472` 文件读、`rt_json.c:78-150` JSON 字符串、`rt_re.c:1290-1310,1637-1700` re 结果、`rt_os.c` 路径函数 | `count = #(b & 0xC0) != 0x80`（对合法 UTF-8 精确；对非法字节也安全） |
| 逐段数（总 O(n) 仍可接受） | split 各段、replace 输出、splitlines 各行、`pa_str_to_list` | 扫描过程中顺带数，别每段从 0 重扫 |
| **必须显式指定（不能用默认扫描）** | bytes 产出：`rt_slice.c:185-201`（`pa_bytes_from_list/zeros`）、`rt_stdlib.c:237-300`（base64）、`rt_hash.c:68-115`（sha256 digest/hex）、`rt_datetime.c:572-578`（`Path.read_bytes`）、bytes 字面量、`encode()` | `pub = len`，用 `str_new_bytes` |
| 特殊情况 | `encode/decode`（§7.2）、二进制文件读（§7.4） | 见对应节 |

**注意**：`str_new_ascii` 只能用在真正可证明 ASCII 的缓冲区，否则会把 nchars 算错。
建议初始实现**不提供**这个 O(1) 优化，先全部走 `str_new` 扫描，S3 再按基准热点逐个
替换（日期/十六进制/路径拼接是明显候选，因为输出本来就是 ASCII）。

### 2.5 单码点串与驻留表

- `pa_char_str`（`rt_core.c:787-790`）：改为只接受 0–127（ASCII 单码点），
  继续返回 `char_tab` 里的永生串（`s[i]` 的 ASCII 快路径不变）。
- 新增 `pa_cp_str(uint32_t cp)`：ASCII 转 `pa_char_str`；非 ASCII 用
  `append_utf8` 语义现编 2–4 字节串（每次分配，CPython 对 >U+00FF 也是新对象）。
- `chr()`、`s[i]`、切片 `count==1`、`%c` 都改用 `pa_cp_str`。
- `char_tab` 的 0x80–0xFF 槽位：从「单字节 str」改为不再作为 str 使用（bytes 路径不再
  需要它们：bytes 下标是 int、bytes 切片用字节实现）。保留数组大小 256 只当 byte lookup 也行，
  但**禁止**把 0x80–0xFF 的单字节对象当 str 暴露。

### 2.6 字面量的编译期常量折叠

- 词法层已经把转义解码成 UTF-8 字节（`lexer.h:141-152`），编译器只要数
  `(b & 0xC0) != 0x80` 就是 nchars。
- 发射格式（`src/codegen.cpp:60-87` 改）：

      .align 8
      .quad 0,0,INT64_MIN,0,0,0,0,0   # 隐藏 RC 头（64B），rc=永生
      .Lstr<i>:
      .quad <pub>                      # +0  编译期码点数（bytes 字面量 = 字节数）
      .quad <len>                      # +8  字节数
      .quad <hash 或 0>                # +16 len>7 时编译期哈希，否则 0
      .quad .Lstr<i>_c                 # +24 data
      .Lstr<i>_c: .ascii "..." ; .byte 0

- **intern 必须按 `(kind, content)` 去重**（`src/ir_types.h:236-242` 现在只按
  content）。反例：`"中"`（str，pub=1，3 字节）与 `b"\xe4\xb8\xad"`（bytes，pub=3）
  字节完全相同，共用一个静态对象必然有一方错。实现上给 `IRModule` 加一张
  bytes 字面量表，或把 `intern_string` 的 key 改成 `(is_bytes, s)`；生成两份对象
  多出的体积可以忽略（同内容同时以 str/bytes 出现的概率极低）。
- 产出 bytes 字面量的 3 处必须改用新入口：`lower_expr_ops.cpp:19-20`、
  `lower_call.cpp:215-223` 的默认值、`lower_impl_util.cpp:190-200`。
- **收益**：字面量的 `len` / 下标基址全部零运行时成本；而且这是唯一可行的字面量
  方案（RELRO 只读）。可选附加优化：在降级期把 `len("字面量")` 折叠成常量
  （`src/lower_builtin_seq.cpp:7-33`，目前 `opt_fold.cpp` 没有这类折叠）——
  纯收益，可放 S1 末尾。

### 2.7 与 RC 头 / GC / freelist / SSO 的共处

| 关注点 | 结论 |
|---|---|
| RC 头 | **不动**。头是 64 字节固定布局、`static_assert` 守着，所有 kind 共用；把 nchars 塞头里要么破坏断言、要么所有对象都 +8。放负载 +0。 |
| GC | STR 是叶子（`rt_core.c:346` 的 `rc_leaf_untracked` 直接返回 1），`data` 不是 GC 指针、不扫描；新增 `pub` 只是普通整数，标记/清扫/`release` 全不受影响。`h->size` 由 `rc_alloc(size)` 自动包含 +8，只影响记账阈值。 |
| 短串 freelist | 按负载大小 8 字节分档（`rt_core.c:279-313,540-552`）。+8 后档位后移，逻辑不变；`memset` 保证复用块字段归零，所以 seal 协议必须每个构造点都走。 |
| SSO | 内联区仍在 `hash` 字（+16），`data` 指向 `&s->hash`；条件仍是字节 `len<=7`。短串 `pub` 与 `len` 都要写。 |
| 静态驻留串 | `PyAdamasStaticStr`（`rt_core.c:745-770`）内嵌 `PyAdamasStr`，布局自动跟着变；`static_str_init` 要写 `pub`（ASCII 时 = `len`）。 |
| `h->size` 与池块复用 | 池块容量按档向上取整，字符串访问仍以 `len` / `pub` 为准，多出的 8 字节只是浪费在档尾。 |

### 2.8 超长串的 `s[i]` 与稀疏索引表（结论：本轮不做）

`s[i]` 从 O(1) 变 O(i) 后，两种访问模式要分开看：

- **顺序访问**（`for ch in s` / `enumerate` / `zip` / 推导式 / `s[a:b]`）：
  完全可以用游标消除 O(i)：S2 给这些降级点挂一个「上一码点下标 + 上一字节偏移」的临时槽，
  运行时 `pa_str_char_at_cursor(s, i, &cursor)` 命中 `i == last+1` 时 O(1)，否则从头
  扫。ASCII 快路径本来就走驻留表，不受影响。**这是 S2 的硬要求**（§6.3）。
- **随机访问**（`s[i]` 直接写在循环里、`range(len(s))` 后任意下标）：仍是 O(i)。
  极端情况 O(n²)。

稀疏索引表（每 N 个码点记一个字节偏移）：

| 方案 | 表大小（1M 码点 CJK，~3MB 字节） | 单次访问 | 适用 |
|---|---|---|---|
| 每 64 码点一个 uint32 | ~64KB（字符区的 ~2%） | 查表 + ≤64 字节扫描 | 随机访问密集 |
| 每 256 码点一个 uint32 | ~16KB（~0.5%） | 查表 + ≤256 字节扫描 | 折中 |
| 不做 | 0 | 从头扫（平均 1.5MB） | 顺序访问/短串占绝大多数 |

不建议本轮做的理由：

1. 结构体 32 字节已经放不下指针；要么把表挂在字符区之后并用 `hash` 字的高位当标志，
   要么做一张**全局 side table**（对象指针 → 表指针）。前者让 `hash` 的语义变复杂（字典
   哈希还要清标志位），后者要在字符串释放/池回收/GC 清扫路径上正确摘表——这是新增的
   一整个失效面，比它省下的 CPU 更贵。
2. 真实热点大概率能被 S2 的游标吃掉；先量再决定（§6.4 有测法）。
3. 触发条件建议写死：只有「`pub >= 4096` 且同一个对象在基准里出现随机下标访问」才建表；
   阈值以下建表纯亏（建表 O(n) 扫描 + 内存）。

如果 S4 真要做，推荐形态：side table（开放寻址 map，key = 对象指针，value = { offsets, 每项
uint32 }），阈值 `pub >= 4096`，缓存上限（如 16 个对象、LRU），对象在
`str_pool_put` / `rc_alloc` 复用点无条件摘除（池复用是最大的悬挂风险）。

### 2.9 bytes 与 str 的运行时区分：靠静态类型，不引入新 kind

结论：**不新增 `PYADAMAS_RC_BYTES`**。理由：

1. 唯一需要「运行时判定长度单位」的通用入口是 `len` 与真值，而 O2 让偏移 0 的语义
   自己就是对的；下标/切片/迭代/方法在降级期都知道静态类型（`TyKind::Str` 还是
   `TyKind::Bytes`），直接把类型作为参数传进共享实现即可。
2. 新增运行时 kind 会牵动：`rc_leaf_untracked`、`pa_register_literals`、GC 扫描
   分类、unknown 类型检查、短串池、`isinstance(b"x", bytes)` 等一串现有假设；
   而 `bytes` 与 `str` 共用表示是这套实现的基础（language.md:150-152），
   为长度单位改地基不划算。

需要加 `is_bytes`（或拆独立入口）的共享函数清单：

| 共享入口 | 位置 | 处理 |
|---|---|---|
| `pa_str_method` | `rt_str.c:352` | 第 6 个整型参数 `is_bytes`；bytes 走字节语义分支（strip 字符集、宽度、find 下标、split/replace 的 pub 计数） |
| `pa_str_slice` | `rt_slice.c:75` | 拆 `pa_str_slice`（码点）/`pa_bytes_slice`（字节），降级按 `ot->kind` 选（`lower_expr_container.cpp:22-31`） |
| `pa_str_concat` | `rt_str.c:52` | 拆 `pa_str_concat`（pub 相加）/`pa_bytes_concat`（pub=len），`gen_bin`（`codegen_ops.cpp:252-266`）按 `in.ty` 选 |
| `pa_str_repeat` | `rt_stdlib.c:198` | 同上；顺手补上 bytes 重复的降级缺口（§1.4 末行） |
| `pa_percent_format` | `rt_format.c:328` | 已传 `is_bytes`，用起来 |
| `pa_str_from_cstr` | `rt_core.c:730` | 只有 str 用；bytes 的 C 返回值目前没有对应 CKind |
| `pa_repr_str` / `pa_print_str` / `pa_str_eq/cmp` / `pa_str_contains` | 各自位置 | 字节级实现对两者都正确，不用加参数 |

---

## 3. API 面

### 3.1 len(s)

- 返回**码点数**，对齐 CPython；O(1)（读偏移 0）。`len(b"中文") == 6` 不变（bytes 的
  偏移 0 是字节数）。
- `len()` 的类型检查（`src/typecheck_builtin.cpp:66-78`）只接受单参；保持单参。
- `len(range)`、`len(deque)`、类 `__len__` 都不受影响（`lower_builtin_seq.cpp:14-28`）。

### 3.2 字节长度怎么取：三个候选

| 候选 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| **A. `s.nbytes` 属性** | 可读性最好，`len_bytes` 这个新名字也不用进 builtin 命名空间 | 现在编译器**不支持**非类接收者的属性读（属性只在 Call 的 attr 位置上被处理，见 `typecheck_call_method.cpp`）；要新增 Attribute 表达式的 String 分支 + `CallRt` + `getattr` 动态路径，工作量比重最大；而且 bytes 对象上 `.nbytes` 语义含混 | 不推荐本轮 |
| **B. `len_bytes(s)` 内置函数**（推荐） | 改动最小：`typecheck_builtin.cpp:66-78` 加个名字、降级加一个 `RtFn::StrLenBytes`（读偏移 8 后 movq）、`ir_ops.h` 加名字；O(1)；语义无歧义 | 非 CPython 命名，需要文档说明；bytes 也可以用它（等值 len，无害） | **推荐**。名字可考虑 `len_bytes` 或 `byte_len`，`len_bytes` 更好搜 |
| C. `len(s, unit)` 双参形式 | 单一命名空间，扩展性强 | 与 Python 的 len 契约冲突，所有读代码/工具/文档默认 len(x) 单参；unit 必须是字面量才能静态选路（否则运行期字符串分派）；类型检查要特判；len 的错误消息/工具提示会一直需要对两套口径 | 不推荐 |

补充建议：

- 保留一条「标准兼容」路径：`len(s.encode())`。`encode()` 在 S2 改成真正的
  str→bytes 换算后，它就是 CPython 用户最熟悉的写法（代价 O(n) 拷贝）。
- `sys.getsizeof` 这类不存在的 API 不要顺手加。
- 若将来真要做 `s.nbytes`，建议只作为 `len_bytes(s)` 的语法糖（同一个 `RtFn`），
  不要另做一条语义。

### 3.3 s[i] 与负索引

- `s[i]` 返回 **1 个码点**的 str（1–4 字节）。ASCII 走 `char_tab` 永生串（零分配，
  与今天一致）；非 ASCII 每次新建（CPython 对 >U+00FF 也是新对象）。
- 负索引：`i += pub` 后判界；「从尾部取」可以在实现里优化成从字符区尾往前找 lead byte
  （O(序列长度) 而不是全扫）。
- 越界错误消息沿用 `IndexError: string index out of range`（`rt_str.c:62`）。
- `s[i] = x` / `del s[i]` 在类型检查期就拒绝（`typecheck_stmt.cpp:355-417`），
  不需要运行时写路径。

### 3.4 切片边界换算的复杂度

`s[a:b:c]`（`rt_slice.c:75-94` 改造）：

1. 用 `slice_bounds(pub, a, b, c)` 在**码点域**归一化（这个函数本身不用改，
   `:13-51` 只是把 `len` 换成 pub）。
2. 把码点下标换成字节偏移：`cp_to_byte(s, k)` 从头扫；步长 1 且非全串时最多两次
   （lo/hi 合并成一次单调扫描即可）。O(字节长)。
3. 正步长：`memcpy(data+blo, bhi-blo)`，pub=hi−lo；负步长：按码点从尾往头逐段拷。
4. `s[:]` 全串快路径保留（边界等价 ⇒ 直接 inc 返回原对象，`:80-83`）。
5. 单元素切片 `count==1` 走码点串（`:84`），别走通用路径。
6. 复杂度：步长 1 是 O(n) 扫描 + O(结果字节) 拷贝；步长 ≠1 是 O(n)。与今天的 O(结果字节)
   相比，长串只切一小段会变慢（最坏全扫），这是本设计接受的代价之一。
7. bytes 切片另走 `pa_bytes_slice`（纯字节，逻辑同今天），不共享。

### 3.5 ord / chr / encode / decode

- `ord(s)`：要求 `pub==1`，返回该码点（0–0x10FFFF）。错误消息建议与 CPython 一致。
- `chr(cp)`：接受 0–0x10FFFF；拒绝代理区（0xD800–0xDFFF）与越界，ValueError。
  这是与 CPython 的有意差异（CPython 允许代理码点进 str；我们的表示无法编码它，见 §7.4）。
- `str.encode([encoding])`：返回 bytes 对象；ASCII 串可以**零复制**返回同一个对象
  （pub 与 len 相等，两种语义同值），非 ASCII 需要新对象：拷贝字节 + 设
  `pub=len`（O(n)）。
- `bytes.decode([encoding])`：返回 str 对象；ASCII 零复制；非 ASCII 扫描算 pub
  （O(n)）。编码名参数继续忽略（现状如此，`lower_method_str.cpp:25-29`）。

---

## 4. 分阶段落地

阶段划分原则：**每一步都能编译、能跑现有测试**；语义翻转尽量集中，避免「一半字节一半
码点」的中间态被外部依赖。

### 4.1 S1：表示层 + len + 字面量常量折叠

| 项 | 内容 |
|---|---|
| 改哪些文件 | `runtime/rt_internal.h`（结构体 + 声明）、`runtime/rt_core.c`（`str_alloc/str_new/str_new_ascii/str_new_bytes/str_seal`、静态串、池）、`runtime/rt_str.c:52-58`（拼接 pub）、`runtime/rt_stdlib.c:198-207`（重复 pub）、`src/ir_types.h`（intern kind + bytes 字量表）、`src/lower_impl.h`（`const_bytes_literal`）、`src/codegen.cpp:60-87`（字面量发射）、`src/lower_expr_ops.cpp:19-20`、`src/lower_call.cpp:215-223`、`src/lower_impl_util.cpp:190-200`、`src/codegen_call.cpp:600-617,771-783,824-833`（cimport 偏移 16→24 + from_cstr 扫 pub）、`src/lexer.h`（str/bytes 的 `\x` 及 bytes 八进制转义区分，见 §7.4）、`src/builtin_sigs.cpp`（如加 len_bytes） |
| 语义变化 | `len(str)` 对非 ASCII 变成码点数；`len(bytes)` 不变；字面量 pub 编译期；str 字面量 `\x80`–`\xff` 变成对应 Unicode 码点（原来是裸字节） |
| 验收 | `./build/dev/pyadamas run tests/run/05_strings.py`（既有）；新 `tests/run/163_codepoint_len.py`：`len("中文")==2`、`len("abc")==3`、`len(b"\xe4\xb8\xad")==3`、`len_bytes("中文")==6`、空串/长串字面量；`bash tests/run_tests.sh build/dev build/dev/pyadamas` 全绿；`python3 tests/diff/run_diff.py --seeds 3` 零差异 |
| 回归风险 | 中：改结构体布局 + 所有构造点，任何漏写 pub 都会静默「长度为 0」。靠 `str_new` 默认扫描 + seal 协议 + 全量 run 用例兜住；`PYADAMAS_STR_DEBUG` 校验建议本阶段就加 |
| 能否独立提交 | **能编译、不能发布**（见 §4.5）。建议 S1+S2 一起合入，中间只作为开发分支 commit |

### 4.2 S2：下标 / 切片 / 迭代 / bytes 分家 / 编解码

| 项 | 内容 |
|---|---|
| 改哪些文件 | `runtime/rt_str.c:60-64`（char_at 码点 + `pa_cp_str`）、`runtime/rt_core.c:787-790`（`pa_char_str` 只管 ASCII）、`runtime/rt_slice.c:75-94,169-174`（str 切片 / `pa_str_to_list`）、新增 `pa_bytes_slice` / `pa_str_char_at_cursor`、`src/ir_ops.h`（BytesSlice / BytesConcat / BytesRepeat / StrCharAtCursor 等 RtFn）、`src/codegen_call_rt.cpp`（新 RtFn 分派）、`src/codegen_ops.cpp:252-266`（concat 分家）、`src/lower_expr_container.cpp:22-31`（切片选路）、`src/lower_stmt.cpp:1015-1026,1063-1108` 与 `:668-670,733-737` 与 `:796-798,895-910`（for/enumerate/zip 游标）、`src/lower_comp.cpp:378-384`（推导式游标）、`src/lower_expr_ops.cpp:144-165`（bytes 重复）、`src/lower_method_str.cpp:25-29,189-194`（encode/decode 真转换） |
| 语义变化 | 索引/切片/迭代按码点；bytes 全部保持字节；encode/decode 变成真转换 |
| 验收 | 新 `tests/run/164_codepoint_index.py`：`"中文"[0]=="中"`、`"中文"[-1]=="文"`、`"中文"[0:1]=="中"`、`"中文"[::-1]=="文中"`、`list("中文")==["中","文"]`、`for ch in "中文"` 次数、enumerate/zip 对 str、bytes 对照 `b"\xe4\xb8\xad"[0]==228` / len==3；既有 tests/run/05_strings.py,15_slice.py,17_slice_assign.py,19_comprehension.py 回归；新增迭代性能用例（§6.4）要求不出现 O(n²)（2N 斜率检查） |
| 回归风险 | 高：迭代/切片是热路径；游标优化做错会算错值（不是慢，是错），必须靠新老用例双跑。建议 S2 拆两个 commit：S2a 语义翻转（先接受 O(n²)）、S2b 游标优化（纯性能，可回退） |
| 能否独立提交 | 与 S1 同批发布（S1+S2 是一次完整语义翻转）；S2a/S2b 可以分开提交 |

### 4.3 S3：派生操作 + 格式化宽度 + re + 文件 I/O

| 项 | 内容 |
|---|---|
| 改哪些文件 | `runtime/rt_str.c` 方法全套（§1.4）、`runtime/rt_format.c`（宽度/精度/%c/is_bytes）、`src/lower_method_str.cpp`（is_bytes 传参、bytes ops）、`runtime/rt_re.c`（§1.7 三项）、`runtime/rt_file.c`（读/写/seek/tell 单位）、`runtime/rt_datetime.c:572-578`（Path.read_bytes 用 bytes 构造）、`runtime/rt_str.c:183-193`（ord/chr） |
| 语义变化 | 方法/格式化/re/文件 API 的可见单位变码点；bytes 方法保持字节 |
| 验收 | 新 `tests/run/165_codepoint_methods.py`（find/index/startswith/count/split/join/strip/replace/zfill/ljust/center/capitalize/title/is*/removeprefix、百分号宽度、f-string 宽度、repr、ord/chr、dict 中文 key）；新 `tests/run/166_codepoint_re_file.py`（`re.findall(".", "中文")==["中","文"]`、m.start()/end() 码点、sub/split、中文文件读写 read(n)/tell）；既有 56_re.py / 57_re2.py / 53_json.py / 07_fstrings.py / 79_datetime_pathlib.py 全绿 |
| 回归风险 | 中高：re 是最大一块，点号/类/量词改错会静默改变匹配结果；文件 I/O 的 seek/tell 一致性要专门测。建议 re 单独 commit |
| 能否独立提交 | 能（S1+S2 之后，S3 内部可按「方法 → 格式化 → re → 文件」拆 commit） |

### 4.4 每阶段通用验收命令

    # 单文件
    ./build/dev/pyadamas check  tests/run/05_strings.py
    ./build/dev/pyadamas run    tests/run/05_strings.py
    # 全量 run 用例（提交前）
    bash tests/run_tests.sh build/dev build/dev/pyadamas
    # 差分（随机程序，两端必须逐字节一致）
    python3 tests/diff/run_diff.py --seeds 3
    # 标准库/RC 基准
    python3 tests/bench/run_bench.py --filter str
    PYADAMAS_RC_STATS=1 ./build/dev/pyadamas run tests/run/05_strings.py
    # 内置函数微基准（S3 前后各跑一次，存基线）
    python3 bench/run_bench.py --repeat 3 --json /tmp/bench-before.json
    python3 bench/run_bench.py --repeat 3 --json /tmp/bench-after.json

（本设计稿不执行这些命令；它们是施工时的验收口径。）

### 4.5 中间态一致性（重要）

S1 之后、S2 之前：`len("中文")==2` 但 `"中文"[0]` 仍是第一个字节（非法 UTF-8 单字节
串，打印是乱码）。这个窗口**不该对外发布**。两个选择：

- 推荐：S1+S2 作为同一次语义翻转合入（开发分支上两个 commit 没问题，发布口径算一次）；
- 若必须每个 commit 都可发布，则把 S1 的语义翻转推迟到 S2 一起做，S1 只落「结构体多一个
  字段、所有构造点填 0、字面量多发射一个 0」的纯机械改动（此时 pub 就是 len），S2 一次性
  切语义。代价是 S1 阶段的 pub 恒等于 len，要到 S2 才产生信息。

---

## 5. 兼容性影响

### 5.1 现有测试扫描结论

对 `tests/`（164 个 run 用例 + 12 个根级检查用例）逐个扫非 ASCII 字符串字面量，结果：

- 非 ASCII 字符出现在代码里的地方**几乎全是注释/docstring**（类型名、说明、进度快照）。
- 真正会被执行的非 ASCII 字面量只有两处：
  - `tests/run/53_json.py:6` `print(json.dumps("中文"))`：`rt_json.c` 本来就是
    「UTF-8 解码成码点再转 `\uXXXX`」，输出不变；
  - `tests/06_lexical.py:11-12` `uni_escape = "\u4e2d\u6587"` / `wide_escape`：
    只赋值、从不打印/断言。
- **没有任何一个用例对非 ASCII 做 len / 下标 / 切片 / find / strip / 格式化宽度 的断言。**
  证据：`grep -rnP 'len\([^)]*[\x{4e00}-\x{9fff}]' tests/ bench/` 无命中；
  `grep -rnP '[\x{4e00}-\x{9fff}][^\x00-\x7F]*\[' tests/` 只命中注释。

因此「现有用例预期会变」的清单**是空的**——这是本次语义改动最有利的一点：回归风险主要
来自实现错误（布局/构造/游标），不是来自测试预期。

### 5.2 「哪些用例的预期会变」表

| 用例 | 现在 | 改后 | 变化 |
|---|---|---|---|
| `tests/run/53_json.py` | `json.dumps("中文")` 输出 `"\u4e2d\u6587"` | 不变 | 无（`rt_json.c:78-150` 已按 UTF-8 解码） |
| `tests/06_lexical.py` | 赋值不输出 | 不变 | 无 |
| tests/run/05_strings.py、15_slice.py、17_slice_assign.py、19_comprehension.py、56_re.py、57_re2.py、07_fstrings.py | 全 ASCII | 不变 | 无（但要作为回归守门用例） |
| tests/bench/*、bench/* | 基准脚本本身用 CPython 跑；被测程序 ASCII | 不变 | 无（bench/03_len.py、19_chr.py 的 N 可能因性能变化重标） |
| 全部其它用例 | ASCII | 不变 | 无 |

**需要新增的用例**（建议命名，接在 162_pep479.py 之后）：

1. `tests/run/163_codepoint_len.py`：len / len_bytes / 字面量 / encode 长度。
2. `tests/run/164_codepoint_index.py`：下标/负下标/切片/正负步长/迭代/enumerate/zip/
   推导式/list()/sorted/reversed 的码点行为 + bytes 对照。
3. `tests/run/165_codepoint_methods.py`：方法、百分号与 f-string 宽度、repr、ord/chr、
   dict 中文 key。
4. `tests/run/166_codepoint_re_file.py`：re 的点号/字符类/match 下标/sub/split +
   中文文件读写与 read(n)/tell/seek。
5. `bench/builtins/51_len_cjk.py`、`52_index_cjk.py`、`53_iter_cjk.py`
   （按 `bench/README.md` 的规则：参数随循环变化、结果打印、N 由驱动校准）。

### 5.3 文档必改点（本设计只写本文件，实施时同步改）

| 文档 | 位置 | 现在 → 改成 |
|---|---|---|
| `docs/compatibility.md:14` | 表行 `len("中文")` 6 vs 2，「字符串是字节串」 | **删掉这一行**（改后与 CPython 一致，不再是差异）。若想说明实现差异，可改写为「str 内部 UTF-8 字节存储、码点语义，len 仍 O(1)」并指向本设计 |
| `docs/compatibility.md:15` | `"中文"[0]` 单个字节 vs "中" | **删掉这一行**（改后一致） |
| `docs/compatibility.md:74` | 「切片：str 按字节」 | 改成「str 切片按码点；bytes 切片按字节」 |
| `docs/compatibility.md:78` | 整段「字节串这条最容易被忽略……要不要改成码点语义是待定项」 | 改成：**字符串已是 UTF-8 存储 + 码点语义**（len/索引/切片/迭代/方法/格式化宽度按码点，bytes 按字节）；**仍存在的差异**：非 ASCII 的大小写映射与 Unicode 字符类别未实现（`"中".isalpha()` 仍 False）、str.strip() 默认空白仍只认 ASCII、re 继续 ASCII 字符类与 \w、chr() 拒绝代理码点、open(...,'rb') 仍按 str 返回原始字节；字节长度出口是 len_bytes(s)。设计见 str-codepoints.md |
| `docs/language.md:50` | 「str：不可变字节串，len 在对象头里（O(1)）」 | 「不可变 UTF-8 字符串；长度按码点缓存，len O(1)」 |
| `docs/language.md:135-136` | 「str 按字节序比较……」 | 可保留（结论仍成立），补一句「比较仍按 UTF-8 字节序，结果与码点序一致」 |
| `docs/language.md:142-146` | 「len 为字节数（不是字符数）……切片按字节」 | 改为码点语义 + len_bytes 出口 + 切片按码点 |
| `docs/language.md:148-160` | bytes 段：「和 str 同一套运行时表示……len/索引/切片按字节」 | 保留（bytes 语义确实不变），补一句「与 str 共用结构体，但长度/下标/切片/方法全部按字节，由静态类型选择实现」 |
| `docs/language.md:193,198` | 「str 新字节串」「右侧可为 str（按字节拆成字符列表）」 | 改为「新字符串」「按码点拆成字符列表」 |
| `docs/language.md:991` | 「re……字符串按字节匹配」 | 改为「匹配按 UTF-8 字节引擎 + 码点边界；点号/字符类按码点步进；\w/大小写折叠仍 ASCII；Match.start/end/span 返回码点下标」 |
| `docs/language.md:1065-1070` | 「字符串方法（字节串语义、ASCII 大小写）」 | 改为「码点语义；大小写/字符类别仍 ASCII」 |
| `docs/runtime.md:29,63-91` | Py23Str 布局、SSO、偏移 0、hash 缓存、「单字节字符串」 | 更新结构体（32 字节、pub 在 +0、len 在 +8）、SSO 内联区在 +16、字面量四字布局、字面量 pub 编译期常量、pa_char_str 只管 ASCII + pa_cp_str |
| `docs/runtime.md:154-162` | 切片段「str 结果新建字节串，按字节复制」 | 改为「str 按码点切片（内部先换算字节偏移），bytes 按字节切片」 |
| `docs/runtime.md:638-639` | pa_str_char_at 表「返回驻留的单字节串」；pa_char_str「256 个」 | 改为「负索引加 nchars；返回单码点串（ASCII 驻留）」；pa_char_str「128 个 ASCII」+ 新增 pa_cp_str |
| `docs/runtime.md:669` | pa_print_str 表「原样写出 len 个字节」 | 不变（仍写字节），可补一句 len 是字节数 |
| `docs/roadmap.md:141` | 待决策表里「字符串语义」 | 移到「已完成」，并更新链接指向本文件的实施版本 |
| `docs/index.md:9-19` | 文档地图 | 加一行 str-codepoints.md（本次任务范围外，实施时补） |
| `docs/development.md:63` | 「运行 158 个」等规模数字 | 新用例落地后更新计数 |

---

## 6. 性能估算

### 6.1 微操作成本

| 操作 | 现在 | 改后 | 变化 |
|---|---|---|---|
| len(s) | O(1) 读偏移 0 | O(1) 读偏移 0 | **不变**（这正是加缓存的目的） |
| if s: | O(1) | O(1) | 不变 |
| 字面量 s[i]（ASCII） | O(1)，返回驻留串 | O(1)，返回驻留串 | 不变 |
| 字面量构造 | 静态对象 | 静态对象 + 编译期多写一个字 | 运行期不变；编译器多一条 .quad |
| s[i]（非 ASCII） | O(1) 取 1 字节（结果其实是错的） | O(i) 扫描 + O(1) 分配 1–4 字节串 | **显著变慢**；顺序循环靠游标拉回 O(1) 摊还 |
| 拼接 a+b | O(n) 拷贝 | O(n) 拷贝 + O(1) pub 相加 | 编译后代码多一条 add，可忽略 |
| str_new 从缓冲区 | O(n) 拷贝 | O(n) 拷贝 + O(n) 数码点（约 0.1–0.3 ns/字节，简单循环） | 构造慢 ~1.2–1.5×（受缓存带宽影响） |
| s[a:b] | O(结果字节)，memcpy | O(n) 扫描 + O(结果字节) | 长串切短段变慢（最坏全扫） |
| find/index/count | O(n) memchr 匹配 | O(n) 匹配 + O(n) 下标换算 | ~2× 左右（换算可和匹配合并成一遍） |
| split/replace | O(n) | O(n) + 每段数码点（同一次扫描内可做） | ~1.2–1.5× |
| 字典查字符串 key | 字节 FNV + 缓存 | 完全不变 | 不变 |
| re.search 单次 | 字节 VM | 字节 VM + 码点步进/换算 | 非 ASCII 文本变慢 ~1.5–2×；ASCII 文本基本不变 |
| re.finditer/findall 多匹配 | 逐匹配 | 额外一次 byte→cp 映射（O(n)） | 总 O(n + k)，不再 O(n·k)（§1.7） |
| 格式化宽度 | 按字节 strlen | 按 pub（O(1)） | 基本不变 |
| re 编译的模式 | 字节解析 | 不变（模式按字节，点号/类在 VM 里加判断） | 可忽略 |

### 6.2 内存

- 每个 str/bytes 对象负载 +8 字节：短串总分配 88 → 96（+9%），长串固定 +8。
- 字面量：每个字面量 +8 字节（.data.rel.ro），通常可忽略；如果同一个内容既有 str 又有
  bytes 字面量，会多一份对象（§2.6）。
- 短串 freelist：档位后移，缓存块数不变（每档 64 块），池总占用略增（约 16 档 × 64 × 8B ≈ 8KB）。
- nchars 是常数，**不存在**「按字符数的额外表」——除非 S4 做稀疏索引（§2.8）。

### 6.3 热点路径影响与缓解

1. **str 迭代 O(n²)**（最严重）：for ch in s、推导式、enumerate/zip、内置 min/max/sum/any/all
   的 str 分支都走「Len + 每轮 IndexLoad」。1 万字符的串迭代一次约 1000 万字节扫描
   （~3–5ms），1000 次就是秒级。**S2 必须同时上顺序游标**：
   - 降级：在循环头前 cursor=0（临时槽），循环体把 IndexLoad(s,i) 换成
     CallRt(StrCharAtCursor, s, i, &cursor)（SlotAddr 已有，IR 指令存在）；
   - 运行时：i == cursor.last+1 时从 cursor.byte_off 直接取下一个码点（ASCII 直接查表），
     否则回退从头扫；顺序访问整体 O(n)。
   - 内置聚合（min/max/sum/any/all）可以直接改成 pa_str_to_list（一次物化 O(n)，
     与今天 list 路径一致），避免在 C 里逐下标回调。
2. **range(len(s)) 里 s[i]**（用户手写循环）：降级器现在有「list/tuple 免检查」的
   bounds-hint 机制（`lower_expr_container.cpp:87-105,117`）。S2/S3 可以扩成
   「str 的顺序游标 hint」：识别 for i in range(len(s)) + s[i] 同一对名字，挂游标槽。
   识别不了的手写随机访问就接受 O(i)，文档写明。
3. **re 多匹配的下标换算**：见 §1.7，一次扫全文建映射，别逐匹配从头扫。
4. **文件 tell/seek**：文本模式本来就是「从文件头重解码」O(n)（`rt_file.c:587-613`），
   维持 O(n) 不变；read(n) 多出的码点计数与拷贝同一次循环完成。
5. **格式化**：宽度用 pub（O(1)），没有新增扫描。

### 6.4 怎么量（建议测法）

1. **内置函数微基准**（`bench/run_bench.py`，对比 CPython，口径见 `bench/README.md`）：
   - 已有 03_len、06_str、18_ord、19_chr、26_format、44_hash 直接看回归（这些是当前
     PyAdamas 领先 CPython 的项目，掉一个量级就是问题）；
   - 新增 CJK 用例（§5.2 的 51–53），以及 54_split_cjk、55_find_cjk、56_format_width_cjk、
     57_dict_cjk；
   - 用 --json 存改动前后两份，按用例比 speedup 而不是绝对时间。
2. **迭代斜率**（防 O(n²)）：单独写一个「同一段代码，N 和 2N 各跑一次」的用例；现有 bench
   驱动的「线性」列就是干这个的（`bench/README.md` 的已知口径）——如果 2N 耗时翻倍
   以上，说明循环体被优化掉或退化了，都要查。
3. **内存/RC**：`tests/bench/cases/bench_rc_strings.py`（短命字符串拼接/存储/释放）测
   分配开销；`PYADAMAS_RC_STATS=1` 看统计的分配字节/次数；必要时用
   `tests/bench/run_bench.py` 的 RC 段。
4. **标准库行为回归**：`tests/bench/cases/bench_string_textwrap.py`（string/textwrap）
   与 bench_json / bench_base64_hashlib 在 S3 前后各跑一次，确认字符串方法与 bytes 路径
   没有功能退化。
5. **建议阈值**（拍板用）：len/len_bytes 不得劣化；字面量下标不得劣化；顺序迭代不得出现
   O(n²)（2N 斜率 ≤ 2.5）；拼接/常见 ASCII 方法劣化 ≤ 15%；内存总量 + ≤ 10%；
   re 在 ASCII 文本上劣化 ≤ 10%、非 ASCII ≤ 2×。超出就把对应优化（游标/str_new_ascii/
   批量化换算）提前到该阶段内，不许记账到 S4。

---

## 7. 其它

### 7.1 Win64 目标

**确认无额外影响**，理由：

- 字符串布局是**目标无关**的：结构体、SSO、字面量发射（`src/codegen.cpp`）都是同一份
  代码；Win64 只改 TargetABI（实参寄存器/影子空间/callee-saved/setjmp 符号），
  `codegen_call.cpp:600-617` 里取 data 的偏移是硬编码 16→24，与 ABI 无关。
- 运行时（`runtime/rt_*.c`）是同一份源码，平台层差异只在 poll/clock/mkdir 等
  （`rt_internal.h:16-195`），不碰字符串。现有 Win64 已验证（roadmap.md:112-116）。
- 唯一注意点：cimport 边界上 C 的 char* 在 Windows 上仍是字节指针，UTF-8 内容无差异；
  wchar_t/_UNICODE 不在当前 cdecl 子集里，不用管。

### 7.2 bytes 与 str 的互转（如果现在有）

现状：互转就是改标类型（`lower_method_str.cpp:25-29,189-194`），运行时零成本，因为两者
表示完全一样。码点语义下公共长度不同，必须改成：

| 方向 | 实现 | 成本 | 说明 |
|---|---|---|---|
| str.encode(encoding=None) | pub == len（全 ASCII）⇒ 返回 s 本身（inc）；否则新对象：memcpy 字节 + pub=len | ASCII O(1)，非 ASCII O(n) | 编码名参数继续忽略（现状） |
| bytes.decode(encoding=None) | pub == len（全 ASCII）⇒ 返回本身；否则扫描算 nchars 写 pub | ASCII O(1)，非 ASCII O(n) | 内容不是合法 UTF-8 时按「每个非续字节算 1 码点」定义 pub，或抛 UnicodeDecodeError（现状是忽略编码、无校验；建议**保持不校验**，文档化） |
| 字面量 | 编译期按 kind 生成（§2.6） | 0 | b"..." 的 pub=字节数 |
| bytes(...) 构造 | pa_bytes_from_list/zeros 用 str_new_bytes | O(1) 计长 | |
| 其它 bytes 产出 | base64/hashlib/Path.read_bytes/二进制文件读 | str_new_bytes | O(1) 计长 |

如果实现时不想做「encode 非 ASCII 拷贝」，也可以让 encode() 产出**视图对象**（共享
字符区）——但当前运行时没有共享字符区的机制（data 跟着对象走、str_alloc 内联在结构体后），
加视图比拷贝更贵，不建议。

### 7.3 与 unknown 类型的交互

- len(unknown)：O2 布局下偏移 0 就是答案，**天然正确**（str 是码点、bytes 是字节、
  其它堆对象是各自长度）。这是选 O2 的核心理由。
- unknown 参与下标/切片：降级器目前只对已知静态类型选 pa_str_char_at / pa_str_slice；
  unknown 走不通这些路径（维持现状）。
- str(unknown)（pa_str_any）：运行时靠 RC 索引确认是 STR 再原样返回；如果 unknown 实际是
  bytes，会把它当 str 返回（既有类型谎言，len 仍正确，因为偏移 0 是 bytes 的字节数）。
  不扩大这个洞。
- 容器里混装 unknown 字符串：与今天一致（kind 2 保守 RC/GC），nchars 只是对象内字段。

### 7.4 非法 UTF-8 与二进制文件（不变量例外）

本设计把「str 是合法 UTF-8」作为不变量，但现状有两个口子会在 str 里塞任意字节：

1. **二进制模式文件读**：`open(path, 'rb').read()` 静态类型是 str
   （`lower_method_misc.cpp:282` 固定 t_str()），运行时 `rt_file.c:382-407` 直接把
   原始字节 str_new。
2. **str 字面量的 \xNN 转义**（现状）：`"\xff"` 是单字节 0xFF。

处理建议：

- 字面量问题在 S1 根治：str 的 \xNN 按「U+00NN 再编 UTF-8」处理（`lexer.h:91-96`
  改为 append_utf8）；bytes 字面量的 \xNN 保持原始字节，并且 bytes 的八进制转义也改成原始
  字节（现在错误地走 append_utf8，b"\377" 会变成 2 字节——这是顺带修掉的既有 bug）。
  \u/\U/\N 在 bytes 字面量里应报错（CPython 语义）。
- 二进制文件读维持现状：**不崩、但内容可能非法 UTF-8**。为了让 len 有定义，这类对象
  用 str_new_bytes 构造（pub=字节数），作为「字节直通字符串」文档化；下标/切片/方法
  在非法字节上按「每个非续字节算 1 码点」的宽松规则定义（实现里数码点的公式天然如此），
  不会越界、不会崩，结果不保证与 CPython 一致。要根治得让 rb 的 read() 返回 bytes 类型
  （File 类型需要携带 binary 标记），超出本设计，列为 S4 候选。
- 代理码点（\uD800）：str 字面量/chr() 拒绝（无法编成 UTF-8），与 CPython 的差异写进
  compatibility.md。

### 7.5 未决问题（需要拍板）

1. **字节长度出口**：len_bytes(s)（推荐）还是 s.nbytes？后者要补一套属性读取路径（§3.2）。
2. **len/len_bytes 的命名**：是否接受非 CPython 名字进 builtin 命名空间；若不接受，
   退路是只留 len(s.encode())（O(n)）并文档化。
3. **非 ASCII 大小写/字符类别**：本轮明确不做（`"中".isalpha()` False），S4 是否排期？
4. **re 的 Unicode 程度**：S3 只做「码点步进 + 下标换算」；Unicode 属性类、全量 \w/I 折叠
   继续不做，可接受？
5. **文件 I/O 的 read(n)/tell 单位**：S3 一并改成码点，还是先留字节（更省事但与 CPython
   的文本模式不一致）？
6. **稀疏索引表**：确认本轮不做、S4 先量再决定（§2.8）。
7. **chr() 与代理码点**：接受「拒绝代理码点」的差异，还是让 chr(0xD800) 抛 ValueError？
   （推荐后者：我们的表示无法安全承载代理码点。）

---

## 附录 A：可复现的盘点命令

    cd /home/lenovo/cpp/cpy23
    # 运行时字符串构造点
    grep -rn 'str_alloc(\|str_new(' runtime/*.c | wc -l          # 168
    # 降级器字符串常量点
    grep -rn 'const_str(' src/*.cpp | wc -l                       # 123
    # 运行时长度的读取（含 list/dict 同名字段，用于定位字符串长度假设）
    grep -rn 's->len\|a->len\|b->len\|src->len\|v->len\|hay->len\|needle->len' runtime/*.c | wc -l
    # 现有关键点
    grep -n 'IrOp::Len' src/codegen_ops.cpp
    grep -n 'pa_str_char_at\|pa_str_slice\|pa_str_method\|pa_str_repeat\|pa_str_concat' runtime/*.c
    # 非 ASCII 字面量扫描（结论：现有用例不依赖字节语义）
    grep -rnP 'len\([^)]*[\x{4e00}-\x{9fff}]' tests/ bench/

## 附录 B：本次不改、但实施时会碰到的相邻点

- bytes*int 的降级缺口（类型层允许、降级层漏了 Bytes，§1.4）——S2 顺手补。
- str 字面量 \xNN 与 bytes 八进制转义的语义错误（§7.4）——S1 顺带修。
- re 每次 findall/split 重新编译模式（roadmap.md:169 技术债）——与本设计无关，不动。
- bytes 没有运行期种类导致 isinstance(b"x", bytes) 对 unknown 保守返回 false
  （compatibility.md:287）——本设计不引入新 kind，维持现状。
- unknown 裸值表示（roadmap.md:143「维持裸 i64」）——不做 shadow tag，本设计不依赖
  运行期类型标签。
