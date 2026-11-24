# P4 语言缺口 块1-4 —— 异常类当值 + suppress/closing；file 协议泛化 + io.StringIO/BytesIO；traceback/sys.exc_info；dataclass 编译期展开（0.343 块1+2 / 0.346 块3+4）

> 范围：docs/numeric-types-design.md §3/§4 P4 的第一块（最高 ROI、最自包含）。
> 基线 master 9b4a6f9 / 0.341（含 P2 CLASS=11 预留），独立 worktree
> /tmp/wt-lang1（分支 lang1），不推 master、不动主树。门控
> `PYADAMAS_LANG_GAPS=1`，**默认 0**；开启时驱动强制 `type_tags=true`
> （CLASS tag=11 等精确堆子类标签是 P4 的表示基础）。0.342 预留 big1
> （P3 BigInt+f32），集成按落地顺序重编号。域隔离：不碰 rt_big/Big
> TyKind/BIG tag=12/f32。
>
> **结论（先说）**：块1+2 落地。门控开时内建/自定义异常类作运行期类对象
> 值，过 unknown 带 CLASS tag=11 不退化成 0；contextlib.suppress/closing
> 与 CPython 逐字节一致（tests/run/340）；file 协议泛化 + io.StringIO/
> BytesIO + csv 互操作与 CPython 逐字节一致（tests/run/341），O0/O1/O2。
> 门控关时 tag 编号/
> 判定/所有生成代码逐字沿用基线（410 个基准/语料 asm cmp 全同）。
> 块2（file 协议/io.StringIO）、块3（traceback/sys.exc_info）、块4
> （dataclass 编译期展开）后续落地，本文档随之扩展。

## 1. 表示：异常类对象单例

- **类型**：内建异常名值位置 → `ClassObj[Exception[name]]`（没有
  ClassInfo：内建异常无编译期布局，内层 Exception 记类型名）。自定义
  异常类名值位置 → `ClassObj[Class[cls]]`（与普通类一致；门控关时异常
  类保持 Class 型，raise/except 名字机制不动）。static_tag_of(ClassObj)
  → CLASS=11（P2 已实现，本轮只负责让值存在）。
- **运行期**：`pa_exc_class_obj(name)`（rt_lang.c）按名字缓存单例：
  `pa_class_new(cid=-1, ctor=0, vtable=NULL, name=异常名)`，魔数
  PYADAMAS_CLASS_MAGIC，kind OBJ。注册表持创建时那份永久引用，调用方
  拿 +1；**注册表槽地址登记成 GC 根**（C 全局不在帧槽/临时栈里，major
  GC 看不见——与 rt_exc.c 的 exc_gc_roots_ensure 同一个坑）。同一名字
  全进程一个单例 → `E is ValueError` 指针同一性成立，跨模块也成立。
- **自定义异常类**：单例由既有类对象机制创建（lower_classdef 的
  need_class_obj 分支）。pass-only 异常类旧路径在 ExcRegister 后早退、
  不建单例——门控开且 value_used 时继续走类对象创建（无方法/字段，
  vtable 计划为空）。value_used 由值位置引用标记（typecheck_expr Name）。
- **名字口径**：类对象 name 存限定名（mod.MyErr，与 MRO 元数据 m[4]
  同口径）；异常父子登记表（pa_exc_register）用裸类名。凡按名字走
  exc_is_a 的地方（pa_exc_class_is_a/pa_exc_subclass/异常值构造）统一
  取限定名末段（lang_plain_name）。**这是本轮抓到的第一个命门 bug**：
  M=MyErr; raise M("x") 曾用限定名构造异常值，except MyErr 按裸名匹配
  不上 → uncaught 崩溃。

## 2. 发射/传播

- **产生点**：Name 降级（lower/expr_assign.cpp）门控开且名字是异常类
  （内建按 is_exception_type，自定义按 classes_）且无本地/全局遮蔽 →
  `ExcClassObj(const_str(name))`，push_temp 接管 +1。门控关时逐字旧样
  （内建落到 LoadGlobal 空槽=0，自定义走 local_class_object）。
- **搬运**：ClassObj 是堆类型， Mov/LoadSlot/StoreSlot/容器存储按既有
  堆路径 retain/release；tag 跟值走（P2 机制，CLASS=11）。容器元素没有
  逐元素 tag（P2 §5.1 边界）——从 list[unknown] 取出的类值 tag=UNK，
  RC 回退探针（注册表认得），str/print/repr 探针路径按魔数认出类对象
  （pa_str_any/pa_print_any/repr_any_into 新增分支，门控关时类对象不
  存在、分支永不命中）。
