# P3-1b 落地记录（0.328，p3next 分支）

> 接续 docs/p3-tagged-union-design.md §7 的 P3-1b。独立 worktree /tmp/wt-p3next
> （分支 p3next），基线 0.327（HEAD 1501040）。与 stdlib2 并行，集成时 CHANGELOG
> 重编号。本文记录：落地的四块增量、双字 ABI 的具体形态、三个基线 leak/UAF
> 的根治、P3-2/P3-3 的可行性评估与剩余清单。

## 1. 落地增量（每块独立构建验证 + 提交）

| # | 内容 | 提交 | 量化 |
|---|---|---|---|
| 1 | unknown 调用结果所有权洞 + leaf×unknown RC 探针对称 | 8db2bc7 | j12/m5/144B 三形态 leak 清零 |
| 2 | tag 过直调形参/返回（双字 ABI 旁路 + 双入口 shim） | 5046fd7 | u4_call_tag −34.2% |
| 3 | dict 值/类字段装载一次探针导 tag | aa52c0f | u5_dict_multiuse −12.9% |
| 4 | object 槽升级 RC 洞 + yield unknown 消费份额 + 生成器 vreg tag + send 装箱 | 34b34fc + 步骤3 提交（分支 HEAD） | 265/266 钉住，leak 抽查全 0 |

u1/u3 无用户调用/无 dict 多使用，A/B 噪声级（−1.9%/−2.0%），无回归。
26 热基准 asm 逐字节 26/26 全同（每块增量后都 cmp）。

## 2. 直调 tag ABI 的具体形态（「旁路双字」而非值宽 ABI）

设计 §7 原计划「双字 ABI + 闭包 shim 适配」。落地选了**值 ABI 零变化**的旁路
形态，把破坏面压到最小：

- **payload 落位逐字不变**：classify 原样（寄存器/xmm/栈），容器/字段/运行时/
  间接调用边界全部不感知。
- **形参 tag 走平行数组 + %r11 指针通道**：调用方在出参区栈实参之后写
  unknown 形参的 tag 数组（outgoing_bytes 按字数扩容），leaq 数组指针 → %r11，
  call 被调 `.LfN$pt` 入口。
- **被调双入口**：旧入口 `.LfN`/`fname`（闭包/vtable/间接/跨模块的全部既有
  引用）先 `xorl %r11d` 再 jmp 函数体；`$pt` 入口直通。序言 tag 区清零后按
  %r11 装载形参 tag（%r11==0 → 全 UNK 探针回退）。gadget 不动 rsp，CFI
  CFA=rsp+8 不变，LSDA 按标签差由汇编器解析自动让位。
- **返回 tag 走 %rdx**：unknown 返回函数在 gen_return 发 `movq tag_src, %rdx`
  （与 AnyArithT 双字返回同形）。旧约定调用方只读 %rax，忽略 rdx —— 无兼容
  问题，不需要返回 shim。
- **资格判定**：门控内 + 本地非生成器 + 实参/形参个数对齐 + 形参**槽类型**
  unknown（不是声明类型！槽升级会把 object 形参槽定型成 bool/str，argparse
  add_argument 实测：按声明类型数会往 $0 里 movq）。
- **正确性不依赖传播完备**：任何旧约定调用落旧入口 → 形参 UNK；跨模块走
  间接 Call → 旧入口；生成器调用走 GenNew → 不涉及。tag 丢失 = 探针回退。

被调内效果：形参 tag → LoadSlot 搬 tag → RC 三入口/AnyArithT 全免探针；
返回 tag → 调用方 dst tag 字 → 后续 RC/算术免探针。u4（40 万轮 object 调用
累加）−34.1% 即此链收益。

## 3. dict/字段：一次探针导 tag（布局零变化）

dict 值/类字段没有平行 tag 来源（容器/对象仍 8B 槽）。门控内 unknown 装载
发一次 pa_any_load_tag（探针+kind），后续使用免探针：DictGet/GetOr/GetOpt/
Pop/PopLast（emit_call_dst_tag 路由）、FieldLoad、DictTryGet 融合直写槽
（原清 UNK 改探针；miss 槽留旧值，探针与旧 payload 一致，释放判定等价）。
u5（小 dict 多使用：取→存→算术→比较）−11.9%；u2（24 万条目大 dict，字符串
拼接/哈希主导）+0.3% 噪声级——探针收益被字典本征成本淹没，符合预期。

