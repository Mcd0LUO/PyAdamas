# borrow 细化：只读借用循环变量槽免每轮 inc/dec（0.338，borrow1 分支）

门控 `PYADAMAS_BORROW_OPT=0/1`，**默认 0**。flag-off 不跑本遍，IR/asm 逐字节
不变（38 热基准 cmp 全同）。manifest 缓存键加 `borrowopt`（切开关强制失效）。
实现：`src/lower/borrow.cpp`（降级完成后、opt 之前，与 `finalize_param_borrows`
同位置）；诊断 `PYADAMAS_BORROW_OPT_STATS=1`。

## 1. 背景：R3 取证定位的真正通用 RC 杠杆

rc1（0.335，未合）取证：RC 密集容器循环

```python
for i in range(n):
    s = xs[0]          # 每轮 inc(index.get xs[0]) / dec(load.slot s2 旧值)
    total += len(s)
```

每轮对是 `inc(%14=index.get xs[0]) / dec(%15=load.slot s2)`——**不同值编号但
运行时同对象**，块内对消除（R3 安全子集）匹配不到。churn 的根因：循环变量槽
s2 被当成 owned（每轮 inc 新值 / dec 旧值），而它其实只作**借用**——容器 xs
持有元素的那份引用贯穿循环。把这类槽识别为 borrow，整段去掉每轮 inc/dec 与
出口 release，是压 RC 密集容器循环的通用杠杆（真实日常语料受益，非基准特化）。

borrow 是历史 UAF/leak 重灾区（lower/impl_slots.cpp 注释记载 0.323 每轮漏一份、
gc02_uaf_junk 等）。本轮极保守：**可证安全才做，任何一条不确定整槽跳过**。

## 2. 计数守恒论证（为什么不需要跨迭代值 identity）

设容器持有元素那份引用，元素计数 C≥1（容器活着即 C≥1）。

- **基线**：首轮 store inc → C+1（old=None，dec 空操作）；随后每轮
  inc(新)→C+2、dec(旧)→C+1（旧值就是上一轮借的同对象或容器另一元素，
  计数始终 ≥1）；出口 release_slots dec → C。槽在循环期间净持 +1。
- **借用**：不 inc、不 dec、出口不 release → 恒 C。

两条路径下元素在函数体内都恒活（C≥1 / C+1≥1），**没有任何对象的计数在
任何时刻降到 0**，析构时机逐点不变；窗口外（出口之后）计数也逐点一致。
即使每轮借的是**不同**元素（`s = xs[i]`），容器对每个元素的引用都贯穿
函数体，守恒同样成立——R3 所说的「xs[0] 不变 + s2 归纳持有 + 首轮 None」
跨迭代 identity 分析是**外提 retain** 的前置，不是 borrow 的前置。borrow
只需要：**每个被借对象在 s 的整个活跃期内由容器持有那份引用**。

首轮 None：槽在序言清零，借用语义下首轮 store 前读 s 读到 None，与基线
一致（基线首轮 dec(None) 也是空操作）。末轮：出口不清引用（本来就没有），
只清零槽（保留 store 0，防内联复用时陈旧指针）。

## 3. 判定规则（fail-closed）

候选槽 s：**已知堆类型**（ir_is_heap，非 unknown）、非形参、!slot_unknown、
不取址（SlotAddr）。s 的每条 StoreSlot 必须精确匹配 retain 形状

```
call.rt pa_rc_inc(v)        ← 删
%old = load.slot s          ← 删（单用）
store.slot s, v             ← 保留
call.rt pa_rc_dec(%old)     ← 删
```

其中 v 经 Mov 链溯源到 `IndexLoad/IndexLoadUnchecked`（list/tuple 借用读，
运行时不产生新引用；str 下标产生新串走 temp_take，形状不符自然跳过），
容器经 Mov/LoadSlot/LoadGlobal 溯源到**同一根 r**。出口 release 形状
`load s; dec; [const 0]; store s,0`（清零常量可能夹在中间）逐条匹配删
load+dec、保留清零；任何其它 load+dec 形态 → 整槽跳过（防漏删释放点）。
del s 是 load/store0/dec（顺序不同）、s=s 源不是 IndexLoad、s=None 没有
inc——都匹配不上，整槽跳过。

### 3.1 根稳定性（三种根）

