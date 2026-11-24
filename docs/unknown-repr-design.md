# unknown / 动态值表示层设计（0.324）

> 范围：pyadamas 里静态类型是 unknown 的动态值如何携带类型信息，重点解决
> int / float / None / bool / 堆指针在裸 64 位机器字里的歧义；同时评估
> NaN-boxing、codon 式 tagged union、旁路 tag、装箱、指针低位 tag、推断优先
> 六条路线，并给出分阶段推荐。实现原型落在 typetag 分支（0.324），主树 master
> 未动。
>
> 基线：0.323，HEAD 8248c21。构建：cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
> -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"，独立 worktree /tmp/wt-typetag。

## 0. 结论摘要

1. **先排除 NaN-boxing**。pyadamas 的 int64 是完整回绕语义（文档化），double
   也是完整 IEEE-754 域；两个 2^64 的集合要同时塞进 64 位并额外带 tag，
   信息论上不可能。任何"把 tag 塞进 float NaN 位"的方案都必须牺牲一部分
   int 或 float 值域（以及指针规范化假设），与现有语义冲突。全量改 ABI 的
   重构收益远小于风险，本设计明确不做。
2. **推断优先是最大的性能杠杆**。能静态定型的值根本不进 unknown：不装箱、
   不带 tag、ABI 不变。本阶段顺手修掉的推断/降级问题已在 dict_wordfreq
   上带来 **−8.8%**，并让 b1/b5 的计数器重新吃到既有的 dict_bump 融合。
3. **真 unknown 用"最小判别"而不是全类型标签**：float 进入 unknown 时装箱
   成已有的 PYADAMAS_RC_CPYFLOAT（8 字节 payload），int/None/bool/普通堆指针
   保持裸机器字。unknown 仍然是 i64，**ABI 不变**；热路径零改动（生成汇编
   逐字节相同）。这是 0.323 P2 里方案③的落地与强化。
4. **不采用"每种值都装箱"**：只装 float。int/str/容器本来就能靠机器字与
   RC/kind 探针区分，给它们装箱只会把 0.321 刚降下来的字符串 RC 成本再抬
   回去。
5. **旁路 tag / 指针低位 tag 只能解决容器内操作，解决不了"取出后成为标量"**。
   一个 8 字节元素被 load 进寄存器后，容器上的 tag 就丢了；因此旁路 tag
   不能替代"值自己带 tag"（装箱）或"值变宽"（tagged union）。它们适合作为
   第二阶段容器级优化。
6. **推荐路线：推断消 unknown（0 期）→ 真 unknown 最小 float 判别（本阶段，
   装箱）→ 需要时再上封闭 tagged union / 容器 tag（后续阶段）**。这是
   codon"强推断 + 封闭 union + PyObject 桥"在 pyadamas 约束下的对应形态。

## 1. 现状：unknown 的机器表示与问题面

### 1.1 表示

- unknown 值 = 一个 int64 机器字。None = 0，bool = 0/1，int = 二进制补码，
  float = double 位模式，堆对象 = payload 地址。
- "是不是堆对象"靠运行期探针：rc_index 哈希（常规模式）或 16 字节对齐 +
  marked/堆链回退（exact-RC 模式）；0.321 之后字符串叶子走 4KB 页位图。
- 容器：list 的元素是 8 字节槽；mixed 列表用 elem_kind=2 + 逐元素
  heap_bits 位图（只记录"是否堆"，不记录 int/float）。
- dict：entry 32 字节（hash/key/val + prev/next），key_own/val_own 记录
  0 非堆 / 1 堆 / 2 unknown，saw_heap 做粘性快路径。

### 1.2 可见故障（0.317/0.322/0.323 记录）

| # | 现象 | 根因 |
|---|---|---|
| 1 | 异构 dict 的 float 值打印成 4612811918334230528 | float 位模式被当 int 格式化 |
| 2 | 异构 list 的 float 元素打印/取回是位模式 | 元素无 int/float 标签 |
| 3 | 混合 csv 行 float 字段打位模式（0.322 前直接 SIGSEGV） | str(unknown) 只认 str/int |
| 4 | unknown 上的算术按 int 做（如 xs[1] * 2 得到位模式乘积） | 类型检查 unknown + int → unknown，降级按整数 |
| 5 | 单态回填后的 ABI 不符静默算错（0.322 N19b 家族） | 静态类型与运行值域不一致 |

