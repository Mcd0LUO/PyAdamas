# unknown 类型运行时语义审计：为什么「种类未知」总是退化成指针比较

> 2026-11-23（0.422–0.425）。本文归档当天连续发现的**同一类 4 个真实 bug**：
> 当类型推断给不出具体类型时，PyAdamas 把「种类」传成 `3`（其它）/ `'x'`，
> 多处运行时分派把它实现成「按机器字（堆指针）比较」。对 int/float/None 是对的，
> 对堆对象（str / bytes / big / 容器 / 类实例）就错——轻则 `in`/`==` 查不到，
> 重则排序静默乱序、字典 KeyError。
>
> 相关：[runtime.md](runtime.md)（对象模型）、[compatibility.md](compatibility.md)（与 CPython 差异与验证）、
> [optimization-roadmap.md](optimization-roadmap.md)（pass 顺序依赖的类似「隐式假设」问题）。

## 1. 问题模式

PyAdamas 是静态类型编译器，但**允许局部「未知类型」**：

- `d = {}` 在无类型上下文下是 `dict[unknown, unknown]`；
- `["a" + str(0), 1]` 这种混合容器是 `list[unknown]`；
- 从 `list(unknown_dict.keys())` / `unknown` 函数返回值等流出的值也是 `unknown`。

lowerer 在生成运行时调用时，会把静态种类（int/str/float/big/其它）作为一个**立即数**
或**描述符字符串**传给 runtime：

| 分派点 | 种类编码 |
|---|---|
| dict 的 `hash_key_kind` | `0` int、`1` str、`2` float、`3` **其它/未知**、`4` big、`9` raw/or |
| list 元素 kind（`list_value_eq`） | 同上 0/1/2/3/4 |
| 容器相等描述符串（`chain_elem_eq`） | `i/f/s/b/l/d/t/x`，`x` = **其它/未知** |
| 动态比较描述符（`pa_any_cmp*`） | `i/f/s/b/o/x`，`o/x` = 其它/未知 |
| 排序 `sort_kind` | `0` int/bool、`1` float、`2` str、`4` 容器、`5` u64、`3` **其它/未知** |

危险就出在这里：`3` / `'x'` 的兜底实现如果只是 `a == b`（比较两个 `int64_t` 机器字），
那么**两个内容相等的 str 会因为对象地址不同而不相等**；容器会被当成指针排序。
str 是不可变堆对象，`in`/`==`/`dict` 键本该按**内容**比较——这就是 CPython 语义。

**修法统一思路**（与 0.422 dict 修复同款）：在兜底分支里用 **RC kind 探针**
（`pa_unknown_is_heap` + `PyAdamasRc.kind`）认出堆对象，再按内容语义分派；
非堆值（int/bool/None 的机器字）保持原语义。

## 2. 审计表

### 2.1 找到并修复的 4 个 bug

| # | 点位（文件:行，修复版本） | 分派种类 | 未知时的（修复前）行为 | 复现（pyadamas vs CPython） |
|---|---|---|---|---|
| 1 | `runtime/rt_dict.c:113` `dict_key_eq` / `:155` `dict_hash_key`（**0.422**） | dict key_kind 3/9 | 按机器字比指针：内容相等的动态 str 键查不到 | 无类型 `{}` + `"k"+str(i)` 键：pyadamas 三档 KeyError / CPython 正常 |
| 2 | `runtime/rt_list.c` `list_value_eq`（kind=3）、`chain_elem_eq`（`'x'`）（**0.424**） | list/tuple/dict/set 元素 kind | `in` / `.index` / `==` 全按指针：等值新 str 查不到、`list[unknown] == list[unknown]` 为 False | `p in ["a"+str(0), 1]`：CPython True / 修前 False；`.index`：CPython 0 / 修前 ValueError |
| 3 | `runtime/rt_any.c` `pa_any_cmp` / `pa_any_cmp_t` 的 other 分支（**0.424**） | 动态比较描述符 `o/x` | 堆 list/tuple 落到 `eq = (a == b)` 指针比：unknown 域 `list == list` 为 False | 函数内 `eq(xs, ys)`（xs/ys 是 `list[unknown]`）：CPython True / 修前 False |
| 4 | `runtime/rt_list.c:848` `pa_list_sort` 的 kind=3（**0.425**） | 排序 `sort_kind` | 走 `sort_i64`（机器字/指针）：元素族混杂时**静默乱序**，而 CPython 抛 TypeError | `sorted(["a"+str(0), 1])`：CPython TypeError / 修前 `[1, 'a0']` |

