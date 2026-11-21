# 标准库自举第七批（stdlib-batch7）：configparser 重做

> 分支 **stdlib7**，基线 master 1589e34（0.353）。这一批是**纯 PyAdamas
> 源码**：按 CPython 3.14 语义重写 stdlib/configparser.py，**不改
> src / runtime / codegen，无 C 胶水、无 BigInt**，模块表也未改
> （configparser 在 batch1 已登记）。轻量标尺：同参数同功能同结果；错误
> 类型对、消息合理即可，不逐字复刻 CPython（batch1 钉住的 253/254 路径
> 仍逐字对齐）。CHANGELOG 预留 **0.354**。

## 一、可行性复评

| 依赖面 | 结论 |
|---|---|
| 字符串面 | `find/rfind/strip/rstrip/lower/replace/split/join/startswith/endswith` 齐备；解析不需要正则（头/键值行用索引判定，注释行用首字符判定） |
| 容器 | `dict[str, str]` / `dict[str, list[str]]` / `list[tuple[str,str]]` 可用；嵌套 dict 读取正常 |
| 文件 | `open(path, "r", encoding=...)` + `file` 形参 + `f.name`；read/write 走整读整写 |
| fallback 哨兵 | pa 默认参数只收**字面常量**，对象哨兵不可用；改用私有串哨兵 + `_is_unset`（is/== 双判，见四） |
| 类型标签 | 不需要 `isinstance` 分派（只用 `isinstance(x, str)` / `isinstance(x, list)` / 自家类判定） |
| 数值 | int/float 够用；getboolean 用 if 链（不依赖 dict 常量表） |
| 代理对象 | `__getitem__` 返回自家类实例（前向引用字段 OK），但 pa 里 `c["s"]` 的静态类型是 unknown，需类型标注，见四 |

结论：CPython configparser 的**日常面可纯 pa 实现**；allow_no_value /
自定义分隔符与注释前缀 / ExtendedInterpolation / converters 等长尾保守跳过
（见六）。

## 二、交付清单

| 文件 | 内容 |
|---|---|
| stdlib/configparser.py | 异常族 12 个；`_repr_str/_line_tag/_indent_of/_is_unset`；`Interpolation/BasicInterpolation` 标记类；`SectionProxy`；`RawConfigParser`；`ConfigParser`。853 行 |
| tests/run/390_configparser_core.py | 解析变体、查询、DEFAULT 继承、fallback、vars、read 多文件、defaults、optionxform、重复 read、重复 [DEFAULT] |
| tests/run/391_configparser_types_errors.py | getint/getfloat/getboolean 全形态、异常类型与消息（NoSection/NoOption/Duplicate*/Parsing/MissingSectionHeader/Interpolation*）、异常层级 |
| tests/run/392_configparser_interp.py | %(key)s 链式/%% 转义/DEFAULT 参与/vars 覆盖/RawConfigParser/write 往返（含 space_around_delimiters=False） |
| tests/run/393_configparser_mutation.py | set/add_section/remove_section/remove_option、DEFAULT 节读写、映射协议、SectionProxy 全方法、write 键校验 |
| tests/run/394_configparser_edges.py | strict=False、read_dict、DEFAULT-only、空文件、CRLF、节名/键名空白、解析错误行号、空值与多行、`interpolation=None`、空写出 |

## 三、语义边界（轻量标尺）

### 解析
- `[section]`：`[` 起头且行内存在 `]`（`rfind`）即节头，头后可跟尾随
  文本（CPython `SECTCRE.match` 口径）；`[]` 不构成节头，落到键值行报
  ParsingError。
- 键值行取**最左** `=` / `:`；键右剥空白、值两端 strip；键经 optionxform
  小写化（可覆盖）。
- 缩进续行：比上一个键/节头行缩进更深即续行（空格/tab 都算 1 列）；空行
  计入多行值（追加空串后 join+rstrip），整行注释行不计入值；注释行不终止
  续行。
- 重复节/键按 `strict` 报 DuplicateSectionError / DuplicateOptionError
  （行号与消息格式同 254 钉住）；`strict=False` 时后者覆盖、不报错；
  `[DEFAULT]` 可重复声明（不参与重复节检查）。
- 解析错误**累积到文件末尾**一次性抛 ParsingError（`[line %2d]: repr(line)`
  逐行列出），并先提交已解析的节/键/多行 join（CPython `_read` 的
  finally join 同口径）；`MissingSectionHeaderError` 立即抛。
- 换行统一：`\r\n` / `\r` 先归一成 `\n`（StringIO universal newlines 口径）。

### 查询与取值
- `sections()` 不含 DEFAULT；`has_section("DEFAULT")` False；
  `options("DEFAULT")` 抛 NoSectionError（同 CPython）。
- `has_option` 的空节名与 `"DEFAULT"` 都按默认节查；`get("DEFAULT", k)`
  走默认节；`items("DEFAULT")` 等价默认项。
- `get` 查找顺序 **vars > 节 > DEFAULT**；`vars` 的键经 optionxform、
  值 str() 化；命中 vars 的键可直接取值（CPython `_unify_values` 口径）。