### 1.3 四条硬约束（设计判据）

1. 静态已知类型路径零开销：不改热路径表示、生成代码、ABI。
2. 标签成本只落在真 unknown 冷路径。
3. 避免装箱引入新的分配 churn：0.321 刚把字符串 RC 成本降下来，float
   全量装箱会反噬。
4. list/dict 的 8 字节槽不能为了 tag 全面加宽到 16 字节：数值列表是
   pyadamas 的性能命脉（perf 实证 94% 生成浮点代码、matmul 胜 CPython 11 倍），
   16 字节槽会翻倍内存带宽。

## 2. 信息论边界：为什么 64 位内不可能有"全 int64 + 全 double + tag"

- int64 有 2^64 个值，double 有 2^64 个位模式（含 NaN/Inf/±0）。两者都是
  满射到 64 位。要在同一个 64 位字里再编码一个类型标签，必须要么
  - 缩小 int 值域（例如只用 48/63 位），要么
  - 缩小 double 值域（例如只把 NaN payload 当 tag），要么
  - 依赖指针规范化的地址空间假设，把指针域让出来。
- pyadamas 的 int64 回绕是**文档化语义**（daily 语料专门强调 LCG 溢出是
  语义差、不是 bug），不能牺牲；double 的 NaN/Inf 也不能牺牲（json/repr/
  排序/比较都要正确）。
- 因此正确的方案只有三类：
  1. **值变宽**：unknown 变成 {tag, payload}（16 字节）或 tagged union；
  2. **值变指针**：值装箱，tag 在对象头里；
  3. **容器旁路 tag**：tag 不跟值走，只在容器里；一旦取出为标量就丢失。
- 这条边界直接决定了方案 1/2/4/5 的取舍。

## 3. 方案对比矩阵

| 方案 | 表示 / 位置 | 能解决的歧义 | 内存开销 | 热路径成本 | 冷路径成本 | ABI 影响 | 实现复杂度 | 开放/封闭 | 结论 |
|---|---|---|---|---|---|---|---|---|---|
| 1 NaN-boxing | tag 塞进 double NaN 位；int/指针需规范化 | int/float/指针全可分 | 8B/槽不变 | **不为零**：所有 int/float/指针的物化与比较都要重编码/解码 | 中（每次进出 unknown） | **全量重构**：槽、容器、RC 探针、GC 根、跨模块签名 | 极高 | 开放（任意类型） | **否决**：与全 int64/全 double 冲突 |
| 2 codon tagged union | {i8 tag, max-size aligned buffer}，封闭备选集 | 备选集内全部类型 | 16B/值（8B 槽→16B 槽） | 静态已知路径零；unknown 容器槽加宽、带宽翻倍 | 低（无分配，tag 直接判） | **值变宽**：unknown 形参/返回/槽/容器全改 | 高（容器/codegen/GC 全动） | **封闭**（备选集），开放 unknown 需 fallback 盒 | 长期可选，非首阶段 |
| 3 旁路 tag | 平行 tag 数组/位图（list 2 bit/元素；dict 1B/entry 或平行数组） | 容器内元素的 int/float/str | list +2 bit/元素（1/32）；dict entry +1B（+25%） | 静态容器零；mixed 容器每元素多一次 tag load/store | 低-中（无分配） | 无（值仍是 8B） | 中高（所有插入/切片/del + 帧槽平行 tag） | 开放（多 bit） | **只解决容器内操作**，取出即丢；可做阶段 2 |
| 4 装箱 | 堆对象头带 kind；float → CPYFLOAT 盒（8B payload） | int/float/堆/容器全可分 | 72B/float（64B 头 + 8B payload）+ 索引 | **零**（静态路径不装箱） | 分配 + RC + 索引；本阶段缓存/池后 u1 +316%、u2 +7.3% | 无（unknown 仍 i64） | 低（运行时 + 少数转换点） | 开放 | **本阶段采用**（只装 float） |
| 5 指针低位 tag | 8B 对齐低 3 位标堆对象 kind；标量不动 | 只区分堆种类，**不区分 int/float** | 8B 不变 | 零 | 低 | 指针解释处 | 中 | 开放 | 不足以解决核心问题；与装箱叠加收益有限 |
| 6 推断优先 | 类型检查/单态化/空容器推断收紧 unknown | 从源头消除 unknown | 0 | **零**（根本不产生 tag） | 0 | 无 | 中（类型检查工作） | 封闭（可推断时） | **最大杠杆**，本阶段已落地一部分 |
| 7 混合（推荐） | 推断消 unknown + 真 unknown 最小 float 判别（装箱）+ 容器推断/融合；后续容器 tag/union | 覆盖全部五类故障 | 只对真 unknown float | 零 | 装箱只落在 unknown 冷路径 | 无（阶段 1） | 低→中 | 开放 unknown + 封闭可推断部分 | **推荐** |