| 根 | 条件 | 存活论证 |
|---|---|---|
| PARAM | 形参槽（list/tuple），体内从不覆盖（出口清零除外）、不取址 | 实参由调用方持有，调用期间存活；调用方帧是 GC 根 |
| GLOBAL | 全局（list/tuple），本函数内从不写、不取址 | 全局根表持有；无用户调用 → 执行期间不会被改 |
| FRESHLOCAL | 本地槽唯一实写来自新分配（MakeList/MakeTuple/ListNew），分配值只用于 temp/take/该 store；LoadSlot(r) 只作 IndexLoad 容器或 Len 实参（纯读不逃逸） | 新分配对象与任何既有对象不别名；不逃逸 → 用户代码够不着 |

FRESHLOCAL 的不逃逸检查同时排除了：传参、存别处（`ys = xs`）、返回、
方法调用（`xs.append` 的接收者是 LoadSlot(xs)）、闭包捕获。容器自环
（`xs.append(xs)`）也因逃逸被排除。

### 3.2 全函数扫描

- 生成器/协程/async/vthread/chan（RtFn Susp 类 + Gen* op）→ 整函数跳过
  （挂起期容器可能被外部改；帧槽跨挂起活跃）。
- 任何 Class 类型值（含嵌套元素类型）→ 跳过（析构/__str__ 等用户代码
  时机不可控）。
- extern/ptr/GlobalAddr/ObjNew/ClassAddr → 跳过。
- **用户调用**（Call/CallDirect/VCall*/CallPtr/ImportModule）：PARAM/GLOBAL
  根 → 跳过（外部别名可改容器）；FRESHLOCAL 根 → 允许（容器不逃逸）。
  CallExtern 一律跳过。
- **list 修改类**（ListAppend/Set/SetLen/Prep/Insert/Reverse/Sort/Extend/
  InplaceRepeat/SliceAssign/DelIndex/CopyRange、RandShuffle、IndexStore、
  FieldStore）：接收者必须可证不与根别名——
  - PARAM/GLOBAL 根：接收者必须是新分配（或唯一实写为新分配的本地槽）；
  - FRESHLOCAL 根：接收者不得溯源到根本身（根不逃逸，其余来源够不着）。
  - **dict/set/deque/file 的修改不在此列**：它们改的是 dict/set/deque/file
    对象，运行时类型上不可能别名 list/tuple 根；往 dict 里存根/借元素只是
    retain，替换/弹出只是 dec，根槽那份引用保证对象存活。这是
    `for w in words: d[w] = d.get(w,0)+1`（dict_wordfreq 形态）能命中的
    关键：d 是新分配 dict，words 是形参 list，两者类型上就不别名。
- **CallRt 白名单**：Pure（纯计算/只读查询/新值分配/RC 记账/EH 簿记，
  完整枚举见 classify_rt）恒允许；Dispatch（unknown 域分派/repr/哈希/
  StrMethod/FuncReduce/RegexSubFn/JsonDumps/DYLIB 等可能跑用户代码）仅
  FRESHLOCAL 根允许；**未知编号默认 Bad → 整函数跳过**（未来新增 RtFn
  不会被误判为纯）。DictSetDefaultFactory 落 Bad（defaultdict 的 DictGet
  会调工厂，rt_dict.c:409）。

### 3.3 借用值的消费（use 分析，Mov 传播）

s 的每条 LoadSlot 结果只允许：纯运算（Bin/Un/Cmp/Convert/Len）、
IndexLoad 容器、FieldLoad、分支、Return（own_return_value **先 inc 再
清理**，lower/stmt.cpp:187）、存入别处（StoreSlot/StoreGlobal/IndexStore
值位/FieldStore 值位/MakeList/MakeTuple——降级都先 retain）、白名单
CallRt（RcInc 是别处 store 的 retain；RcDec/RcTemp*/RcCleanup **显式
禁止**）、用户调用实参（仅 FRESHLOCAL；被调方 inc_params 自留引用）。
SlotAddr(s) 与任何未知消费 → 跳过。

## 4. 协同（RC 正确性命门）

- **帧释放/异常展开**：改写置 `slot_owned[s]=false`。codegen_block 的
  frame_owned_ 按 slot_owned 构建 → setjmp longjmp 与零开销 _Unwind 展开
  都不再 dec 借用槽（容器才是 owner，由容器槽的释放负责）。eh_emit 的
  landing pad 不直接 dec 槽（grep 证实），无旁路释放点。
- **GC**：借元素经容器根可达——形参根由调用方帧持有，本地根 owned 进
  帧记录/寄存器扫描，全局根进根表。借用槽本身（heap、owned=false）不进
  GC 扫描清单，也不影响可达性（元素本来经容器可达）。基线里 s 的 +1
  引用不增加可达性，删除不改变 GC 存活图。