- `fallback`：省略（私有哨兵）→ 缺失抛 NoSection/NoOption；显式传
  `None` / 任意值 → 原值返回（不再抛）。getint/getfloat 只对
  NoSection/NoOption 回退，值转换失败照常 ValueError（fallback 不吞）。
- getboolean 的 8 种布尔字面量大小写不敏感；非法值
  `ValueError("Not a boolean: <原值>")`。
- items 顺序 = DEFAULT 项在前（覆盖后的值）、节自有键按插入序；`raw=True`
  关闭插值；`vars` 只影响取值与插值 map，不新增条目（同 CPython）。

### 插值（BasicInterpolation）
- `%(key)s`（键经 optionxform）、`%%` 转义；引用值仍含 `%` 时递归展开，
  深度上限 10（`InterpolationDepthError`），缺失键
  `InterpolationMissingOptionError`，语法错 `InterpolationSyntaxError`；
  三条消息与 254 逐字一致。
- 隐式 DEFAULT 的引用在 `get("section", ...)` 下可见；`get("DEFAULT", ...)`
  只看默认节（缺失则插值错误）。
- `set` 前校验（值非空时）：`%%` 去掉、合法 `%(x)s` 去掉后仍有 `%`
  → `ValueError("invalid interpolation syntax in '...' at position N")`。
- `RawConfigParser` 无插值；`ConfigParser(interpolation=None)` 关闭；
  传入 `Interpolation()` 实例也关闭，`BasicInterpolation()` 实例开启。

### 修改与写出
- `set`/`remove_option` 的空节名与 `"DEFAULT"` 都写/删默认节；
  `add_section("DEFAULT")` → ValueError；`remove_section` 返回是否删除。
- `write(fp, space_around_delimiters=True)`：DEFAULT 在前，逐节 `[name]` +
  `key = value`（False 时 `key=value`），多行值 `\n` → `\n\t`，每节尾空行。
- 键含 `=` / `:` 或形如节头 → `InvalidWriteError`（消息合理）。
- 映射协议：parser `c["s"]` / 下标赋值（清空后 read_dict）/ `in` / `len`
  （节数+1）/ `iter`（DEFAULT 在前）；SectionProxy 的
  `get/getint/getfloat/getboolean/items/keys/values/update/setdefault/pop/clear`
  与下标读写、`in`、`len`、`iter`、`repr`（`<Section: name>`）。

## 四、pa 语言面限制与取舍（影响写法，不影响语义）

- **默认参数只收字面常量**：`fallback` / `read_file(source=None)` /
  `SectionProxy.pop(default)` 的「省略」用私有串
  `__configparser_unset_9f3c__` 哨兵；且默认参数字面量与模块级常量的
  `is` 在独立编译单元里不保证同一对象，判定用 `_is_unset`（is 再 ==）。
  用户显式传同名字符串会被当作省略（实际上不可达）。
- **object 表示层**：object 静态类型的 `None`/`False` 经 print/str 会显示
  为 `0`（值比较 `== None` / `== False` 正确）；非 str 值存进 str 槽后
  再做 `str()` 会崩。因此：`get` 的静态返回类型保持 `object`（fallback
  可为任意值）；`getboolean` 返回类型标成 `bool`（普通路径 print/str 正确，
  fallback 为非 bool 值时会被真值化，属已知边界）；`defaults=` /
  `read_dict` / `vars` 的值要求是 str（CPython 会 `str()` 化任意值，
  这里不做，见六）。
- **下标返回值的静态类型是 unknown**：pa 不把 `c["s"]` 的结果窄化成
  SectionProxy（即使 `__getitem__` 标了返回类型），需在用户代码里标注：
  `from configparser import SectionProxy` 后
  `p: SectionProxy = c["s"]`；之后 `p.get(...)` / `p["k"]` 可用。
  另：`del c["s"]` 这类**实例下标删除** pa 不支持，用
  `remove_section` / `pop` 代替；`list(parser)` 也不支持，`for k in parser`
  可用。
- `SectionProxy` 定义在 parser 之前（字段用前向引用 `"RawConfigParser"`），
  这样 parser 的 `__getitem__` 能标出具体类型；默认参数、字段前向引用、
  `super().__init__` 均已实测可用。

## 五、CPython 对拍

5 个用例都是**同一份 .py 在 CPython 3.14.4 与 pa 下跑、输出逐字节一致**
（expect 块由 CPython 输出钉死，`# --- expect ---` 之后逐行 `# ` 前缀）：

