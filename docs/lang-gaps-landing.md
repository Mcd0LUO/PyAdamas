# P4 语言缺口 块1+2 —— 异常类当值 + suppress/closing；file 协议泛化 + io.StringIO/BytesIO 落地与闸门（0.343 预留，lang1 分支）

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

- **产生点**：Name 降级（lower_expr_assign.cpp）门控开且名字是异常类
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
  的降级检查同口径放宽（lower_print.cpp）。
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
14. **tarfile 模块未实现**：FileLike + BytesIO 基础已就绪（fileobj
    协议面可用），tarfile 本身的 ustar 头解析/目录遍历是独立模块工程，
    留给后续批次（stdlib-b3 跳过清单的 tarfile 项部分解锁）。
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
- **check_size**：无新增超限。typecheck_expr.cpp 1214→976（Attribute
  case 逐字拆出 typecheck_expr_attr.cpp）；超限文件仍是基线那 5 个
  （codegen_regalloc/driver_build/ir_ops.h/lower_impl.h/
  typecheck_types.h，后两者本轮行数微增但本已超限）。
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

## 8. 剩余块（P4 块3/4：设计就绪，本轮未实现）

>  lead 指示：块1+2 已是实质 P4 成果，先交付；块3/4 复杂/风险高时
> 文档化剩余。以下为代码取证后的具体方案与风险评估，供后续批次直接
> 开工。**本轮未动块3/4 代码**（避免半截改动危及已绿闸门）。

### 块3 traceback 对象 + sys.exc_info（方案就绪，工程量中）

- **帧捕获漏斗点已定位**：所有 setjmp 路径 raise 经 exc_longjmp
  （rt_exc.c），所有 _Unwind 路径经 pa_eh_throw（rt_eh.c）——两点
  前置捕获即可覆盖全部抛出。捕获用 _Unwind_Backtrace +
  pa_debug_lookup_pc（rt_debug.c 既有 PC→(file,func,line) 表），
  只留生成代码帧，挂到 pending_exc->trace（PyAdamasTraceFrame 链，
  exc_free/pa_trace_free 既有生命周期）。生成器帧已有 pa_trace_attach
  （rt_gen.c:153/rt_async.c:772/rt_agen.c:179），捕获链与之合并。
- **门控**：运行期全局开关（生成代码门控开时在入口模块体首发
  LangGapsEnable 调用——需新 RtFn + lower.cpp 模块体发射点）；
  门控关不捕获，零行为/性能变化。
- **sys.exc_info()**：sys 是原生模块（builtin_sigs.cpp:128 +
  lower_import.cpp:409 成员分派）。pa_exc_info() 读 current_exc
  （except 体内由 pa_exc_accept_value 设置），返回三元组：类值
  （pa_exc_class_obj(e->type)，块1 机制复用）+ 异常值
  （pa_exc_value_from_exc）+ tb 对象（新 RC OBJ+魔数，包 pa_trace_clone
  的帧链）。outside except → (None,None,None)。
- **tb 内省**：intrinsic __pa_tb_frames(tb) → list[(file,line,func)]
  外→内物化；stdlib/traceback.py 纯 pa 格式化。
- **CPython 逐字节风险（主要工程量）**：format_exc 要帧源行
  （open(file).readlines() 运行期读源文件 + 缓存）、函数名映射
  （pa 调试名 __pa_main→<module>；方法名 Cls_meth→meth 需 IRFunction
  加 debug_name 字段，lowerer 门控开时填，codegen.cpp:363
  debug_display_name 处消费）、行号归因核对。CPython 3.14 对
  ZeroDivisionError 等运行时错误带 ~~~^^^ 锚点——逐字节对拍只用
  显式 raise 用例，锚点场景文档化边界。
- **钉子风险**：trace 链 malloc 生命周期跨 exc_free/exc_clone/
  pa_exc_take_value（finally 复制）/生成器挂起——92/93/94/119 必复验。

### 块4 dataclass 编译期展开（方案就绪，工程量中重）

- **现状取证**：ClassInfo.is_dataclass 已标记（typecheck_func.cpp:514
  识别 @dataclass/@dataclasses.dataclass/@dataclass(...)），
  typecheck_call_forms.cpp:491 对 is_dataclass 类放行带参实例化，
  但**运行期没有合成 __init__**——当前 @dataclass 带参构造实际不工作
  （注释所谓「运行期合成」并不存在）。
- **方案**：类型检查类体收尾后（字段已收集），门控开且 is_dataclass
  且无用户 __init__ 时，按字段注解**合成 FunctionDef AST 注入类体**
  （__init__ 逐字段赋值 + 默认参；__repr__ 拼 "Name(f=repr,...)"；
  __eq__ 逐字段比较），复用既有方法收集/槽分配/降级机制（AST 所有权
  挂 Module，ClassDef.body 是 vector<StmtPtr>）。非运行期反射。
- **范围**：v1 只做单类 + 字面量默认值 + 标量/str/容器字段；继承
  dataclass（父字段并入 __init__）、field() 描述符、frozen/order/
  kw_only 保守跳过 + 负面结论。__repr__ 的字段 repr 走既有 dunder/
  探针口径（容器元素 unknown 边界同 P2）。
- **风险**：AST 注入时机与骨架类（complete 标志）/跨模块接口编码
  （合成方法要进导出签名）交互；__eq__ 与既有哈希口径（定义 __eq__
  无 __hash__ → unhashable，lower_builtin_reflect.cpp:280 已有规则）。

### 其它剩余（stdlib-b3 跳过清单）

- **tarfile**：FileLike+BytesIO 基础已就绪（块2），模块本身（ustar
  头解析/pax/GNU 长名/目录遍历）未实现。
- **pprint**：P2 已解锁（0.340）。