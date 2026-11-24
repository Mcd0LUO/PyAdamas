# 类型窄化（`T | None`）设计记录（0.498）

## 目标与不变量

- 窄化**只影响静态类型**，不改变运行期表示：`T | None` 仍是 `TyKind::Optional`，槽里还是
  8 字节字（int/指针），窄化只是把 `Symbol::ty` 在某个语句区间内换成内层类型。
- 与 `typing.Optional[T]` 共用同一套语义（两者都解析成 `TyKind::Optional`）。
- 窄化是**保守**的：只认能静态证明的模式，认不出来就保持原类型（不误杀）。

## 支持的模式

| # | 模式 | 测试 |
|---|---|---|
| 1 | `if x is not None:` / `if x is None: ... else:` 两个分支内 | `tests/run/narrow_optional.py`（one/three） |
| 2 | 提前返回：`if x is None: return ...` 之后余下部分非 None | 同上（two） |
| 3 | 真值：`if xs:`（容器/可选容器）分支内去掉 None | 同上（four） |
| 4 | `assert x is not None` | 同上（five） |
| 5 | 短路：`if b is not None and b.v > 0:` 的右操作数/分支体 | 同上（six）；`narrow_all` 收集全部合取项 |
| 6 | 多层联合：`int \| str \| None` 只去掉 None，剩下 `int \| str` | `tests/narrow_optional_static.py` |
| 7 | 窄化后重赋值：`x = other` 后类型更新，不固化窄化结果 | `tests/run/narrow_optional.py`（eight/nine） |
| 8 | 循环体：`while cur is not None:` 体内 cur 非 None | `tests/run/09_class.py`（既有用例） |
| 9 | 属性：`if self.x is not None:` 之后 `self.x.foo()` | `tests/09_class.py`（既有用例） |

## 收紧：未判空就使用 `T | None` 报错

```
def f(x: int | None) -> int:
    return x + 1
# error: 'x' may be None here; check it first (if x is not None:) or use assert
```

覆盖的「使用」：二元算术/比较（排序比较，`is`/`==` 判空本身不算）、一元运算、属性访问、
下标。**不覆盖**函数实参（`assignable` 对 `Optional[T]` 仍然反向放宽，避免大面积误杀）。

## 实现要点

- `narrow_on(cond, truthy)` 识别 `is [not] None`、`not`、裸名字真值、单层属性，返回
  `Narrowing`（记录 `Symbol*` 的旧类型 + 属性键）；`narrow_all` 负责 `and`/`or` 的合取/析取
  展开（`A and B` 为真 ⇒ 两者都真；`A or B` 为假 ⇒ 两者都假）。
- `stmt_if`：每个分支先 `restore_types()` → 求值条件 → `narrow_all` → 检查分支体 →
  **先 `merge_in()` 再恢复窄化**（顺序反了会让分支末尾的确定类型被 Optional 覆盖回去）；
  隐式空 `else` 也按「条件为假」窄化。
- `stmts()`：单分支且分支必离开时，按「条件为假」窄化后续语句（提前返回模式）。
- `stmt_while`：循环体按「条件为真」窄化；退出后不反向窄化（可能零次执行）。
- `declare()`：给 `T | None` 标注的变量赋确定值后，槽类型更新为确定类型（否则
  `if x is None: x = 0` 的分支合并会把 Optional 并回来）。只对 Optional 生效，
  其它标注（float 等）的槽语义不变。

## 已知边界（不在本次范围）

- **跨模块**：函数签名里的联合/Optional **不序列化成员表**（`enc_type` 对带成员表的 Unknown
  走 unknown 码），跨模块调用退化成 unknown —— check 比 build 宽，符合「宁可漏报」。
- `unknown` 上的下标/方法调用仍按动态放行（另一条任务线在做）。
- `isinstance` 窄化、walrus（`(x := d.get(k)) is None`）**未实现**：CPython 标准库里有 3 例
  因此被误报（graphlib/pyclbr 等），已记录；实现方向是在 `narrow_on` 里加 `NamedExpr` 与
  `isinstance` 分支。
- C 结构体/联合指针的 `T | None`（cimport 返回值）**不报**「may be None」：那是裸指针的
  可空性，不是 Python None 语义（否则 104_cstruct/215_c_float32/527_union_array 会被误杀）。
## 已修的三类假阳性（0.501，分支 fe3）

