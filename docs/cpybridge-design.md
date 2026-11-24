# cpybridge —— CPython 接入先验设计（薄原型 + 实测）

状态：先验原型（分支 cpybridge，基于 0.302 / c467230）。不是生产轮：
目标是把路径走通、把坑标出来，完备性留给后续轮次。

> **归属（2026-11-24 决定）**：cpybridge **只作逃生口**——迫不得已要调 CPython 生态时才用，
> **不做产品化**（协议完备性、关键字参数、IPC 性能都不投）。因此「借 pip 生态」不构成
> 「不必自己做包管理」的理由：包管理 registry 仍是主线（见
> [package-management-design.md](package-management-design.md) §1.4 与 §6）。
> 本文档继续作为「已知限制」的事实来源；新增能力不在此列。

## 1. 目标与非目标

目标：pyadamas 程序里写

    from python import math
    print(math.sqrt(2.0))          # 1.4142135623730951

调用真正发生在 CPython（服务器 python3 = 3.14.4）里，结果回到 pyadamas
原生值域。传输层是 **CPython 子进程 + 管道 IPC**，不嵌入 libpython：
没有 GIL 互嵌、没有 Py_INCREF/DECREF 跨边界所有权、没有解释器初始化
与主程序 RC/GC 的耦合。codon 的 libpython 嵌入路线只借鉴语义面（§7）。

非目标（先验阶段明确不做，见 §8）：kwargs、持久 handle 代理、
模块级 from python import name、裸属性读（sys.version）、worker 池、
调用超时、生成器/async 交互、Windows。

## 2. 语法面

- from python import <module> [as <alias>]，可逗号分隔多个。
  例：from python import math / from python import json as cjson。
- 点子模块链调用：cpyos.path.basename("/a/b/c.txt") —— 编译期把
  代理名 + 属性链拼成完整模块名（os.path），仍是一条 IPC。
- 拒绝（编译期报错，口径明确）：
  - import python / import python.x → 提示改用 from python import；
  - from python import * → 不支持（代理没有可枚举的签名表）；
  - 裸属性读（math.pi 当值用）→ 只支持调用；getattr 帧是后续工作；
  - 关键字参数 → marshalling 协议先验只带位置参数；
  - 给代理属性赋值（math.x = 1）→ 拒绝。
- python 是保留桥命名空间：模块表里登记为 Native（无源文件、无运行期
  模块对象），与同名用户模块冲突时按既有模块表口径处理。
- **lexer/parser 零改动**：from python import 本来就是合法 ImportFrom，
  python 只是普通 NAME。全部语义在 typecheck/lower 的模块分支里加。

## 3. 语义与类型规则

### 3.1 模块代理是什么类型

ModuleInfo 增加 cpy_proxy 标志（src/ir/module.h）。builtin_module("python")
返回根代理；builtin_module("python.<任意名>") 按需生成子代理（开集，
不预登记 stdlib/第三方包名）。代理没有 globals/classes 签名表。

typecheck 放行规则：
- stmt_import_from：把子代理挂进 cur_->modules[bind]（模块别名，
  不是变量）。hoist_scope_bindings 对 from python import 跳过
  bind_alias —— 否则一个 unknown 变量会抢先命中 lookup_module_alias
  （与 Import 同口径，这是探索中发现的第一个坑）。
- 调用（call_attr / 链式调用）：实参照常求值，结果 **t_unknown()**。
  不猜签名：CPython 侧签名编译期不可知，宁可漏报不可误杀。
- 属性/下标/len 等作用于 unknown 结果时，走 pyadamas 既有的 unknown
  保守路径。

### 3.2 调用 marshalling 子集（双向）