- **构造**：E("x") / E("a","b") / raise E → ExcClassValueNew/
  ExcClassValueNewArgs（运行期读类名取末段构造 ExcValue）。**不用静态
  内层名**：Exception 的 type_eq 不区分名字，形参回填可能留着首个调用
  点的类型（def f(E): raise E("x") 先传 ValueError 再传 IndexError）。
- **isinstance/issubclass**：isinstance(e, ValueError) → IsException
  （pa_is_exc：扁平 ExcValue 按 type 名走 exc_is_a；富异常实例按 MRO
  首类名末段走 exc_is_a——新增 INSTANCE 分支）。issubclass 异常类×异常
  类（含自定义×内建）→ ExcSubclass 运行期判。静态已知非异常类型直接
  false，不发运行期调用。

## 3. suppress/closing 与方法名→槽登记表

- **with 协议口径**：__exit__ 的 et 是异常类型名串（ExcTypeName），
  ev 是消息串，tb 是 None；正常路径三者皆 None。协议**不改**（改 et
  为类对象会破坏既有 stdlib/用户 __exit__ 的 str 比较）。
- **suppress**：*exceptions 存 unknown 元组；__exit__ 先判 et is None，
  再逐元素 `__pa_exc_is_a(et, E)`（pa_exc_class_is_a：et 名串 + 类对象
  末段名走 exc_is_a）。非类实参 → TypeError（CPython 口径，在 __exit__
  时抛）。零参 suppress() 空元组迭代 → 不抑制。
- **closing**：thing 过 unknown；__exit__ 调 `__pa_close(thing)` →
  pa_method_call0(obj, "close")：file 对象（pa_is_file：RC 注册表 +
  kind OBJ + sizeof(PyAdamasFile)）直调 pa_file_close；类实例沿 MRO
  逐类查方法登记表，命中且槽在该类 own_slots 里（尊重覆盖）才调该类
  vtable 的实现（与 pa_super_lookup 同元数据布局）。不抑制异常。
- **登记表**：门控开时类定义点把本类声明的普通方法按「限定名.方法名→
  槽」登记（pa_method_register，malloc 链，atexit 还）。导入类由定义
  模块登记（门控是程序级 env，所有模块同门控）。**只支持零参方法**：
  带参动态调用的 ABI 装填是块2 file 协议的工作；返回堆值的方法动态调
  会丢引用——closing 只发 close（返回 None），文档化边界。


## 3b. 块2：file 协议泛化 + io.StringIO/BytesIO

- **类型面**：门控开时 `file` 标注/形参/字段收 FileLike——带 read/write
  方法面的类实例（assignable 末尾放宽分支；门控经 lang_gaps_enabled()
  全局开关传入，因为 assignable 是自由函数）。静态类型仍是 file，
  方法检查（typecheck_call_method 的 File 分支）不变。print(file=...)
  的降级检查同口径放宽（lower/print.cpp）。
- **运行期分派（零 codegen 改动）**：pa_file_* 入口前置
  `if (f && !pa_is_file(f)) return pa_fileinst_*(f, ...);`——非 file 值
  委托 rt_lang.c 的实例侧分派（方法名→槽登记表沿 MRO 查表，命中且槽
  在该类 own_slots 里才调，尊重覆盖）。门控关时 file 槽只可能是真
  file，委托分支永不命中；**IR/codegen 一字不改**，asm 天然不变。
  方法 ABI：self + int/str/指针参（file 协议面无浮点参）；默认参由
  调用方显式传规范值（read(-1)/seek(off,0)/truncate(-1)）。堆返回值
  带 +1（与 pa_file_read 同口径）。
- **pa_is_file 探针**（命门，§4.5）：pa_unknown_is_heap（索引 +
  exact-RC 堆链回退）+ kind OBJ + size >= sizeof(PyAdamasFile)。
  File 是最大的 OBJ 种类（Class 40B / ExcValue 40B / Complex 24B，
  File 208B+），下限判定唯一。