- **内联**：opt_inline 把 slot_owned=false 随槽搬进调用方；内联入口对
  借用槽只清零不释放（「借用槽只清零不释放」既有口径）；根槽的所有权
  记账原样保留。本遍在 opt 之前跑，内联搬的是已优化身体，收益自动传播
  到调用点（含调用点在循环里的反复执行）。
- **临时栈**：IndexLoad 结果是借用，不入临时栈（lower_expr_container:134
  注释）；删 inc/dec 不触碰 rc_pushes/fn_mark 水位。
- **后续 opt**：forward_slot_loads/LICM/循环版本化看到的仍是带 store 的
  槽（store 保留），转发/外提规则不受影响；版本化把 index.get 提出循环
  后借用 store 每轮照写，语义不变（实测 probe 命中 ver.guard 路径）。

## 5. 命中形态与覆盖（38 热基准，flag-on stats）

命中 7 案：dict_wordfreq（count_words）、sort_struct（aggregate）、
b1_log_parse（gen_logs，3 槽：path/method/status）、b2_normalize
（gen_phrases）、b3_template（main）、b5_re_extract（gen_records，2 槽）、
d6_rec_memo（main）。回归 304-310 另覆盖 while/嵌套/tuple 常量下标/
fresh-local/异常/内联形态。

**保守跳过的形态（负面清单，如实报）**：
- 根在循环内被修改（queue_sim 的 pending.append/重建、fasta 的 parts、
  c1_ledger gen_rows 的 rows.append）——即使 append 只增不删也一律跳过
  （append 后可能跟随 `pending = rest` 重赋值，旧容器释放 → 借元素 UAF）。
- Class 值（binary_trees 的 Node）——析构时机不可控。
- 非堆元素（d5_finance 的 list[float]：float 是机器 double，本就没有
  RC churn，无 borrow 可做）。
- 根来源不是新分配（c1_ledger 的 `data = table[1:]` ListSlice、b4_wordfreq
  的 `text.split()` StrMethod）——运行时虽返回新容器，v1 不信任非
  MakeList/ListNew 来源（扩展见 §7）。
- tuple 变量下标（`t[i%3]` 元素类型 unknown）——v1 只做已知堆类型槽。
- 入口函数的全局根（模块顶层 `xs=[...]; for...`：xs 在入口被赋值 →
  GLOBAL 根「从不写」不满足）。函数级命中经内联传播，入口级收益有限。
- 生成器/async/vthread/chan、unknown 槽、object 标注槽、del/覆盖写/
  自赋值、容器逃逸/别名修改、用户调用 + PARAM/GLOBAL 根。

## 6. 闸门（实测）

- **ASan detect_leaks=1 + UAF 全 0**：钉子 13_gc/174_gc_construct_roots/
  88_defaultdict/192_dict_cache/66_genexpr/119_gen_except_suspend/
  92_exc_inline_rc/93_gen_exc_rc/94_suspend_positions/225_vthread_gc_suspend
  + 新回归 304-310，flag-on/off 各一遍，34/34 LEAK_OK。
- **全套件**：normal flag-on EXIT=0（312 ran ok，0 失败）；ASan flag-on
  JOBS=1 EXIT=0（312 ran ok，0 失败）。
- **difftest flag-on**（O0 vs O2）：294 用例 0 不一致。
- **语料零差异（输出+rc）**：tests/run + bench/daily(23) + _bugs(13) +
  realistic(6) + compare(9) + unknown(5) + json/re，353 案 flag-on vs off：
  351 逐字节同；bench_re 仅计时列差异（语义列逐字节同，计时本征非确定）；
  277_urllib_errors 并行端口竞争 flaky（产物 cmp 逐字节同，独跑 on/off
  各 3 遍全 0，套件 ran ok）。
- **CPython 对拍**：daily 23 案 flag-on 全同（a1/a3/a5 用 build+argv 对拍）。
- **flag-off asm 逐字节不变**：38 热基准（realistic 6 + compare 9 +
  daily 23）对 lead 0.337 基线 cmp 38/38 IDENTICAL。
- **check_size**：无新增超限（lower/borrow.cpp 738 行 < 1200；既有 5 个
  超限文件与基线同一组，driver_build 1540→1551 本就超限）。
- 回归用例 tests/run/304-310（expect 钉死，CPython 对拍过的 6 案）。

## 7. 剩余/风险