4 个 bug 的共同点：**都是 -O0 就能复现**（不是优化器引入），且都只在该模块**没被精确索引/类型推断**时出现。

### 2.2 审计过的安全点（5 个）

| 点位 | 分派种类 | 未知时行为 | 结论 |
|---|---|---|---|
| `runtime/rt_class.c:288` `pa_str_eq_unknown` | 运行时探针 | 认出堆 str 后 `pa_str_eq` 按内容比 | **安全**（本来就是探针分离散类型） |
| `runtime/rt_class.c:297` `pa_complex_eq_unknown` | 探针 + magic | 认出 complex 后按值比 | **安全** |
| `runtime/rt_any.c` `any_decode`（动态算术/比较解码） | 探针 RC kind | 堆 str→`ANY_STR`、float 盒→`ANY_FLOAT`、big→`ANY_BIG`；数值/字符串语义正确 | **安全**（问题只在其后的 other 兜底分支，已随 bug 3 修） |
| `runtime/rt_list.c` `pa_hash_value`→`dict_hash_value`（kind 3/9） | key kind | 0.422 起堆 str 按内容哈希 | **安全**（`hash(dynamic) == hash(literal)` 三档一致） |
| `runtime/rt_deque.c` `pa_deque_index/count/remove` | 元素 kind | 都走 `list_value_eq(kind)`，0.424 修好后 kind=3 走内容比较 | **安全**（`deque.index`/`count` 三档一致） |

另外：`in` 对 dict/set（`pa_dict_contains`，走 dict key 分派）在 0.422 后安全；
`pa_range_contains` 是纯数值，安全；普通 `print`/`repr` 走 tag/探针分派，抽查安全。

## 3. 复现方法（重要，踩过坑）

### 3.1 「裸 untyped 参数」逼不出 unknown

PyAdamas 会按调用点/函数体做类型推断与单态化，所以下面写法**测不出** bug：

```python
def f(xs, p):          # xs 在调用点被推成 list[str] → ListContains kind=1 → 内容比较正常
    return p in xs

f(["k" + str(0), "k" + str(1)], "k" + str(0))   # 三档都对，bug 不出现
```

必须构造**混合容器**，让静态元素类型真的是 `unknown`：

```python
xs = ["a" + str(0), 1, 2.5]      # list[unknown]：ListContains 的 kind 立即数 = 3
p  = "a" + str(0)                # 动态构造、与 xs[0] 等值但不同对象
print(p in xs)                  # 修前 False，CPython True
```

同理：`list[unknown]` 的 `==`（`pa_any_cmp`）、`list[unknown]` 的 `sorted`（sort_kind=3）都要用混合容器构造。
**类型推断的「过度精确」会掩盖 unknown 分支的 bug。**

### 3.2 `pa_any_cmp` 实际跑的是 `_t` 版

`-build` 默认开 type_tags，`src/codegen_tag.cpp:117` 会把 `RtFn::AnyCmp` 重定向到
`pa_any_cmp_t`，**但 IR 文本仍打印 `pa_any_cmp`**。只改非 `_t` 版会完全没有效果
（入口加 `fprintf` 都打不出来），排查时先 `grep codegen_tag.cpp` 确认实际符号；
两个实现要一起改（本例用共享的 `any_unknown_eq` 辅助函数保证语义一致）。

### 3.3 修复后要跑的最小对拍集

