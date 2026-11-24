# 多继承的字段布局与「非主基类方法」问题（3.9）

> 状态：**A（精确护栏，0.503）+ 布局前缀选择（0.505）已落地**；B（adjustor thunk）仅剩「两个状态基类」需要。记录实测、根因与两条修法。
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
## 7. 布局前缀选择（layout base）已落地（0.505，分支 mi-prefix）

**规则**：`layout_base` = MRO 顺序里第一个**有字段**的基类（`[主基类] + extra_bases` 的
先后就是 C3 保序的基类先后）；`base_fields = layout_base->n_fields()`，所有字段偏移相对它
计算。没有带字段的基类 → 无前缀（与 0.503 相同）。

于是 `ThreadingUDPServer(ThreadingMixIn, UDPServer)` 这类「mixin 在前、状态在后」的写法
**不用改**就正确：UDPServer 成为 layout base，它的字段前缀保持原偏移，`TCPServer.server_bind`
等按声明类布局读 `self.socket` 就是对的（实测 `s.sock, s.bind()` = `9 9`，CPython 一致）。

**跨模块（模块接口）**：字段**偏移不进接口**（`encode_module_interface` 只写名字/类型/默认值，
没有下标），导入方的 `field_index` 就是编码列表里的位置。所以这个改动**不需要 bump 格式版本**，
但要求「导出顺序 = 运行期布局顺序」：编码端改成从 `layout_base` 链走（根 → layout base → 本类），
解码端本来就按收到的顺序压平（`base_fields` 留 0），两边一致。

**护栏同步放宽**：`mi_guard.cpp` 改成「只有**非 layout base** 的字段方法在布局里挪了位置才拒绝」
（判据是 `C.field_index(f) != eb.field_index(f)`）。`class C(B, A)`（B 有字段、A 也有字段）仍然拒 ——
那就是真正的字段交错，需要 adjustor thunk 才能支持。

**语料**：628 文件默认口径 **496 → 496**（没有净增，原因见下），本仓库 898 个 `.py` **780 → 780**
（0 新拒绝）。5 个上一轮被护栏拒的文件：

| 文件 | 结果 |
|---|---|
| `socketserver.py` | 护栏不再触发；改为报**既有**的 `super() has no method 'server_close' after class 'ForkingMixIn'`（0.500 master 同样报，与本改动无关） |
| `_pyio.py` | 护栏不再触发；改为报既有的 `nreadahead` 标注赋值问题 |
| `xmlrpc/server.py` | 仍拒：`SimpleXMLRPCServer` 与 `XMLRPCDocGenerator` **都**带字段（`self.server_name` 等），布局只能容纳一个前缀 |
| `urllib/request.py` | 仍拒：`BaseHandler` 与 `AbstractDigestAuthHandler` 都带字段（`self.passwd` 等） |
| `xml/dom/expatbuilder.py` | 仍拒：`Namespaces` 与 `ExpatBuilder` 都带字段（`self._parser` 等） |

即：前缀选择解决了「mixin 在前、状态在后」这一类，**不能**解决「两个状态基类」——那确实要 thunk。
## 8. 0.506 中期结论：thunk 的前提是「布局里要有那块字段」

**实测**（`class C(B, A)`，B 有 `y`、A 有 `x`、A.get 读 `self.x`）：
- 护栏生效：编译期拒绝（`base 'A' of 'C' is not the primary base ...`），不再静默错值；
- 把护栏临时短路后运行：**`cb.x, cb.get()` = `1 2`**（CPython `1 1`）——A 的方法读到 B 的 y。

**布局探针**（临时打印，已还原）：

```
MI C base=B layout=B base_fields=1
  extra A.x -> in_A 0, in_C -1
```

即 **C 的布局里根本没有 A 的字段块**：`layout_base` 是 B（y 占 0），C 的 `self.x = 1` 走
`add_implicit_field` 又新建了一个**遮蔽**的 C 字段。所以：