- **性能**：见 §8（abbench 实测，如实报）。
- 可扩展方向（本轮不做，风险/收益比不划算）：
  1. 新分配来源扩展到 ListSlice/StrToList/DictKeys 等「运行时必返回新
     容器」的 RtFn（c1_ledger main 的 row 借用可命中，但只有 800 对，
     收益微小，徒增信任面）。
  2. unknown 槽（RcIncUnknown/RcDecUnknown 形状）——注册表探针语义下
     守恒论证同样成立，但 unknown 域历史洞多（0.323/0.327），不碰。
  3. dict/set 根（DictGet/DictKeyAt 借用）——需「键不被替换/删除」的
     修改分析，比 list 根复杂（DictSet 同键替换会 dec 旧值）。
  4. append-only 根修改细化——需证明无重赋值/无删除，queue_sim 形态
     实际伴随 `pending = rest`，细化后仍不命中，不值得。
  5. 入口全局根「赋值先于所有借用读」的 CFG 分析——入口级收益有限。
- **风险**：本遍改的是所有权记账，误判即 UAF/leak。防线：fail-closed
  白名单（未知 RtFn/op 一律跳过）、形状精确匹配（del/自赋值/None 覆盖
  自然失配）、slot_owned 与帧释放/GC/内联的既有口径协同、ASan leak+UAF
  钉子与语料对拍每块增量都跑。门控默认关，集成后可随时回退。

## 8. 性能（abbench 式交替 A/B 绑核，如实报）

方法：同一编译器 flag-on/off 两侧 `--rebuild` 编译（modcache 键含
borrowopt），taskset 绑 cpuset 首核（24），每轮打乱用例顺序 + 奇偶轮交换
A/B 先后，BURST 连跑取平均压周期干扰，多轮中位数/p25。三轮：
B5×R9 → B20×R11 → B40×R13（初轮异常值高精度复测证伪）。

**噪声标定**：未命中案（binary_trees/d5_finance/c1_ledger/queue_sim/
fasta/fib）flag-on/off 产物 **cmp 逐字节相同**，R1 却量出 binary_trees
+4.8%、d5_finance +11.0% —— 纯机器噪声（多租户）。短案（<10ms）噪声带
±3%；长案（>100ms）中位数/p25 双稳才认。

| 用例 | R1 中位/p25 | R2 中位/p25 | R3 中位/p25 | 结论 |
|---|---|---|---|---|
| dict_wordfreq | -4.1/-3.7 | -4.4/-4.5 | **-4.6/-4.4** | **稳健 -4.5%** |
| sort_struct | -1.6/-2.0 | -1.6/-1.6 | **-1.8/-1.8** | **稳健 -1.7%** |
| b1_log_parse | +1.5/+1.3 | -1.9/-1.8 | **-1.1/-1.3** |  mild ~-1.2%（R1 证伪） |
| b3_template | +2.9/+0.6 | -1.2/-3.3 | **-1.6/-1.2** | mild ~-1.4% |
| b2_normalize | -5.8/-3.3 | +1.3/+1.4 | **+1.1/+0.1** | 中性（R1 -5.8% 证伪） |
| b5_re_extract | -0.4/+1.7 | -0.0/+0.6 | **+0.7/-0.2** | 中性 |
| d6_rec_memo | +1.2/+4.3 | +1.4/+2.8 | **+1.7/+0.2** | +1~2% 轻微回归方向一致（6ms 短案，噪声带内，疑布局效应，如实报） |

- **dict_wordfreq（RC 密集主目标）**：count_words 借 w（形参 words 根，
  fresh dict counts 写入不别名 list 根）。静态 RC inc/dec 调用 130→124
  （count_words 体 14→8，循环版本化双身体各删 inc/dec-old/dec-exit 3 条）；
  动态 12000 轮 × 400 词 = **480 万次借用迭代 × 2 条 RC 调用 ≈ 960 万条
  运行时调用消除** → 239ms→228ms（-4.5%，三轮中位/p25 全负，稳健）。
- **sort_struct**：aggregate 借用命中，436ms→429ms（-1.7%，三轮一致）。
- **b1_log_parse**：gen_logs 3 槽（path/method/status，fresh-local 根 +
  用户调用 lcg/str/zfill 允许）→ ~-1.2%；b3_template ~-1.4%。
- **未命中 RC 密集案如实报零**：queue_sim（根 pending 循环内 append+重建）、
  binary_trees（Class Node）、fasta（str 下标新串/根 parts append）保守
  跳过，flag-on/off 二进制逐字节同；d5_finance（list[float] 非堆元素）
  本就没有 RC churn。它们的「RC 密集」开销不是可省记账——与 R3 结论一致，
  borrow 只压「稳定容器只读借用」这一类。