- **stdlib/io.py**：纯 pa StringIO/BytesIO（_buf/_pos/_closed 三字段）。
  CPython 口径逐字复刻：write 越界 NUL 填充、seek whence 1/2 只收
  offset 0（非零 OSError "Can't do nonzero cur-relative seeks"）、
  错误消息（Negative seek position N / Invalid whence (N, should be
  0, 1 or 2) / I/O operation on closed file）、readline 按 \n 分行、
  readlines hint 累计、truncate 默认用 pos、getvalue 全缓冲、with
  协议（__exit__ close 不抑制）、closed 属性、readable/writable/
  seekable 恒 True。属性 getter 也进方法登记表（closed 经 file 分派）。
- **csv 互操作**：stdlib/csv.py 源码零改动——writer/reader 的 f: file
  形参经 assignable 放宽收 StringIO，f.read()/f.write() 经 pa_file_*
  分派到实例方法。341 钉 writerow/writerows/reader round-trip。
- **模块注册**：kModuleTable 加 {"io", Python, "io.py", ""}。
## 4. 修复清单（本轮实测抓到）

1. **qualname 异常匹配**（§1）：限定名构造的异常值 except 匹配不上。
2. **repr 模块前缀**：repr(MyErr) 缺 __main__.（CPython 要完整限定名）。
   pa_class_repr 对「裸名在异常注册表」的类显示完整限定名；普通用户类
   保持既有末段口径（门控关行为不变）。判据用注册表而不是改布局：
   异常类对象只在门控开时存在，普通类 repr 一字不变。
3. **name 串泄漏**：异常类单例持永久引用活到 GcShutdown；字符串是索引
   叶子只靠 RC 释放，pa_class_new 旧 release=NULL → name 串永远到不了
   rc==0（340 ASan 实测 6×120B）。补 class_gc_release（pa_gc_unref_child，
   与 exc_value_gc_release 同模式）。普通用户类单例在模块退出时经 RC
   先释放（走 class_destroy），这条回调对它们是纯加法。
4. **富异常实例 str 回归**（语料对拍 277_urllib_errors 抓到）：曾给
   Class 型异常值加 print/repr/str 特判（ClassRepr），但富异常**实例**
   也是 Class 型 → 实例被印成 <class '?'>。门控开时类对象值统一
   ClassObj 型，Class 型异常值只可能是实例——特判多余且有害，删除。
   类对象走既有 ClassObj 分支，实例走 dunder_str（__str__）。
5. **file 探针门控关回归**（全套件 167_stdlib_glob/81_os_extra 抓到，
   交付前命门）：pa_is_file 原用 rc_index_has + size==sizeof。两个坑：
   (a) 无 unknown 值的程序跳过索引维护（ir_module_can_skip_index_maint），
   真 file 也查不到 → pa_file_write 误委托实例分派 → AttributeError
   崩溃（门控关！）；(b) h->size 是池对齐后的 alloc_size，== 会漏。
   改 pa_unknown_is_heap（带 exact-RC 堆链回退，两种模式都认得）+
   size>=sizeof（File 是最大 OBJ 种类，下限唯一）。lang_as_class/
   lang_method_fn 同步换探针。修后 167/81 对 expect 逐字节一致。
6. **341 expect marker 粘行**：钉 expect 时源文件末尾无换行，marker
   粘在最后一行代码上 → run_tests.sh 的 ^# --- expect ---$ 提取空。
   补换行（含 NUL 字节的 expect 用 python 按字节处理）。

## 5. 已知边界（本轮明确不做 / 既有 pa 口径）

1. **多参异常 str() 不引号**：pa 既有口径（join_args 逗号连接，
   CPython 是 args 元组 repr）。340 避开多参 str 输出。
2. **异常实例 e.__name__ 不支持**：扁平 Exception 值的属性只有 args/
   __cause__（既有）；类值的 __name__ 支持。
3. **with suppress() as s 绑定 None**：__enter__ 返回 None 时 as 绑定
   的值 print 成 0（pa 既有 None-VCall 口径，门控关同样复现，非本轮
   引入）。340 不绑定 suppress 的 enter 结果。
4. **closing.__enter__ 返回 unknown**：as g 拿到的值不支持方法调用
   （pa 对 unknown 值拒绝方法调用），请用原引用。closing 的目的是
   保证 close，不是转发方法面。
5. **零参动态方法调用**：pa_method_call0 只装填 self；带参方法/返回
   堆值的方法不在本轮（块2 file 协议泛化时做带参分派）。
6. **富异常类值裸 raise**：零参构造实例后抛；__init__ 带必填参时编译
   报错（CPython 是运行期 TypeError，口径合理但不逐字）。