| 类型 | 去程（pyadamas → CPython） | 回程（CPython → pyadamas） |
|---|---|---|
| None | 静态 desc n → null | null → 0（unknown 里 None 就是 0） |
| bool | desc b → true/false | true/false → 0/1（与 int 不可分，见 §3.3） |
| int | desc i → {"i":N}（int64） | {"i":N} → 裸字；超 int64 → OverflowError |
| float | desc f → {"f":x}（format_double 最短往返） | {"f":x} → **CPYFLOAT 盒**（§3.3） |
| str | desc s → {"s":"..."}（ensure_ascii 口径） | {"s":"..."} → 真 str |
| bytes | desc y → {"y":"base64"} | {"y":"..."} → 真 bytes（str_new_bytes） |
| list | desc (l<d>) → {"l":[...]} | {"l":[...]} → 真 list（elem_kind 2） |
| tuple | desc (t<d>...) → {"t":[...]}（定长；变长 tuple 拒绝） | {"t":[...]} → list 表示（运行期同构，§3.3） |
| dict | desc (d<k><v>) → {"d":[[k,v],...]}（键不限 str） | {"d":[...]} → 真 dict；全 str 键用 key_kind 1（内容哈希），否则 raw（§3.3） |

不可 marshalling 的对象（文件句柄、迭代器、生成器、自定义类实例、
模块对象、函数……）：**明确拒绝**，不硬做。
- 回程：worker 的 enc() 抛 TypeError("cannot marshal <type> to pyadamas")，
  映射成 pyadamas TypeError，try/except 可捕获。
- 去程：静态类型不在子集内 → 编译期报错；unknown 动态探针遇到
  非 str/list/dict/float 盒的堆对象 → 运行期 TypeError。
- handle 代理（codon pyobj 路线）是后续工作：需要运行期对象表 +
  生命周期协议 + 方法调用帧，先验阶段不碰。

### 3.3 unknown 值域的固有歧义（探索中确认的硬约束）

pyadamas 的 unknown 机器字**分不开 int/bool/float/None**（rt_core.c
pa_print_any 原注释），str/bytes 运行期同构（区别全在静态类型），
tuple/list 同构（都是 PyAdamasList）。桥是第一个真正的动态值来源，
这些歧义第一次变成用户可见问题。先验的处置：

- **float 盒**：新 RC kind PYADAMAS_RC_CPYFLOAT（8），payload 8 字节
  double 位模式。回程 float 装盒；print/探针/marshalling 按 kind 识别。
  必须走 **tracked** rc_alloc：rc_alloc_untracked 不进 rc_index，而索引
  开启时 unknown 探针只认索引（无回退），盒会变成「看不见的堆值」
  （实测：print 出指针整数）。这是第二个坑。
- **print_any 扩展**：原实现只认 str。现按共享探针 pa_unknown_is_heap
  分派 str / CPYFLOAT / LIST / DICT（后三者走 rt_cpy.c 的 repr：容器内
  字符串带引号，与 CPython 一致）。标量仍按整数输出。
- None/bool 进 unknown 后与 int 不可分：回程 None→0、bool→0/1，
  print 出来是 0/1 而不是 None/True。**静态类型已知的位置不受影响**
  （print 按静态类型分派）。文档写明，不硬做动态标签（那会改动
  unknown 的全局表示，远超先验范围）。
- 去程 unknown 实参（动态探针）：0 按 None 发；str/bytes 按 str 发；
  tuple/list 按 list 发。静态类型已知的实参走描述符，无歧义。
- tuple 回程打印成 [..] 而非 (..)（运行期无 tuple 标记）。
- dict 回程非 str 键（int 键按字相等可用；float 盒/容器键按指针哈希，
  等值查不到）——先验限制。
- unknown 结果的下标/算术：pyadamas 既有 unknown 路径把下标当 list
  （dict 结果 d["k"] 会 IndexError）、bin.add 当整数加。先验不扩展，
  测试避开，文档写明。float 盒参与 unknown 算术是后续工作。

### 3.4 异常映射

worker 捕获 Exception，应答带 **type(e).__mro__ 的名表** + str(e)。
C 侧取第一个命中 pyadamas 内置异常名表的（与 typecheck
is_exception_name 常用子集对齐，rt_cpy.c cpy_exc_known），pa_raise_c
抛出 → pyadamas try/except 按既有名字/父链匹配捕获。

- math.sqrt(-1) → ValueError（CPython 3.14 消息 "expected a nonnegative
  input, got -1.0"，与原生 rt_math 的 "math domain error" 不同——
  消息来自真正的 CPython，这正是桥的证明）。
