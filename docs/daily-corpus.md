# 日常级脚本语料对拍与性能报告（daily-corpus）

> 任务：为 pyadamas 写一批**真实日常级**脚本语料，系统对拍 CPython（找 bug + 量性能）。
> 本报告随语料落在 `dailycorpus` 分支。基线：**pyadamas 0.311，HEAD `e855662`**；
> 独立 worktree `/tmp/wt-daily` 自建编译器快照（`cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
> -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'`，`cmake --build build -j32`）。
> **未触碰主树 / 未推 master / 未动主树 build**。发现的 bug 一律记录绕行，不在本分支修。

## 0. 环境与方法

| 项 | 值 |
|---|---|
| 编译器 | pyadamas 0.311（`e855662`），本 worktree `build/pyadamas`，Release -O2 -DNDEBUG |
| 参照 | CPython **3.14.4**（`python3`） |
| 机器 | Debian 13，Xeon Gold 6252 @ 2.10GHz，容器 cpuset **24-47,72-95**（nproc 96，可用 48）|
| 绑核 | `taskset -c 24`（cpuset 第一核）|
| 多租户负载 | 测量期间 `/proc/loadavg` 1min 在 **9~28** 波动（其他代理/租户在跑）；已注明 |
| 计时 | 每脚本 **9 轮**，每轮打乱脚本顺序 + 奇偶轮交换 CPython/pyadamas 先后；每次测量 BURST=3 取中位，再对 9 轮取中位 |
| 编译计时 | `pyadamas build -O2 --rebuild -o bin src`（强制全量重编，含前端+as+ld）|
| 运行计时 | CPython `python3 src`；pyadamas 直接跑产物 `./bin`（argv 两边一致）|
| 对拍 | 同源码两边跑，stdout **逐字节 `cmp`**，并比退出码；CSV 脚本在独立 scratch cwd 跑 |
| 工具 | `bench/daily/_tools/run_daily.py`（CPython 计时 harness，非语料）；`REPS/BURST/CORE/PA/PY` 可调 |

**抗噪纪律**（沿用主树 HANDOFF §6 教训）：交替 A/B + 打乱顺序 + BURST 连跑 + 多轮中位；
perf 只用来找热点 candidate，占比≠边际成本。短用例（<100ms）数字看量级与相对关系。

**数据生成纪律**：所有脚本自包含、确定性、无外部依赖、带类型标注（走快路径）。
随机数据用 **int64 安全 LCG**（Numerical Recipes：`(s*1664525+1013904223) % 2^32`，
乘积 < 2^63 不回绕）或 `random.Random(seed)` 的已验证一致方法。**刻意避开 int64 溢出回绕**
（CPython 任意精度 vs pyadamas 回绕，是文档化语义差，不是 bug——早期探针曾用 64 位 LCG
常量导致中间乘积溢出、两边序列分叉，已纠正）。

## 1. 语料清单（23 个，四类均衡 + 2 个已知 bug 覆盖）

| 类别 | 数量 | 脚本 |
|---|---|---|
| A 快速往返型（小 CLI/短文本/即时计算/配置解析）| 5 | a1_cli_calc, a2_config_parse, a3_text_transform, a4_unit_convert, a5_todo_cli |
| B 字符串处理型（日志/规范化/模板/词频/正则抽取）| 5 | b1_log_parse, b2_normalize, b3_template, b4_wordfreq, b5_re_extract |
| C csv 分录型（财务/库存/成绩/销售/清洗）| 5 | c1_ledger, c2_inventory, c3_grades, c4_sales, c5_csv_clean |
| D 数值算子型（矩阵/统计/积分/素数/金融/递归记忆化）| 6 | d1_matrix, d2_stats, d3_integrate, d4_primes, d5_finance, d6_rec_memo |
| K 已知 bug 故意覆盖 | 2 | k1_tuple_sort（→0.312）, k2_float_dict（→0.313）|

每个脚本都是真实日常任务（CLI 计算器、INI 配置解析、单位换算、待办管理、apache 日志聚合、
文本规范化/slugify、发票模板渲染、词频统计、正则信息抽取、财务日记账试算平衡、库存补货、
成绩排名、销售透视、脏 CSV 清洗去重、矩阵乘法/行列式/Cramer、描述统计/百分位、Simpson 积分收敛、
埃氏筛+孪生素数+Goldbach、贷款摊销/NPV/IRR、fib/网格路径/编辑距离/零钱兑换/Catalan）。
**不是微基准堆砌**：有过滤/聚合/分组/排序/写回/校验等真实逻辑。

