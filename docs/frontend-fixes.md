# 前端三件修复（分支 fe-fixes，基线 master d6937be / 0.495）

三件事的取证、实现与判负都记在这里。`CHANGELOG.md` 未改（合并时统一写）。

## 0. 结论摘要

| # | 事项 | 结论 | 关键数字 |
|---|---|---|---|
| 1 | 类体里的方法别名（`__str__ = short`） | **已修** | 标准库 628 文件基线：默认口径 `check` **480/623 → 500/623**；27 个文件的 `undefined name` 消失，其中 20 个整文件转绿 |
| 2 | 局部变量丢类方法面（stdlib-round2 §7.2） | **判负（不可复现）** | 12 组形状 + 0.487 旧构建复核：链式与局部变量走同一条路径；真实现象是 unknown 接收者的方法面（§2） |
| 3 | `unknown` 上的下标（§7.3） | **部分落地** | 全仓 195 处（tests 143 / bench 22 / stdlib 30，36 个文件）；运行期按真实种类分派，堵掉 unknown dict/str 的假象与段错误；大整数仍缺口（要 P2 标签） |

## 1. 类体里的方法别名

```python
class A:
    def short(self) -> str:
        return "x"
    __str__ = short          # CPython 合法；stdlib 很常见

a: A = A()
print(a.__str__())
```

修前 `check` 与 `build` 都报 `undefined name 'short'`。

### 1.1 根因

两处：

1. `typecheck/func.cpp` 的类体遍历只对**带装饰器**的 `def` 做事，普通 `def` 直接
   `break`，名字从没进 `class_body_attrs_`（类体命名空间）。CPython 的类体是顺序
   求值，`def` 当场绑定名字 —— 这就是「前面的 def 对后面的表达式不可见」。
2. pyadamas 的方法不是一等值（`A.short` 在降级层明确拒绝），所以就算类型检查放行，
   降级也拿不到一个可赋值的函数对象。

### 1.2 实现（方法别名 → 同一实现的两个槽）

- `typecheck/func.cpp`：类体里**已经执行过**的 `def` 记进 `class_body_methods_`
  （名字 → `cls->methods` 下标；记下标而不是指针，因为后面会往 methods 里追加），
  同时把 `class_body_attrs_[name]` 填成函数类型（含 `@property`/`@staticmethod`/
  `@classmethod`）。
- 类体 `NAME = 其它方法名`（右边必须是**前面**出现过的 def）识别成**方法别名**：
  往 `cls->methods` 追加一份 `MethodInfo`，`name` 用别名、`slot` 用别名的槽、
  `sig/call_sig/def` 共享原方法。降级时 vtable 的两个槽写同一份实现，
  调用点（含 dunder 分派、属性 getter）按别名查得到。
  `@property` 别名同时登记 `cls->properties[别名]`，`obj.别名` 也能读。
- `ClassInfo::alias_assign_stmts` 记下这些赋值语句，降级层跳过它们（不再当模块
  全局执行）。
- 方法体检查按 `FunctionDef*` 去重，一条错误不会因为别名条目报两遍。

顺序语义保持不变：别名只能看见**前面**的 def（`tests/fail/class_body_alias_forward.py`
钉住 `__str__ = short` 写在 def 之前仍然报 `undefined name`）。

### 1.3 标准库基线（628 文件，CPython 3.14.4）

命令同 `compatibility.md` 的「前端鲁棒性基线」（`check --syntax` 623 通过后再全量
`check`）。修前/修后同机同文件集：

| 口径 | 修前 | 修后 |
|---|---|---|
| `check --syntax` | 623/628 | 623/628 |
| **`check`（默认，本页口径）** | **480/623** | **500/623（80.3%）** |
| `check --no-imports --lenient-names` | 505 | 504 |

> 注：本机 lenient 实测 **504**（不是 0.485 记录的 505），默认口径的**上限就是它**。

细账（修前 143 个失败文件，其中首条错误是 `undefined name` 的 45 个）：

