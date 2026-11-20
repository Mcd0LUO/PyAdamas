# P2 unknown 运行期类型标签 —— 落地与闸门（0.340 预留，types1 分支）

> 范围：docs/numeric-types-design.md §2 的 P2——扩展 unknown 值的运行期类型
> 信息，解锁 isinstance/repr/pprint（stdlib-b3 取证的根本缺口）。基线
> master 4ba447a / 0.339，独立 worktree /tmp/wt-types1（分支 types1），不推
> master、不动主树。门控 `PYADAMAS_TYPE_TAGS=1`，**默认 0**。
>
> **结论（先说）**：P2 落地。门控开时 None/bool/int/float/str/bytes/list/
> dict/tuple/set 在静态类型已知的 unknown 边界可精确识别，repr 与 pprint
> 对 CPython 逐字节一致；门控关时 tag 编号/判定/所有生成代码逐字沿用 0.339
> （117 个基准/语料 asm cmp 全同）。**跨模块/间接调用 tag 传递**用
> `pa_ptag_in/pa_ptag_len` 侧信道实现（不改 `%r11`、不改闭包/vtable/旧入口
> gadget），所以 stdlib/pprint.py 这类独立模块也能收到精确 tag。
> 明确未做：容器元素逐元素 tag、虚调用（vtable）tag、异常类当值
> （内建异常类无运行期类对象）。边界见 §5。

## 1. tag 编码（不改 P3 既有取值）

| tag | 名 | 堆? | 说明 |
|---|---|---|---|
| 0 | UNK | 否 | 旧表示；RC/算术/判型全部回退探针 |
| 1 | INT | 否 | int64 裸字（门控关时也含 bool/None，保持 0.339 语义） |
| 2 | FRAW | 否 | 静态 float 实参裸位模式 |
| 3 | FBOX | 是 | CPYFLOAT 盒 |
| 4 | STR | 是 | str 指针 |
| 5 | HEAP | 是 | 其它堆指针（list/dict/实例/…，形状读头 kind） |
| 6 | NONE | 否 | None（payload 0）——**仅门控开** |
| 7 | BOOL | 否 | bool（payload 0/1）——仅门控开 |
| 8 | TUPLE | 是 | tuple（头 kind 仍是 LIST） |
| 9 | SET | 是 | set（头 kind 仍是 DICT） |
| 10 | BYTES | 是 | bytes（头 kind 仍是 STR） |
| 11 | CLASS | 是 | 类对象（头 kind OBJ + class magic） |
| 12 | BIG | 是 | **预留**：P3 big BigInt 落地时用 |

堆判定：门控关时**逐字** `tag >= 3`；门控开时 `3..5 或 >= 8`（6/7 非堆
标量，RC 跳过）。新 tag 只在门控开时发射，因此 flag-off 的 tag 编号、判定
指令与 asm 全部不变。算术域里 NONE/BOOL 仍解码成 ANY_INT（None=0、True=1），
与 0.339 的 P3 语义逐条一致——类型内省才用专 tag 区分。

## 2. 发射/传播

- **产生点**：ConstNone→NONE；bool 常量（const.int ty=bool）→BOOL；
  ConstStr→STR；AnyFromF64→FBOX；静态类型导出（typed vreg 作 unknown 实参/
  存储源）→ 对应 tag（Bool→BOOL、None→NONE、Tuple→TUPLE、Set→SET、
  Bytes→BYTES、ClassObj/TypeObj→CLASS、其它堆→HEAP）。
- **搬运**：Mov/LoadSlot/StoreSlot/LoadGlobal/StoreGlobal 与 P3 相同，tag
  跟值走；容器元素装载/字典值/字段仍按 P3 三路（位图/kind/一次探针）导
  HEAP/INT——这些来源没有子类信息（§5）。
