# unknown 收敛计划（实测基线 + 第一批目标）

> 起点：2026-11-27（0.712 之后）。目标不是再优化 GC 内部，而是**从源头减少 unknown**，
> 让容器走精确扫描（`elem_kind` 0/1 而不是 2），顺带消掉装箱、RC 探针、运行期类型分派。

## 1. 实测基线（`PYADAMAS_TYPE_STATS=1`）

| 程序 | unknown 统计 |
|---|---|
| container（基准） | `container_elem` 7 + `local_unknown` 8 + `call_*` 9 = 24 处 |
| gcbench（基准） | `container_elem` 2（`roots`、`a`）|
| interp2（多模块真实程序） | `local_unknown` 21（3 个函数/文件），**零 import 类、零 container_elem** |

关键观察：
- interp2 是**多模块**程序，但 type-stats 里**没有任何 import 相关类别** →
  跨模块接口机制（`ModuleInfo` 序列化 `globals`/`classes`）是工作的，
  **跨模块不是当前的主要 unknown 来源**；
- 目前的主源是 `local_unknown`（局部变量）与 `container_elem`（容器元素）。

## 2. 第一批具体目标（interp2，21 个局部变量）

```
tokenize.kw, tokenize.word, tokenize.k, tokenize.spaces, tokenize.j, tokenize.sid
stmt.nd, func.pn, assign.nd, cmp.nd, cmp.right, cmp.op
eval.y, eval.op, eval.x, eval.k, exec.r, exec.k
```

## 3. 根因假设（待验证）

以 `lexer.py` 的 `tokenize` 为例：

```python
def tokenize(src):          # src 没有注解 → 形参 unknown
    ...
    c = src[i]              # 从 unknown 派生 → unknown
    word = src[i:j]         # unknown
    for k in KW:            # KW 若是未知容器 → k unknown
    sid = sym.intern(word)  # intern 返回取决于 word → unknown
```

**所有 unknown 都溯源到形参 `src` 没有类型**。也就是：「局部变量 unknown」其实是
「**形参类型洞没有被回填**」的下游表现。

仓库里已经有这套机制（`src/typecheck/func.cpp:272`）：

> // **用函数体的实际用法回填参数标注里的类型洞。**

所以第一批工作不是造新的推断，而是**扩展这个回填机制**：
把「形参在下游被用作 X」的信息（下标/切片/属性/传给已知函数）收集得更全，
再据此确定形参类型。

## 4. 执行顺序

1. **读 `func.cpp` 的回填实现**，搞清它现在覆盖哪些用法、哪些漏了（下标、切片、
   `for x in param`、传给已知签名的函数）；
2. 先只做「形参被下标/切片」这一种（`src[i]`、`src[i:j]`），看 interp2 的 21 处降多少；
3. 用 `PYADAMAS_TYPE_STATS=1` 前后对比作为唯一验收口径（不看墙钟、不看猜测）；
4. 每步跑闸门（套件 602 / difftest 545-0），因为放宽推断会改变降级与生成代码。

## 5. 风险

- 回填是**推断放宽**，误判会把 unknown 变成错类型 → 静默错值。必须有 difftest + 套件兜底；
- 跨模块接口要遵守 `kModuleAbiVersion`：接口编码变化必须 bump，否则旧 modcache 不失效；
- 判负要记录（项目规矩）：放宽后若 pass 数下降，回退并写清理由。

## 6. 读了回填实现之后：它覆盖什么、缺什么

现有机制在 src/typecheck/func.cpp:296：

    for (auto& p : info->pos) {
        if (p.inferred) continue;   // 没标注的本来就是 unknown，不用回填
        ...
        fill_holes_recursive(p.ty, it->second.ty, 0);
    }

它只处理「标注了裸 list/dict 但元素类型是洞」的情况（p.inferred == false）。
对 def tokenize(src) 这种完全不标注的形参，它显式跳过（p.inferred 为真）。

注释里说「不标注时反而能推出精确类型」，靠的是函数体用法（for x in xs 把元素并进 xs）。
但 src[i] / src[i:j] 这类下标/切片用法不会反推 base 的类型，
所以 src 停在 unknown，派生出的 c/word/sid 全部 unknown。这就是 interp2 那 21 处的来源。

## 7. 真正的修法：调用点参数类型传播

两条路：

A 函数体内反推：从 src[i]、src[i:j]、len(src)、元素与 str 常量比较等用法反推 base 类型。
  规则多、易误判；单文件内可做。

B 调用点传播：记录「形参 unknown 的函数」及其调用点；参数类型已知时回填形参
  （必要时迭代到不动点）。需要跨函数甚至跨模块的一轮；这是 mypy/pyright 的做法。

