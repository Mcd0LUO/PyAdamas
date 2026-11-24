# 装饰器建模：三种合成包装类型（待做）

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