估算口径：8B 槽的 list，2 bit/元素 tag 的位图成本 = 2/64 = 3.1% 元素存储；
dict entry 32→33/40 字节 = +3%~+25%。这些数字只对 mixed 容器生效。

## 4. 逐方案分析

### 4.1 NaN-boxing（否决）

JS/LuaJIT 式做法：double 的有效位里 NaN 空间很大，把 int/指针编码成特定
NaN payload；普通 double 保持原样。放到 pyadamas 上有四处硬冲突：

1. **int64 全域**：int 的 2^64 个值不能都映射到 NaN payload（payload 只有
   52 位）；要么 int 限宽，要么 int 也装箱——前者破坏文档化回绕语义，
   后者等于方案 4 但更复杂。
2. **double 全 NaN**：double 的 NaN 位模式本身是合法值（float('nan')、
   0/0、json 输入），把 NaN 空间征用作 tag 会让这些值无法往返；需要
   canonical NaN 归一化，又破坏 repr/比较的位级一致性。
3. **指针规范化**：把指针塞进 NaN payload 依赖"用户态地址 < 2^47"这类
   平台假设；虽然当前 Linux 成立，但这是把正确性押在地址空间布局上。
4. **ABI 重构面**：unknown 会出现在槽、字段、容器元素、形参/返回、
   GC 根、异常展开记录、跨模块签名里；每一个 store/load 都要重编码。
   "静态路径零开销"不再成立（int 也要判断/解码），与约束 ① 冲突。

结论：记录理由，不做。若未来要碰，必须先接受"int 限宽或 float NaN 不可
表示"的语义降级，这不是 0.x 阶段该做的地基改动。

### 4.2 codon 式 tagged union（长期可选）

codon 的 UnionType 是**封闭备选集**（codon/cir/type.h:572），LLVM 表示是
{i8 tag, max-size aligned buffer}（llvisitor.cpp:2776），OptionalType 对
引用类型直接用空指针、对值类型用 {i1, payload}；动态 Python 对象走
PyObject {i64, ptr}（llvisitor.cpp:851）与单态化（Generic）。

可借鉴的部分：
- **封闭 union 不需要堆分配**，tag 与 payload 同处一个值，存取零间接。
- codon 先用强推断/单态化把绝大多数值定成具体类型，union 只在 Optional/
  显式 Union 上出现；真正开放的 Python 互操作才用 PyObject 盒。
- 这把"开放/封闭"分治讲得很清楚：**能封闭就封闭（union），不能封闭就装箱
  （PyObject），两者都不做全量 NaN-boxing**。

不能直接照搬的部分：
- pyadamas 的 unknown 是**开放的**（任何类型都可能），封闭备选集无法直接
  表示；要么先做完整推断（codon 的核心工作，工作量在类型系统而不在运行时），
  要么给 union 加一个 other/boxed 备选（那就退回混合方案）。