- `tests/run/487_append_list_rc.py`（dict str 键，0.422）、
- `tests/run/489_unknown_kind_eq.py`（in/index/==，0.424）、
- `tests/run/490_unknown_kind_sort.py`（排序族分类，0.425）；
- 三档 `-O0/-O1/-O2` 对 CPython、`difftest`、ASan。

## 4. 已知缺口与后续（未修，如实记录）

| 缺口 | 现状 | 为什么没修 / 后续方向 |
|---|---|---|
| **类实例 dunder 在 unknown 域** | 静态类型已知时 `__eq__` 正常（`lower_dunder_eq` 用编译期 dunder 槽）；实例从混合容器取出后 `==` 走 `pa_any_cmp` other 分支 → 按同一性，**不调用 `__eq__`**（CPython 会调）。`hash`/`sorted` 同类。 | **0.433/0.434：`__eq__`、`__lt__` 已打通**——vtable 元数据第 7 字指向 dunder 槽表 `[__eq__, __lt__, __hash__, -1]`（`IRClass::eq_slot/lt_slot/hash_slot`，`lower_class.cpp` 填），`rt_any.c`/`rt_list.c` 按「实例→vtable→槽表→槽」分派：unknown 域 `==`/`!=`/`<`、typed `list[P]` 的 `in`/`index`/`remove`、`sorted(类实例)`（有 `__lt__` 时排序，否则 TypeError）。剩余：**`__hash__` 未分派**——无类型 dict/set 的实例键仍按机器字哈希（需 dict 的 `key_kind=3` 增加「实例 → hash 槽」分支并保证同键等价关系一致，方案见报告）；`hash(instance)` 的 unknown 域同样未接。 |
| **deque 的 `in`** | `dq.index`/`count`/`remove` 有 runtime 实现且安全；但 `p in dq` **编译期报错**（`argument of type 'collections.deque' is not iterable`）。 | 这是 lowering 缺口（没有 deque 的 `in` 降级路径），不是 runtime 分派 bug；实现需要新的容器迭代/包含降级，排在语言 gap 里。 |
| **嵌套容器 chain 深度不足** | `list[list[int]]` 的相等描述符链只覆盖有限深度，深层落回 `'x'` → 同一性比较；如 unknown 域 `[[1,2]] == [[1,2]]` 可能为 False（CPython True）。 | 0.424 的 `chain_elem_eq` 只对 `'x'` 做了 str/big 内容比较（保守），容器递归需要类型描述符深度信息；属于 chain 生成侧的既有口径，未在 audit 范围内改动。 |
| **`sorted` 的「全 other 族」** | kind=3 族分类后：全数值按数值、全 str 按字节；**用户类实例**族从 0.432 起显式抛 `TypeError`（文案 `'<' not supported between instances of 'object' and 'object'`，不再静默按机器字返回乱序）；全容器/None 族仍按机器字（CPython 会按内容）。 | 类实例与「类 dunder unknown 域」同源：运行时没有「类名→`__lt__` 槽」映射，要修需新增类注册机制（新功能）。容器内容比较需要元素描述符（静态类型未知时没有）；改抛错需要区分容器与实例（按 RC kind），已在 `pa_list_sort` 里实现。 |

## 5. 修复一览（代码与测试）

| 版本 | 修复 | 代码 | 测试 / 验证 |
|---|---|---|---|
| 0.422 | dict `key_kind=3/9` 的 str 键按内容哈希/比较 + 保留精确堆索引 | `runtime/rt_dict.c` +70/−26、`src/ir.cpp` +10 | 487 用例；383 ran ok；difftest 362-0；wordfreq +0.078% |
| 0.424 | `list_value_eq(kind=3)` / `chain_elem_eq('x')` 的 str/big 内容比较；`pa_any_cmp(_t)` other 分支的 `any_unknown_eq`（str/big/list 递归） | `runtime/rt_list.c` +21、`runtime/rt_any.c` +30 | 489 用例；385 ran ok；difftest 364-0；12 基准 ±0.05% |
| 0.425 | `pa_list_sort` kind=3 的元素族分类（NUM/STR/OTHER）+ `sort_uknum`（int/float/big 混比）+ 族混杂抛 TypeError | `runtime/rt_list.c` +78 | 490 用例；386 ran ok；difftest 365-0；12 基准 ±0.47%（无 >1%） |