选 B 的理由：
1. 不依赖猜规则，用的是真实的调用事实；
2. 跨模块也能用 —— 接口里已经带着函数签名，调用方的实参类型能反推形参；
3. 只在形参类型是 unknown 时才回填，不改变已有语义。

实施切片（最小闭环）：
1. 建「待回填形参表」：函数 F 的第 k 个形参类型 unknown 且未被用法收窄；
2. 在调用点（call_forms.cpp 已知签名的调用）把实参类型并进该形参；
3. 一轮结束后若有变化，重查这些函数体（不动点，最多 N 轮）；
4. 验收：PYADAMAS_TYPE_STATS=1 看 interp2 的 local_unknown 从 21 降到多少。

风险：这是推断放宽，错判会把 unknown 变错类型，导致静默错值。
防线：套件 602 + difftest 545-0 必须全绿，任何 pass 数下降立即回退并记录。

## 8. 实现接线点（已定位到行号）

不动点机制已经存在：TypeChecker::check(Module&)（src/typecheck/infer.cpp:42）
先 check_once，然后 while (collect_result_hints()) 重建 TypeChecker 再查一轮。
这正是调用结果类型传播的收敛循环；参数传播只需挂到同一循环。

三个接线点：

1. 形参进作用域：src/typecheck/func.cpp:215/217
   cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, !p.inferred, false};
   无标注形参在这里拿到 t_unknown()；回填时把 p.ty 换成 hint，下一轮生效。

2. 调用点观察：src/typecheck/call.cpp:415
   return observe_call_result(c, fn, ...);
   这里已有 Call c 和 FuncInfoPtr fn，把实参类型记进 observed_params_ 即可。

3. 收敛循环：src/typecheck/infer.cpp:45
   while (collect_result_hints())
   改成 while (collect_result_hints() | collect_param_hints())（用 | 不要短路），
   并把 param_hints_ 随 result_hints_ 一起 move 到 next。

## 9. 最小实现规格

- TypeChecker 新增 param_hints_ / observed_params_，
  键为 (const FuncInfo*, size_t)，值为 TypePtr。
- observe_call_result 里追加：遍历 c.args，若 fn->pos[i].inferred 且 is_unknown(fn->pos[i].ty)，
  记录实参类型（用现成的表达式类型查询，与 ty_of 同源）；只记 replayable 类型。
- collect_param_hints 取 observed_params_，与旧值比较，变化即 changed；
  写入前用 replayable() 过滤（类/结构体跨轮不安全，与 result hint 同款限制）。
- func.cpp 声明形参前查 param_hints_，命中则用它代替 t_unknown()。
- 守卫：只在 p.inferred（无标注）且当前为 unknown 时写；有标注的绝不动。

## 10. 验收

基线：local_unknown 21（tokenize 6、parser 7、ev 6）。
目标：先降到 15 以下；同时确认 container_elem 不上升（上升说明类型变宽，是反向效果）。
回归：套件 602、difftest 545-0 全绿；任何 pass 数下降立即回退。

## 11. 更正：interp2 的 unknown 不是「形参无注解」

读源码发现 §3 的根因假设是错的：

    def tokenize(src: str, sym: Sym) -> list[Tok]:   # src 有注解！
        out: list[Tok] = []
        i = 0            # int
        n = len(src)
        while i < n:
            if at_start:
                spaces = 0      # int，却报 unknown
                j = i           # int，却报 unknown
                kw = False      # bool，却报 unknown

也就是说：**21 处里至少 lexer 这 6 处是简单标量局部变量**（int/bool），
既不是形参无注解，也不是 str 下标派生。§3 的假设不成立，§6-§10 的方案不针对这个现象。

## 12. 下一步的排查方向（按可能性）

1. **while 循环体里的变量 join**：`spaces`/`j`/`kw` 都在 `while i < n:` 的循环体内赋值，
   若循环回边的 join 把 int 与 unknown 合并 → unknown。可先写一个最小复现：

       def f(n: int) -> int:
           i = 0
           while i < n:
               j = i
               j = j + 1
               i = i + 1
           return i

   用 PYADAMAS_TYPE_STATS 看 j 是否报 local_unknown。

2. **重查轮（sig_dirty / result-hint）后作用域被重建**：stats 在 funcdef_impl 末尾统计，
   若某一轮重查时 `cur_->vars` 是新的、类型还没填回，统计就会看到 unknown。
   验证：在 funcdef_impl 统计处打印 `f.name` 与实际轮次。