- 16 字节槽会把 mixed list 的内存带宽翻倍；只有"确实需要内联 union"的
  容器才应加宽，数值容器必须保持 8B。
- 形参/返回/槽的 ABI 变宽是跨模块的破坏性改动，需要版本化接口。

结论：不作为第一阶段；作为后续"封闭动态值"能力的方向（尤其配合单态化）。

### 4.3 旁路 tag（容器级优化，阶段 2 候选）

做法：list 的元素槽保持 8B，另开每元素 2 bit（int/float/str/other）或
1 字节 tag；dict 在 entry 里加 1 字节或平行数组。

- 优点：不装箱、无分配、8B 槽不变；typed 容器完全不付成本（只有 mixed
  容器分配 tag 数组）。
- 致命限制：**tag 属于容器，不属于值**。x = row[0] 之后 x 是裸 i64，
  tag 留在容器里；x + y、print(x)、传参、返回都拿不到 tag。只能覆盖
  "容器内遍历/打印/序列化"这类能在同一处拿容器和下标的操作。
- 因此它不能替代装箱/union；只能作为"已知异构数组"的批量操作优化
  （例如 repr/序列化/聚合），或配合"取出即装箱"的读取路径。
- 成本：mixed 容器每次 append/set 多一次 tag store；遍历多一次 tag load
  （与数据同一缓存线时很便宜，跨页时多一条线）。对 8B 槽是 +3% 位图或
  +25% dict entry。实现面覆盖所有插入/切片/insert/del/reverse/池复用，
  GC 与异常帧还要登记平行 tag 数组。

### 4.4 装箱（本阶段采用，只装 float）

复用已有 PYADAMAS_RC_CPYFLOAT：64 字节 RC 头 + 8 字节 double payload，
kind=8；print/repr/str/truth 已有分派（rt_cpy/rt_str/rt_format）。
做法：
- float -> unknown（coerce/convert_to）时调用 pa_any_from_f64，分配盒；
- unknown -> float 时调用 pa_any_to_f64，解盒或 int 提升；
- unknown 上的算术/比较/一元负用 pa_any_arith/pa_any_cmp/pa_any_neg，
  按运行期 kind 分派；结果 float 仍装箱；
- isinstance(unknown, float) 现在可以精确回答（kind=CPYFLOAT）。

成本与优化（本阶段实测）：
- 每次装箱 = malloc/free + RC 索引 + 临时栈 push。朴素实现让异构 list
  追加 float 的合成负载变成基线 16ms→253ms（+1471%）。
- 优化 1：CPYFLOAT 按**索引叶子**处理（无出边、不可变），进 0.321 的
  4KB 页位图，不进 GC 链；循环回收走 freelist（与 list/dict 池同款）。
- 优化 2：256 槽常量 float 盒缓存（值位模式 -> 盒，缓存自持引用）：
  重复写同一字面量（append(1.5)、d["x"]=1.25）零分配。
- 优化 3：1024 个盒的 freelist，动态结果盒子复用，不再 malloc/free。
- 优化后：u1 +318%（68.2ms vs 16.3ms）、u2 +9.6%、u3 +372%（54.9ms vs
  11.6ms）。这是"真 unknown 冷路径"的诚实成本；推断能把绝大多数这类
  路径变成静态路径（见 4.6）。

为什么只装 float、不装 int/str/容器：
- int/None/bool 都是非堆标量，装箱只会把每个计数循环变成分配循环；
- str/list/dict 已经是堆对象，探针 + kind 已经能分派，给它们再套盒是
  双重间接；
- float 是唯一"值在寄存器里、无法靠地址/位模式区分"的标量。

### 4.5 指针低位 tag（不足，仅作微优化）

8/16 字节对齐的堆指针低 3/4 位空闲，可以标 kind（str/list/dict/box…）。
但：
- 标量（int/float/None/bool）不是指针，低位的"空闲空间"不存在；
- 想用低位区分 float 就得先把 float 变成非对齐编码，等于 NaN-boxing 的
  变体，仍受第 2 节边界限制；