- json.loads("{bad") → JSONDecodeError 不在内置表 → 沿 MRO 落到
  ValueError。这是 MRO 映射的价值：不需要预登记所有 CPython 异常。
- 全不命中 → RuntimeError，消息带 CPython 原名（"CPython <Name>: msg"）。
- SystemExit/KeyboardInterrupt 不被 worker 捕获（只 catch Exception）：
  worker 退出 → 崩溃路径报 RuntimeError。先验口径，后续可细化。
- worker 死亡（EOF/EPIPE/id 失配）→ RuntimeError + stderr 尾诊断，
  收尸关 fd；先验不自动重拉（模块状态一致性优先）。

## 4. 传输协议

### 4.1 选型：带标签 JSON 帧，不是 pickle

权衡（任务要求写明理由）：
- **C 侧发射成本**是决定因素。去程序列化必须在 C 运行时里做（实参是
  pyadamas 机器字）。JSON 用字符串拼接即可发射（rt_json.c 已验证这条
  路：描述符驱动的机械遍历）；pickle 是栈虚拟机 opcode 流，C 侧手写
  发射器复杂且易错（协议版本、memo、长整型编码），收益不抵成本。
- 回程解析：JSON 手写递归下降 ~200 行（rt_cpy.c cj_parse），标签自描述；
  pickle 的 C 侧解析器同样要写一个 VM，复杂度更高。
- 覆盖面：pickle 能序列化自定义类实例——但先验**本来就要拒绝**这些
  （handle 代理是后续工作），子集内 JSON 标签全覆盖，且显式区分
  int/float、tuple/list、str/bytes（裸 JSON 分不开 1 与 1.0，标签解决）。
- 可调试性：帧是一行文本，python3 -c 脚本可独立喂帧手工测试；
  pickle 是二进制，排障成本高。
- 安全：两边都是自己人，但 JSON 解析器攻击面天然小于 pickle
  （pickle.loads 任意流 = 任意代码执行；后续若开放不可信输入，
  JSON 帧不需要额外沙箱）。
- 复用：描述符语法、字符串逃逸（ensure_ascii/\uXXXX 代理对）、
  float 最短往返（format_double）全部与 rt_json.c 同口径，代码可对照。
  没有直接调 pa_json_dumps/loads：它们按静态描述符解析成**静态类型**，
  桥的回程是动态标签值，需要自己的 DOM 解析 + 动态解码。

### 4.2 帧格式

一行一帧（\n 分隔），UTF-8，worker 侧 json.loads/json.dumps
（ensure_ascii 默认开，C 侧解析器只见 ASCII + \u 逃逸；接受 Python
json 的 NaN/Infinity 扩展）。

请求：
    {"id":1,"m":"math","f":"sqrt","a":[{"f":2.0}]}
应答（成功）：
    {"id":1,"ok":true,"v":{"f":1.4142135623730951}}
应答（异常）：
    {"id":2,"ok":false,"e":["ValueError","Exception","BaseException"],"msg":"..."}
退出：
    {"cmd":"exit"}

值标签：null / true / false / {"i":int} / {"f":float} / {"s":str} /
{"y":base64} / {"l":[...]} / {"t":[...]} / {"d":[[k,v],...]}。
id 单调递增，C 侧校验应答 id（失配 = 协议脱同步 → 崩溃路径）。

### 4.3 worker 生命周期

- **懒拉起**：首次 pa_cpy_call 时 pipe×3 + fork + execlp("python3",
  "-u","-c",<嵌入脚本>)。脚本是一段 C 字符串常量（rt_cpy.c CPY_WORKER，
  ~60 行 Python：dec/enc + 读行循环 + importlib 缓存模块）。
  python3 取自 PATH（后续可加 PYADAMAS_PYTHON 环境变量覆盖）。
- **复用**：全进程一个 worker，模块在 worker 侧 importlib 缓存。
  SIGPIPE 忽略（写断 pipe 拿 EPIPE 走崩溃路径）。
- **退出 drain**：atexit 发 exit 帧 + 关 stdin（EOF 兜底），500ms 内
  waitpid 收尾，超时 SIGKILL。worker 不碰 pyadamas 对象，与 RC/GC
  shutdown 无耦合。