- **调用边界**：P3-1b 的直调 tag ABI 保留，布局改为「第 i 个形参 = 第 i 个
  字、长度 = 形参个数」：被调按**自己的形参个数**校验，不匹配整体忽略
  （错位 tag 会让 RC 按错误堆性 inc/dec，这是本轮实测抓到的第一个坑）。
  跨模块/间接调用（导入函数、函数值、闭包）无法用 `%r11`（它已是目标
  地址/闭包指针）：改为调用前把数组指针与长度写进 `pa_ptag_in/pa_ptag_len`，
  被调序言读走并立刻清零。运行时回调（vthread/生成器 resume）没有调用点
  写它，读到 0 → 全 UNK 回退。unknown 返回值 tag 仍在 `%rdx`，间接调用点
  按静态签名（不含浮点，即不经 shim 重排）读取。
- **RC**：`gen_rc_unknown_tagged` 三入口按新堆判定内联（0→探针、1/2/6/7→
  跳过、3..5/>=8→直接 inc/dec/temp）。UNK 恒回退旧探针 —— 正确性不依赖
  tag 传播完备。

## 3. 运行期类型码与接口

`pa_any_type(v, tag)` → PA_TYPE_NONE/BOOL/INT/FLOAT/STR/BYTES/LIST/DICT/
TUPLE/SET/OBJECT/CLASS/OTHER。有精确 tag 用 tag；HEAP 读头 kind；UNK 先
探针。`pa_any_type_is(v, tag, code)` 是 isinstance 谓词：object 匹配一切，
int 接受 bool（CPython 口径），其余精确相等。类型码与 `src/tag_defs.h` 的
PaTypeCode 逐字对应。

`pa_repr_any_t(v, tag)` / `pa_print_any_repr_t(v, tag)`：None→None、
bool→True/False、int→十进制、float→format_double、str→带引号 repr、
bytes→b'...'（CPython 的引号选择与 \xNN/\t/\n/\r/\\ 转义）、
tuple→(...)（单元素尾逗号）、set→{...}/set()、类对象→<class '...'>；
HEAP/UNK 回退旧探针路径（容器递归仍按探针口径）。

## 4. pprint

stdlib/pprint.py（新模块，注册进 kModuleTable）：pformat/pprint/
PrettyPrinter。口径：单行 repr 放得下就用它；否则按 CPython 的断行缩进
（容器续行 +1、dict 值从键后对齐、tuple 尾逗号）。为了在「未知值不支持
方法调用」的 pa 里递归取 dict 键值，新增内部 intrinsic `__pa_any_items`
（→ `pa_any_dict_items`，物化 (key,value) 对）；迭代 unknown 容器用
`pa_any_iter_list_t`（tag 版，未进 RC 索引的标量元组也能迭代）。

pprint 是独立模块，**它的函数形参 tag 来自 §2 的侧信道**；这条链跑通
才可能让 pprint(True)/pprint((1,2))/pprint({1,2})/pprint(b"xy") 与 CPython
一致。

## 5. 已知边界（本轮明确不做 / 不可做）

1. **容器元素没有逐元素 tag**：容器只存 8B 值 + 1-bit 堆位图，因此从容器/
   字段/dict 值取出的 unknown 只能按头 kind 判型：嵌套 bool/None→int、
   tuple→list、set→dict、bytes→str。顶层值与静态类型已知的边界不受影响
   （328-331 按这个边界设计）。
2. **虚调用（VCall）不传 tag**：vtable 调用点没有可信的静态方法签名，
   被调形参一律 UNK。所以 PrettyPrinter 的方法版 pformat/pprint 对
   tuple/set/bytes 参数退化成探针口径（模块函数版 pprint/pformat 正常）。
   328-330 只测模块函数入口。
3. **异常类当值不可用**：内建异常类在运行时只有名字（`pa_exc_register`
   只登记父子名），没有类对象；unknown 里仍是 0。contextlib.suppress 需要
   类值同一性 + 比较，与 traceback 对象一起属 P4。本轮跳过并如实报告。
4. **pprint 不排序 dict 键**：CPython 默认 sort_dicts=True。pa 的 unknown
   dict 取不到可排序的键视图（不支持方法调用、排序 unknown 元素按机器字），
   所以按插入顺序输出；用例用已排好序的键。