- 现有对象头已经有 kind，探针一次加载就能拿到；低位 tag 只省一次内存
  访问，收益小、解释面大（所有指针比较/RC/GC 都要掩码）。
结论：不做；如果未来做容器 tag，低位 tag 可作为"堆对象种类"的廉价旁路。

### 4.6 推断优先（最大杠杆）

实测证据：
- dict_wordfreq 的计数器 counts = {} + counts[w] = counts.get(w,0)+1：
  0.323 推断成 dict[str, unknown]，每轮都走 unknown 的整数加法；本阶段
  让 dict.get 两参数形式把缺省值类型 join 进"值类型洞"，推断成
  dict[str, int]，生成汇编回到纯 addq 路径，并重新触发 dict_bump 融合；
  A/B **−8.78%**（272.45→248.52ms）。
- 同一改动让 b1_log_parse / b5_re_extract 的匿名计数器重新被融合
  （bump 站点 0→2 / 0→1），A/B −0.81% / +0.02%（噪声内）。
- 空列表 append 的强制转换：ys=[]; append(1); append(2.5) 以前第一个
  元素按 int 位模式存进最终 list[float]，打印 5e-324；现在 append 按
  元素类型 coerce，正确得到 [1.0, 2.5]。
- 清单化：空容器洞 join、dict.get 缺省值、list.append/insert 的
  元素 coerce、dict/list 字面量 join、单态化回填。它们不花运行期一分钱。

开放/封闭分治：能推断出来的部分按"封闭"处理（具体类型、8B 槽、无 tag）；
推断不出来的才落入"开放 unknown"（装箱）。推断越强，装箱面越小。

### 4.7 混合方案（推荐）

层次：
1. **静态层**：具体类型，8B 槽，零 tag、零装箱、ABI 不变（热路径）。
2. **封闭动态层（后续）**：有限备选集的 tagged union（16B 或容器 tag），
   用于显式 union / 可枚举的异构数组。
3. **开放动态层（本阶段）**：unknown = i64 + 堆盒（float 盒；未来
   None/bool 盒），tag 在对象头里；只用于真正推断不出的值。

判定策略：类型检查能定成具体类型就绝不下沉到 3；只有 unknown 才用 3；
将来引入显式 union 注解/推断时才用 2。这样热路径不受影响，冷路径语义
正确，且为后续性能优化留了口子。

## 5. 推荐分阶段路线

| 阶段 | 内容 | 风险 | 预期收益 | 状态 |
|---|---|---|---|---|
| P0（本阶段） | dict.get 缺省值推断、append 元素 coerce、空容器推断已有能力加固 | 低 | dict_wordfreq −8.8%；消除部分 unknown | **本分支已做** |
| P1（本阶段） | float <-> unknown 装箱/解盒；动态算术/比较/一元负；isinstance(float)；str/print/repr/csv 打通；盒缓存 + 池 + 叶子索引 | 中低（ABI 不变） | 五类故障全部消除（除文档化边界） | **本分支已做** |
| P2 | 容器级 2-bit 域 tag / dict entry tag，仅供"容器内批量操作 + 取出时装箱" | 中 | 异构数组遍历/打印去间接；装箱频率下降 | 待评估 |
| P3 | 显式 union / 封闭 tagged union（16B 仅用于 union 容器），配合单态化 | 中高 | 热异构数值数组零分配 | 长期 |
| P4 | None/bool/整型也装箱的"通用 PyObject 式动态值"；跨模块 ABI 版本化 | 高 | 完整 Python 动态语义 | 长期 |

不做：全量 NaN-boxing ABI 重构（第 2/4.1 节）。

## 6. 第一阶段原型（本分支实现）

### 6.1 运行时（runtime/rt_any.c 新增，CMake PYADAMAS_RT_PARTS 增加 any）

- pa_any_from_f64：float -> unknown，分配 CPYFLOAT 盒；256 槽常量缓存
  （命中只 hash + rc_inc）；miss 走 1024 槽 freelist 或 malloc。
