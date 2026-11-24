# P3 封闭 tagged union 设计（0.327，p3union 分支）

> 范围：unknown 表示层 P3——让 **tag 跟值走**（不跟容器），消除标量 unknown
> 的堆探针（u1 ~40%）与 float 结果盒 churn（u3 ~24%）。基线 0.325，HEAD
> 9346c32。独立 worktree /tmp/wt-p3union（分支 p3union），不推 master、不动
> 主树。设计输入：docs/unknown-repr-design.md（0.324，§2 信息论边界 / §4.2
> codon union / §5 路线）、docs/p2-container-tag-eval.md（0.325，P2 负收益
> 结论与 §5 给 P3 的输入）、codon 参考（.ref_proj/codon，只读）。
>
> **结论（先说）：选 B「平行 shadow tag 跟值走」+「float 盒原位复用」的混合，
> 按模块门控（封闭 union 活跃域）落地第一阶段。** 不选 A（16B 加宽槽）做第一
> 阶段：A 的全部收益（float 内联免盒）在「盒复用」落地后只剩 72B/盒的内存与
> 一次解盒间接，而代价是值宽 ABI 全量重构（形参/返回/槽/容器/EH/GC/生成器帧
> 全动），一轮做不完且半做态不可验证。B 的值仍是 8B i64（float 仍装箱），
> **所有既有 i64 边界（容器/字段/运行时/间接调用）天然正确**，tag 只是 RC/
> 分派快路径的判据，丢失即回退探针——正确性不依赖 tag 传播的完备性。这是
> 「最高价值最低风险步」：u1/u3 的两大成本（探针次数、盒 churn）都被结构性
> 消掉，而 26 个热基准 asm 逐字节不变可以**按构造保证**（见 §6）。
> 16B 内联 float 的封闭 union（A）列为第二阶段，在 B 已验证的地基上做。

## 1. 现状事实（本分支逐条核实，设计的锚点）

1. **unknown = 裸 i64**；float 进 unknown 装箱成 CPYFLOAT（0.324）；堆判别
   靠探针 rc_index_has（主哈希索引 + 叶子位图）+ exact-RC 回退（rt_core.c
   unknown_is_heap）。小整数 <65536 一条比较短路。
2. **unknown 槽/unknown vreg 从不寄存器提升**：backend/regalloc.cpp 槽提升
   门控显式跳过 Unknown（GC 要靠帧记录里的栈槽地址找根）；块内临时 GPR 池
   与跨块 vreg 提升同样跳过 Unknown/Float/Optional。所以 unknown 值全程在
   内存帧槽/vreg 栈槽/全局里——**加 tag 只影响内存路径与 unknown 的
   load/store，静态类型提升槽（热路径）完全不涉及**。
3. **标量 unknown 的 RC 序列**（lower/impl_slots.cpp store_slot/store_global）：
   retain_for_store(v)（unknown 源 → RcIncUnknown）→ old = load → store_raw
   → RcDecUnknown(old)；出口 release_slots/release_globals 再 RcDecUnknown。
   临时值 RcTempUnknown（先探针再入栈）。u3 每轮 IR 实测：
   load.global acc → index.get → pa_any_arith(探针解码×2) →
   rc_temp_unknown(探针) → rc_inc_unknown(探针) → load.global acc →
   store.global → rc_dec_unknown(探针) → cleanup。**每轮 ≥6 次哈希探针 +
   1 次新盒分配/回收**——与 P2 §1 perf 剖析逐条对应。
4. **kind2 内联 1-bit heap_bits 只服务静态已知 kinds**：backend/index.cpp
   gen_index_store 的内联 btsq/btrq 仅在 hk=0/1（静态标量/堆）走；hk=2
   （unknown 新值）只留「kind2 且无位图」内联（无位可维护），其余交运行时
   pa_list_set 探针。**给 unknown 值加平行 tag 不触碰这条内联路径**（P2 的
   死因是 2-bit 域升级要改 bts/btr 语义；本设计位图原样不动，tag 在旁路）。