5. **既有 leak（非本版引入）**：list 在 len>0 时从 kind1（全堆）转 kind2
   （混合）不新建位图，销毁回退全局索引探针，未进索引的**标量元组**漏
   dec。flag-off 基线同样复现（最小用例：`list[object]` 先 append 元组再
   append 字面量 float）。328-331 规避该形状（堆/标量分列表）。修复属 list
   运行时核心（影响所有容器写入），不在 P2 范围。

## 6. 闸门（实测）

- **flag-off asm 逐字节不变**：**117/117 文件 cmp IDENTICAL**（bench 全量
  .py：compare 9 + realistic 6 + builtins 54 + daily 23 + unknown 5 +
  json/re 20）。P3 的 5 个 unknown 基准也逐字节同（新 tag 只在门控开发射）。
- **P3 不变量**：262-266 flag-on/flag-off 全 ran ok；bench/unknown u1/u3
  flag-on/off 输出一致（450000.0 / 1480000.0）。
- **全套件（清空 modcache 后串行跑，避免并发共享缓存干扰）**：
  normal flag-off **323 ran ok EXIT=0**；normal flag-on **323 ran ok
  EXIT=0**；ASan（PYADAMAS_TEST_ASAN=1，JOBS=1）**323 ran ok EXIT=0**
  （含新用例 328-331，runners 按文件头指令自动带 PYADAMAS_TYPE_TAGS=1）。
- **difftest（O0 vs O2）**：305 个用例，**0 不一致，EXIT=0**。
- **语料对拍（flag-on vs off，350 文件：tests/run + bench/daily/realistic/
  compare/unknown/json/re）**：**343/350 stdout 逐字节相同、退出码全 0**。
  7 个不同全部有解释：4 个新用例（328-331）在“裸 off”对照里因缺用例指令
  而按 0.339 语义输出（套件里两者都带标签，323 全过）；bench_re 只差计时列
  （本征非确定）；251/252 argparse 只差 usage 里的程序名（自建对照把
  off/on 产物命名成两个不同的 -o 路径，属对照工具伪差；同路径手工比对逐
  字节同）。**rc 计数不逐字可比**：tag 快路径会把 pa_rc_*_unknown 换成
  pa_rc_inc/dec 并跳过非堆标量（计数是“调用次数”），这是设计使然；改为
  核对**实际释放数**：除 4 个新用例（对照未带指令）外，free_on **0 个回退**，
  且 140/144/162/183/185/188/189/237/239/247 等 free 更多 = 标签修掉了
  探针漏判导致的潜在漏释放。
- **ASan detect_leaks=1 钉子**：13_gc / 174_gc_construct_roots /
  88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend，
  flag-off 与 flag-on 各一遍 **12/12 全 0**；新用例 328-331 各
  **4/4 全 0**。
- **CPython 对拍**：328-331 的 expect 逐行由 CPython 3.14 生成（含 pprint
  断行/缩进输出），O0/O1/O2 输出一致（difftest 0）。
- **check_size**：**无新增超限**。新拆 `src/codegen_call_abi.cpp`（间接/
  虚调用发射 + outgoing_bytes，194 行）；codegen_impl.h 1195、codegen_call
  1169、codegen_block 1153 均 < 1200；超限文件仍是基线那 5 个
  （codegen_regalloc/driver_build/ir_ops.h/lower_impl.h/typecheck_types.h）。
- **缓存失效**：manifest 加 `typetags` 行；切换开关必然重建（否则旧产物
  tag 编号/判定混用会读错类型）。

## 7. 提交

- `dfe96fb` tag 扩展骨架（NONE/BOOL + 堆子类 tag，门控默认关）
- `295fe31` 运行期类型码 + tag 版 repr/isinstance
- `b6ba920` 跨模块/间接 tag 侧信道 + per-param 布局 + stdlib/pprint.py
- `c365a48` 回归 328-331 + 用例级 type-tags 指令 + indirect unknown 返回 tag
- `2c9d7e9` 拆 codegen_call_abi.cpp（文件规模不新增超限）
- （文档/CHANGELOG：另一次提交）
