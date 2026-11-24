# 多继承的字段布局与「非主基类方法」问题（3.9）

> 状态：**A（精确护栏）已落地（0.503）**；B（adjustor thunk）未做。记录实测、根因与两条修法。
> 相关：[language.md](language.md)（继承与 MRO 的契约）、路线图 3.9。

## 1. 契约与实现的错位

`language.md` 写的是：多继承用 C3 MRO，**主基类的字段/方法槽位前缀保持不变**（§继承那一行）。
「主基类前缀不变」意味着：只有**第一个基类**的字段偏移在子类里是原样的；其余基类被排在后面，偏移会变。

实现里方法体的 `self.x` 用的是**声明该方法的类**的字段偏移——这对主基类成立，对非主基类不成立。

## 2. 实测（0.495/0.498 都复现）

```python
class A:
    x: int
    def get(self) -> int:
        return self.x          # 用的是 A 的布局：x 在 offset 0
class B:
    y: int
class C(B, A):                 # A 不是主基类 → 在 C 里 x 被 B 顶到 offset 8
    def __init__(self) -> None:
        self.y = 20
        self.x = 10

cb: C = C()
print(cb.x, cb.get())          # CPython: 10 10；pyadamas: 10 20（读到了 y）
```

关键区分：**属性访问是对的**（`cb.x` = 10，调用点知道静态类 C，按 C 的布局取偏移）；
**只有方法体内的 `self.field` 算错**（按声明类 A 的布局）。这解释了为什么它能活这么久——大部分测试只查属性或单继承。

## 3. 修法选项

### A. 短期护栏（拒绝，而不是算错）

当类有多基类、且某个**非首基类**带字段注解、且该基类的方法访问 `self.<字段>` 时，编译期报错：

```
error: method 'A.get' accesses fields of a non-primary base; put A first or use a mixin without state
```

- 代价：可能拒掉部分合法代码（但那些代码现在**算错值**，拒绝更好）；
- 判定时机：类定义处（此时知道 C 的基类顺序，也知道 A 的方法集合；方法的字段访问需要标记）。

### B. 正解：adjustor thunk（像 C++ 那样）

- 类布局里记录**每个基类的偏移**（`ClassInfo` 加 `base_offsets`）；
- 虚调用：vtable 槽里对非主基类方法存「thunk 地址」，thunk 做 `self += offset` 再跳真实实现；
- 直接调用（静态类型已知且是基类）：调用点插 `add offset`；
- `super()` 的协作式路径同样按目标实现所在基类调偏移；
- 验收：上面的例子输出 `10 10`；`super()` 链、多基类各带字段、菱形继承三种形态要与 CPython 逐字节一致；
- 代价：动布局（`ir/class_info`）、vtable 生成、vcall 守卫（0.496 刚改过，注意与实现指针比较的交互——thunk 是另一个入口，会让「同一实现合并候选」失效，需要按 thunk 分组）、直接调用与 `super()` 降级。

### C. 运行期偏移表（不推荐）

方法体里的字段偏移改成运行期查（按实例真实类）。每次字段访问多一次查表/访存，热路径直接退化，与现有优化方向冲突。

## 4. 建议

先做 **A**（把静默错值变成明确报错，代价小、可立刻止血），再做 **B**（真正支持多继承状态）。
A 落地时注意：mixin（无字段的基类）必须不受影响——`class C(Logger, Base)` 这种是常见且正确的用法。

## 5. 验收（两阶段共用）

- 628 文件基线：拒绝数不得增加（现在 495/626；多继承在 stdlib 里极少，预期 0–1 例，需逐例解释）；
- 本仓库 889 个 `.py` 零新拒绝；
- 回归：单继承、主基类多继承（`class C(A, B)`）、非主基类多继承（`class C(B, A)`）、菱形、mixin、`super()` 六种形态，输出与 CPython 逐字节一致；
- 12 基准 asm 不变（这块不动 codegen 的默认路径时）。
## 6. A 已落地（0.503，分支 mi-fix）

**标记**：`ClassInfo::methods_access_own_fields` —— 方法体里出现 `self.<字段>` 的读
（`typecheck/expr_attr.cpp` 的属性字段分支）或写（`typecheck/stmt.cpp` 的字段赋值
分支）时，在**方法所属类**上置位（`is_self_attr` + `cur_class_`，只做静态访问记录，
不做运行期分析）。纯 mixin 的方法只碰方法/参数时不会置位。

**拒绝条件**（`func.cpp` 的 classdef，类定义处）：类 C 的某个**非主基类** A 同时满足
① `A.methods_access_own_fields`；② A 链上的字段在 C 的布局里**真的挪了位置**
（`C.field_index(f) != A.field_index(f)`，菱形共享祖先偏移不变 → 放行）；③ 不是
导入类、不是 open 类。报错：

```
error: base 'A' of 'C' is not the primary base and its methods access instance fields;
       put A first or make it a stateless mixin (see docs/mi-layout.md)
```

单继承、主基类（第一个）、纯 mixin（无字段 / 方法不碰字段）完全不受影响。

**语料实测**：标准库 628 文件默认口径 **499 → 496/623**，新拒绝 5 例，全部是真的会算错：

| 文件 | 类 | 说明 |
|---|---|---|
| `socketserver.py:722` | `ThreadingUDPServer(ThreadingMixIn, UDPServer)` | 状态基类 `UDPServer` 在第二位，`TCPServer.server_bind` 等按自己布局读 `self.socket` |
| `_pyio.py:1393` | `BufferedRandom(BufferedIOBase, BufferedReader)` | `BufferedReader` 的方法读 `self._read_buf` 等字段 |
| `xmlrpc/server.py:938` | `DocXMLRPCServer(SimpleXMLRPCServer, XMLRPCDocGenerator)` | 生成器的方法读 `self._dispatch` 等 |
| `urllib/request.py:1199` | `HTTPDigestAuthHandler(BaseHandler, AbstractDigestAuthHandler)` | 摘要处理器方法读 `self.passwd` / `self.add_password_file` 状态 |
| `xml/dom/expatbuilder.py:803` | `ExpatBuilderNS(ExpatBuilder)` 多基类组合 | 基类方法读 `self._parser` 等 |

本仓库 898 个 `.py`：**0 新拒绝**（除本任务新增的 `tests/strict/mi_non_primary_base_fields.py`，
它本来就该被拒）。

**下一步（修法 B 的更省事版本）**：主基类没有字段时，让「第一个带字段的基类」提供布局
前缀 —— 这样 `ThreadingUDPServer(ThreadingMixIn, UDPServer)` 这类「mixin 在前、状态在后」
的常见写法会变正确且被允许，上面 5 例里至少 3 例会自己消失。真正的 adjustor thunk 仍按
§3 B 的另一半做（非主基类字段与主基类字段交错时）。