## 4. 三个基线 leak/UAF 根治（stdlib-batch1 §四半移交 + 新发现）

### 4.1 unknown 调用结果所有权洞（j12 str 104B / m5 盒 72B / 144B 双盒）

根因：用户调用返回 object 时，被调按 emit_return 契约给调用方保留一份引用
（RcIncUnknown / fresh take），但 def_heap 只给静态堆类型结果登记临时栈——
unknown 结果直接消费（print/算术）时那份引用无人释放。落槽形态被「全局根
LSan 可达」掩盖，直打形态稳定泄漏。修复：def_heap 对 Call/CallDirect/VCall*
的 unknown 结果补 push_temp_unknown（只限用户调用 op；CallRt/Bin/IndexLoad
的借用结果不登记；不进 fresh_vregs——可能是整数）。26 基准无 unknown 调用
结果（IR 逐文件核实），asm 按构造不受影响。

### 4.2 leaf×unknown RC 探针对称（exact-RC 模式）

pa_rc_inc_unknown / pa_rc_temp_unknown 原先只查 rc_index_has：exact-RC 模式
索引关闭，leaf（str/bytes/float 盒）inc 恒空操作而 dec_unknown 走回退会真 dec
——保留/释放不对称。改用与 dec_unknown 逐位一致的 pa_unknown_is_heap（含
exact-RC 回退）。常规模式行为逐字不变（索引命中即返回）。

### 4.3 object 槽升级 RC 洞 + yield unknown 消费份额（新发现，UAF 链）

store_slot 把 unknown 槽在首次 typed 存储时升级（int 特化）。显式 object 标注
的槽也被升级：`s: object = 0; s = s + x` 按非堆槽裸存——不 retain、旧值不
释放、load 按 int 读回：float 盒被 cleanup 提前释放成野指针、指针按整数打印
（函数内累加器 return 形态）。基线「不漏」是假象：盒提前释放后靠池复用 +
缓存悬挂指针掩盖成 UAF 链（PYADAMAS_RC_TRACE 可见 alloc 后无 free 却「无未
释放对象」——内存已被复用）。修复三管：

1. 显式 object/Any 标注槽禁止升级——按标注 AST 精确判定（AnnAssign 的
   object/Any + Param.ann_unknown，含生成器形参）。**不用 sym.annotated+
   unknown**：typecheck 对循环内 `j: int` 也给该组合（实测），误标会改变
   d4_primes/b3_template/b5_re_extract 三基准 asm（拦截后 26/26 全同）。
2. 兜底 de-upgrade：unknown/堆值流入已升级非堆槽 → 回退 unknown 走 retain/
   释放口径（非标注推断槽的 RC 安全网）。
3. 配套：yield 为消费方 inc 的那份引用，unknown 元素在 next()/__next__/send/
   throw/for-gen/list(gen)/genfor 七个消费点原先只给堆元素登记临时栈——补
   push_temp_unknown / RcDecUnknown 对称口径。槽修复后该洞暴露为 72B/盒干净
   泄漏（265 gen 段 + RC_TRACE 定位），一并根治。

### 4.4 send/throw 值装箱（新发现）

g.send(2.5) 的 float 裸位模式进 gen.sent 被当整数（0.327 基线已存在）。
send/throw 值 coerce 到 unknown（装箱）；同步 resume 内消费，调用方临时份足够。

## 5. 生成器 vreg tag（P3-1b 步骤3 的已验证子集）

一期整函数禁 tag 的两个根因已修：tag 区清零挪到 g.entry 块头（每次 resume
都经过，垃圾 tag 不跨 resume 残留）；GenSent 写 UNK。**unknown vreg 启用
tag**（常规帧，per-resume）；**堆帧命名槽仍无 tag**（装载 UNK 回退）——
堆帧平行 tag 区要动 rt_gen.c 帧布局 + yield/resume 存还原，列剩余。
门控生成器（unknown+unknown 算术）内 RC/算术免探针；266 钉住 for-gen/
list(gen)/next/send/close + str 累加 + send 装箱，118/119/122/123/124/128/
129/131/133/136 回归全过。