7. **跨模块异常类值**：单例/匹配机制支持（裸名登记 + 末段口径），但
   340 只测本模块；导入异常类值的构造/raise 全链路留给后续批次验证。
8. **门控关时 suppress 不可用**：异常类值退化成 0（基线语义），
   __pa_exc_is_a 运行期报 TypeError。stdlib/contextlib.py 的 suppress/
   closing 定义始终存在（纯源码），门控关时编译放行、运行期按上述
   口径失败——与 P2 pprint 的门控语义一致。
9. **普通用户类 repr 末段口径**：既有 pa 行为（所有用户类），本轮只
   给异常类开完整限定名，不改普通类（门控关 asm/行为不变闸门）。
10. **truncate(显式负参)**：按默认处理（用 pos）。pa 的 int 默认参只能
    是字面量；None 默认与显式 0 在 int 槽不可分（实测 t.m(0) 被当
    None），无法复刻 CPython「truncate() 用 pos、truncate(-1) 抛
    ValueError」。truncate()/truncate(非负) 一致。
11. **str repr 控制字符**：pa 既有口径不转义 \xNN（CPython 转义）。
    NUL 填充语义本身一致，341 用 len/ord 验证而非 repr。改 pa_repr_str
    属 str 核心全局行为，不在本轮（会影响既有程序输出）。
12. **BytesIO.write(str)**：不抛 TypeError（pa bytes/str 同运行期表示，
    unknown 实参分不出）；同类型输入结果一致。
13. **file 形参的属性面**：name/mode/encoding/errors 实例侧统一
    AttributeError（CPython StringIO 也没有这些）；closed 经 getter
    登记可读。fileno 实例侧需类自己定义（StringIO 未定义 →
    AttributeError，CPython 是 UnsupportedOperation，口径合理不逐字）。
14. **tarfile 模块**：FileLike + BytesIO 基础已就绪（块2），模块本体已由
    stdlib-batch4（0.347）补齐（ustar 读写 + gzip + GNU L/K + 最小 pax 读；
    pax/GNU 写、稀疏文件仍是负面结论），详见 docs/stdlib-batch4.md。
15. **动态方法调用带参/返回堆值**：pa_method_call0 只零参；fileinst
    分派族按固定签名带参（int/str/指针），返回堆值的方法经 fileinst
    族正确交引用（read/readlines），但经 pa_method_call0 的任意动态
    调用仍限 None/标量返回。

## 6. 闸门（实测，块1+2 终版 HEAD 1ed9182 + 文档提交）

- **flag-off asm 逐字节不变**：409 个基准/语料文件（bench 全量 120 +
  tests/run）pyadamas asm 输出与基线 9b4a6f9 逐字节 cmp
  **410/410 identical**，块2 后复验全同（块2 零 codegen 改动；探针
  修复只动运行期 C）。
- **difftest（O0 vs O2）**：**311 用例 0 不一致 EXIT=0**（JOBS=4，含
  340/341 带 lang-gaps 指令）。JOBS=8 时 277_urllib_errors 抖动一次
  （O0 rc=1/O2 rc=0）——固定端口 18327 的 http.server 子进程在并行
  负载下 /500 请求连接被重置成 URLError；串行与 JOBS=4 复验全过，
  单独跑门控关/开均过。本征负载敏感，非本版引入（urllib 路径无改动）。
- **全套件**：normal（JOBS=1）**329 ran ok EXIT=0**；ASan
  （PYADAMAS_TEST_ASAN=1，JOBS=1）**329 ran ok EXIT=0**
  （327 基线 + 340 + 341，零回归；167/81 探针回归修后全过）。
- **ASan detect_leaks=1 钉子**：13_gc / 174_gc_construct_roots /
  92_exc_inline_rc / 93_gen_exc_rc / 94_suspend_positions /
  119_gen_except_suspend，flag-off 与 flag-on（LANG_GAPS=1）各一遍
  **12/12 全 0**；340/341 lang-gaps **0 泄漏**。
- **语料对拍（LANG_GAPS on vs off，358 文件）**：**353 逐字节同 +
  2 门控用例（340/341）对 expect 一致**；bench_re 仅计时列（本征非
  确定，计数列全同）；180_codepoint_perf 计时跳过；277 负载抖动一次
  （off rc=1，单独跑过，同上）。277 门控开 str 回归曾修（§4.4）。
- **CPython 对拍**：340（53 行）/341（65 行）expect 由 CPython 3.14
  生成，O0/O1/O2 输出逐字节一致（341 含 NUL 字节按字节 cmp）。