| 用例 | 对拍点 |
|---|---|
| 390 | 节头尾随文本、注释/续行/空行、多行 join、键值分隔符、DEFAULT 继承、options/items 顺序、has_option 默认节、fallback（None/值/int/float/bool）、vars、raw、read 列表、read_file、defaults 构造、optionxform 覆盖、重复 read、重复 [DEFAULT] |
| 391 | 8 种布尔值、int/float 转换、非法值 ValueError 消息、缺失/重复/解析/缺头/插值三类错误消息、异常层级、set 插值语法校验、RawConfigParser |
| 392 | 三级链式插值、%% 转义、DEFAULT 与节互引、vars 覆盖、raw、Raw 无插值、write 往返（默认空格与 False）、%% 写回原样 |
| 393 | set/remove_option/add_section/remove_section、DEFAULT 节写删、parser 映射（len/in/iter/getitem/setitem）、SectionProxy 全方法、clear、write 键校验（含分隔符/节头形状） |
| 394 | strict=False、read_dict（含 DEFAULT）、DEFAULT-only 文件、空字符串/空行 read、CRLF、节名含空格、键名含空格、tab 续行、空值、错误行号与自定义 source、interpolation=None、空写出、read_file(source) |

## 六、跳过/负面结论

| 项 | 结论 | 依据 |
|---|---|---|
| allow_no_value / 无值选项（None 值） | 跳过 | 需要 None 值贯穿解析/取值/写出，且 pa 的 object-None 表示层打印为 0；无值选项行仍按 ParsingError 处理 |
| 自定义 delimiters / comment_prefixes / inline_comment_prefixes | 跳过 | 需要构造期正则/前缀表；本批按 CPython 默认（`=`/`:`、`#`/`;`、无行内注释） |
| empty_lines_in_values=False / dict_type / default_section 构造参数 | 跳过 | 长尾；`default_section` 实例字段仍在（默认 "DEFAULT"） |
| converters / `converters` 映射与动态 `get*` 方法 | 跳过 | 需要实例上 setattr 方法/闭包；pa 不支持把函数当值再挂到实例 |
| ExtendedInterpolation（`${section:key}`） | 跳过 | 跨节取值 + 递归 map 重建，属长尾；`configparser.ExtendedInterpolation` 不存在（编译期即报模块无此属性） |
| popitem / allow_unnamed_section / UNNAMED_SECTION / MultilineContinuationError / UnnamedSectionDisabledError | 跳过 | 未建模；相关构造参数不存在 |
| 错误对象属性（`.section`/`.option`/`.source`/ParsingError.append） | 跳过 | 只保证类型与消息；异常类不定义 `__init__`（254 用位置参数构造异常仍可） |
| SectionProxy 代理缓存 | 不做 | 每次 `c["s"]` 新建代理（CPython 缓存，`is` 成立；这里不成立，值语义一致） |
| keys()/items()/values() 视图对象 | 返回 list | CPython 返回 KeysView/ItemsView/ValuesView；这里返回 list（迭代/len/取值一致，repr 不同） |
| defaults / read_dict / vars 的非 str 值 | 要求 str | CPython 会 str() 化；pa 的 object/str 槽表示层对非 str 做 str() 会崩（见四） |

## 七、闸门（实测，HEAD 见仓库）

| 闸门 | 结果 |
|---|---|
| pa 编译（configparser.py，门控开/关、type-tags 开） | ✅ `pyadamas check/run` 通过；5 个新用例在默认、`PYADAMAS_LANG_GAPS=1`、以及 `LANG_GAPS=1 + TYPE_TAGS=1` 下均 EXIT=0；模块源码（stdlib/configparser.py）在门控开/关两种模式都能编译 |
| 新用例 390-394 CPython 3.14 对拍 | **5/5 逐字节一致**（expect 块由 CPython 3.14.4 生成） |
| 全套件 normal（`PYADAMAS_TEST_JOBS=1`） | **EXIT=0，365 个 ran ok**，0 FAILED/DIFF（含既有 253/254） |
| 全套件 ASan（`PYADAMAS_TEST_ASAN=1`，JOBS=1） | **EXIT=0，365 个 ran ok**，0 FAILED/DIFF（含既有 253/254 与新 390-394） |
| 现有程序 asm 不变 | 在 `/tmp/wt-baseline7` 用 `git archive 1589e34` 独立构建基线编译器（同 Release 配置），对 `tests/*.py + tests/run/*.py + tests/modules/*/*.py + bench/*.py` 逐个 `pyadamas asm` 对拍：**375 个可比程序逐字节全同，0 DIFF**；11 个默认开关下本就编译失败（starred/相对 import/lang-gaps 用例/缺 platform 模块），两个编译器失败点与首行错误一致；7 个跳过的是 253/254（行为按本批对齐）与 390-394（新用例，基线缺符号）。编译器二进制差异仅来自构建路径字符串（`/tmp/wt-baseline7` vs `/tmp/wt-stdlib7`，`strings` 可见），src/runtime/codegen 零改动 |
| check_size | ✅ 通过（stdlib/configparser.py 853 行 < 1200，无新增超限文件；函数长度仅警告） |
| ASan detect_leaks=1（补充观察，非闸门） | 390 3 个直接泄漏/324B、391 2/220B、392 6/624B、393/394 0；均为**既有运行期**「异常跨函数 raise/catch 的 RC 环」现象（batch4 §六同源）：基线编译器跑 253 同样报 1 个直接泄漏，新实现因 fallback 路径多出 1 个 raise 站点。套件闸门用 `detect_leaks=0`，不受影响；修它要动运行期异常/GC 释放链，不在本标准库批次范围 |
