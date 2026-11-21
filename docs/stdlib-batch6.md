# 标准库自举第六批（stdlib-batch6）：statistics

> 分支 **stdlib6**，基线 master f70d7e8（0.351）。这一批是**纯 PyAdamas
> 源码**：重写 stdlib/statistics.py（模块表原本就登记过，**不改 src /
> runtime / codegen，无 C 胶水、无 BigInt**），因此编译器二进制与基线
> 同源。轻量标尺：同参数同功能同结果；错误类型对、消息合理即可，不逐字
> 复刻 CPython。CHANGELOG 预留 **0.352**。

## 一、可行性复评

| 依赖面 | 结论 |
|---|---|
| 求和精度 | `math.fsum` 可用（正确舍入的 double 和），但**没有 BigInt/Fraction**；CPython statistics 用精确有理数中间量，本模块只能用 float 近似（边界见三） |
| 数学函数 | math.sqrt/log/exp/pow/isnan/isinf/isfinite 齐备（builtin_sigs 确认） |
| 容器/排序 | list / sorted / len / range / 列表推导可用 |
| 众数分组 | 没有可用的 Counter 泛型面；用 O(n²) 的 list 分组（保留首现顺序），数值口径正确 |
| namedtuple | 无；`linear_regression` 返回轻量类 `LinearRegression`（字段 + 同 CPython repr），不支持元组解包（pa 用户类 `__iter__` 解包会段错误） |
| 任意可哈希数据 | mode/multimode 只做 `list[int]`（`list[object]` 对 float 元素的运行期表示有已知缺陷，会把 float 打成 int 位模式） |

结论：CPython 3.14 statistics 的**数值核心全部可纯 pa 实现**；`NormalDist` /
`kde` / `kde_random`（要 random 分布与开方精度辅助）不在本批。

## 二、交付清单

| 文件 | 内容 |
|---|---|
| stdlib/statistics.py | StatisticsError(ValueError)；LinearRegression；mean/fmean/median/median_low/median_high/median_grouped/mode/multimode；pvariance/variance/pstdev/stdev；geometric_mean/harmonic_mean；quantiles；covariance/correlation/linear_regression |
| tests/run/380 | 集中趋势：mean/fmean（含 fsum 敏感大数抵消）、median 家族、median_grouped、空数据错误 |
| tests/run/381 | mode/multimode：并列首现、首现序、空数据/单点 |
| tests/run/382 | 离散度：总体/样本自由度、mu/xbar 显式、单点/空错误、大数抵消 |
| tests/run/383 | geometric/harmonic/quantiles：0/负数/空、单点展开、n<1、未知 method、inclusive/exclusive |
| tests/run/384 | covariance/correlation/linear_regression：Pearson/Spearman、proportional、错误面 |
| tests/run/385 | 可选均值哨兵 0.0、inf/nan、非整数组距、quantiles 两侧插值 |
| tests/run/171 | 旧 quantiles 用例更新为 CPython 3.14 口径（单点展开、未知 method 抛 ValueError） |

## 三、语义边界（轻量标尺）

### 集中趋势
- mean/fmean 空数据 `StatisticsError`；median 家族空数据 `StatisticsError`。
- mean 对 +inf/-inf/nan 按 CPython：nan 传染、单符号 inf 返回该 inf、双符号
  返回 nan；fmean 走 fsum 口径，双符号 **ValueError("-inf + inf in fsum")**。
- median_grouped 与 CPython 同公式（L=x-interval/2，cf=小于 x 的个数，
  f=等于 x 的个数），double 运算序相同；单点返回 x。
- mode 并列时返回**最先出现**的值（CPython 3.14 语义；3.8–3.10 的
  “no unique mode” 抛错口径已废弃）；multimode 是全部众数、按首次出现顺序，
  空数据 `[]`。
- mode/multimode 只接受 `list[int]`（见一）。

### 离散度
- pvariance/variance 的 mu/xbar 用 `object=None` 哨兵：**省略**时自动求均值，
  显式传 0.0 时按给定均值（tests/run/385 钉住），避免把 0.0 误当缺省。
- 单点 pvariance/pstdev 返回 0.0；variance/stdev 单点 `StatisticsError`
  （消息合理即可，与 CPython 同类型）。