- **check_size**：无新增超限。typecheck/expr.cpp 1214→976（Attribute
  case 逐字拆出 typecheck/expr_attr.cpp）；超限文件仍是基线那 5 个
  （codegen_regalloc/driver_build/ir_ops.h/lower_impl.h/
  typecheck/types.h，后两者本轮行数微增但本已超限）。
- **缓存失效**：manifest 加 `langgaps` 行；切换开关必然重建。

## 7. 提交

- `15c7b80` 块1 主体：异常类当值 + suppress/closing（门控默认关）
- `fe5a067` 自定义异常类值统一 ClassObj + qualname 末段构造 + 拆
  typecheck_expr_attr + 回归 340
- `78868a8` 异常类 repr 完整限定名 + 类对象 GC release 修 name 串泄漏
- `917a0f5` 删除 Class 型异常值 print/repr/str 特判（富异常实例回归）
- `71c1d86` 块1 落地文档 + CHANGELOG 0.343 预留
- `cbaf18b` 块2：file 协议泛化 + io.StringIO/BytesIO + csv 互操作 + 341
- `1ed9182` file 探针修 167/81 门控关回归 + 341 expect marker
- （CHANGELOG 块2 扩展/文档定版：另一次提交）

## 8. 块3+4 落地（0.346，lang2 分支，基线 master 2ef0a9d / 0.345）

>  块3（traceback 对象 / sys.exc_info）与块4（dataclass 编译期展开）已
> 实现并过闸门。门控仍是 `PYADAMAS_LANG_GAPS=1`（默认 0，强制
> type_tags=true）；门控关时既有程序 asm 逐字节不变（448 个基准/语料
> cmp 全同）。以下为落地机制、实测抓到的命门 bug 与已知边界。

### 块3 traceback 对象 + sys.exc_info（已落地）

- **表示**：PyAdamasException 加两条 malloc 帧链（PyAdamasTraceFrame
  复用，节点加 cfa 字段）：`phys` = 已提交链（sys.exc_info 的 tb 读它），
  `phys_extra` = 待提交链。**不复用旧 trace 链**——那条是生成器逻辑帧
  （pa_trace_attach），未捕获打印（pa_print_traceback + pa_trace_print）
  门控关/开逐字不变是语料闸门要求，物理链独立才不污染它。
  `handler_cfa` = 零开销展开的 handler 帧 CFA（见下）。
- **捕获点**：不在 exc_longjmp/pa_eh_throw 漏斗里统一捕——各 raise 入口
  按语义分三模式（rt_debug.c pa_trace_capture）：
  - NEW（pa_raise/pa_raise_args/pa_raise_c/pa_raise_instance/pa_eh_raise）：
    整条物理链入 phys_extra。
  - VALUE（pa_raise_value_from/pa_eh_raise_value_from，即 raise e）：
    整链前插旧 extra（CPython 实测：显式 raise e 加当前帧，旧 tb 原样接后）。
  - RERAISE（pa_reraise/pa_exc_reraise/pa_eh_reraise，裸 raise）：去掉
    最内层重抛帧再前插（CPython 实测：裸 raise 不加当前帧）。
  - **不捕获**：生成器/协程/异步生成器兜底重抛（gen_run 等）、finally 重抛
    （pa_exc_reraise_value_if 改为直接搬值重抛，不走 pa_raise_value）、
    零开销分发不匹配重抛（pa_eh_rethrow）——它们是同一次展开的延续，
    extra 原样保留。with 的 __exit__ 后重抛走 RERAISE，但「待提交链已含
    本次全部帧（按 CFA 判子集）」时跳过，不重复前插。
- **提交（命门）**：抛出时不知道会被谁接住，整链先记 extra；except 落地
  （pa_exc_accept / pa_exc_accept_value / pa_exc_accept_instance →
  pa_trace_commit）按 handler 帧 CFA 裁剪：cfa > handler 的帧（异常没走到
  就被接住）丢弃，其余并入 phys。handler CFA 两条来源：
  - 零开销：personality phase 2 钩子（rt_eh.c pa_eh_phase2_frame_release）
    写 exc->handler_cfa。**不能在钩子里裁剪**——分发不匹配会 rethrow 继续
    向外，中途裁剪丢中间帧；每次 handler 帧人格调用刷新，最终接住的才准。
    落地时 handler 自己的帧记录已被方案 G 释放（cfa <= target 含等号），
    pa_frame_top 是调用方记录，不能直接用——这是实测抓到的第一个坑。
  - setjmp：pa_frame_top 就是 handler 记录，但记录 cfa = rsp+frame_bytes+8
    比 DWARF CFA 少压栈寄存器段，不能直接比——用地址锚点：比记录地址大的
    最小节点 CFA 即 handler 帧。
  - 锚点不在链里：全节点更外层 → 裸 raise 被重抛帧自己的包围 try 接住
    （CPython 口径：外层帧一个不加）→ 整链丢弃；CFA 混杂（生成器挂起后
    恢复另一条栈，旧 CFA 失效）→ 整链保留不裁剪。
