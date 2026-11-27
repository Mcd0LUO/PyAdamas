# unknown 收敛：实测台账与真实杠杆（2026-11-27）

> 本文是 `unknown-convergence-plan.md` 的**结论版**。计划文档记录了 0.713–0.722 的
> 全部排查过程（含 5 次被证伪的假设），本文只保留**可执行的结论**。
>
> ## 1. 一句话结论
>
> **当前 unknown 的主要代价不来自「局部变量推不出来」，而来自「泛型函数 + 分别编译」。**
> 修局部变量推断（0.719–0.722 方向）收益接近零；真正值得做的是让泛型函数的形参
>在分别编译下拿到调用点的实参类型。
>
> ## 2. 实测数据（`PYADAMAS_TYPE_STATS=1`）
>
> ### 2.1 类型统计
>
>| 程序 | 统计 |
>|---|---|
>| container 基准 | `container_elem` 7 + `local_unknown` 8 + `call_*` 9 |
>| gcbench 基准 | `container_elem` 2 |
>| interp2（多模块） | `local_unknown` 21，**零 import 类、零 container_elem** |
>
> ### 2.2 未知元素对生成代码的实际影响（反汇编计数）
>
>| 程序 | unknown 元素 append | 总 append | 比例 |
>|---|---|---|---|
>| interp2 | 1 | 49 | 2% |
>| **container** | **14** | 18 | **78%** |
>| gcbench | 1 | 11 | 9% |
>| oopbench | 0 | 6 | 0% |
>
>**统计数 ≠ 代价。** interp2 的 21 处只占 1/49 的 append；container 的 7 处
>`container_elem` 却占 78% —— 因为前者大多是「if 分支内定义、分支内使用」，
>后者是泛型函数的元素类型贯穿整个函数体。
>
> ## 3. container 那 14/18 的来源（追到根）
>
>container 基准源码里没有这些名字，全部来自 `stdlib/heapq.py`：
>
>```python
>def nlargest(n: int, iterable: list, key: object = None) -> list:
>    ks: list = []
>    for x in iterable:          # iterable: 裸 list -> x 的元素类型未知
>        ks.append(key(x))       # -> ks 元素 unknown（container_elem）
>    out: list = []              # -> out 元素 unknown
>    ...
>```
>
>`nlargest` / `nsmallest` / `merge` 都是**泛型函数**：元素类型由调用方决定。
>
>## 4. 为什么现在修不了：两个架构事实
>
>### 4.1 没有类型变量
>
>`grep TypeVar src/typecheck/` 结果为零；`ret_specialized` 只服务硬编码的 `random.choice`。
>所以泛型函数没有「按调用点实例化」的表达方式。
>
>### 4.2 分别编译切断了反向传播
>
>heapq.py 是独立编译单元：它检查函数体时 `iterable` 的元素类型是洞；
>调用点（main.py）填的是**接口副本**里的洞，**改不到 heapq 已编译的函数体**。
>这不是注解缺失，也不是标记缺失，是「泛型 + 分别编译」的组合缺口。
>
>## 5. 被证伪的方向（不要再走）
>
>| 方向 | 证伪方式 | 结果 |
>|---|---|---|
>| 形参无注解是主因 | 读 lexer.py 第一行（`src: str` 有注解） | 假设不成立 |
>| while 循环 join 污染 | 最小复现 | 零 unknown |
>| str 下标/切片推断差 | 最小复现 | 零 unknown |
>| 统计是重查轮中间态 | `round-boundary` 标记实测 | 每单元只跑一轮，是真 unknown |
>| 模块级全局循环（`for k in KW`） | 最小复现 | 零 unknown |
>| 让具体类型穿过 if 合并 | 三次实现 | 套件 600/600/598，静默错值 |
>| GC 内部优化（标记位外移 / 头部快路径） | 0.710/0.711 | 归因错 / 崩 19 用例 |
>
>## 6. 唯一值得做的方向
>
>**让泛型函数在分别编译下拿到实参类型。** 最小可行形态：
>
>1. 对「形参是带洞容器」的函数，在**本模块内**的调用点把洞填上后触发 `sig_dirty` 重查
>   （机制已存在：`src/typecheck/call.cpp:229` 的 `fill_holes_recursive` + worklist）；
>2. 跨模块沿用同一套：接口里携带「形参洞是否已被调用点填充」的信息，
>   编译顺序上双方都能收敛；
>3. 验收：container 的 unknown 元素 append **14 → 个位数**；
>   套件 602 + difftest 545-0 全绿。
>
>⚠️ 改跨模块接口要 bump `kModuleAbiVersion`（否则旧 modcache 不失效）；
>⚠️ 这是推断放宽，错判会静默错值 —— difftest + 套件是唯一防线。
>
>## 7. 相关文档
>
>- 排查全过程：[unknown-convergence-plan.md](unknown-convergence-plan.md)
>- unknown 的运行期语义与缺口：[unknown-semantics-audit.md](unknown-semantics-audit.md)
>- unknown 来源总账（628 文件口径）：[frontend-gaps.md](frontend-gaps.md)