## 6. P3-2 评估（FBOX→FLOAT 内联）：本轮不做，记录可行路径

**新事实（本轮核实）**：GC 的保守扫描用 gc_mark → gc_tab_find **查表**，
不对 unknown 字解引用——裸 double 位模式查不到表即不标记，**GC 扫描对
FRAW 天然安全**（设计 §2 担心的「GC 扫描全要重做」比预想轻）。EH 释放走
dec_unknown 探针（常规模式只查索引）+ tagged RC tag 1/2 跳过，也安全。

**真正的代价在边界装箱完备性**：FRAW 的 payload 不再是「合法的旧 unknown
i64」——所有**无 tag 消费 payload** 的边界必须先装箱，漏一处 = 静默值损坏
（正确性从「不依赖传播完备」变成「依赖边界装箱完备」，违反一期硬约束的
精神）。需审计的边界清单：print/str/repr/f-string、isinstance/truth、容器
存入（append/index_store/dict_set/field_set/make_list）、间接调用/闭包/跨
模块、生成器 yield（堆帧槽无 tag）、cell 写入。约 20 处，每处都要「tag==
FRAW → 装箱」分支 + 回归。

**收益**：盒复用已消 u3 churn，P3-2 增量只剩 72B/盒内存 + 一次解盒间接
（设计 §2 同结论）。u1/u3 边际收益小。

**结论**：风险/收益比不划算，本轮不做。下一轮若做：先落「边界装箱审计表 +
逐边界测试」，再开 FRAW 生产（AnyArithT/AnyNegT 返回 FRAW），门控域内灰度。

## 7. P3-3 评估（跨模块 ABI 版本化 + None/bool 盒）：长期，记录

- 跨模块 tag ABI：本轮 tag ABI 是模块内旁路（直调本地函数）。跨模块要走
  接口版本 bump（modcache 失效）+ 导出 $pt 符号 + 门控协调（调用方门控开、
  被调方可能关）。收益面：跨模块 unknown 调用（实际负载少见）。间接调用
  旧入口 UNK 回退已保正确。
- None/bool 盒：完整动态语义（isinstance(unknown,int) 精确、None/bool 身份）
  是语义工程，不是表示层优化——0.324 边界（None/bool 裸 0/1）维持。

## 8. 剩余清单（下一步）

1. 生成器堆帧命名槽平行 tag（rt_gen.c 帧布局 + yield/resume 存还原）。
2. P3-2 边界装箱审计表 + FRAW 灰度（§6 路径）。
3. 跨模块 tag ABI 版本化（P3-3）。
4. 已记录移交的独立 bug：dict 值里 list[unknown] 的 print repr 把 boxed
   float 按整数打（iso1 形态，0.327 基线已存在，基线/本分支输出逐字相同）；
   253 fallback 路径 32B 异常对象泄漏（stdlib-batch1 移交，rt_exc.c）。
5. 复用匹配扩展：Mov 链/跨块（一期边界 §9.4 维持）。

## 9. 验证汇总

- 26 热基准 asm 逐字节 cmp：每块增量后 26/26 IDENTICAL。
- difftest：257 用例 0 不一致（O0/O2 自差分）。
- 普通套件 + ASan 套件：ran ok EXIT=0（PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查：247/248/13_gc/231/262/263/264/265/266 全 0；
  j12/m5/pf3/pf5（leak 复现族）全 0。
- 生成器回归：118/119/122/123/124/128/129/131/133/136 全过。
- check_size：无新增超限（发射集中 backend/tag.cpp 382 行；
  backend/call.cpp 1199 / backend/block.cpp 1200 / lower/stmt.cpp 1200；
  既有 5 处超限文件不变）。
- A/B（绑核 24，交替多轮中位，同一运行时仅生成代码不同）：
  u4_call_tag −34.2%（73.3→48.3ms，7 轮）、u5_dict_multiuse −12.9%
  （67.0→58.4ms，7 轮）、u1 +0.8% / u3 +0.0%（11 轮）/ u2 +0.3%（噪声级，
  无回归）。