- **行号归因**：_Unwind_Backtrace 给返回地址，查表用 **ip-1** 落回 call
  指令所在行（call 是某行最后一条指令时返回地址顶着下一行 .Ldbg 标签）。
  旧 pa_print_traceback 口径不动。内部包装帧（__pa_new_Cls 构造包装 /
  __pa_bound_m 绑定方法 / lru/partial 包装）捕获时跳过——CPython 没有
  对应帧（构造异常直接显示 __init__）。
- **门控**：pa_lang_gaps_rt 运行期全局，入口模块体**首发** LangGapsEnable
  （新 RtFn，lower/lower.cpp run() 在 __name__ 存储前发射，仅 is_entry）。
  关时捕获/提交第一行即返回，零分配零行为变化。
- **O2 内联门控开时关**（lower_module 里 can_inline=false）：小函数内联
  把被内联帧并进调用者（调试表按调用者函数索引归因），帧链与 CPython
  对不上——实测 f→g→module 三层 raise 内联后只剩 <module> 一帧。
  直接化保留（call 仍是 call，帧不丢）。门控关逐字沿用基线。
- **debug_name**：IRFunction 新字段，门控开时 lowerer 填（入口 <module>、
  方法裸名 Cls.meth→meth、生成器 resume 同口径），backend/codegen.cpp 调试表
  函数名处消费；空则旧 debug_display_name 口径（门控关恒空，asm 不变）。
- **sys.exc_info()**：pa_exc_info（rt_lang.c）读 current_exc，返回
  (类值, 异常值, tb)：类值 pa_exc_class_obj 单例（块1 复用，+1）；异常值
  pa_exc_value_from_exc（+1）；tb = RC OBJ + PYADAMAS_TB_MAGIC，包
  pa_trace_clone(phys)（malloc 链，destroy 里 pa_trace_free；帧字符串是
  调试表 rodata 借用，无 GC 出边）。outside except → (None,None,None)。
  静态类型 tuple[ClassObj[Exception], unknown, unknown]：类值位可读
  __name__，异常值/tb 位 unknown 才能按 None 打印/判同一。
- **富异常上下文接管**：pa_exc_accept_instance 门控开时补 pending→current
  + save 节点（与 accept_value 同口径）——否则富异常 except 体里
  exc_info 读不到、ExcDone 的 save 配对不完整。门控关逐字旧样。
- **tb 内省**：__pa_tb_frames(tb) intrinsic → list[(file,line,func)]
  外→内（内层元组 kind2 混合种类，外层 kind1）；None → 空表。
- **stdlib/traceback.py**：纯 pa format_exc/print_exc/extract_tb。源行
  运行期 open(file).readlines() + 模块级 dict 缓存，strip 后印（CPython
  口径）；读不到省略源行。print_exc 写 sys.stderr。
- **ExcValue 搬运**：PyAdamasExcValue 加 trace/trace_extra（malloc 链
  owned）：value_from_exc 克隆、exc_from_value 搬回（move）、destroy
  释放——finally 复制（pa_exc_take_value）/ raise e / 生成器挂起经值
  搬运时 traceback 不丢。exc_clone 同步克隆两条链（async/task 投递）。
- **unknown 域异常值 str/repr/print**（门控开）：pa_str_any/pa_print_any
  加 ExcValue 魔数分支印消息；repr_any_into 印 Name('arg', ...)（单空串
  args 按无参印）。门控关逐字旧样（印机器字）。
- **sys.executable**：argv[0] 借用口径（同 sys.argv，不 inc）——153/350
  的 print_exc 子进程自执行在 CPython/pa 两运行时同一套分支。

**块3 实测抓到的 bug**：
1. 重查轮不传门控（块1 遗留）：typecheck/infer.cpp check() 的 replay
   TypeChecker 不复制 lang_gaps_，递归函数触发 result-hint 重查时异常类名
   退回 unknown，isinstance(e, ValueError) 降级报「第二参不是类」。
   修：next.set_lang_gaps(lang_gaps_)。