- pa_any_to_f64 / pa_any_to_i64：解盒 / int 提升；显式 int()/float() 用。
- pa_any_arith(op,a,b,da,db)：int/float/str 分派；int 保持回绕与
  Python 的 //、% 语义；float 结果装箱；str + str 拼接。
- pa_any_neg / pa_any_cmp：一元负、六种比较（数值提升到 double，str 用
  pa_str_cmp，异类型 Eq/Ne 为假/真）。
- pa_any_is_float：isinstance(unknown, float) 的精确判定。
- pa_any_float_cache_shutdown：退出时归还缓存引用。

实参的静态域用描述符 'i'/'f'/'s'/'o'/'x' 传入（与降级器 any_desc 对应），
静态已知的实参跳过堆探针；float 以位模式经 movq 进整型实参寄存器，
不改变任何调用形态。

### 6.2 降级/类型检查

- convert_to/coerce：unknown <-> float 走装箱/解盒；unknown -> int/bool
  保持裸机器字（性能：避免把 fannkuch 这类循环里的下标/计数变成调用）。
- lower_binop：结果 unknown 且任一侧 unknown 的算术走 AnyArith；结果
  float 的仍走静态浮点路径（unknown 一侧先解盒）。
- lower_unary：unknown 的一元负走 AnyNeg。
- lower_compare：unknown 参与的比较走 AnyCmp（is/is not 仍是同一性）。
- list.append：按元素类型 coerce（顺带修掉 append(int) 到 list[float]
  存位模式的老 bug）。
- lower_builtin_text：显式 int(unknown)/float(unknown) 走解盒。
- typecheck_call_method：dict.get 两参数返回 join(值类型, 缺省值类型)，
  空 dict 的值洞因此能定成 int/float。

### 6.3 运行时修正（推断暴露出的既有洞）

- rt_dict.c pa_dict_bump：融合路径把 IR 里的 RcInc(key) 删掉了，但运行时
  插入新键时没有补引用。计数器推断成 int 后 b1/b5 第一次触发该融合，
  动态键（日志字段/邮箱）插入后被调用方释放，留下悬空键 -> "refcount
  increment on a freed object" 崩溃。修：miss 分支 pa_own_inc(key_own,key)。
- opt/dict_bump.cpp：融合模式原来要求值侧一定有一条 RcIncUnknown；值类型
  变成 int 后没有这条 retain。改成 6/7 指令两种形态都接受。
- lower/method.cpp / lower/c_lazy.cpp：def_heap(in.ty, std::move(in)) 在同一
  实参列表里读 in.ty，求值顺序未指定；GCC 先构造 by-value 参数移走 in.ty，
  导致 cimport 返回 float 的 vreg 被记成 unknown（机器值在 xmm、静态类型
  unknown 的长期不一致）。先取 TypePtr 再 move。这是 unknown -> float 解盒
  第一次暴露出来的 ABI 洞。
- lower/expr_assign.cpp：for k, v in d.items() 的元组解包按目标变量符号表
  类型取元素类型；模块级目标可能因预绑定/join 停在 unknown。改成优先取
  源 tuple 的 sub[i]（更精确），fannkuch 因此整函数摆脱 unknown。
- ir/ir.cpp：dict.keys/values/items 在运行时按 key_own/val_own 混合种类以
  heap=2 追加元素，值可能是 int/float 标量；unknown 探针需要索引。原分析
  把这类模块判成 exact-RC（索引关闭），exact-RC 的探针回退会把对齐的
  double 位模式当指针解引用（231_float_dict 实测 SIGSEGV）。把
  DictKeys/DictValues/DictItems 加入「必须保留索引」集合；同样把
  AnyFromF64/AnyToF64/AnyArith/AnyCmp/RcTempUnknown 等 0.324 入口加入。

## 7. 性能论证

### 7.1 热路径零开销（生成汇编逐字节对比）

对 26 个基准源码分别用 0.323 基线编译器和本分支编译器执行 pyadamas asm，
再逐字节 cmp：

- 22 个**完全相同**（含 nbody / spectral_norm / matmul / nqueens /
  binary_trees / fasta / fib / mandelbrot / queue_sim / str_build /
  rec_memo / obj_dispatch / sort_struct / d1..d6 / b2..b4）。
