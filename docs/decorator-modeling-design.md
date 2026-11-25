# 装饰器建模：三种合成包装类型（待做）

> ## 0. 结论更新（0.517 后实测）：**这个设计大部分是空的**
>
> 在动手实现前先量了三个装饰器的**运行期存在性**，结果推翻了设计前提：
>
> | 装饰器 | 实测 |
> |---|---|
> | `contextlib.contextmanager` | **可用**（`with tag() as v:` 输出 7），类型层是 unknown，但 `with` 已经能工作 |
> | `functools.singledispatch` | **不存在**——`check` 直接报 `module 'functools' has no attribute 'singledispatch'` |
> | `types.coroutine` | **不存在**——`import types` 报 `cannot find module 'types'` |
>
> 也就是说：0.500 判负「装饰器保留签名」时那 4 个文件里的 `@singledispatch` / `@types.coroutine` 用法，
>**在我们的 stdlib 里根本没有对应实现**；建模它们等于先实现这两个模块/属性（新功能），而不是「补类型」。
>剩下唯一可建模的是 `contextmanager`，而它**运行期已经能用**（unknown 类型不影响 `with`），建模只提升类型保真度、不改基线数字。
>
> **结论**：装饰器建模从「待做」改为「**暂不适用**」；若将来实现 `functools.singledispatch` 或 `types` 模块，再回来做第 1–3 步。
> 0.500 的判负（未知装饰器不保留原签名）**维持不变**。

> 状态：**未实现**。0.500 的「装饰器保留原签名」判负是因为这三个装饰器**真的改变返回类型**；
> 要安全地让未知装饰器保留签名，必须先给这三个建模。相关：[frontend-gaps.md](frontend-gaps.md) §3.1、§8.3。

## 1. 为什么必须先建模

`@contextmanager`、`@functools.singledispatch`、`@types.coroutine` 分别把函数变成：

| 装饰器 | 返回类型 | 需要的成员 |
|---|---|---|
| `contextlib.contextmanager` | `_GeneratorContextManager` | `__enter__`（返回生成器的 yield 类型）、`__exit__` |
| `functools.singledispatch` | 包装对象 | `register`、`__call__`、`dispatch` |
| `types.coroutine` | awaitable | `__await__`（或等价） |

如果对未知装饰器一律保留原签名，这三个会被**按原签名检查**——0.500 实测 628 语料里 4 个文件因此被拒：
`asyncio/tasks.py`（`'generator[None]' object is not awaitable`）、`importlib/resources/_common.py` 与 `pkgutil.py`（`@singledispatch` 的 `.register`）、
`test/support/import_helper.py`（`'generator' object has no method '__enter__'`）。

## 2. 实现路径（按可行性排序）

### 方案 A：类型检查层的「合成结果类型」（推荐先做）

在 `typecheck` 里给这三个装饰器建**静态合成类型**（不产生运行期类）：

- `@contextmanager def f(...) -> Generator[T, None, None]` → 表达式类型记成 `CM[T]`，其中 `CM` 是一个内部 TyKind 或 `unknown` + 方法表；
  至少要能解析 `__enter__` 返回 `T`、`__exit__` 存在；
- `@singledispatch def f(x) -> R` → 结果类型带 `register`（返回原函数）与 `__call__(x) -> R`；
- `@types.coroutine def f(...)` → 可 await（`await` 判定接受它）。

### 方案 B：运行期合成类（重）

照 `compile_ctor_wrapper` 合成真正的类与包装函数。缺点：三个模块的语义细节多（`_GeneratorContextManager` 的 `__exit__` 参数、
`singledispatch` 的 dispatcher 缓存），且要为每个用例生成代码，收益与复杂度不成比例。

## 3. 分步（每步可独立验收）

1. **`@contextmanager`**：类型检查层给 `__enter__`/`__exit__`（`__enter__` 返回 yield 类型，能推就推、推不出 unknown）；
   验收：`asyncio/tasks.py`、`importlib/resources/_common.py`、`test/support/import_helper.py` 三个文件 `check` 通过；
2. **`@singledispatch`**：给 `register` 与 `__call__`；验收：`pkgutil.py` 通过，且 `@f.register(int)` 写法能过；
3. **`@types.coroutine`**：`await` 接受它；验收：`asyncio/tasks.py` 里 await 该函数的分支通过；
4. **最后才**打开「未知装饰器保留原签名」，并重新量 628 口径（0.500 时是 500 → 496，建模后应回到 500 或更高）；
   如果仍有下降，说明还有别的装饰器在改签名，**继续判负**，不要放宽。

## 4. 红线

- 不许新增对 CPython 合法代码的拒绝（628 与 902 两个语料）；
- 不许把「未知装饰器保留签名」和「三个建模」混在一个提交里——分开验，谁回退清楚；
- 类型检查层的合成类型不要泄漏到模块接口（跨模块仍是 unknown，与联合类型的边界一致）。

## 5. 验收命令

```bash
bash tools/frontend_baseline.sh          # 628 口径：default 不许低于 501/626
# 目标文件逐个
build/lead/pyadamas check /usr/lib/python3.14/asyncio/tasks.py
build/lead/pyadamas check /usr/lib/python3.14/pkgutil.py
build/lead/pyadamas check /usr/lib/python3.14/importlib/resources/_common.py
build/lead/pyadamas check /usr/lib/python3.14/test/support/import_helper.py
```