- **崩溃报错**：写 EPIPE / 读 EOF / id 失配 → 收 stderr 尾（非阻塞
  drain，每次调用前也机会性 drain 防警告写满 pipe）→ 收尸 →
  RuntimeError("cpybridge: CPython worker ...: <stderr>")。
- **无超时**（先验）：worker 卡死会挂住调用方。poll 超时 + 杀 worker
  是后续工作（rt_subprocess.c 有现成超时模式可抄）。

### 4.4 参数描述符（编译期静态类型 → 运行时机械遍历）

lower/cpy.cpp cpy_desc 把每个实参的静态类型编码成自定界描述符
（rt_json 同款语法）：n/b/i/f/s/y/?、(l<d>)、(t<d>...)、(d<k><v>)。
逐个拼接（自定界，无需分隔符），随实参打包列表一起传给运行时。

- 实参列表是 **kind 0 借用打包**（itertools pack_seqs 同口径）：
  pa_list_append 不做隐式 retain（所有权由降级器决定），kind 0 销毁
  不 dec 元素，元素引用仍归原主（槽/临时栈）。这是第三个坑：
  若按 kind 2 建列表又不 retain，销毁会过度释放借来的引用。
- ?（unknown 实参）：运行时 pa_unknown_is_heap 探针 + kind 分派
  （§3.3 的歧义口径）。支持链式调用：内层桥结果（unknown）直接
  作外层实参（209 用例验证）。
- 不支持的静态类型（类实例/函数/Optional/变长 tuple…）→ 编译期报错。

### 4.5 回程解码与 RC 所有权

cj_parse 先把整帧解析成纯 malloc 的 JSON DOM，再解码成 pyadamas 值，
最后 cj_free —— 异常路径（longjmp）不漏 DOM（先摘异常信息再 free 再抛）。
- 容器构造沿用 rt_json 的 RAII 口径：pa_rc_temp + PA_PIN_SCOPE，
  正常路径任意出口自动 unpin，异常路径临时栈 cleanup 兜底。
- pa_list_append 是**所有权转移**（不 inc）；pa_dict_set **消费调用方
  的入表引用**（不 inc，覆盖时 dec 多余新键）。dict 解码：键先进
  kind 2 临时列表（RC 托管），全 str 键判定后建 dict，逐对
  「先 own_inc 键 → set → unpin 值」，最后销毁临时键列表还它那份引用。
- 结果交回生成代码：RcTempUnknown 按注册表接管初始引用（标量空操作）。
  用了桥的模块必须保留 unknown 索引维护：ir/ir.cpp 的**两级**分析
  （needs_unknown_index 与 can_skip_index_maint→index_readonly）都把
  CpyCall/RcTempUnknown 拉黑。只改一级时实测 float 盒 print 出指针
  整数（index_readonly 让运行时新分配不进索引）——第四个坑。

## 5. 落地路径（文件清单）

前端（语法零改动，语义在模块分支）：
- src/ir/module.h：ModuleInfo.cpy_proxy。
- src/driver/build.cpp：模块表加 {"python", Native}；program_module /
  check_load 对 python.* 前缀按需返回 builtin_module 代理。
- src/typecheck/builtin_sigs.cpp：builtin_module 生成根/子代理。
- src/typecheck/typecheck.cpp：hoist 对 from python import 跳过 bind_alias。
- src/typecheck/stmt_forms.cpp：stmt_import_from 代理分支（含 * 拒绝）；
  stmt_import 拒绝 import python。
- src/typecheck/expr.cpp：代理裸属性读拒绝（只支持调用）。
- src/typecheck/call_forms.cpp：代理调用放行 unknown + kwargs 拒绝；
  点子模块链调用分支。
- src/typecheck/stmt.cpp：代理属性赋值拒绝。

降级：
- src/lower/lower.cpp：scan_globals 对 from python import 不占全局槽。
- src/lower/import.cpp：lower_import_from 代理别名登记（不发
  ImportModule）；lower_import 拒绝 import python。