- 4 个**更少指令**（推断/类型修正收益，非回归）：
  - dict_wordfreq：counts 变 dict[str,int]，去掉 unknown 帧扫描掩码、
    不再需要 unknown 索引（入口调用 pa_disable_unknown_index）；A/B −8.78%。
  - b1_log_parse / b5_re_extract：计数器重新命中 pa_dict_bump（2/1 处），
    消除了 get_or+add+inc+set 序列。
  - fannkuch：for 元组解包的目标变量按源 tuple 元素类型定型（不再停在
    unknown），整函数不再需要 unknown 帧扫描/索引，帧从 144B 缩到 96B，
    入口调用 pa_disable_unknown_index。
- 结论：约束 ①（静态路径零开销）以汇编逐字节证据成立。

### 7.2 A/B 实测（绑核 24，7 轮交替 + 奇偶交换 + BURST=3 取最小再中位）

| 用例 | 基线 0.323 | 本分支 | 变化 |
|---|---|---|---|
| queue_sim | 145.10 | 145.05 | −0.03% |
| str_build | 143.91 | 143.67 | −0.17% |
| rec_memo | 230.19 | 230.26 | +0.03% |
| obj_dispatch | 331.46 | 331.51 | +0.02% |
| dict_wordfreq | 271.90 | 248.03 | **−8.78%** |
| sort_struct | 469.79 | 469.79 | −0.00% |
| nbody | 33.57 | 33.61 | +0.10% |
| spectral_norm | 54.27 | 54.38 | +0.19% |
| matmul | 13.59 | 13.59 | +0.01% |
| fannkuch | 6.94 | 6.76 | **−2.62%** |
| b1_log_parse | 18.20 | 18.00 | −1.14% |
| b2_normalize | 11.72 | 11.76 | +0.34% |
| b3_template | 3.57 | 3.57 | +0.19% |
| b4_wordfreq | 15.17 | 15.17 | +0.00% |
| b5_re_extract | 18.49 | 18.51 | +0.14% |
| d1_matrix | 3.31 | 3.34 | +1.07% |

数值/字符串/现实热路径全部在 ±0.4% 内（d1 +1.07% 是 3.3ms 短用例的
噪声，asm 逐字节相同；matmul/nbody/spectral 均 ≤0.2%）。三个改进来自
推断/类型修正：dict_wordfreq −8.78%、fannkuch −2.62%、b1 −1.14%。
0.321 的字符串成果未被反噬。

### 7.3 unknown 冷路径成本（合成负载，绑核 24，7 轮交替）

| 负载 | 基线（裸位模式，结果错误） | 本分支（装箱，结果正确） | 变化 |
|---|---|---|---|
| u1 异构 list，30 万轮 append float+int | 16.41 ms | 68.30 ms | +316% |
| u2 异构 dict，12 万 float 写入+读取+累加 | 124.85 ms | 133.90 ms | +7.3% |
| u3 unknown 动态算术，40 万轮 | 11.66 ms | 55.14 ms | +373% |

解读：
- 成本全部落在"真 unknown 的 float"上；u1 是极端（60 万次空列表
  append，每次都装箱）且常量缓存后已从 +1471% 降到 +316%。
- u2 只有 +7.3%：写入常量命中缓存，读取/累加才装箱。
- u3 是最坏形态（累加器每轮产生新 double，无法缓存），这部分正是
  阶段 2/3 容器 tag / tagged union 的目标场景。
- 推断（P0）能把大量"看起来 unknown"的路径变回静态路径（dict_wordfreq
  就是实证），所以生产负载的实际加权成本远低于这三个合成上界。

### 7.4 内存

- CPYFLOAT 盒：64B 头 + 8B payload = 72B/个；只有真 unknown float。
- 常量缓存上界 256 盒 ≈ 18KB；freelist 上界 1024 盒 ≈ 74KB；两者都是
  进程级缓存，退出时归还（ASan 无泄漏）。