- **净结论**：通用杠杆成立（真实日常语料 b1/b3 受益，非基准特化），
  主目标 -4.5% 稳健可测；代价是 d6 短案 +1~2% 疑似布局回归（噪声带内）。
  门控默认关，集成后按语料净收益决定默认值。

## 9. 0.474：入口全局根 + append-only 根（obj_dispatch）

### 9.1 动机与卡点

obj_dispatch 的内层循环是模块顶层写法：

```python
shapes = []
for i in range(100):
    shapes.append(...)
total = 0
for r in range(N):
    for s in shapes:
        total += s.area()
```

每轮 4 条可省的记账：`rc_inc(元素)`、`load.global s`（旧值）、`store.global s`、
`rc_dec(旧值)`——动态 640000×100 轮。0.472 之前六道门逐条挡住：

| # | 卡点 | 0.474 的处理 |
|---|---|---|
| 1 | 候选必须是**帧槽**，而循环变量 `s` 是模块全局 | 新增「全局候选」：候选形状对 `LoadGlobal/StoreGlobal` 同口径匹配 |
| 2 | GLOBAL 根要求「本函数内从不写」，但容器 `shapes` 在入口里被赋值/append | 新增根类别 **PrivateGlobal**：入口私有 + 唯一实写来自新分配 + 只增不删 |
| 3 | `class_free` 整函数否决（任何 Class 类型值） | 只在「容器不可达」的根（FreshLocal/PrivateGlobal）上放行 Class |
| 4 | `has_hard_bad` 否决含 `ObjNew`/`ClassAddr` 的函数；`ClassNew` 落 Bad | ObjNew/ClassAddr 移出硬拒清单；`ClassNew` 归 Pure |
| 5 | 根的 `LoadGlobal` 被迭代源持有（`rc_inc`+`rc_temp`）与 `append` 接收者消费 → use 检查拒 | 根的消费白名单加：迭代源持有四件（RcInc/RcTemp/RcTempTake/RcTempUnknown）、append/extend 接收者 |
| 6 | 全局没有 `release_slots` 的清零 store | 出口释放形状 `[load.global g; rc_dec]` **原地**换成 `[const.int 0; store.global g, 0]` |

### 9.2 全局候选（`analyze_candidate(true, g)`，仅入口函数）

- 仅 `__pa_main`、且 `!fn.has_handlers`（含 handler 的函数不借，红线）；
- 全局是**已知**堆类型（非 unknown）、且是入口私有（`entry_private_globals`：
  除入口外没有函数 Load/StoreGlobal 它，全模块无 `GlobalAddr`）；
- 每一条 `StoreGlobal(g)` 都精确匹配
  `[r=IndexLoad/…; inc(r); old=LoadGlobal g; StoreGlobal g,r; dec(old)]`，
  `old` 单用（多定义/其它来源一律跳过，fail-closed）；
- 每条 `LoadGlobal(g)`：store 旧值装载（豁免）/ 出口释放（改写）/ 普通读（use 分析）；
- 出口释放必须落在**含 `GcShutdown`/`RcShutdown` 的块**里（`release_globals` 与
  它们同块发射），不靠「只被 dec 消费」这种可伪造的形状。

### 9.3 PrivateGlobal 根

容器全局满足：容器类型（list/tuple）、入口私有、**唯一实写**且值 `trace` 到新分配
（`MakeList/MakeTuple/StructNew/ListNew/…`，且该分配值只流进 RC 临时栈记账与该 store）、
容器的每条 `LoadGlobal` 只被 `Len` / `IndexLoad` 容器 / `append/extend` 接收者 /
迭代源持有 / store 旧值 / 出口释放消费。

**存活论证**：私有 ⇒ 除本函数外没人能命名它，被调用户代码也读不到；唯一实写是新分配
⇒ 容器对象在函数内自始唯一、不会被替换或释放；只允许 append/extend ⇒ 既有元素那份
容器引用一直有效。于是「借用元素的存活期覆盖整个函数」，与 §2 的计数守恒完全同构；
出口的 `release_globals` 照旧释放容器（全局根表），候选全局在**同一位置**被清零，
所以 `GcShutdown` 看到的根表里不会留下借来的指针。