## 2. 输出一致性（对拍结果）

- **21 个干净语料：21/21 逐字节一致（100%），退出码均 0，`pyadamas check` 全过。**
- 2 个已知 bug 覆盖脚本（k1/k2）：**故意不一致**（标注「已知」，见 §5）。
- 含已知脚本口径：21/23 = 91.3%。

| 脚本 | 类 | check | 输出 | 备注 |
|---|---|---|---|---|
| a1_cli_calc | A | ok | SAME | argv 算式 + 默认演示 |
| a2_config_parse | A | ok | SAME | INI 解析/类型强制/校验 |
| a3_text_transform | A | ok | SAME | lower/upper/reverse/slug/title/rot13 |
| a4_unit_convert | A | ok | SAME | 温度/长度/数据换算 + 往返校验 |
| a5_todo_cli | A | ok | SAME | 类实例 + key 排序 + 统计 |
| b1_log_parse | B | ok | SAME | re 解析 4000 行 + 多维聚合 |
| b2_normalize | B | ok | SAME | 规范化 + slugify 去重 2000 条 |
| b3_template | B | ok | SAME | {{key}} 模板引擎渲染 500 行发票 |
| b4_wordfreq | B | ok | SAME | 3 万词 Counter + most_common + tie-break |
| b5_re_extract | B | ok | SAME | 日期/金额/邮箱/电话/IP 抽取（**dict 去重绕行 N17**）|
| c1_ledger | C | ok | SAME | 800 分录试算平衡（int 分）|
| c2_inventory | C | ok | SAME | 600 库存分组补货（**int 复合键排序绕行 N18**）|
| c3_grades | C | ok | SAME | 400 成绩 fsum 平均 + 排名 |
| c4_sales | C | ok | SAME | 1000 销售透视 |
| c5_csv_clean | C | ok | SAME | 500 脏行清洗去重 QUOTE_ALL |
| d1_matrix | D | ok | SAME | 64×64 matmul + det4（**call-free 绕行 N19a**）|
| d2_stats | D | ok | SAME | 5000 样本 fsum 统计 + 百分位 |
| d3_integrate | D | ok | SAME | Simpson/梯形收敛 + Richardson |
| d4_primes | D | ok | SAME | 20 万筛 + 孪生 + Goldbach |
| d5_finance | D | ok | SAME | 360 期摊销 + IRR（**ipow 绕行 N19b**）|
| d6_rec_memo | D | ok | SAME | lru_cache + 手写 dict 记忆化 |
| k1_tuple_sort | K | ok | **KNOWN** | 元组列表自然排序 no-op（0.312）|
| k2_float_dict | K | ok | **KNOWN** | float 值 dict 损坏（0.313）|

## 3. 性能（绑核 24，9 轮中位，BURST=3；单位 ms）

