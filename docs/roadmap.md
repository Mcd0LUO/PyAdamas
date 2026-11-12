# 路线图

按「收益 / 风险」排序，不是按「看起来酷」排序。每项都写了验收标准——没有验收标准的需求不算需求。

## 已完成

| 阶段 | 内容 |
|---|---|
| 词法 / 语法 | 缩进敏感的词法器、递归下降解析器、PEP 701 f-string、全语法 dump |
| 类型检查 | 标注 + 推断 + `unknown` 放行、Optional 收窄、分支合并、内置函数/方法签名 |
| 增量构建 | 拆 TU + `-MMD`，改一个文件不再全量重编 |
| IR | 基本块 + 三地址码 + vreg，类型挂在指令上 |
| 后端 | IR → x86-64 汇编，System V 调用约定，栈帧与槽位复用 |
| 寄存器分配 | 热变量提升：整型/指针用 callee-saved，浮点用 `xmm8`–`xmm15`（仅无调用函数）；块内别名、结果融合、常量延迟、比较连分支 |
| 对象模型 | `class` / 方法 / 字段 / 多继承（C3 MRO）/ vtable 虚调用；属性访问是常量偏移 |
| 内存管理 | 引用计数：隐藏头、临时值栈、字段/列表析构、无 RC 代码的脚手架消除 |
| 闭包 | cell 捕获、`nonlocal`、多层嵌套、自递归、方法捕获 self；函数值是一等闭包对象 |
| 匿名函数 | `lambda` 脱糖成合成嵌套 `def`，复用闭包/cell；函数值默认参数可用 |
| 循环收集 | 保守标记-清扫：堆注册表、全局/临时栈/真实栈/寄存器根、存活堆自适应阈值（下限 2MB）、退出清空 |
| 字典 / 集合 | 开放寻址哈希表 + 插入顺序；字面量、下标读写、`len`、`in`、`for`；RC/GC |
| 容器方法与切片 | dict get/keys/values/items/update、set add/update；str/list/tuple 读切片（缺省/负数/步长） |
| 构造补齐 | 删除类方法、切片赋值、`*args`/`**kwargs`、推导式、`with`、函数装饰器 |
| 异常 | setjmp/longjmp handler 栈；try/except/else/finally、raise、with 异常路径；运行时错误可捕获 |
| 运行时 | 字符串、列表、字典、集合、对象分配、输出、panic；浮点 repr 与 CPython 一致 |
| 验证 | 四类用例（正例 / 反例 / 运行 / 未建模） + 差分测试 + 标准库基线 |
| 0.6–0.38 | 对象模型、异常值、match、C3 MRO、优化遍/O2、块内寄存器；逐版本记录见 [archive/roadmap-0.6-0.38.md](archive/roadmap-0.6-0.38.md) |
| 0.39–0.80 | -O0/O1/O2、dunder 第二批、random/os/subprocess、差分 runner 自举、stdlib 批次（string/time/os/itertools/collections/json）、re 模块 v1/v2；详见 [CHANGELOG](../CHANGELOG.md) |
| 0.96–0.97 | `collections.defaultdict`；`bytes` 收尾：`for c in b"..."`、printf 风格 `%`、`bytes` 标注与跨模块接口 |
| 0.98 | `functools.partial` / `lru_cache`：编译期合成转发闭包与缓存包装 |
| 0.99 | `csv`：纯 py23 stdlib（reader/writer、QUOTE_*）；`file` 标注与模块接口编码修复 |
| 0.100 | 短字符串 freelist、`split` 去泄漏与预分配（`string.capwords` 反超 CPython） |
| 许可证 | Apache-2.0（见 [LICENSE](../LICENSE) / [NOTICE](../NOTICE)） |

## 当前状态（0.100）

流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通。

- **语言面**：标量、字符串、`bytes`（含迭代与 `%` 格式化）、列表/元组/字典/集合、切片读写、闭包/lambda/`*args`/`**kwargs`/装饰器、推导式、`with`、异常值（`args`/`raise ... from`/自定义异常）、类（字段/方法/C3 多继承/零参 `super()`/`@staticmethod`/`@classmethod`/`@property`/类属性）、`match/case`、`del`、复数字面量、f-string 格式说明、dunder 协议、`open`/`input`/`sys.stdin`。
- **标准库**：`math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`/`csv`。
- **验证**：91 个运行用例、37 个反例、9 个模块目录；差分 runner 已自举，默认 810 例。
- **主要缺口**：C FFI/BLAS；跨模块生成器/协程；asyncio 边角。

## 下一步

语言与标准库的完备性优先，科学计算放后面。

### 1. 语言面收尾

- **自定义方法装饰器**：现在只认 `@staticmethod`/`@classmethod`/`@property`，其它装饰器的方法体不检查实参。

### 2. C FFI + buffer + BLAS

科学计算的唯一捷径。py23 的 ABI 已经是 SysV + `int64`/`double`/指针，运行时是纯 C；补一层 `extern "C"` 声明和 buffer 类型，就能把 `list[float]` 的连续数据直接传给 `cblas_dgemm`。验收：512³ matmul 进入毫秒级；不要求自己实现 BLAS。