5. **pa_list_append(heap=2) 会丢弃位图**（「一旦遇到 unknown，放弃逐元素
   跟踪」）→ list[object] 逐元素 append 后 heap_bits=NULL → 下标装载拿不到
   堆性位，元素侧判别只能探针。**若 append 时把 tag 已知的堆性传成 heap=0/1
   （而非 2），位图得以保留**，下标装载就能从位图直接得到 tag——元素侧探针
   也一并消掉。这是 B 方案自然导出的容器侧收益（P2 §5「容器元素域 tag 作为
   P3 子集导出」），且不改位图格式、不改内联 bts/btr。
6. **26 个热基准的 unknown 面**（基线 asm 逐文件 grep 核实）：12 个含
   call pa_rc_dec_unknown（b1/b3/b4/b5/binary_trees/d2/d4/d6/fannkuch/
   matmul/nqueens/queue_sim），全部在函数出口/全局释放等冷路径；4 个含
   pa_disable_unknown_index（入口一次）。**26 个全部不含 pa_any_* 调用**
   （P0 推断后无动态算术）。这是 §6 门控保证 asm 不变的实证基础。
7. **u3 的 acc+0.5 被降级器优化成 解盒→静态加→再装箱**（any_to_f64 +
   bin.add + any_from_f64）：0.324 为省 any_arith 调用做的优化，但每轮仍
   产生新盒。tag+复用落地后 AnyArithT 一次调用即完成分派+原位复用，严格
   更优——第一阶段移除该特例（unknown ± float 常量统一走 AnyArithT）。
8. codon 对照（llvisitor.cpp:2776 / type.h UnionType）：{i8 tag, max-size
   对齐缓冲}、封闭备选集、强推断先把绝大多数值定型、开放互操作走 PyObject
   盒。本设计同构：**推断优先（P0 已有）→ 封闭活跃域用内联 tag（本阶段旁路
   tag，第二阶段内联 16B）→ 开放 unknown 回退探针+盒（永远兜底）**。

## 2. 选型对比（A / B / C，ABI / 容器 / codegen / RC 四维）

| 维度 | A 加宽槽 {i8 tag,8B payload}→16B | B 平行 shadow tag 跟值走（本阶段） | C 指针低位 tag + 标量判别 |
|---|---|---|---|
| 值表示 | unknown 值变 16B；float 可内联（免盒） | 值仍 8B i64（float 仍 CPYFLOAT 盒）；tag 在平行帧区/平行全局符号，随值传播 | 堆指针低 3/4 位标 kind；标量无判别位（信息论边界 §2 不变） |
| ABI | **破坏性**：unknown 形参/返回/槽/全局/容器槽全变宽；跨模块签名要版本化；闭包统一约定重做 | **不变**：值仍是 i64，运行时/容器/字段/间接调用边界原样；tag 是编译器内部旁路，边界处丢失=回退探针 | 指针解释处全要掩码；unknown↔typed 边界要置位/清位 |
| 容器布局 | unknown 容器槽 16B，带宽翻倍；数值容器必须豁免（双布局） | 8B 槽不变；kind2 位图不变；append 传 tag 导出的 heap=0/1 保住位图（§1.5） | 容器槽不变；但 kind2 内联 bts/btr 与低位 tag 的堆性判定要重新对齐 |
| codegen 改动面 | 所有触碰 unknown 的指令生成器改宽度；regalloc 槽偏移/帧布局/EH 帧记录/GC 扫描/生成器帧全动；~100 个 CallRt 边界要装拆 pair | 只在「有动态算术的模块」（门控）里：unknown load/store/mov 多一条 tag 搬运；RC unknown 三入口改 tag 判别；Any* 改带 tag 入口；其余路径**逐字旧代码** | 所有指针消费点掩码（热路径 deref 也要）→ 静态路径 asm 变，违反约束①；或只在 unknown 边界置位→str 等未装箱堆值进 unknown 仍要探针 |
| RC/GC | 标量 pair 零 RC（最干净）；但帧记录/扫描/展开全要按新宽度重做，正确性风险集中 | tag≥3 → 直接 rc_inc/dec（免探针）；tag 1/2 → 跳过；tag 0 → 旧探针路径。EH 展开/GC 扫描/生成器帧**完全不动**（payload 字仍是权威值，tag 字是小整数不进扫描区） | 低位 tag 与 RC 索引键冲突（索引按完整指针哈希），所有 inc/dec/索引要规范化掩码 |
| 消探针（u1 ~40% / u3 ~25%） | 全消（tag 语义判别） | 全消（标量侧 tag 传播 + 元素侧位图 tag，§1.5） | 只消「已置位堆值」的探针；int/未置位堆值仍探针；float 盒仍探针或靠低位 kind |
| 消盒 churn（u3 ~24%） | 结构性消除（float 内联无盒） | **盒原位复用**消除（§4，rc==1 校验 + 引用计数证明）；盒仍在但零分配/回收 | 不解决（float 仍装箱） |
| 内存 | 16B/unknown 值（容器翻倍） | 8B/值 + 8B/unknown 槽·vreg·全局（仅门控模块的帧/数据段；容器不付） | 8B 不变 |
| 热路径 asm | 静态路径可豁免，但 unknown 槽偏移位移会波及同帧静态槽 | **门控模块外逐字节不变（按构造）**；门控模块内静态路径也不变（tag 只挂 unknown） | 静态 deref 掩码 → 变 |
| 一轮可验证性 | 低（全量 ABI，半做态不可验证） | **高**（tag 是提示，漏传播=回退探针=旧行为；值语义零变化） | 中（指针规范化面大，正确性押在掩码完备） |
| 开放/封闭分治 | 封闭 union 内联；开放要 other 盒备选 | 封闭活跃域（有动态算术的模块）用 tag；开放 unknown = tag 0 探针+盒兜底 | 开放（但标量判别不足） |