- src/lower/method.cpp：代理调用 → lower_cpy_call；点链拼模块名。
- src/lower/cpy.cpp（新）：cpy_desc + lower_cpy_call（kind 0 实参打包
  + 描述符拼接 + CallRt CpyCall + RcTempUnknown）。
- src/ir/ops.h：RtFn::CpyCall + rt_name → pa_cpy_call。
- src/backend/call_rt.cpp：CpyCall 归 gen_rt_i4（四个整型/指针参数）。
- src/ir/ir.cpp：两级 unknown 索引分析拉黑 CpyCall/RcTempUnknown。

运行时：
- runtime/rt_cpy.c（新，~1100 行）：worker 脚本 + 生命周期 + 帧发射 +
  描述符 marshalling + 动态探针 + JSON DOM 解析 + 解码 + 异常映射 +
  unknown repr/print 扩展。POSIX 实现；_WIN32 分支明确报 OSError
  （CreateProcess 路径与 rt_subprocess.c Win 分支同构，后续补）。
- runtime/rt_internal.h：PYADAMAS_RC_CPYFLOAT + pa_cpy_call /
  pa_print_cpy_value 声明。
- runtime/rt_core.c：pa_print_any 改用共享探针 + 桥类型分派。
- CMakeLists.txt：RT_PARTS 加 cpy；编译器源加 lower/cpy.cpp。

与 cimport 的界限：cimport 是**编译期 C ABI 直调**（.cpya 声明 →
CallExtern，进程内、零 marshalling、类型静态确定）；cpybridge 是
**运行期跨进程值 marshalling**（签名不可知、结果 unknown、每次调用
付 IPC 成本）。两者不共享降级路径，只共享「模块别名」这一前端机制。
原生 stdlib 模块（math/json/os…）继续走既有原生降级；from python
import 的同名模块是**另一个绑定**（代理），互不干扰。

## 6. 成本实测（Debug 编译器，-O2 产物，python3 3.14.4，本机）

bench/cpy_ipc.py，N=3000 次循环取均值，三轮：

| 负载 | IPC | pyadamas 原生 | 倍数 | CPython 进程内 |
|---|---|---|---|---|
| 首次调用（含 spawn） | 36.5 / 39.7 / 38.0 ms | — | — | — |
| os.getpid | 26.2 / 25.5 / 25.5 µs | 0.59 µs | ~44× | 0.36 µs |
| math.sqrt(2.0) | 26.8 / 30.0 / 28.8 µs | 0.002–0.006 µs（内联 sqrtsd） | ~10^4× | — |
| json.dumps(100 元素 list) | 103 / 130 / 122 µs | 8.1–13.1 µs | ~10–13× | 8.3 µs |

解读：
- 轻调用 IPC 地板 ~25µs（两次 pipe 往返 + worker json 解析/发射 +
  C 侧 marshalling/解析），与 CPython 进程内 getpid（0.36µs）差 ~70×：
  成本几乎全在传输，不在 CPython 执行。
- 中负载（100 元素 list）IPC ~100–130µs，payload 序列化开始占比；
  与原生 json（C 实现）差 ~10×，与 CPython 进程内 json 差 ~12–15×。
- 原生 sqrt 被 codegen 内联成一条 sqrtsd，对比倍数极端（10^4），
  说明**热循环里逐元素跨桥是灾难**。

适用边界（写进用户文档的口径）：
- 适合：粗粒度库调用（一次调用干 ≥ms 级的活：绘图、DataFrame 批处理、
  模型推理、复杂正则/解析），或**批量数据一次过桥**（codon 文档同款
  建议：convert a batch once, work natively）。
- 不适合：热循环逐元素调用、把桥当数学库用（原生 math 快 10^4×）。
- 量化口径：~25µs/次 ⇒ 单 worker 上限 ~4 万次调用/秒；每次调用
  搬运的数据越大，传输占比越高（JSON 文本膨胀，bytes 走 base64 +33%）。
- spawn ~37–40ms 一次性：短命进程（<100 次调用）桥不划算。

## 7. codon 借鉴与取舍（.ref_proj/codon，只读研读）

研读面：stdlib/python.codon + stdlib/internal/python.codon（2375 行
C API 声明）、docs/integrations/python/python-from-codon.md、
test/python/pybridge.codon、parser/visitors/typecheck/import.cpp
（from python 在 typecheck 阶段变换成 internal.python 调用）。