- 27 个文件的 `undefined name` 由这次改动消掉（`_weakrefset` 的 `__sub__ = difference`、
  `doctest` 的 `__str__ = shortDescription`、`threading` 的 `__enter__ = acquire`、
  `multiprocessing/process` 的 `pid = ident`（property 别名）等）；
- 其中 **20 个整文件转绿**，7 个转绿后仍卡在别的簇（窄化 2、`bytes+str` 1、
  解包 1、property/字段同名 1、动态属性 1、float/非 float 形参 1）——这些与类体无关，
  本来就是已知的 top-10 失败簇；
- 18 个文件的 `undefined name` 还在，全是模块级条件导入 / 动态命名空间，
  其中 **6 个是「方法默认参数引用类体名字」**（见 §1.4）。

### 1.4 没做的一步：def 点的默认参数在类命名空间求值

```python
class C:
    X = 5
    def f(self, y=X) -> int: ...
```

CPython 在类体里求值默认值，`X` 是类命名空间的；pyadamas 在 `funcdef_impl` 里
求值默认值时类体命名空间已经还原，于是报 `undefined name 'X'`。标准库里这类还有
6 个文件（`collections/__init__.py` 的 `__marker`、`logging/handlers.py` 的
`LOG_USER`、`compression/zstd/_zstdfile.py` 的 `FLUSH_BLOCK`、`pydoc.py` 的
`_GoInteractive`、`turtle.py` 的 `DEFAULT_MODE`、`http/cookiejar.py` 的
`DomainLiberal`）。

**不在本轮做**：只放宽类型检查会让 `check` 过、`build` 变成
「default value of 'X' is not a literal constant」——不好看但也不算更糟。真做要动
**两层**：类型检查在求值默认值期间装回类体命名空间（小），降级层把「def 点的
默认值」物化一次（隐藏全局 + 模块初始化时求值 + 跨模块导入时带上），是**新机制**，
`bind_args` 现在只认字面量默认值。代价约一天，收益 6 个文件（其中还有的文件
另有别的错误）。

### 1.5 回归

- `tests/run/class_body_alias.py`：dunder 别名（`__str__`/`__sub__`/`__len__`）、
  普通别名、property/classmethod/staticmethod 别名；`print(a)`/`str(a)`/`a.__str__()`
  真的走别名。期望值取 CPython 3.14 实跑。
- `tests/fail/class_body_alias_forward.py`：别名在前、def 在后 → 仍然拒绝。

## 2. 「先把返回值赋给局部变量」会丢类方法面 —— 判负

§7.2 的说法（`f = rd.extractfile("a.txt"); f.read()` 报 `method 'read' is not
supported by the code generator yet`，链式可以；并称 `re.search` 在函数里同样）
在 0.495 / d6937be 上**复现不出来**。取证：

| 形状 | 链式 | 局部变量 | 说明 |
|---|---|---|---|
| 两个自定义类 + 工厂方法（带返回标注） | ok | ok | §2 要求的最小复现 |
| 同上，落在函数里 / 模块顶层 | ok | ok | 两个作用域都对 |
| 跨模块类（`m2lib.A`） | ok | ok | 类型是导入类的 mirror，仍走类方法面 |
| 未标注返回类型的方法（靠函数体推断） | ok | ok | `peek()` → `Reader` 推断得到 |
| 容器元素接住（`q = pool[0]`） | ok | ok | |
| 闭包捕获 / 全局 / 分支合并 / with / for / 元组解包 | ok | ok | |
| pyadamas stdlib `tarfile`（`rd.extractfile` 真路径） | ok | ok | 用 tests/run/361 的内存归档复现 |
| 0.487 旧构建（9ca49e5）同样形状 | ok | ok | 不是这期间被顺带修的 |

唯一会产生同一句错误消息的是**静态类型确实为 `unknown` 的接收者**：

```python
import queue
q = queue.Queue(); q.put(1)
v = q.get_nowait()      # unknown
v.read()                # error: method 'read' is not supported by the code generator yet
```

