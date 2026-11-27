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