| 脚本 | 类 | CPython run | pyadamas compile | pyadamas run | compile+run | speedup(run) | speedup(c+r) |
|---|---|---|---|---|---|---|---|
| a1_cli_calc | A | 18.16 | 52.29 | 2.88 | 55.16 | **6.32×** | 0.33× |
| a2_config_parse | A | 18.34 | 56.28 | 3.01 | 59.29 | **6.09×** | 0.31× |
| a3_text_transform | A | 17.94 | 50.96 | 2.89 | 53.84 | **6.21×** | 0.33× |
| a4_unit_convert | A | 18.27 | 51.90 | 2.93 | 54.83 | **6.25×** | 0.33× |
| a5_todo_cli | A | 18.50 | 57.77 | 2.90 | 60.66 | **6.39×** | 0.31× |
| b1_log_parse | B | 47.64 | 62.60 | 21.75 | 84.35 | **2.19×** | 0.57× |
| b2_normalize | B | 47.78 | 58.93 | 17.56 | 76.49 | **2.72×** | 0.63× |
| b3_template | B | 28.37 | 50.66 | 5.04 | 55.70 | **5.63×** | 0.51× |
| b4_wordfreq | B | 60.99 | 56.70 | 20.13 | 76.82 | **3.03×** | 0.79× |
| b5_re_extract | B | 50.44 | 67.76 | 26.47 | 94.23 | **1.91×** | 0.54× |
| c1_ledger | C | 24.28 | 74.92 | 8.07 | 82.98 | **3.01×** | 0.29× |
| c2_inventory | C | 22.45 | 76.48 | 6.40 | 82.89 | **3.51×** | 0.27× |
| c3_grades | C | 21.62 | 82.86 | 4.74 | 87.60 | **4.56×** | 0.25× |
| c4_sales | C | 23.69 | 79.35 | 7.70 | 87.04 | **3.08×** | 0.27× |
| c5_csv_clean | C | 21.90 | 80.86 | 5.86 | 86.72 | **3.74×** | 0.25× |
| d1_matrix | D | 51.07 | 62.23 | 4.46 | 66.69 | **11.46×** | 0.77× |
| d2_stats | D | 39.73 | 70.13 | 5.18 | 75.31 | **7.67×** | 0.53× |
| d3_integrate | D | 31.03 | 51.04 | 4.04 | 55.07 | **7.69×** | 0.56× |
| d4_primes | D | 55.62 | 53.77 | 7.07 | 60.84 | **7.87×** | 0.91× |
| d5_finance | D | 19.08 | 58.16 | 2.91 | 61.08 | **6.55×** | 0.31× |
| d6_rec_memo | D | 22.56 | 65.05 | 3.39 | 68.45 | **6.65×** | 0.33× |
| k1_tuple_sort | K | 17.57 | 51.13 | 2.93 | 54.06 | 6.00× | 0.33× |
| k2_float_dict | K | 17.40 | 42.46 | 2.91 | 45.37 | 5.99× | 0.38× |

### 类别中位汇总

| 类 | n | CPython run | compile | pa run | speedup(run) | speedup(c+r) | 一致 |
|---|---|---|---|---|---|---|---|
| A 快速往返 | 5 | 18.27 | 52.29 | 2.90 | **6.25×** | 0.33× | 5/5 |
| B 字符串 | 5 | 47.78 | 58.93 | 20.13 | **2.72×** | 0.57× | 5/5 |
| C csv | 5 | 22.45 | 79.35 | 6.40 | **3.51×** | 0.27× | 5/5 |
| D 数值 | 6 | 35.38 | 60.20 | 4.25 | **7.68×** | 0.55× | 6/6 |

## 4. 性能模式总结（哪类胜/负 + 热点）

**纯运行（run-only，产物复用场景）pyadamas 全胜，但幅度分类明显：**

- **D 数值算子最胜（中位 7.68×，d1 matmul 11.46×）**。标量 int/float 列表是 kind1 裸值表，
  逐元素**无 RC/GC**，生成代码就是紧浮点循环。perf（d1 n=160 加重版，单跑绑核）：
  `matmul 75.7% + pa_math_fsum 9.3% + gen_matrix 6.6% + transpose 2.2%`，**~94% 用户态计算，
  RC/GC 几乎为 0**。这是静态类型 + 直出机器码的红利区。
- **A 快速往返 run 6.25×**：脚本本身极轻，pyadamas 产物启动 ~0.5ms vs CPython 解释器启动 ~18ms，
  run 差距主要是**启动开销差**（不是计算）。
- **C csv run 3.51×**：聚合/分组是 int dict + 标量列表，RC 轻；但 csv 走 stdlib 纯 Python 源
  （reader/writer 逐字符扫描 + 大量 str 临时），拉低到 3~4.6×。
- **B 字符串最弱（中位 2.72×，b5 re 抽取仅 1.91×）**。**印证已知「str 是 tracked 叶子致 RC/GC 开销大」**：
  - b2_normalize（str 分配密集）perf：`rc_alloc_core 10.9% + rc_index_put 10.3% + pa_rc_dec_slow 5.7%
    + rc_link 3.3% + rc_unlink 3.2%` ⇒ **RC 机制 ~33%**；`str_new 5.2% + str_alloc_raw 3.4% + pa_str_method 2.6%`
    ⇒ str 分配 ~11%；`gc_collect 2.6%`。生成代码本身只占 ~27%。
  - b1_log_parse（re+str）perf：RC 族（rc_alloc_core/rc_index_put/pa_rc_dec_slow/rc_unlink/rc_index_del/pa_rc_cleanup）
    **~36%** + gc_collect 3.6% + malloc/cfree/memmove ~11%。
  - b5_re_extract（re 重）perf：生成代码 run 49% + RC ~20% + re 引擎（find_from/pa_re_findall）~7%。
  - 对照 b3_template（拼接为主、逐字符扫描少）run 5.63×——**字符串脚本里 RC/GC 占比越高，speedup 越低**。