借鉴：
- **语法面完全一致**：from python import <module> [as alias]，
  支持点模块（matplotlib.pyplot）。codon 在 typecheck 的 import
  visitor 里把 from python 变换成内部模块调用——与我们在
  stmt_import_from/lower_import_from 加代理分支同构。
- **异常携带类型信息**：codon 的 PyError 带 pytype + message，
  test_pythrow 断言 "ValueError:foo"。我们简化成 MRO 名表 → 内置名
  映射（pyadamas 的 except 按名字匹配，不需要 PyError 包装类）。
  codon 还有 native exception conversion（FileNotFoundError 等双向
  转换）——后续可借鉴做 OSError.errno 级映射。
- **转换是拷贝不是视图**：codon 文档明确 list[int](pyobj) 产生原生
  副本，改副本不影响 Python 对象。我们的值 marshalling 天然是拷贝
  语义，文档同口径写明。
- **批量转换性能哲学**：codon 文档「convert a batch once and do the
  subsequent work natively」——与 §6 适用边界一致。
- **@python 装饰器 + 返回标注驱动转换**：codon 用标注决定
  __from_py__ 目标类型。对应我们的后续方向：给桥调用加返回类型
  标注（def f() -> int 包装）以摆脱 unknown，先验不做。

取舍（不学）：
- **pyobj 持久 handle**：codon 的核心是 Python 对象留在 CPython 堆，
  codon 侧持引用，所有操作走 C API。我们选**每次调用值 marshalling**：
  没有跨边界引用所有权（GIL/DECREF 全免），代价是不可 marshalling
  对象直接拒绝 + 链式调用重复过桥。先验阶段这是正确的复杂度取舍；
  handle 代理列为后续工作（需要运行期对象表 + 释放协议）。
- **libpython 嵌入 / CODON_PYTHON 配置**：不学。子进程路线只需要
  PATH 里的 python3，部署/版本隔离/崩溃隔离都更简单。
- **__to_py__/__from_py__ 用户钩子**：先验不开用户扩展面。

## 8. 已知限制、风险与后续工作

先验明确拒绝/限制（都有编译期或运行期明确报错，不静默错）：
1. kwargs：协议只带位置参数。后续：帧加 "k":{name:val}，worker
   fn(*args, **kwargs)；描述符侧按关键字名拼接。
2. handle 代理：文件句柄/迭代器/自定义实例拒绝（TypeError）。
   后续：worker 侧对象表（id → PyObject*），C 侧新 RC kind 持 id，
   方法调用帧 + 显式 close/__del__ 协议 + worker 重启时的失效处理。
   这是最大的一块后续工作，codon pyobj 是参照。
3. 模块级 from python import name（函数/常量绑定）与裸属性读
   （math.pi / sys.version）：需要 getattr 帧 + 值绑定降级。
4. 结果类型永远是 unknown：float 盒已解决打印，但 unknown 算术/
   下标/dict 键查找仍受限（§3.3）。后续：返回类型标注、或桥调用
   点的类型收窄（样本值口径，类似原生 json.loads(text, sample)）。
5. 超时：worker 卡死挂住调用方。后续：poll 超时 + SIGKILL +
   重拉（rt_subprocess.c 超时模式）。
6. worker 池 / 并发：单 worker 串行；asyncio 集成需要每 loop 一个
   worker 或请求多路复用（id 已预留）。
7. 生成器/async：迭代器不能过桥；后续可加「worker 侧 drain 成 list」
   的便利帧（语义是拷贝，文档写明），真流式交互需要 handle 协议。
8. 安全：worker 继承父进程环境/权限，import 任意模块 = 任意代码
   执行（与用户自己写 Python 同权）。不可信模块名/参数场景需要
   沙箱（seccomp/独立用户），先验不做。帧解析器有 256 层深度闸。
9. Windows：rt_cpy.c 明确报 OSError；CreateProcess + 匿名管道路径
   与 rt_subprocess.c Win 分支同构，后续补。