### 3. 工程收尾

- 重跑标准库前端基线：0.81 已在 Debian python3.13.5 上重跑过一次（见 [compatibility.md](compatibility.md#前端鲁棒性基线)），但原口径（CPython 3.11.1 + site-packages，38582 个文件）还没重跑；
- 保持 `docs/` 与 README 的验证表同步；
- 把临时性能基准固化成 `tests/bench/`（可选）。

## 待决策

这些不是「做不做」，是「选哪条路」，需要先定下来再动手：

| 问题 | 选项 | 影响 |
|---|---|---|
| 字符串语义 | 字节串（现状）/ UTF-8 码点 | 改码点则 `len` 变成 O(n)，索引要解码；不改则非 ASCII 的 `len` 与 CPython 不一致 |
| 整数溢出 | 回绕（现状）/ 溢出检查 / 升格 bignum | 检查要每条算术加一条 `jo`；bignum 要一整套运行时 |
| `print` 求值顺序 | 边算边写（现状）/ 对齐 CPython | 只有副作用可见时才看得出来 |
| 值表示 | 裸 i64（现状）/ 全量 tag / NaN-boxing / 只在 unknown 加 shadow tag | 有 tag 才能对 `unknown` 做运行时分派；全量 tag 值翻倍、寄存器压力减半，NaN-boxing 会废掉 AVX2 归约 |

## 技术债

按「什么时候会疼」排序：

1. **GC 是保守且 stop-the-world**：栈/寄存器里的整数可能被误认为根，回收时机不确定；没有分代/增量/栈映射
2. **unknown 引用的计数不精确**：unknown 容器里指向仍有其它根的对象的引用不做 rc 递减，要等下一轮不可达才回收
3. **Optional 的收窄能力不足**：`x: Optional[str]` 在 `if x is None: ...` 之后、跨语句块或跨函数调用时仍按 Optional 处理；修它需要正经的数据流分析
4. **优化遍仍不完整**：有常量折叠、DCE、小函数内联、LICM、下标版本化/免检查；没有 CSE、强度削减、循环旋转和通用向量化
5. **`tuple` 与 `list` 表示相同**：tuple 也是可变的动态数组，语义上不严谨
6. **诊断不带列区间**：只指向起点，长表达式不好定位
7. **没有增量重编 Python 源码**：每次 `build` 都是全量编译，只有 import 模块有缓存
8. **浮点变量只在无调用函数里提升**：`xmm8–15` 用于无调用函数的跨块浮点值；含调用/handler 的函数仍走内存槽，跨调用要 save/restore
9. **dict/set 删除是 O(n)**：没有墓碑，每次删除按 `order` 整表重建；大表高频删除会退化
10. **异常路径的 RC 不精确**：longjmp 跳过普通函数帧时局部变量不执行 `release_slots`，要等 GC
11. **异常对象仍缺 traceback**：有异常值、`args`、自定义异常类和 `raise ... from`，但 `args` 只保存消息
12. **含 handler 的函数没有寄存器提升**：longjmp 会回滚 callee-saved 寄存器，只能全走内存槽
13. **生成器/协程的表达式能力不全**：`yield`/`yield from`/`await` 只能作为语句或赋值右值，不能位于 `finally`/`except` 体内；`send`/await 跨挂起值的静态类型不精确；`yield from` 的值固定为 None、不能 throw。`asend`/`athrow`/`aclose` 已支持
14. **生成器/协程异常逃出时槽位不精确释放**：异常从 resume longjmp 出去时只把帧标成 aborted，堆槽引用交给 GC 下一轮回收
15. **跨模块生成器/协程不支持**：`.py23i` 里 `Generator[T]` / `Coroutine[T]` 降级成 unknown
16. **asyncio 仍是最小集合**：自定义 `__aiter__`、fd 读写、asyncio 子进程、线程安全循环未支持；`wait_readable` 不是 CPython 标准 API；Future 回调同步执行；`gather` 结果按 unknown 容器处理；Task 只允许一个并发等待者；`await` 位置受限
17. **stdlib 覆盖不足**：没有 `logging`/`argparse`；`csv` 已接上（reader/writer 子集），`functools` 已覆盖 `reduce`/`wraps`/`partial`/`lru_cache`（`lru_cache` 无淘汰、键类型受限），`json`/`collections`/`itertools` 是子集；`re` 有后向引用/四向断言/原子组/内联标志，但缺 Unicode 属性类和 `findall` 的分组语义
18. **没有 C FFI / 包管理 / 调试信息**：第三方库接入和科学计算受限；出错没有 traceback
19. **`re` 是纯回溯 VM**：递归实现，深模式或灾难性模式会吃栈，只靠 2 亿步上限兜底；没有 DFA/JIT，`findall`/`split` 每次重新编译模式字符串
20. **形参类型推断只做单态**：未标注形参由调用点回填，同一形参吃多种类型时退回 unknown（`print` 会按整数输出）；嵌套函数不参与重查。根治要运行时类型标签，代价是全量 tag 或 NaN-boxing（见「待决策」）