**C 否决**：解决不了 int/float 判别（信息论边界），且指针掩码面与约束①冲突
（0.324 §4.5 已记录）。**A 不做第一阶段**：收益主体（免探针+免 churn）B 已
覆盖，A 的增量（float 内联省 72B/盒 + 一次间接）值得做但不值得在第一轮押上
全量 ABI 重构。**B 采用**，配盒复用补上 churn；A 作为第二阶段在 B 的 tag
传播地基上把 FBOX 换成内联 FLOAT（届时值宽 ABI 只影响已门控的封闭域）。

## 3. 第一阶段表示与传播（B 的具体形态）

### 3.1 tag 编码（i64，与 payload 平行存放）

| tag | 名 | payload 含义 | RC 动作 | 算术解码 |
|---|---|---|---|---|
| 0 | UNK | 任意 unknown i64（旧表示） | **探针回退**（pa_rc_*_unknown 原路径） | 探针 + kind（any_decode 原路径） |
| 1 | INT | int64 / bool / None（裸字） | 跳过 | 直接取 i |
| 2 | FRAW | double 位模式（静态 float 实参，未装箱） | 跳过 | memcpy 取 f |
| 3 | FBOX | CPYFLOAT 盒指针 | 直接 rc_inc/dec | 读盒 payload（免探针免 kind 加载） |
| 4 | STR | str 指针 | 直接 rc_inc/dec | 直接当 str |
| 5 | HEAP | 其它堆指针（list/dict/实例/…） | 直接 rc_inc/dec | 加载头 kind 分派（免探针） |

堆 ⇔ tag≥3。**tag 是提示不是语义**：payload 单独仍是合法的旧 unknown i64
（float 已装箱），任何边界丢 tag 都只损失性能、不损失正确性。

### 3.2 存放位置（只影响内存，§1.2）

- **帧**：vreg 栈槽区之后新增平行 tag 区，每个 unknown 类型 vreg 一个专属
  tag 字（不复用，避免陈旧 tag）；每个 unknown 命名槽一个 tag 字。序言清零
  （=UNK）。tag 字是小整数 0..5，不进 GC 扫描区/位图（扫描区仍只覆盖 vreg
  payload 区，build_frame_scan_info 不动）。
- **全局**：每个 unknown 全局在 payload .quad 旁发一个平行 tag 符号
  （.LgtN / 跨模块 pyadamas$mod$gt$name），初值 0。GC 根表仍登记 payload
  地址（不动）。
- **生成器堆帧 / 类字段 / 容器槽**：第一阶段不加 tag（装载得 UNK → 探针
  回退，正确；性能与今天持平）。列为第二阶段。

### 3.3 传播规则（tag 跟值走）

