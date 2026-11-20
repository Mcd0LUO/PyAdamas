# P4 语言缺口 块1 —— 异常类当值 + suppress/closing 落地与闸门（0.343 预留，lang1 分支）

> 范围：docs/numeric-types-design.md §3/§4 P4 的第一块（最高 ROI、最自包含）。
> 基线 master 9b4a6f9 / 0.341（含 P2 CLASS=11 预留），独立 worktree
> /tmp/wt-lang1（分支 lang1），不推 master、不动主树。门控
> `PYADAMAS_LANG_GAPS=1`，**默认 0**；开启时驱动强制 `type_tags=true`
> （CLASS tag=11 等精确堆子类标签是 P4 的表示基础）。0.342 预留 big1
> （P3 BigInt+f32），集成按落地顺序重编号。域隔离：不碰 rt_big/Big
> TyKind/BIG tag=12/f32。
>
> **结论（先说）**：块1 落地。门控开时内建/自定义异常类作运行期类对象
> 值，过 unknown 带 CLASS tag=11 不退化成 0；contextlib.suppress/closing
> 与 CPython 逐字节一致（tests/run/340，O0/O1/O2）。门控关时 tag 编号/
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

## 6. 闸门（实测，HEAD 917a0f5）

- **flag-off asm 逐字节不变**：409 个基准/语料文件（bench 全量 120 +
  tests/run 289）pyadamas asm 输出与基线 9b4a6f9 逐字节 cmp
  **410/410 identical**（含 failures 清单文件）。后续修复只动门控开
  路径与运行期 C，flag-off 发射逐字不变。
- **difftest（O0 vs O2）**：310 用例 **0 不一致 EXIT=0**（含 340 带
  lang-gaps 指令）。
- **全套件**：normal（JOBS=1）**328 ran ok EXIT=0**；ASan
  （PYADAMAS_TEST_ASAN=1，JOBS=1）**328 ran ok EXIT=0**（327 基线 + 340）。
- **ASan detect_leaks=1 钉子**：13_gc / 174_gc_construct_roots /
  92_exc_inline_rc / 93_gen_exc_rc / 94_suspend_positions /
  119_gen_except_suspend，flag-off 与 flag-on（LANG_GAPS=1）各一遍
  **12/12 全 0**；340 lang-gaps **0 泄漏**。
- **语料对拍（LANG_GAPS on vs off，357 文件）**：**354 逐字节同 +
  340 门控用例对 expect 一致**；bench_re 仅计时列（本征非确定，计数列
  全同）；180_codepoint_perf 计时跳过。277 曾回归已修（§4.4）。
- **CPython 对拍**：340 的 53 行 expect 由 CPython 3.14 生成，O0/O1/O2
  输出逐字节一致。
- **check_size**：无新增超限。typecheck_expr.cpp 1214→976（Attribute
  case 逐字拆出 typecheck_expr_attr.cpp 250 行）；超限文件仍是基线那
  5 个（codegen_regalloc/driver_build/ir_ops.h/lower_impl.h/
  typecheck_types.h）。
- **缓存失效**：manifest 加 `langgaps` 行；切换开关必然重建。

## 7. 提交

- `15c7b80` 块1 主体：异常类当值 + suppress/closing（门控默认关）
- `fe5a067` 自定义异常类值统一 ClassObj + qualname 末段构造 + 拆
  typecheck_expr_attr + 回归 340
- `78868a8` 异常类 repr 完整限定名 + 类对象 GC release 修 name 串泄漏
- `917a0f5` 删除 Class 型异常值 print/repr/str 特判（富异常实例回归）
- （CHANGELOG/文档：另一次提交）

## 8. 后续块（P4 剩余）

- **块2 file 协议泛化 + io.StringIO/BytesIO**：file 标注门控开时收
  FileLike（read/write 方法面的类实例），file 方法调用改发运行期分派
  包装（file 直调 / 实例走方法登记表带参分派）；stdlib/io.py 纯 pa
  StringIO/BytesIO；csv.writer/reader 经同一分派收 StringIO。
- **块3 traceback + sys.exc_info**：异常值挂帧链（exc_longjmp/
  pa_eh_throw 两个漏斗点捕获 _Unwind_Backtrace 生成帧，门控运行期
  开关），sys.exc_info() 返回 (类值, 异常值, tb 对象)，stdlib/
  traceback.py format_exc/print_exc/format_exception_only。
- **块4 dataclass 编译期展开**：@dataclass 按字段注解编译期合成
  __init__/__repr__/__eq__（AST 注入，复用既有方法机制），非运行期
  反射；复杂继承保守跳过 + 负面结论。