而且**链式与局部变量一样报错**（`q.get_nowait().read()` 同一条消息），不存在
「只对链式走类方法面」的不对称。所以 §7.2 的处方（「局部变量丢方法面」）是
**误诊**：真正丢的是 unknown 值上的方法面，属于 unknown 语义，不是变量作用域。

**要动哪一层**：unknown 接收者的方法调用需要在降级层做运行期按类查表
（运行期已有 `pa_method_register` 表，lang_gaps 的 file 协议就靠它），或者让
unknown 携带类身份（P2 标签）。代价：前者要设计「unknown 实例 → vtable」的
动态调用 ABI 与实参约定（未知签名只能按机器字传，返回值/所有权也未知），
后者是表示层改动；两者都不是本轮范围。

回归：`tests/run/local_var_method.py` 钉住「局部变量 / 链式 / 容器元素三条路
行为一致」，`tests/unsupported/unknown_receiver_method.py` 钉住 unknown 接收者
仍然**明确拒绝**（不静默按机器字调用）。

## 3. `unknown` 上的下标

### 3.1 取证：有多少地方依赖它

计数办法（可复现）：降级层加一条环境变量门控的诊断
（`PYADAMAS_DIAG_UNKNOWN_INDEX=1`，`lower/expr_container.cpp` 与
`lower/expr_assign.cpp`），逐个文件跑 `pyadamas build --rebuild`
（**必须 `--rebuild`**：程序产物缓存会跳过降级，数字会假小）与
`pyadamas ir`（stdlib）。

| 组 | 文件数 | 未知接收者下标处数 |
|---|---|---|
| `tests/run/*.py` | 26 | 141 读 + 2 写 |
| `tests/bench/cases/*.py` | 5 | 22 读 |
| `stdlib/*.py` | 5 | 30 读 |
| 合计 | **36** | **195**（193 读 / 2 写） |

stdlib 命中的 5 个模块是 `contextlib`(10)、`itertools`(11)、`pprint`(6)、`string`(2)、
`tarfile`(1) —— 全是「形参是 unknown / 容器没写元素类型，运行期确实是容器」的
正当用法（如 `contextlib._make_cm` 的 `args[0]`、`itertools.starmap` 的 `row[0]`）。

### 3.2 方案 ①（编译期报错）：判负

195 处 / 36 个文件里包含 **5 个核心标准库模块**，禁掉等于让它们编不过。要落地只能
①给 `unknown` 加 `cast_*` 逃生口（现在只有整数版 `cast_i8..u64`）+ ②把 195 处逐个
标注（`tuple[unknown, ...]` / `list[tuple[unknown, ...]]`）。标注不是纯注释：
`list[...]` 的元素类型会参与 `elem_kind`/`heap_bits` 定型，改标注等于改代码生成，
风险远大于收益。

### 3.3 方案 ②（运行期按类型判）：部分落地

实现：`pa_any_index(v, i)`（`runtime/rt_class.c`，与 `pa_any_iter_list` 同族）。
降级层对「接收者静态类型是 unknown」的下标改发 `RtFn::AnyIndex`（结果按拥有引用
进临时栈）。运行期：

1. `pa_unknown_is_heap(v)` 探针命中 → 按**头上 kind** 分派：
   - `LIST`（含 tuple）→ `pa_list_get`（负下标归一、越界 IndexError），元素 `+1`；
   - `DICT` → `pa_dict_get`（缺键 KeyError），值 `+1`；
   - `STR` → `pa_str_char_at`（码点，返回新串）；
   - `JSONNULL`/`JSONBOOL` → TypeError（与 CPython 消息一致）；
   - 其它堆对象（类实例、range…）→ TypeError。
2. 探针说不是堆 → **沿用改动前的机器语义**（按 list 布局取元素），因为「没进 RC
   索引的标量元组」探针看不见，而它与 int 在运行期分不开（`pa_any_iter_list` 有同一个
   文档化边界）。这一步保证**零回归**：`with pair() as v: v[0]`、
   `itertools.starmap(pow, [(2,3)])` 这类标量元组照旧正确。