**compile+run（一次性编译即跑场景）pyadamas 全负（0.25~0.91×）**：
编译耗时 **42~83ms**（前端 + as + ld；csv 脚本因引入 stdlib 模块偏高到 ~80ms），
而 CPython 启动仅 ~18ms 且无需编译。对这批**短日常脚本**，一次性 compile+run 比 CPython 慢
2~4×；只有 d4_primes（计算稍重）接近打平（0.91×）。**结论：pyadamas 的收益在「产物复用 / 长运行」，
不在「一次性短脚本」**；compile 吞吐是日常 CLI 场景的关键瓶颈（与主树 0.309-0.311 编译吞吐优化方向一致）。

**热点可操作方向**（取证给 lead，不在本分支修）：str 的 RC 快路径/池化、re 引擎与 str 临时减少、
dict 探测内联——这些正是 B/C 类 speedup 的天花板。

## 5. 已知 bug 覆盖（正在别处修，本分支只标注）

### K1 → 0.312：元组列表自然排序 no-op（`k1_tuple_sort.py`）
`list[tuple].sort()` / `sorted(list[tuple])` / `sorted(..., reverse=True)` **原样返回不排序**。
CPython 正确排序。绕行：`sorted(xs, key=用户函数)`（key 返回 int/float 正确）。
**本轮扩展发现**：`list[list[int]]` / `list[list[str]]` 的自然 `.sort()/sorted()` **同样 no-op**
（同族：容器元素自然序排序失效）；标量 int/float/str 列表自然排序正确。

### K2 → 0.313：float 值 dict 损坏（`k2_float_dict.py`）
`dict[str, float]`：下标读返回错值（即使字面量存储）、非字面量存储后查找丢失、`d[k] += x` 损坏下标读；
`.get(k, default)` 有时正确。CPython 全部正确。绕行：金额用 **int 分**（`dict[str, int]`）/类字段/平行列表。

## 6. 新 bug 清单（本轮发现，最小复现见 `bench/daily/_bugs/`，均已验证复现）

> 严重度：🔴 崩溃 / 🟠 静默算错 / 🟡 精度或次要。所有复现脚本在 0.311 -O2 下确定性复现。
> 根因为**疑似**（取证推断，未改源码验证）。

### 🔴 N10 — list 原地 `+=` / `*=` 段错误（`n10_list_iadd.py`）
```python
l: list[int] = [1, 2]
l += [3, 4]      # SIGSEGV (exit 139)；l *= 2 同样崩
```
check/build 均过，运行段错误。所有元素类型、字面量/变量 RHS 都崩。`extend()`/`del l[i]`/`pop` 正常。
**疑似根因**：list 的 AugAssign 降级走了未支持的 concat/repeat 路径（显式 `list + list` 是 codegen 报错，
`+=` 却漏过编译）或临时值/RC 所有权处理错。**绕行**：`extend` / append 循环 / 推导式重赋值。

### 🔴 N16 — `dict.setdefault(k, []).append(v)` RC 双重释放（`n16_setdefault_list.py`）
```python
d: dict[str, list[int]] = {}
d.setdefault("k", []).append(1)   # "refcount decrement on a freed object" (exit 1)
```
链式调用运行中崩；把结果绑到变量再 append 则退出时崩。int 默认值正常。
**疑似根因**：key 已存在时新建的默认 `[]` 临时被过度释放（返回的既有 list 引用未正确 +1）。
**绕行**：`if k not in d: d[k] = []` 再 `d[k].append(v)`；或 `defaultdict(list)`（两者已验证正常）。

### 🔴 N17 — 空 `set[str]` 不认标注元素类型：动态字符串不去重 + RC 崩（`n17_set_str_empty.py`）
```python
s: set[str] = set()
a: str = "x" + "y"; b: str = "x" + "y"   # 不同对象、同内容
s.add(a); s.add(b)
print(len(s))    # CPython 1；pyadamas 2 + 退出时 RC 崩 (exit 1)
```
**驻留字面量因同指针巧合去重**，掩盖了 bug；动态字符串（re 结果/拼接/str()）不去重。
`set(list)` 与集合推导式**正常**（构造时带元素类型）；`dict[str, int]` 空标注**正常**。
**疑似根因**：空 `set()` 降级给运行期表传 unknown key-kind，未像 dict 标注路径那样传播 str 类型。
**绕行**：用 `dict[str, int]` 做去重映射（取 `sorted(d.keys())`），或 `set(list)`/集合推导式。