10. int64 边界：CPython int 超 int64 → worker OverflowError（回程）；
    去程 desc i 本来就是 int64。float NaN/Inf 全链路支持（format_double
    ↔ Python json 扩展 ↔ C 解析器）。
11. dict 非 str 键：raw 哈希按指针（float 盒/容器键等值查不到）。
12. unknown 索引维护成本：用了桥的模块强制保留索引（两级分析拉黑），
    放弃 exact-RC 优化。先验正确性优先；后续可给桥结果加静态类型
    信息来解锁。
13. 崩溃后重拉：worker 死后 w_pid=-1，下次调用 cpy_spawn 会拉一个
    **干净** worker（模块状态丢失，文档写明）；崩溃当次报 RuntimeError。
14. 每调用一次 malloc DOM + 帧缓冲：先验不池化；热路径优化后续。

## 9. 验收与测试

tests/run/207–211（# --- expect --- 单 # 口径），O0/O2 全过：
- 207_cpy_math：sqrt(2.0)=1.4142135623730951、sqrt(144.0)=12.0、
  floor(3.7)=3、factorial(6)=720；sqrt(-1) ValueError 捕获；
  except Exception 兜底捕获。
- 208_cpy_json：dumps(list/dict/tuple/非ASCII str)、loads 回 list/dict
  （float 盒 repr、len）。
- 209_cpy_str：os.path 点链调用、getcwd（len>0）、unknown 结果链式
  传参（dumps(basename(...))）。
- 210_cpy_exc：ValueError / JSONDecodeError→ValueError（MRO）/
  TypeError / ZeroDivisionError。
- 211_cpy_marshal：None/bool/bytes(b64)/int/float/list 拼接 +
  50 次循环 worker 复用。

全量回归（tests/run_tests.sh，Debug 构建）：227 ran ok；
118_suspend_expr / 154_nested_await 两个 OUTPUT DIFF 为 **Debug 构建
基线既有问题**：base c467230 的独立快照 Debug 构建（git archive 到
/tmp/base-check，不碰主树）复现完全相同的崩溃——编译器侧
std::vector<unsigned char> 越界断言（SIGABRT，exit 134），与桥无关。
Release 口径不触发断言。建议单独立项修（疑似 lower_gen_spill /
regalloc 的块编号越界），不阻塞桥集成。

## 10. 集成到 master 的建议步骤与风险

步骤（0.303 正确性轮落地后）：
1. rebase cpybridge 到 0.303 后的 master；冲突面预估：
   - ir/ir.cpp 两级索引分析：0.303 若动 exact-RC 判定需重对拉黑清单；
   - rt_core.c pa_print_any：0.302 的 saw_heap/探针改动已对齐，
     若 0.303 再动探针需重对；
   - typecheck hoist / lower scan_globals：小改，易合。
2. 先跑全量 + ASan（PYADAMAS_TEST_ASAN=1）：桥的 RC 路径
   （float 盒/容器解码/异常 longjmp）重点看；detect_leaks=1 单跑
   207–211（DOM/帧缓冲是 malloc，泄漏检查口径要区分）。
3. Debug 基线的 118/154 崩溃单独立项（见 §9），不阻塞桥集成，
   但 CI 若开 Debug+断言会红。
4. 文档：README/CHANGELOG 记「CPython 桥先验」；用户文档口径
   用 §6 适用边界 + §8 限制清单。
5. 后续轮次优先级建议：返回类型标注（摆脱 unknown）> kwargs >
   getattr/常量绑定 > 超时 > handle 代理（最大块，单独立项）。

风险：
- unknown 值域歧义（§3.3）是表示层硬约束，桥把它暴露给用户；
  在返回类型标注落地前，用户文档必须写明 print/算术的限制。
- 强制索引维护对含桥模块的性能影响未量化（先验正确性优先）。
- worker 继承环境：PYTHONPATH/PYTHONHOME 会改变 worker 的模块解析
  （与 codon 文档同款排障口径：sys.version/sys.path 先查环境）。
- 子进程僵尸：atexit drain 已覆盖正常退出；父进程被 SIGKILL 时
  worker 靠 stdin EOF 自退（脚本读行循环 EOF 即 break），无孤儿。