- 含 nan/inf 时按 CPython `_ss` 特殊值分支：pvariance=非有限值之和/n
  （`[inf,1]`→inf、`[-inf,1]`→-inf、`[inf,-inf]`→nan）；pstdev/stdev
  对非有限 mss 抛 `ValueError("inf or nan encountered in data")`。
  （注：pa 目前对“try 块里把 inf/nan 存进局部再打印”有独立的异常帧 float
  缺陷，与本模块无关；tests 不在 try 里做这种取值。）

### 分位数与二元关系
- quantiles 对齐 **CPython 3.14**：数据 1 个点返回 n-1 个该值（旧版抛错）；
  exclusive 的 `j` 钳制到 1..ld-1；空数据 `StatisticsError("must have at
  least one data point")`；n<1 `StatisticsError`；未知 method 抛
  **ValueError**（不是 StatisticsError）。
- covariance/correlation 长度不一致/点太少 `StatisticsError`；相关常量输入
  `StatisticsError`；correlation 支持 `method="linear"/"ranked"`（ranked 用
  平均并列秩，start=-(n-1)/2），未知 method `ValueError`。
- linear_regression 支持 `proportional=True`（过原点，intercept=0.0）；x 常量
  `StatisticsError("x is constant")`；返回 LinearRegression 类（字段 + repr，
  不是 namedtuple，不能解包）。

### 浮点精度边界（无 BigInt 的硬边界，tests 已钉住）
CPython 用精确有理数中间量，最后一次舍入；本模块统一 float + fsum。已知差异
（tests/run/380–385 逐行 CPython 3.14 对拍，仅这三行不同）：

| 输入 | 本模块 | CPython 3.14 | 原因 |
|---|---|---|---|
| `mean([0.1, 0.2, 0.3])` | 0.19999999999999998 | 0.2 | fsum 先把和舍入成 0.6 再除（double rounding） |
| `pvariance([1e50, 1, -1e50])` | 6.666666666666668e+99 | 6.666666666666667e+99 | 均值先舍入的二阶误差（差 1 ulp） |
| `harmonic_mean([40, 60])` | 47.99999999999999 | 48.0 | CPython 对 1/x 做精确有理数求和，本模块对已舍入的 1/x 做 fsum |

其余覆盖行（普通量级、10 以内有限小数、`variance([1e50,1,-1e50])`、
`covariance/correlation` 大数、quantiles、geometric_mean、median_grouped）
与 CPython **逐位一致**。

类型面差异（功能等价、显示口径不同）：mean/median 对 int 数据恒返回 float
（CPython 保留 int）；单点 pvariance 的 0.0 vs int 0；harmonic_mean 含 0 时
0.0 vs int 0；这些在模块头与 tests 里都有注明。

## 四、CPython 3.14 对拍

每个用例的期望输出都由 CPython 3.14.4 跑同一份源码生成，再与 pa 输出逐行
diff；380/381/382/383/384/385 中只有上表三行不同，其余零差异。171 因
CPython 3.14 quantiles 语义变化同步更新（单点展开 + ValueError）。

## 五、闸门

- 全套件 normal **360 ran ok / EXIT=0**；ASan（`--asan` 生成代码 + rt_asan
  运行期插桩，JOBS=1）**360 ran ok / EXIT=0**。
- 既有程序 asm 对基线 f70d7e8 同源构建：342 个 tests/run 中 336 逐字节全同；
  仅 105（pvariance 新增可选 mu 形参，调用点多传 None）因被内联模块签名
  本身变化；新用例 380/381/382/384/385 在基线缺符号按预期编译失败。
- check_size.sh 通过（无新增超限；本批不新增 src 文件）。

## 六、剩余/负面

- `NormalDist`（含 samples/pdf/cdf/inv_cdf/quantiles/overlap）、`kde`、
  `kde_random`：不做——需要 random 正态采样与更精确的逆 CDF 数值面，
  超出本批 tractable 核心。
- `fmean(weights=...)`、`harmonic_mean(weights=...)`：不做（pa 没有
  sumprod 的浮点泛型面；传 weights 是 TypeError）。
- 任意可哈希数据的 mode/multimode、Decimal/Fraction 输入：不做（表示层
  限制见一）。
- `linear_regression` 返回的不是 namedtuple：不支持 `slope, intercept = ...`
  解包/索引。