### 🔴 N18 — `sorted(key=返回str的函数)` 规模化 RC 过度释放（`n18_str_key_sort.py`）
```python
rows: list[str] = [...2000 个动态串...]
def idk(s: str) -> str: return s
ordered = sorted(rows, key=idk)   # 输出正确，但退出 RC 崩 (exit 1)
```
key 返回 **str（堆值）** 时规模化触发；返回 int/float 或自然 `sorted(xs)` 正常；小规模可能不触发。
**疑似根因**：key 装饰（Schwartzian transform）临时对堆键的 RC retain/release 错。
**绕行**：int/float key、int 复合键、或 str 自然排序。

### 🟠 N19a — 函数内「浮点累加器 + 嵌套调用」循环静默算错（`n19_float_loop_call.py`）
```python
def det4(m):           # 4×4 余子式展开
    s = 0.0
    for j in range(4):
        sub = minor(m, 4, 0, j)      # 返回 list[float] 的调用
        s += sign * m[j] * det3(sub) # 返回 float 的调用
    return s
# det4(单位阵) = 0.0（应 1.0）；det4(diag(1,2,3,4)) = 0.0（应 24.0）
```
**模块级同样循环正确**；整数余子式正确；孤立小用例正确（**Heisenbug，随全程序寄存器压力变化**）。
**疑似根因**：浮点槽/寄存器提升跨调用未溢出（xmm 全 caller-saved，被调用破坏），提升压力高时触发
（与 HANDOFF §5.3/§6.7 浮点寄存器门控/槽提升注意事项同族）。**绕行**：把算术**内联进循环（无调用）**——已验证同上下文正确。

### 🟠 N19b — `math.pow` 结果浮点局部量被破坏（`n19_float_pow.py`）
```python
factor = math.pow(1.0 + r, float(months))
return principal * r * factor / (factor - 1.0)
# monthly_payment(300000, 0.045, 360) = -269472.137276（应 1520.055929）
```
同 N19a 族（寄存器压力 Heisenbug）；**命名临时量拆分仍不能修**（factor 本身被破坏）。
**疑似根因**：math.pow 返回（xmm0）/浮点槽提升在后续运算中被破坏。
**绕行**：用**无调用的整数指数快速幂 ipow**（重复平方，纯浮点）——两边跑同一份纯浮点代码，输出仍逐字节一致；已验证正确。

### 🟠 N2 — 文本写文件折叠 CRLF/CR 为 LF，忽略 `newline=`（`n2_file_write_crlf.py`）
```python
with open(p, "w", newline="") as f: f.write("a\r\nb")
# 文件内容 CPython 'a\r\nb'；pyadamas 'a\nb'。newline="\r\n" 也不转换。
```
**csv.writer 默认 `lineterminator="\r\n"` 因此写出 LF**（与 CPython 不一致）。
**疑似根因**：rt_file 写路径套用了读侧通用换行折叠；`newline=` 写转换未实现。
**绕行**：`csv.writer(..., lineterminator="\n")`；写入内容避免 `\r`。

### 🟠 N3 — `random.getrandbits(k>=31)` 及 randrange/sample/choices 序列分叉（`n3_random_getrandbits.py`）
`getrandbits(8/16)` 一致；`(31/32)` 低位差（+3/+6）；`(33/64)` 结构性差（字组装序）。
`randrange/sample/choices` 下游分叉。`random()/randint/uniform/choice/shuffle` **一致**（MT19937 高位正确）。
**疑似根因**：多字 getrandbits 组装端序 + tempering 低位 bug；randrange/sample 消耗 RNG 方式不同。
**绕行**：只用 `random()/randint/uniform/choice/shuffle`（已验证同种子同序列）。