0.499 的真窄化在 628 文件语料上多拒了 6 个文件。本轮修了两类、外加一条模块级
规则，默认口径 **495 → 500/623**：

| 类 | 根因 | 修法 | 语料里被修的文件 |
|---|---|---|---|
| walrus | `if (n := d.get(k)) is not None:` 的判断对象是 `NamedExpr`，`narrow_on` 只认 `Name`，被赋名的 n 没被窄化 | `narrow_name_of()`：条件里取名字时同时认 `NamedExpr` 的 target（`is`/`is not None`、裸真值、`isinstance` 第一参都走它） | `graphlib.py`（`(nodeinfo := n2i.get(node)) is None` 提前返回之后） |
| `isinstance` | 没有 refinement，`if isinstance(x, C)` 之后 `x` 仍是联合 | `narrow_isinstance()`：真值分支收窄成 C（联合里必须有一个与之兼容的类成员，否则不动）；假值分支去掉「C 或其子类」的成员（去空了就不动）。只认用户类对象 + `Name`/walrus 目标，内置类型/元组形式/属性目标一律保守 | `pyclbr.py`（`isinstance(parent, Class)` 之后 `parent.methods[...]`） |
| 模块级「先定义后初始化」 | 模块级未标注名字的条件赋值（`_P = None if cond else 92`）会把类型并成 `T \| None`，之后 `a < _P` 报「可能为 None」 | 模块级未标注名字不因分支里的 None 并回 Optional（与既有「模块级 None 占位 → unknown」同一条口径）；`declare()` 与 `stmt_if` 的分支合并两处都做 | 语料里另有 3 个文件转绿（`multiprocessing/util.py` 的 `_SUN_PATH_MAX` 仍受下一节限制） |

回归：`tests/run/narrow_walrus_isinstance.py`（walrus 两种极性 + isinstance 两分支 + Optional 组合，正反输出对齐 CPython），并加进 `tests/opt_levels.sh`（-O0/-O1/-O2 都跑）。

### 仍剩下的假阳性（不同根因，本轮不做）

| 文件 | 现象 | 根因 | 要做的事 |
|---|---|---|---|
| `base64.py:482` | `acc * 85 + _b85dec[c]` 报可能为 None | `[None] * 256` + 逐个写 int → 元素类型 `int \| None`；代码**故意**在 None 时靠 `except TypeError` 兜底 | 要么承认「try/except TypeError 里的 Optional 算术」是合法模式，要么让容器占位元素（`[None]*n`）单独跟踪「已填充」区间 —— 后者是新的流分析层 |
| `xml/dom/pulldom.py:110` | `self.lastEvent[1]` 报容器可能为 None | 字段先赋 `[None, None]`、再把 `lastEvent[1]`（元素类型 None）赋回字段 → 字段变成 `list \| None` | 同上的容器占位问题（字段版） |
| `modulefinder.py:442` | `parent.__name__` 报可能为 None | `determine_parent()` 未标注、有 `return None` 分支，推断成 `Module \| None`；调用点靠外部不变量（level != 0） | 需要跨函数的「不变量/契约」信息，静态上判不出来 —— 这是真 Optional，不是窄化缺陷 |
| `multiprocessing/util.py:182` | `sun_path_len < _SUN_PATH_MAX` | `_SUN_PATH_MAX` 走 if/elif/else（108 / 104 / `None if os.name=='nt' else 92`），模块级合并后仍是 `int \| None` | 同上（条件定义 + 平台常量） |

### 3.12：`if xs:` 对空 list 在 -O0 崩溃 —— 不可复现

按 `xs: list[int] = []` + `if xs:` 在 -O0/-O1/-O2 三个档位实测，**都不崩**（输出正确）；
扩展覆盖 list/dict/set/str/tuple、字段、参数、全局、walrus、`and` 链、`while`、
生成器内、`try` 内、`not`/`bool()`/`len()` 共 20+ 形状全绿；0.498 构建（cb217e0）
同样不崩。回归 `tests/run/empty_container_truth.py` 已进 `tests/opt_levels.sh`
（ctest 的 `pyadamas_opt_levels` 会跑 -O0/-O1/-O2）。

顺带记一个**真实存在**的既有崩溃（与本条无关）：`check --imports` 在循环 import 的
文件上栈溢出（`encode_module_interface → enc_sig → enc_sig …`），0.498/0.500 都崩
（sched.py、random.py、threading.py 等 20+ 文件），需要给签名编码加递归保护。