- 生产：ConstInt/None/Bool→unknown 写 INT；AnyFromF64 返回 FBOX（rdx）；
  str/堆 coerce 写 STR/HEAP；AnyArithT/AnyNegT 结果 tag 由运行时经 rdx 返回
  （INT/FBOX/STR）；静态类型 vreg 作 unknown 实参时 tag 按类型常量导出
  （Int→INT、Float→FRAW、Str→STR、堆→HEAP）。
- 搬运：Mov/LoadSlot/StoreSlot/LoadGlobal/StoreGlobal（unknown）payload 与
  tag 同搬。store_slot 的「unknown 槽升级成已知类型」既有逻辑不动（升级后
  按静态类型导 tag）。
- 容器边界：
  - **取出**（IndexLoad，list[unknown]）：kind0→INT、kind1→HEAP、kind2 有
    位图→读位（HEAP/INT，免探针）、kind2 无位图→call pa_any_load_tag（一次
    探针得 tag，比今天「每次使用都探针」少）。DictGet/字段/生成器等其它
    i64 来源→UNK（回退）。
  - **存入**（ListAppend/IndexStore/MakeList 等，unknown 值）：heap 标志由
    tag 导出（≥3→1、1/2→0、0→2 回退）。append 传 0/1 保住位图（§1.5）；
    tag 0 仍传 2（丢位图，旧行为）。位图格式与内联 bts/btr 逐字不动。
- 调用边界（第一阶段保守）：直调/间接/闭包统一约定**仍传 i64 payload**，
  被调 unknown 形参槽 tag=UNK（探针回退），unknown 返回 tag=UNK。tag 过
  调用传播（双字 ABI）列第二阶段——u1/u3 无用户调用，不影响本阶段目标。
- RC：RcIncUnknown/RcDecUnknown/RcTempUnknown 在门控模块内联 tag 判别
  （0→旧调用回退；≥3→直接 pa_rc_inc/dec/temp；1/2→跳过）。临时栈本身
  不动（只推堆指针，与今天一致）；EH 展开按帧记录释放仍走探针（冷路径，
  不动）；GC 扫描不动。

### 3.4 门控（封闭活跃域 / 开放 unknown 分治）

**门控 = 模块 IR 含 AnyArithT/AnyCmpT/AnyNegT**（降级器把 unknown 动态
算术/比较统一降到 T 入口；没有动态算术的模块根本不含这些指令）。门控关：
不分配 tag 区、不发 tag 搬运、RC 三入口与别名/转发优化**逐字旧代码**，
Any*T 实参 tag 用静态类型常量/0。门控开：启用 §3.3 全部传播。

- 26 个热基准全部不含 pa_any_*（§1.6）→ 门控全关 → **asm 逐字节不变按
  构造成立**（不是「测出来不变」，是「代码路径根本没进」）。
- 只有异构 print/isinstance 而无动态算术的模块也保持旧行为（正确，性能
  持平）；有动态算术的模块（u1/u2/u3、247、新增 248+）进入封闭活跃域。
- 语义上这就是 codon 式分治：能封闭（可枚举域上的动态分派）就内联 tag，
  不能封闭就探针+盒兜底；推断（P0）继续从源头缩小 unknown 面。

## 4. float 盒原位复用（消 u3 ~24% churn）

**模式**：acc = acc + x（acc 是 unknown 槽/全局，x 任意）。IR 序列固定为
arith → temp_unknown → inc_unknown → load old → store → dec_unknown(old)
→ cleanup（§1.3）。

**机制**：AnyArithT/AnyNegT 增加 reuse 提示实参（第 6 个整型寄存器）。
codegen 在同一基本块内做模式匹配：结果 dst 被 StoreSlot/StoreGlobal 存回
S，且某实参的定义恰是 LoadSlot/LoadGlobal S，中间无对 S 的其它写入 →
把该实参 payload 作提示传入。运行时：结果应为 float 且提示是 CPYFLOAT 盒
且 **rc==1**（唯一持有者就是即将被覆盖的 S——装载是借用不 inc，rc==1 排除
了容器/别处共享）→ 原位写新 double，**返回同指针且 rc++**（调用方拿到
独立的一份，S 的那份不动）。否则正常新盒。