2. 单值 BoolOp 悬空块（块4 合成单字段 __eq__ 暴露）：lower_boolop 循环
   从 i=1 起，单值时一条跳转不发射，当前块落进下一块（eq 恒 False）。
   修：单值直接求值。语法分析器不产生单值 BoolOp，基线程序不受影响。
3. 提交裁剪的 handler CFA 口径（见上「提交」）：零开销落地 pa_frame_top
   是调用方记录、setjmp 记录 cfa 与 DWARF CFA 差压栈段——两个都不能直接比。

**块3 已知边界**：
1. **~~~^^^ 锚点**：CPython 3.13+ 给调用行加细粒度锚点（不止运行时错误，
   显式 raise 的调用帧也有）。pa 调试表只有行号没有列范围——对拍用
   PYTHONNODEBUGRANGES=1（-X no_debug_ranges）口径，锚点不复刻。
2. **KeyError str 引号**：CPython str(KeyError('k')) = "'k'"（repr 口径），
   pa 印消息 k（pa 全局口径，静态 str(e) 同样）。对拍避开 KeyError 消息。
3. **多参异常 str**：pa join 口径（§5.1），exc_info 的异常值 args 在
   except 体里按消息重建（pending_exc_args 已被 accept 释放）。
4. **raise-from / 处理中再抛的链格式化**：format_exc 只格式化当前异常；
   __cause__/__context__ 链（"The above exception was..."）不在本轮。
5. **outside except 直接 print 三元组**：静态 ClassObj 位对 None 印
   <class '?'>、unknown 位印 0（pa None-VCall 口径）——用解包 + is None
   判断，不直接印元组。
6. **with 的 __exit__ 体里 exc_info**：current_exc 只在 except 落地设置，
   __exit__ 执行期间 pending 未接管 → (None,None,None)（CPython 有值）。
7. **生成器挂起跨 finally 重抛的陈旧 CFA**：extra 链 CFA 来自旧栈，锚点
   校验失败时整链保留（宁多勿删）；帧可能多于 CPython。
8. **门控关时 exc_info**：类值/异常值可用，tb 帧链恒空（不捕获）。
9. **async 协程 await 链帧**：挂起的协程不在物理栈上，其帧无法捕获——
   tb 只有抛出帧与接住帧（如 asyncio.run(outer()) 里 inner 抛：tb =
   [module, inner]，缺 outer 的 await 行）；pa 的 asyncio 是原生 C，
   CPython 的 runners/base_events 内部帧也没有对应物。with 块穿过的异常
   帧链与 CPython 一致（350 钉住）。协程逻辑帧的旧 trace 链
   （pa_trace_attach）不受影响。

### 块4 dataclass 编译期展开（已落地）

- **机制**：typecheck/dataclass.cpp expand_dataclass，classdef_impl 字段
  收集完后调用（门控开 + is_dataclass）。按字段注解合成 FunctionDef AST
  注入 ClassDef.body（StmtPtr 所有权归类体），之后的方法签名登记/槽分配/
  体检查/降级/跨模块接口编码**全部复用既有机制**。重查轮按名字判重不重复
  注入。用户已定义的 __init__/__repr__/__eq__ 逐方法跳过（CPython 口径）。
- **合成形状**：
  - __init__(self, f: T, g: T = lit) -> None：逐字段 self.f = f；默认参
    克隆字段字面量（build_sig 的 default_lit 机制自动接管调用点填充与
    跨模块编码）。
  - __repr__(self) -> str："Name(f=" + repr(self.f) + ... + ")" 左结合
    加法链；函数内类用 func.<locals>.Cls qualname（cur_fn_def_）。
  - __eq__(self, other) -> bool：isinstance(other, Cls) 守卫（other 不
    标注，运行期探针）+ 守卫内 `__pa_dc_o: Cls = other` 类型化局部绑定
    （unknown 域没有属性面，容器 == 会退化指针比较——必须静态化）+
    逐字段 == 的 and 链；异类 False。
- **装饰器**：stdlib/dataclasses.py 恒等 dataclass + field() 运行期兜底
  （NotImplementedError）；kModuleTable 登记。门控开时降级跳过装饰器
  调用（裸 @dataclass 名字无运行期绑定，求值落空全局；导入形是恒等，
  调不调语义相同）。门控关逐字旧样。