3. **`src[j]`/`len(src)` 的返回类型**：如果 str 下标返回 unknown，会污染 `c`/`word`，
   但不会解释 `spaces = 0`。优先级低于 1、2。

排查顺序：先 1（最小复现 5 分钟），能复现就修循环 join；不能复现再查 2。

## 13. 最小复现结果：两条假设都被排除

用 PYADAMAS_TYPE_STATS 跑两个最小复现（build 路径）：

复现 A（while 循环体内赋值 int/bool）：

    def f(n: int) -> int:
        i = 0
        while i < n:
            j = i; j = j + 1; k = True; k = False; i = i + 1
        return i

结果：**零 type-stats 输出**（没有 local_unknown）→ 循环回边的 join 是正常的。

复现 B（str 下标/切片/与 str 比较）：

    def g(src: str) -> int:
        while i < n:
            c = src[i]; j = i + 1; w = src[i:j]
            if c == "x": total += 1

结果：**零 type-stats**，且程序输出正确（1）→ str 下标/切片类型推断是正常的。

## 14. 于是 interp2 那 21 处的可能解释（待定）

1. **`KW` 这类模块级全局未知**：lexer.py 里 `for k in KW` 的 `KW` 若推不出来，
   `k` 就 unknown；相邻变量可能被连带（word/sid 依赖 src 的反而不该 unknown）。
2. **统计是「重查轮」的中间态**：`funcdef_impl` 会被 sig_dirty 工作列多次调用，
   若 stats 每轮都打印，第一轮的 unknown 会被计入，而最终类型其实已经收敛。
   → 这会**让「21 处 unknown」这个数字本身失真**。

**必须先分辨这两者再动手**：如果是 2，那当前没有可优化的未知；
如果是 1，则修法是给模块级全局补类型（不是形参传播）。

分辨办法（下一轮第一件事）：
- 在 func.cpp 的统计处加上「第几轮」和「是否最后一轮」的标记（临时打印）；
- 或直接对比：把 interp2 编出的 asm 里 lexer 的 append 调用检查一下 heap 参数，
  heap=2（unknown 元素）才说明真的退化了。

## 15. 方法教训

§3 的「形参无注解」和 §6-§10 的参数传播方案，都是**没看源码就下的结论**；
看了 lexer.py 第一行（src: str）就推翻了。
**这条路今晚重复了三次（0.706 原子操作、0.709 false sharing、0.711 pad 下标）。**
规矩应该是：先看目标源码 → 用最小复现证明成因 → 再设计改动。

## 16. 分辨结果：不是统计假象，是真 unknown

在 infer.cpp 的 check() 里加了一行 `[type-stats] round-boundary` 标记（仅 TYPE_STATS 开启时打印），
重跑 interp2：

    [type-stats] cat=local_unknown count=8 examples=tokenize.kw,...   <- lexer 单元
    [type-stats] round-boundary
    [type-stats] cat=local_unknown count=7 examples=stmt.nd,...       <- parser 单元
    [type-stats] round-boundary
    [type-stats] cat=local_unknown count=6 examples=eval.y,...       <- ev 单元
    [type-stats] round-boundary
    [type-stats] round-boundary                                      <- 主单元/收尾

**每个单元只出现一次统计、且紧跟一个边界** → 三个文件各自**只跑了一轮**，没有重查轮。
所以 21 处是**最终态的真 unknown**，不是中间态假象。（假设 b 也被排除。）

## 17. 下一步：用 bisect 定位 lexer 的 6 处

最小复现 A/B 都推断正常，但真实 lexer.py 的 6 个局部变量确实 unknown，说明**差异在别处**。
做法（下轮第一件事，20 分钟内可出结果）：

1. 复制 lexer.py 到 /tmp，删掉 `class Sym` 之外的部分，只留 `tokenize` + 依赖，
   用 `PYADAMAS_TYPE_STATS` 看 stats 是否仍在；
2. 若仍在，逐段删减 `tokenize`（二分：先删 while 前半、再删后半），直到 stats 消失，
   最后剩下的那段就是触发器；
3. 常见嫌疑：
   - `KW: list[str] = [...]` 这种**模块级带注解的全局**在 `for k in KW` 里是否被识别；
   - `SP = chr(32)` 的 `chr` 返回类型未建模 → `SP` unknown → 但按理不该污染 `kw`；
   - `Tok(T_KW if kw else T_NAME, sid)` 里条件表达式与类构造的交互；
   - `indents[len(indents)-1]` 这种**用 len() 作下标**的表达式。

**注意**：本轮之后不要再猜根因。bisect 出具体那一段之前，不写任何修复代码。