**对外语义**：入口全局被清零发生在函数出口（return 前），不在循环出口——所以
`for s in xs: ...` 之后读 `s` 仍拿到最后一个元素（CPython 语义），新增用例 566 第 2 条
钉死。异常路径不执行 `release_globals`（入口无 handler，未捕获异常即进程终止），
不会多 dec。

### 9.4 四个放宽的安全边界

1. **Class 值**（原整函数否决）：只在 FreshLocal/PrivateGlobal 根上放行。容器不可达，
   被调用户代码拿不到容器 → 无法让元素的计数在借用窗口内归零 → `__del__` 触发时机与
   基线逐点相同（借用只删掉本槽那份 +1）。
2. **ObjNew / ClassAddr** 移出硬拒清单：前者只分配**新**对象（不可能与既有容器别名），
   后者只取类对象地址、不写内存。`CallExtern/LoadExtern/StoreExtern/PtrLoad/PtrStore`
   与 `Gen*` 仍在清单里（任意外部写 / 挂起）。
3. **ClassNew → Pure**：`rt_class.c pa_class_new` 只分配类对象，不跑用户代码、不碰容器。
   （落 Bad 会把整个 `__pa_main` 挡掉——入口模块建类对象太常见。）
4. **根值的 RcInc/RcTemp 消费**：迭代源持有 `rc_inc`+`push_temp` 只增加引用，不会改/释放
   容器；容器仍由根槽（owned）或全局根持有到函数结束。`RcDec/RcTempTake/RcCleanup` 仍拒。

### 9.5 实测（abbench 式交替 A/B 绑核，flag-on vs flag-off）

方法：同一编译器、同一份源码，两侧独立 `PYADAMAS_CACHE_DIR` + `--rebuild`，每轮打乱
用例顺序、奇偶轮交换 A/B 先后、BURST=3 连跑取平均，15 轮中位数/p25。

| 用例 | off | on | 中位 | p25 | 产物 |
|---|---|---|---|---|---|
| obj_dispatch | 300.59 ms | 202.24 ms | **-32.7%** | -32.7% | 指令数 3.110e9 → 1.766e9（-43.2%） |
| dict_wordfreq | 232.56 ms | 217.55 ms | -6.5% | -6.8% | asm 变 |
| sort_struct | 298.70 ms | 290.79 ms | -2.6% | -3.0% | asm 变 |
| queue_sim / rec_memo / str_build | — | — | -0.2 / +0.1 / -0.2% | 噪声 | asm 逐字节同 |
| realistic 合计 | 1331.99 ms | 1210.39 ms | **-9.1%** | — | |

classic 窗 13 例合计 311.07 → 309.87 ms（-0.4%），无一例 >2% 的真实回退：
fasta +2.9%（p25 +15.8%）、fannkuch -5.9%、matmul +1.6%、binary_trees +1.3% 四例的
flag-on/off asm **逐字节相同**，是短用例机器噪声（docs/testing.md 的 ±3% 口径）。

内层循环（bb15）从 45 条指令降到 25 条：`index.get` 后直接 `store.global s` +
`vcall`，无 inc/dec、无旧值装载；`load.global s` 被后端 store→load 转发吃掉。

### 9.6 闸门

- 普通套件 **470 ran ok / 0 失败**、ASan 套件 **470 ran ok / 0 失败**（均 flag-on，8 作业）；
- difftest（O0 vs O2）flag-on **441 / 0 不一致**；
- 18 个基准用例 flag-on 输出与 CPython 逐字节一致；
- 新增回归 `tests/run/566_entry_global_borrow.py`（expect 来自 CPython 实跑）：命中形态、
  循环后读循环变量、append-only 根、循环体内 pop **必须拒绝**、容器被别的函数写过
  **必须拒绝**；
- flag-off 未跑本遍 → 12 个基准 + 6 个 realistic 的 asm 与改前逐字节相同（只 3 个案变）。

### 9.7 风险与遗留

- **门控仍默认关**（`PYADAMAS_BORROW_OPT=1` 才开）。默认化需要全量日常语料 A/B
  （0.338 的 d6_rec_memo +1~2% 布局效应尚未解释），按 2.4 的同口径另开一项。
- 已知未覆盖：`del s`、循环变量先被赋值再被 for 覆盖、容器来自 `ListSlice/StrToList`
  等运行时新容器、dict/set 根、生成器/协程。
- 未做支配性分析：私有全局根的唯一实写不检查「是否支配所有借用点」。靠的是
  「唯一写者 + 新分配值非空」——容器为 0 时 `len()` 先崩，走不到借用点；
  与 FreshLocal 根的既有口径一致。