3. 只有能安全排除的小整数（`v < 65536`，malloc 不会返回那么低的地址）与 `None`
   直接抛 TypeError。

修前/修后（`/tmp/fe487` 的 0.487 构建复现「修前」，与 0.495 同一段降级代码）：

| 程序（`v = queue.get_nowait()`，静态 unknown） | 修前 | 修后 | CPython |
|---|---|---|---|
| `q.put({"k": 7}); v["k"]` | `IndexError: list index out of range`（假象） | `7` | `7` |
| `q.put("abcd"); v[1]` | **段错误** | `b` | `b` |
| `q.put(99); v[0]` | **段错误** | `TypeError` | `TypeError` |
| `q.put((7,8)); v[0]` | `7` | `7` | `7` |

### 3.4 剩余缺口（都记在这里，别当已修）

- **大整数上的下标仍是段错误**：`v` 是 `1 << 40` 这种大整数时，`v < 65536` 不成立，
  探针又看不见（不是堆），只能按机器语义解引用。彻底修需要 **P2 类型标签默认开**
  （`pa_any_index_t(v, tag, i)`：tag 直接给 TUPLE/BYTES/CLASS，标量元组不再靠
  「猜」），或让**标量元组进 RC 索引**（最热容器的 GC 记账成本，`rc_alloc_untracked`
  的注释解释了为什么现在不进）。两条都是表示层/运行时层的改动，不在本轮。
- **bytes 与 str 同形**（`PyAdamasStr` 没有运行期标记，区别全在静态类型）：unknown
  的 bytes 下标按 str 口径返回 1 字符串，而 CPython 返回 int。与 `pa_any_iter_list`
  的既有口径一致，同样要标签才能精确。
- **unknown 实例的 `__getitem__`**：现在抛 TypeError（以前是段错误/垃圾）。要支持得
  走运行期方法表，与 §2 的未知接收者方法调用是同一件事。
- **下标写**（`v[i] = x`，接收者 unknown）仍是机器语义：全仓 2 处、stdlib 0 处。
  写路径要正确处理元素所有权（`store_list_elem` 的 retain/take）与 `heap` 种类参数，
  本轮只堵了读。

### 3.5 回归

`tests/run/unknown_subscript.py`：tuple / list / 标量元组 / dict / str 五种运行期
种类 + 小整数 TypeError + None TypeError + 越界 IndexError，输出与 CPython 3.14
逐字节一致（修前 `dict` 那行是假象 `IndexError`、`str` 那行段错误）。

复现诊断数字：

```bash
PYADAMAS_DIAG_UNKNOWN_INDEX=1 build/fe/pyadamas build --rebuild -o /tmp/o FILE.py 2>&1 | grep DIAG
PYADAMAS_DIAG_UNKNOWN_INDEX=1 build/fe/pyadamas ir stdlib/contextlib.py 2>&1 >/dev/null | grep DIAG
```
## 4. 闸门（三个 commit 合起来的最终状态，`build/fe`）

| 闸门 | 结果 |
|---|---|
| `cmake --build build/fe -j8` | ok（无 warning，`-Werror`） |
| `PYADAMAS_TEST_JOBS=10 bash tests/run_tests.sh build/fe build/fe/pyadamas` | **491 ran ok / 0 失败**（488 旧 + 3 个新 run 用例；新 strict 用例 1 个） |
| `PYADAMAS_TEST_JOBS=10 bash tools/difftest.sh build/fe/pyadamas` | **461 个用例 / 0 个不一致**（458 旧 + 3 新） |
| `bash tests/check_size.sh` | PASS（最大 `src/typecheck/func.cpp` 1187 行；新文件 `class_alias.cpp` 76 行） |
| `ctest --test-dir build/fe` | 8/8 通过 |
| `PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=10 bash tests/run_tests.sh ...` | **491 ran ok / 0 失败**，无 sanitizer 报告 |
| 标准库 628 文件基线（默认口径） | 480/623 → **500/623**；lenient 504/623 |