- **只做调用点 `self += delta` 不行**：`in_C = -1`，没有可指向的 A 块，delta 无从谈起；
- 第一步必须先改布局：把每个带字段的非 layout base 的字段块**实体化**进子类布局（按 MRO 顺序
  追加），记 `ClassInfo::base_field_offsets[base] = 块起始下标`，并让 `field()/field_index()`
  解析进对应块（不再产生遮蔽的隐式字段）；
- 之后才有 thunk：直接调用点插 `self += 8 * base_field_offsets[A]`；虚调用因为 vtable 是「每个类
  一份 function 下标表」（`mod_.classes[cid].vtable`），thunk 要作为**额外函数**（先 `add self, delta`
  再尾调基类实现）拿到自己的下标，槽位填 thunk 的下标；
- 与 0.496 的实现指针守卫的相互作用：守卫依赖「槽里的实现下标 == 已知实现」，thunk 是另一个入口，
  「同一实现合并候选」会失效 —— 要么按 thunk 分组（(class, base) → thunk 下标一一对应），
  要么守卫里改成比较 thunk 指针，两者语义都对，取简单的那个但要同步改守卫。
- `super()` 协作式路径同样要按目标实现所在基类加 delta（`super().get()` 解析到 A.get 时）。

**结论**：这一轮没有落地（布局实体化 + thunk + 守卫同步是成套改动，半成品会静默错值）。
护栏保持现状（明确报错），三个文件（xmlrpc/server、urllib/request、xml/dom/expatbuilder）继续被拒。
下一步按上面四步走，先做布局实体化（单独一轮、可用 `field_index` 单测钉住），再做 thunk。
## 9. 布局实体化已落地（0.507，分支 layout）

**做了什么**（只做布局，不动 vtable/调用点/护栏）：

- `ClassInfo::base_field_offsets`：基类 → 该基类字段块在子类布局里的**起始下标**；
- 布局顺序 = `[layout_base 前缀] + [各非 layout base 的字段块（MRO 顺序追加）] + [本类声明]`；
- `field()` / `field_index()` 解析进这些块（own-first：子类声明仍遮蔽基类同名，Python 语义），
  `add_implicit_field` 不再为「基类已有的字段」造遮蔽槽；
- 调试探针 `PYADAMAS_DUMP_LAYOUT=1`：类定义处打印 class/base/layout_base/base_fields、块起点、
  每个字段的全局下标（默认关闭、零开销）。

**阶段证据**（`class C(B, A)`，A.x / B.y；护栏临时短路）：

```
layout C base=B layout_base=B base_fields=1
  block A -> start 1
  field x -> 1 (block/implicit)
cb.x = 1        # 属性访问仍正确
cb.get() = 2    # CPython 是 1；本步不修 thunk，预期如此
```

即 `A.x` 在 C 里的下标从 **-1 变成 1**（= B 的字段数 + 0），块真的进了布局。

**护栏仍是拒绝**：方法体里 `self.x` 仍按声明类布局算偏移，没有 delta/thunk 就必然读错，
所以两个状态基类的类定义继续报错（不是静默错值）。

**下一轮**（按依赖顺序）：
1. 直接调用点 delta：目标方法所属类 ≠ 接收者静态类且是带字段的非 layout base 时，
   插 `self += 8 * base_field_offsets[归属基类]`；
2. vtable thunk：thunk 作为额外函数（`add self, delta` + 尾调实现）拿自己的 fidx，槽位填它；
3. 同批改 `src/backend/vcall_inline.cpp` 的 `gen_guarded_vcall`（约 153 行起；`movq {imm*8}(%r11), %r11`
   在 206 行、`leaq entry_ref(fn)` 在 229 行）与 `gen_guarded_vcall_direct`（267 行起、`movq {imm*8}`
   在 299 行），按实现指针去重的分组逻辑在 208 行附近：thunk 是另一个入口，要么按 (class, base)
   分组，要么守卫里比较 thunk 指针；
4. `super()` 协作路径按目标实现所在基类加 delta。
