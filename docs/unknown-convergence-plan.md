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