**引用计数证明**（复用命中，box=B）：
arith 返回 B rc=2（S 的 1 + 产生者 1）→ temp_unknown 接管产生者份（rc 2）
→ inc_unknown（rc 3）→ store S=B → dec_unknown(old=B)（rc 2）→ cleanup
dec B（rc 1，S 持有）。与不复用路径（新盒 rc1→…→S 持有 1，旧盒归零释放）
净效果一致，只是旧盒不被释放而是被改写。rc>1（常量缓存盒 rc≥2、列表共享
盒 rc≥2、被别处 retain 的盒）一律不复用——**常量缓存的 is 语义、容器元素
完整性都不受影响**。统计模式下复用路径少计一次 alloc（文档标注）。

**匹配的安全性**：提示只来自「即将被同一结果覆盖的槽的当前值」；运行时
rc==1 再验唯一所有权。匹配失败/跨块/Mov 链 → 提示 0 → 正常新盒（正确，
只是不复用）。u3 的 acc+0.5 特例移除后也走 AnyArithT（§1.7），同模式命中。

## 5. 改动面（第一阶段）

- runtime/rt_any.c：any_decode_tagged（tag 直接分派，UNK 回退旧解码）；
  pa_any_arith_t / pa_any_neg_t（op,a,tag_a,b,tag_b,hint → rax payload,
  rdx tag；复用逻辑）；pa_any_cmp_t；pa_any_is_float_t；pa_any_to_f64_t /
  pa_any_to_i64_t；pa_any_load_tag（i64→tag，一次探针+kind）。旧入口全保留
  （门控关模块/容器内部仍用）。
- runtime/rt_internal.h：声明 + tag 常量（与 codegen 共享口径）。
- src/ir/ops.h：RtFn 增 AnyArithT/AnyCmpT/AnyNegT/AnyIsFloatT/AnyToF64T/
  AnyToI64T/AnyLoadTag + rt_name；src/ir/ir.cpp：新入口进「必须保留 unknown
  索引」集合（UNK 回退要探针）。
- src/lower/expr_ops.cpp：AnyArith/AnyCmp/AnyNeg → T 入口（tag 由 codegen
  装填，降级器只改实参表：去掉 desc，留 op/a/b）；移除 unknown±float 常量
  的解盒-再装箱特例（§1.7）。isinstance/int()/float() 的 unknown 分支 → T。
- src/backend/impl.h：tags_enabled_ 门控、tag 区偏移/操作数缓存、
  is_pair 辅助、静态类型导 tag。
- src/backend/regalloc.cpp：tag 区分配（unknown vreg/槽专属字）、帧布局
  计入 tag 字数、序言清零。
- src/backend/ops.cpp：Mov/LoadSlot/StoreSlot/LoadGlobal/StoreGlobal/
  Const*/Convert 的 unknown 分支搬 tag（门控内）；unknown 槽禁用别名/
  常量折叠（门控内，冷路径让位正确性）。
- src/backend/block.cpp：store-load 转发/load 省略对 unknown 槽禁用（门控
  内）；DictTryGet 融合写 unknown 槽时清 tag。
- src/backend/call_rt.cpp：Any*T 装填（payload+tag，rdx tag 落 dst tag 字）；
  RC 三入口 tag 内联；reuse 模式匹配；ListAppend/MakeList 等 heap 实参由
  tag 导出（门控内）。
- src/backend/call.cpp / backend/index.cpp：IndexLoad unknown dst 的 tag
  导出（kind/位图/探针三路）；IndexStore hk2 门控内交运行时带 tag heap。
- src/backend/codegen.cpp：unknown 全局平行 tag 符号发射。
- **不动**：容器布局/位图格式/内联 bts/btr、临时栈、EH 帧记录与展开、GC
  扫描、生成器帧、类字段、闭包统一约定、cimport/C ABI、推断/单态化。

## 6. 硬约束对照

1. **静态已知路径 asm 逐字节不变**：门控按构造保证 26 基准（§3.4）；门控
   模块内静态类型路径也不进 tag 分支。验证仍做 26 基准逐字节 cmp + 数值
   循环（nbody/spectral/matmul）重点复核 + A/B。
