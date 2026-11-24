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