## 6. 教训

1. **静态类型系统的「未知」必须被运行时分派认真对待**：`3`/`'x'` 这类兜底编码一旦实现成
   机器字比较，就等于把「内容语义」悄悄换成「指针语义」，而且 -O0 就错、不依赖优化。
2. **审计这类问题要「先钉死门序」再动手**（与 0.423 的 pass 顺序问题同一教训）：
   用最小混合容器复现 → 打印实际分派点 → 再改；不要按印象修「看起来该修的那处」。
3. **测试要构造出未知形状**：类型推断会单态化掉大多数用例，裸 untyped 参数通常测不到 unknown 分支。
4. **同一语义可能有多份实现**：`pa_any_cmp` / `pa_any_cmp_t`、dict/set/list/tuple/deque 各一套，
   修一处必须扫同类；本次 4 个 bug 就是「同一模式在 4 个不同分派点」的重复出现。

## 7. 最终矩阵（0.425，实测）

### 7.1 12 基准 × -O0/-O1/-O2 输出对 CPython

36/36 一致（0 failures）：binary_trees / collatz / fannkuch / fasta / fib / mandelbrot / matmul /
nbody / nqueens / sieve / spectral_norm / wordfreq 三档全部 OK。

### 7.2 指令数（`perf stat -e instructions:u`，3 轮最小；默认管线）

基线 = 今日工作开始前（0.418，`sieve 104,389,573` 口径）。

| 基准 | 基线 | 0.425 | Δ |
|---|---|---|---|
| binary_trees | 4,715,319 | 4,713,376 | −0.04% |
| collatz | 82,204,158 | 81,503,944 | −0.85% |
| fannkuch | 35,007,122 | 34,926,419 | −0.23% |
| fasta | 5,955,784 | 5,955,815 | +0.00% |
| fib | 130,639,539 | 130,639,570 | +0.00% |
| mandelbrot | 69,678,191 | 69,310,369 | −0.53% |
| matmul | 10,605,949 | 10,126,744 | **−4.52%** |
| nbody | 199,390,637 | 109,040,119 | **−45.31%** |
| nqueens | 350,734,364 | 349,892,341 | −0.24% |
| sieve | 104,389,573 | 95,431,676 | **−8.58%** |
| spectral_norm | 247,401,494 | 94,637,438 | **−61.75%** |
| wordfreq | 257,507,514 | 257,707,471 | +0.08%（dict 精确索引保留的既有代价） |

`PYADAMAS_SIMD=1`（量化模板对照）：matmul **10,126,743**（10.1M 量级）、
spectral_norm **94,637,440**（94.6M 量级）。

### 7.3 向量化触发（`PYADAMAS_VECT_DBG=1`，-O2 默认管线）

| 基准 | `[vect]` 日志行 | 说明 |
|---|---|---|
| matmul | 4 | 2 个循环 × 2 行（`vectorize` + `avx2 4-lane`） |
| spectral_norm | 8 | 4 个循环 × 2 行（其中 2 个在未调用的死代码副本里） |
| nbody | 2 | 2 个循环 |
| sieve | 1 | 1 个循环（0.423 计数循环） |
| 其余 8 个 | 0 | 见 [vectorization-coverage.md](vectorization-coverage.md) |

合计 **4 个基准 / 15 条日志（9 个循环）**。
（注意：旧文档里的「11 条触发记录」把 matmul 按 4 行、spectral_norm 按 4 循环混算，
口径不一致；以本表实测为准。）

### 7.4 测试规模

| 闸门 | 数值 |
|---|---|
| `tests/run_tests.sh`（build/gfix + build/lead） | **386 ran ok**，0 failed（含 485/486/487/488/489/490） |
| `tools/difftest.sh` | **365 个用例，0 个不一致** |
| ASan | 386 ran ok，0 sanitizer 报告 |
| `tests/check_size.sh` | 通过 |