- **v1 范围**：单类（无基类）+ 字面量默认（None/Bool/Int/Float/Str/Bytes）
  + int/float/bool/str/bytes/list/dict/tuple/set 字段（容器递归查元素）。
  以下门控开时**编译期明确报错**（tests/fail/352-358 钉住）：
  frozen/order/kw_only/slots/unsafe_hash/init=False 等装饰器选项、
  位置装饰器参、多装饰器、继承、field()/调用默认、可变字面量默认
  （[]/{}/set()）、非字面量默认、默认参序违规、未解析/不支持标注、
  字段名撞合成局部（self/other/__pa_dc_o）。
- **unhashable**：合成 __eq__ 无 __hash__ → 既有规则
  （lower/builtin_reflect.cpp hash()）编译期报 unhashable（CPython 是
  运行期 TypeError，口径合理不逐字）。
- **静态容器/bytes repr**（门控开）：repr(list/dict) 走 ReprAny 探针；
  set 专用 pa_repr_set（运行期表示是 dict 值恒 0，探针分不出 set 与
  {k:0}）；bytes 专用 pa_repr_bytes（bytes/str 同表示无标志位）。
  门控关这些调用原本就报「不支持」，既有程序 asm 不变。
- **跨模块**：合成方法进 cls->methods → encode_module_interface 自动
  编码（签名/默认值/has_init）；导入方构造/eq/repr 全链路
  （tests/modules/dc_dataclass 钉住，含缓存命中）。

**块4 已知边界**：
1. **None 默认进容器字段**：pa 一般类型规则拒绝（x: list[int] = None
   报 cannot initialize field）；Optional 标注不在 v1 范围。CPython 允许。
2. **函数内类的跨调用 eq**：pa 类静态编译一次，CPython 每次调用执行
   class 语句（跨调用实例类对象不同 → eq False）。repr qualname 一致。
3. **嵌套 set 元素 repr**：探针口径按 dict 印（P2 容器元素无 tag 边界）。
4. **类类型字段 / Optional / 定宽整数 / 更深层嵌套 qualname**：v1 报错
   或不完整（单层 func.<locals>.Cls）。
5. **门控关时 @dataclass**：导入可用、恒等装饰器，但带参构造不合成
   __init__（字段零初始化，基线口径）。

### 块3+4 闸门（实测，终版 HEAD 见 CHANGELOG 0.346）

- flag-off asm 逐字节不变：**448/448** 既有基准/语料文件 cmp 全同
  （bench 全量 + tests/run + tests/modules；唯一 BASEFAIL_NEWOK 是新
  回归 351，基线无 dataclasses 模块编不了）。
- 异常/生成器钉子 92/93/94/119/153：门控关/开 × 普通/ASan
  detect_leaks=1 全过全 0。
- 350/351：O0/O1/O2 与 CPython 3.14 逐字节一致（350 用
  PYTHONNODEBUGRANGES=1 口径）；ASan detect_leaks=1 为 0。
- 全套件 normal（JOBS=1）**338 ran ok EXIT=0**；ASan（JOBS=1）
  **338 ran ok EXIT=0**。
- difftest O0 vs O2：**319 用例 0 不一致 EXIT=0**（JOBS=4）。
- 语料 gate ON vs OFF（440 文件）：**431 逐字节同 + 4 门控用例对 expect
  一致 + 5 计时基准仅计时列**（浮点归一化后逐字节同）。
- check_size 无新增超限（基线 5 个）。
- 350 含 with 穿过帧链 + tb 跨 GC（3000 环引用分配触发主收集后帧链仍可读）
  钉子；async 协程 await 链帧边界见上。

### 其它剩余（stdlib-b3 跳过清单）

- **tarfile**：已被 stdlib-batch4（0.347）落地——ustar 读写 + fileobj/
  BytesIO + gzip + GNU L/K + 最小 pax 读面，见
  [stdlib-batch4.md](stdlib-batch4.md)。剩余：pax/GNU**写**、稀疏文件、
  bz2/xz、data_filter。
- **pprint**：P2 已解锁（0.340）。
- **traceback 链格式化 / tb 属性面**：format_exception（cause/context
  链）、TracebackType.tb_frame/tb_lineno 等属性面不在本轮（块3 边界4）。
- **dataclass 进阶**：继承/field()/frozen/order/kw_only/slots/
  default_factory/ClassVar/InitVar 不在 v1（块4 负面清单）。