### 🟠 N4 — 格式说明符 `','`（浮点）与 `'#'` 被静默忽略（`n4_format_spec.py`）
`format(x, ",.2f")` / `f"{x:,.2f}"` → 无千分位（int 的 `','` 正常）；
`format(255,"#x")` / `f"{255:#x/#o/#b}"` → 无 `0x/0o/0b` 前缀。
**疑似根因**：浮点格式路径在有精度/类型时丢 `','`；`'#'` 交替形式两路径都未实现。
**绕行**：浮点不用 `','`；不用 `'#'`（需要前缀时手动拼）。

### 🟠 N5 — `strftime` 星期/年积日字段错 + naive 时区（`n5_strftime.py`）
2026-11-19 是周四，pa `%A`=Sunday、`%j`=001（应 323）、`%w`=0（应 4）、`%U/%W` 错；
naive datetime `%Z`=CST、`%z`=+0000（CPython 空）。`%b/%B` 与数字字段正确；`date.weekday()` 正确。
**疑似根因**：strftime 填 C tm 时未算 tm_wday/tm_yday（未调 mktime），naive 值用了本地时区。
**绕行**：`isoformat()` + `weekday()` + 手工星期名表；避开上述 directives。

### 🟡 N6 — `dict(**kwargs)` 构造得 unknown dict（`n6_dict_kwargs.py`）
`dict(x=1, y=2)` 打印 `<dict object>`（CPython `{'x': 1, 'y': 2}`）。
**疑似根因**：kwargs 构造路径未推断 `dict[str, V]` 静态类型。**绕行**：字面量 `{"x":1}` 或 `dict(pairs)`。

### 🟡 N7 — `filter(None, 异构列表)` 真值与打印错（`n7_filter_none_hetero.py`）
`filter(None, [0,1,"","a",2])` → pa `[1, , a, 2]`（保留了空串、unknown 元素无 repr 引号）；CPython `[1,'a',2]`。
**同构** `filter(None, list[str]/list[int])` 正确。**疑似根因**：filter(None) 脱糖对 unknown 用整数/len 真值路径；
unknown 元素列表打印缺 repr。**绕行**： typed 列表上 `[x for x in xs if x]` 或带类型谓词的 filter。

### 🟡 N9 — `sum(float)` / `statistics.mean` 精度算法差（`n9_sum_float_precision.py`）
pa `sum([0.1]*10)=0.9999999999999999`、`sum([1e16,1,-1e16])=0.0`；CPython 3.12+ 用 Neumaier 补偿求和得 1.0/1.0。
`statistics.mean([0.1,0.2,0.3])` pa 0.20000000000000004 vs CPython 0.2（精确 Fraction）。
**性质**：算法/版本差异（非 codegen bug），但破坏逐字节对拍。**绕行**：浮点求和/均值用 `math.fsum`（正确舍入，两边一致）。

## 7. 文档化语义差 / 次要差异（非 bug，语料已规避，记录备查）

- **int64 溢出回绕**（vs 任意精度）、`1<<70==64`、`2**-1==0`：数据生成刻意避开。
- **print 求值时机**：`print("x:", f())` pa 边算边写（先写 "x:" 再调 f），CPython 先算完参数；语料不把副作用调用放进 print 参数。
- **None==0**、`d.get(k)` 缺失与值 0 不可分；assert 无消息 pa "assertion failed" vs CPython ""。
- **re.findall 多捕获组**返回整段匹配（language.md 已载明）而非组元组；`m.groups()` 返回 list 非 tuple；
  `keys()/values()` 返回 list（打印 `dict_keys` 差异）；`Counter` 返回普通 dict（打印 `Counter({...})` 差异）。语料用单组 findall / 索引 group(i) / 迭代或 list() / 不直接打印这些。
- **str(KeyError("k"))** pa "k" vs CPython "'k'"（异常消息格式）；语料捕获后打印自定义标签。
- **异构/嵌套容器 repr**：`[[1,2],["a"]]` pa 内层 str 无引号（unknown 元素）；heapq 装元组按指针比较且打印成 list。语料用同构容器、int 堆、类实例。
- **str % 映射** `"%(k)s" % dict` pa 运行期 ValueError "unsupported format character"（check/build 却过）——应编译期拒或实现；语料不用。
- **json.dumps 混合数值列表** `[1, 2.5]` pa `[1.0, 2.5]`（字面量 int→float 提升，文档化语义的可见后果）；语料用同构列表。
- **tuple 作 dict 键 / tuple 排序 / tuple `<`** 按机器字（指针）语义（文档化「其他类型按机器字」）；语料不用 tuple 键/自然序。