2. **不撞 kind2 内联 1-bit heap_bits**：位图格式/语义/bts/btr 逐字不动；
   tag 在旁路；append 的 heap=0/1 只是让运行时**保留**位图（§1.5），内联
   写入路径（静态 hk）完全不涉及。
3. **0.321 字符串 RC 成果**：字符串叶子索引/页位图/池不动；tag STR 只是
   免探针直接 inc/dec，走同一 pa_rc_inc/dec。
4. **RC/GC 正确**：tag 提示+探针回退（§3.1 健全性）；复用 rc==1 校验+引用
   计数证明（§4）；EH/GC/临时栈不动。ASan detect_leaks=1 抽查。
5. **异构正确性对拍 CPython**：247 保持 + 新增 248+（O0/O1/O2 expect 对拍
   + difftest）。

## 7. 分阶段计划

| 阶段 | 内容 | 风险 | 状态 |
|---|---|---|---|
| P3-1（本阶段） | B 平行 shadow tag 跟值走（槽/全局/vreg/容器边界）+ 盒原位复用 + 模块门控；Any*T 运行时；移除解盒-再装箱特例 | 中低（值语义零变化，tag 可回退） | 本分支落地 |
| P3-1b | tag 过直调形参/返回传播（双字 ABI，闭包 shim 适配）；DictGet/字段/生成器帧 tag 化 | 中 | **0.328 落地（p3next，docs/p3-1b-landing.md）**：直调旁路双字 ABI（值 ABI 零变化 + 双入口 shim）、dict/字段一次探针导 tag、生成器 vreg tag（堆帧槽列剩余）；顺带根治三个基线 leak/UAF |
| P3-2 | A 内联：封闭域 FBOX→FLOAT 内联（16B {tag,payload} 仅 unknown 槽/vreg，容器仍 8B+边界装箱）；容器元素域 tag 由本阶段位图+tag 自然导出 | 中高（值宽 ABI，门控域内） | 0.328 评估：GC 查表标记对 FRAW 天然安全（新事实），但边界装箱完备性取代探针回退成为正确性前提，收益只剩 72B/盒+一次间接 → 本轮不做，路径见 p3-1b-landing.md §6 |
| P3-3 | 跨模块 ABI 版本化；None/bool 盒/小整数 tag；开放 unknown 的 PyObject 式通用盒（对齐 codon Generic 桥） | 高 | 长期（0.328 评估记录见 p3-1b-landing.md §7） |

P3-2 的前置条件已由 P3-1 铺好：tag 传播链、门控域、边界装拆点、复用证明。
届时 float 内联只改「FBOX 的生产/消费」为「FLOAT 内联」，探针回退链不变。

## 8. 验证计划（本阶段）

- tests/run/248 起新增：异构标量循环（acc/循环变量/复用命中与不命中：共享盒
  rc>1、常量缓存盒、列表元素盒）、tag 回退（容器取出/字段/生成器/间接调用）、
  复用正确性（acc=acc+acc、y=acc; acc=acc+1; print(y)）、异构 print/算术/
  比较对拍 CPython，O0/O1/O2。
- difftest 全 0；普通套件 + ASan 套件 ran ok EXIT=0（PYADAMAS_TEST_JOBS=8）；
  ASan detect_leaks=1 抽查 247/248/13_gc/231；check_size 不新增超限。
- 26 基准 asm 逐字节 cmp（门控按构造应全同）；u1/u2/u3 冷路径 A/B（绑核
  交替多轮取中位）+ perf 复核探针/盒 churn 占比下降。
- CHANGELOG 0.327 预留（与 stdlib 并行，集成重编号）；提交落 p3union。

## 9. 已知边界（本阶段明确不做）

1. tag 不过调用边界（形参/返回 UNK 回退）——P3-1b。
2. 生成器帧/类字段/dict 值无 tag（UNK 回退，性能持平）——P3-1b。
3. float 仍装箱（72B/盒），复用只消 churn 不消内存——P3-2 内联。
4. 复用只匹配同块「load S → arith → store S」直接形态；Mov 链/跨块不匹配
   （正确，只是不复用）。
5. 统计模式复用路径少计一次 alloc/free（perf 口径文档标注）。
6. None/bool 仍裸 0/1（0.324 边界不变）；isinstance(unknown,int) 仍不精确。