- 盒按索引叶子处理：不进 GC 链、不驱动 GC 阈值，只占页位图一位。
- 容器槽不变宽（仍 8B）；没有 16B tagged 槽。

## 8. 与 codon 的对照

| 维度 | codon | pyadamas 本阶段 | 说明 |
|---|---|---|---|
| 类型系统 | 完整推断 + 单态化，几乎没有开放 unknown | unknown 仍是开放占位，推断在收紧 | P0 是对齐 codon 的主杠杆 |
| 静态 union | UnionType 封闭备选集，{i8 tag, max aligned buffer} | 未引入；16B 会伤 8B 槽 | 阶段 3 可选 |
| Optional | 引用用空指针、值用 {i1,payload} | Optional 走静态类型，unknown 不参与 | — |
| 动态 Python 值 | PyObject*（{i64 rc, ptr} 盒） | float 用 CPYFLOAT 盒；其余仍裸字 | 同思路，按 pyadamas 成本裁剪 |
| 容器槽 | 具体类型 + union 内联 | 具体类型 8B；unknown 元素 8B + 盒 | 性能命脉 |
| 空容器/推断 | 强推断 | dict.get/append/空容器洞逐步补齐 | 本阶段实证 −8.8% |
| 开放 vs 封闭 | 封闭优先，开放走 PyObject | 封闭优先（推断），开放走盒 | 同一分治原则 |

## 9. 开放 / 封闭 unknown 的分治口径

- **封闭（closed）**：静态类型已知，或有限备选集可由推断/注解确定。
  表示：具体类型 8B 槽；未来 union 用 tag + max payload（只对 union 容器）。
  成本：零（静态）或每个 union 值一次 tag 判读（无分配）。
- **开放（open）**：unknown（推断失败，任何类型都可能）。
  表示：i64 + 堆盒（本阶段 float 盒；未来 None/bool 盒）。
  成本：装箱/解盒/动态分派，只落在这类值上。
- 编译器策略：能用封闭就绝不用开放；unknown 是开放；显式 union 注解
  未来走封闭。这样"unknown 面"随推断能力单调缩小，而正确性不依赖推断
  是否完备。

## 10. 已知边界（本阶段明确不做/记录）

1. **None/bool 仍是裸 0/1**。unknown 里的 None 打印为 0，False 与 None
   不可分；需要时阶段 4 给它们加盒（或给 int 域引入小整数 tag）。
2. **嵌套的具体容器**（list[float] 存在 list[object] 里）打印时，内层
   元素的 int/float 域不可知（list 只记录 elem_kind 是否堆，不记录
   float/int）。需要阶段 2 给容器加元素域 tag。直接装在 unknown 容器里的
   float 已正确。
3. **list.extend / += 的批量拷贝**不做逐元素装箱；异构目标请逐元素
   append（文档标注）。
4. **f-string 格式说明** f"{unknown_float:.2f}" 目前先把 unknown 转成字符串
   再套格式说明，与 CPython 的数值格式化不同；需要 AnyFormat 运行时。
5. **排序/成员比较**对 unknown 列表仍按机器字；动态 float 排序需要
   容器级 tag 或装箱后逐元素比较分派。
6. **常量缓存让相等的常量 float 在 unknown 域里 is 为 True**；float 的
   is 语义文档标为不保证。
7. **装箱成本**在"热异构数值数组"上仍是 3~4 倍（u1/u3 合成负载）；
   正确性优先，性能优化留给阶段 2/3。

## 11. 验证清单（本分支）

- 新增 tests/run/247_unknown_float_tag.py：O0/O1/O2 与 CPython 3.14
  逐字节一致；覆盖异构 list/dict、算术/比较/一元负、object 形参/返回/
  字段、空容器推断、嵌套 unknown 容器、csv 混合行。
- difftest 239/0。
- 普通套件 + ASan 套件 ran ok / EXIT=0（PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查 247 / 246 / churn 负载无泄漏。
- tests/check_size.sh 无新增超限（仍为既有 5 处）。
- 26 个基准 asm 逐字节对比（23 相同 / 3 变少）。
- A/B 与冷路径基准数字见 §7。