## 8. 编译期子集缺口（check 过、build 拒；非运行期 bug，附录备查）

`list.sort(key=/reverse=)` 关键字、`str.format` 方法、`str.partition/expandtabs/rsplit/split(None,n)`、
`startswith/endswith` 元组参、`min/max key=`、`enumerate start`、`math.log` 双参、
`Random` 实例 `getrandbits/randrange/sample`、`deque[int]` 标注（致方法 unknown）、`Counter(生成器)`、
`str(list)`、星号解包 `a,*rest=`、set `| & -` 运算、`list/tuple +` 拼接、`f"{None}"`、
json 混合 bool/None 列表、`re.sub` 可调用 repl / `re.subn`、`string.capwords(s,sep)`、`itertools.zip_longest fillvalue=`。
模块级缺口：argparse/dataclasses/contextlib/io/struct/tempfile/decimal/threading 等（见 stdlib-gaps.md）。
**这些是子集边界，语料全部用已验证等价写法绕开。**

## 9. 语料中应用的绕行（与 bug 对应）

| 脚本 | 绕行 | 对应 |
|---|---|---|
| b5_re_extract | dict[str,int] 去重代替 set[str]=set() | N17 |
| c2_inventory | int 复合键单次排序代替 str-key/双重排序 | N18 |
| c3_grades/c5_csv_clean | 标量名预排序 + 单次稳定堆排序 / 标量自然序 | N18 |
| c1_ledger 等 | 金额 int 分；csv lineterminator="\n" | K2 / N2 |
| d1_matrix | det4 余子式内联（call-free 循环）| N19a |
| d5_finance | ipow 快速幂代替 math.pow | N19b |
| d2_stats | math.fsum 均值/方差代替 sum/mean | N9 |
| 全部 | extend 代替 list +=；显式 guard 代替 setdefault([])；int64 安全 LCG | N10 / N16 / 溢出 |

## 10. 复现与提交

```bash
# 对拍 + 计时（默认 9 轮 BURST=3 绑核 24）
cd /tmp/wt-daily
REPS=9 BURST=3 CORE=24 PA=build/pyadamas python3 bench/daily/_tools/run_daily.py
# 单脚本对拍
python3 bench/daily/b1_log_parse.py > ref.out
build/pyadamas run --rebuild -O2 -o /tmp/x bench/daily/b1_log_parse.py > got.out
cmp ref.out got.out
# bug 复现
for f in bench/daily/_bugs/*.py; do python3 "$f"; build/pyadamas run --rebuild -O2 -o /tmp/b "$f"; done
```

- 语料：`bench/daily/[abcdk]*.py`（23）；工具：`bench/daily/_tools/`；复现：`bench/daily/_bugs/`（13）。
- 分支 `dailycorpus`，基线 `e855662`（0.311）；本批语料+报告+工具+复现落该分支 HEAD（**未推 master**），commit hash 见完成报告。
- 计时原始数据：`bench/daily/_tools/results.json`；perf 热点：`bench/daily/_tools/perf_hotspots.txt`。

## 11. 结论

1. **正确性**：21 个干净日常语料 **100% 逐字节一致**；故意覆盖的 2 个已知 bug 如预期分叉。
   本轮新发现 **13 个 bug**（4 崩溃 🔴 N10/N16/N17/N18、6 静默算错 🟠 N19a/N19b/N2/N3/N4/N5、
   3 次要 🟡 N6/N7/N9），均最小化复现 + 疑似根因 + 绕行，落 `_bugs/`。
2. **性能**：纯运行 pyadamas 全胜——**数值 7.68×（matmul 11.46×）> 快速 CLI 6.25× > csv 3.51× > 字符串 2.72×（re 1.91×）**；
   字符串/分配密集脚本 **RC/GC 占 30~50%**（perf 实证 str tracked 叶子开销），是 speedup 天花板；
   数值标量路径 RC/GC≈0 是红利区。**compile+run 一次性场景全负（0.25~0.91×）**——短日常脚本编译开销（42~83ms）
   远超 CPython 启动（~18ms），收益在产物复用/长运行。
3. **方向**：B/C 类提速靠 str RC 快路径/池化、re 与 str 临时削减；日常 CLI 场景靠 compile 吞吐（与 0.309-0.311 一致）。
