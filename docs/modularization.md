# 模块重组路线（2026-11-22 起）

## 1. 现状基线（实测，2026-11-22）

| 指标 | 值 |
|---|---|
| 源文件 | 124 个 `.cpp` + 49 个 `.h`，共约 73,900 行 |
| 超 1000 行的文件 | 19 个（前几名卡在 1190–1195，紧贴 1200 硬限制） |
| `AsmGen` | 约 300 个方法、**161 个字段**，实现散在 **29 个 `.cpp`** |
| `Lowerer` | 约 224 个方法、**97 个字段**，实现散在 **43 个 `.cpp`** |
| `codegen_impl.h` | 1191 行，被 **33 个 TU** include（含 81 处内联函数体） |
| `lower_impl.h` | 1114 行，被 **44 个 TU** include |
| 改 `codegen_impl.h` 一行 | 重编译 **33 个 TU，53.7 秒** |
| 改一个普通 `.cpp` | 重编译 **1 个 TU，8.2 秒** |
| 文件级可变 `static` | **1 个**（几乎无全局状态） |
| `codegen` ↔ `typecheck` 反向依赖 | **0**（阶段边界干净） |

**判断**：阶段级（lexer→parser→desugar→typecheck→lower→opt→codegen）是干净的，
问题在**类级聚合**——所有碎片都挂在三个巨型类上。

## 2. 五步计划

- [x] **第 0 步 · 让约束有名有姓**（2026-11-22 完成，见 §3）
- [x] **第 1 步 · 拆头文件**（2026-11-22 完成，见 §5）
      ⚠ 原定验收「改 regalloc 头后重编译 TU ≤ 8」**没有达成，也达不成** ——
      见 §5.1 的更正：单个类的声明拆成几个头不改变重编译代价，那条要等第 2 步。
      实际达成的是「改被搬走的函数体 = 1 个 TU」（54.3 s → 7.9 s）。
- [~] **第 2 步 · AsmGen 拆组合对象**（进行中，1/5）
      ✅ `Frame`（帧布局 + 帧记录）—— 2026-11-22 完成，见 §6
      ✅ `Emitter`（输出缓冲 / 标签号 / 行缓冲）—— 2026-11-22 完成，见 §6.4
      ✅ `TagLower`（tag 影子类型 + 零开销异常状态）—— 2026-11-22 完成，见 §6.5
      ✅ `RegAlloc`（槽/跨块 vreg 提升状态）—— 2026-11-22 完成，见 §6.7
      ⬜ `RegAlloc`（全局分配 / 扫描 / live range splitting）
      ⬜ `TagLower`（shadow tag / 直调 tag ABI / 零开销异常）
      ⬜ `VectPlanner`（codegen_vect* / codegen_simd*，约 2500 行）
      验收：单类方法数 ≤ 60、字段数 ≤ 40。
- [ ] **第 3 步 · 目录按阶段分**：`src/{driver,front,ir,low,opt,gen}/`（纯移动）。
- [ ] **第 4 步 · Lowerer 拆分**（优先级低于 AsmGen：它职责更单一）。
- [ ] **第 5 步 · 换闸门**：1200 行限制保留，新增「单类方法数 ≤ 60 / 字段数 ≤ 40 /
      头文件扇入 ≤ 15」三条结构指标，脚本化进 `tests/check_size.sh`。

## 3. 第 0 步（已完成）：约束有名有姓

### 问题

向量化器的核心判据以前是 `try_vect_loop` 里的匿名 lambda：

```cpp
auto int_ok = [](VK k) { return k == VK::InvI || k == VK::VecI; };
```

它没有名字、没有注释、没有测试，而且**漏了 `VK::J`**（归纳变量）。后果：任何
「j 参与算术」的循环整段判 Fail，持续了多个版本没人发现 —— 直到 0.391 排查
`spectral_norm` 时才发现。

### 做法

| 文件 | 内容 |
|---|---|
| `src/intmath.h`（新） | `is_pow2` / `pow2_log2`：标量除法强度削减与向量化判据**共用一份算法** |
| `src/vect_rules.h`（新） | `enum class VK` + `int_operand_ok` / `float_operand_ok` / `index_affine_ok` / `reducible_ok`，每条带「为什么」的注释 |
| `src/vect_rules_test.cpp`（新） | 40 条 `static_assert` 真值表，**每次构建都验证** |
| `codegen_vect.h` | 枚举改成 `using VK = vect::VK;`，类内写法不变 |
| `codegen_vect.cpp` | 两个 lambda 换成具名判据 |
| `codegen_div.h` | `pow2_log2` 不再自己实现，复用 `intmath.h`（保留 uint64 域与 2^63 边界） |

### 验证

- 反向验证：故意把 `VK::J` 从 `int_operand_ok` 删掉 → 构建**立刻** `static assertion failed`；
  恢复后重建通过。这条规则从此不可能再被静默改坏。
- 行为中性：12 个基准输出与 CPython 全一致；matmul 指令数 10,605,901 → 10,605,949
  （+48 条，链接/对齐噪声）；向量化触发数不变（matmul 4、spectral_norm 2、470=44、472=41）。
- 闸门：369 ran ok、difftest 348/0、check_size 通过。

## 4. 度量口径

改前改后都按这两条量，别用「感觉」：

```bash
# 改一个头文件的重编译代价（关 ccache，否则看不出真实增量）
cp src/codegen_impl.h /tmp/b; echo "// x" >> src/codegen_impl.h
CCACHE_DISABLE=1 /usr/bin/time -f "%e 秒" cmake --build build/lead -j4
cp /tmp/b src/codegen_impl.h

# 类规模
awk "/^class AsmGen/,0" src/codegen_impl.h | grep -cE "^\s+[A-Za-z_:<>,\* &]+ [a-z_][A-Za-z0-9_]*\s*(=|;)"
```

**注意**：`touch` 不会触发重编译（ninja 按 mtime 判），必须真改内容；
而且改完要恢复内容再量下一个，否则恢复动作本身会让下一次测量失真（踩过）。
## 5. 第 1 步（已完成）：拆 `codegen_impl.h`

### 5.1 先更正一条错误的验收标准

路线图原本写「拆完头文件后，改 regalloc 相关头只需重编译 ≤ 8 个 TU」。
**这条是错的**：一个类的声明拆成几个头文件，不会减少重编译 —— 任何实现该类
任一方法的 TU 都需要**完整的类定义**。实测确认：

| 改动 | 重编译 | 耗时 |
|---|---|---|
| 改被搬走的函数体（`codegen_impl.cpp`） | **1 个 TU** | **7.9 s** |
| 改薄壳 `codegen_impl.h` 一行 | 30 个 TU | 55.0 s |
| 改 `codegen_regalloc.h` 一行 | 30 个 TU | 54.6 s |

所以「33 → 8」必须等**第 2 步**把 `AsmGen` 拆成多个对象之后才可能。
第 1 步的真实收益只有一条：**改那 41 个有逻辑的函数体从 54 s 变成 8 s**。

### 5.2 结果

| 文件 | 行数 | 内容 |
|---|---|---|
| `src/codegen_impl.h` | 1192 → **249** | 薄壳：public 开关 + 23 个核心字段 + 4 个类体片段 include |
| `src/codegen_core.h` | 507 | 位置状态 / 输出辅助 / 目标指示符 / 函数·块·指令·对象·调用 |
| `src/codegen_regalloc.h` | 101 | 全局寄存器分配 / live range splitting |
| `src/codegen_frame.h` | 90 | 栈槽分配 / 帧记录 / 帧布局·尾声 |
| `src/codegen_tag.h` | 129 | shadow tag / 直调 tag ABI / 零开销异常 |
| `src/codegen_impl.cpp` | 276（新） | 41 个搬出来的函数体 |

**内联体：搬 41 / 留 40。** 留下的都是每行发射路径上的：`line`（13/40 行，
每行发射入口）、`fresh_label`、`func_label`、`slot_mop`（34 行，查表核心）、
`src_mop`、`dst_mop`、`mem_mop` 一族，以及 9 个一行 `set_*` 开关和 21 个一行 getter。
判据是「被别的内联函数调用 / 在热路径上」。

类体成员程序化比对：原 320 个 = 新 5 个文件拼起来 320 个，missing/extra 均为空。
`codegen_vect.h` / `codegen_simd.h` / `codegen_emit.h` 仍是**在类体内 `#include`**
的片段，机制不变（类体里不许出现 `namespace` / `#include` —— 踩过）。

### 5.3 验证

| 项 | 结果 |
|---|---|
| 构建 | 绿 |
| 普通套件 | 369 ran ok，0 fail |
| difftest | 348/0 |
| ASan | 369 ran ok，0 报告 |
| check_size | 通过（最大新文件 507 行） |
| 12 基准 × 3 优化档 | 输出与 CPython 全一致；指令数最大偏差 +0.17%（`binary_trees`，小程序的固有噪声） |
| 向量化触发数 | 不变（matmul 4、spectral_norm 2、470=44、472=41、473=17） |

更强的证据是**生成汇编逐字节相同**：15 个输入 × 3 档（45 份）md5 全同，
`tests/run` 全部 349 个用例 -O2 汇编 md5 全同。

### 5.4 没做的事

- `codegen_impl.h` 里 13 个**文件级** `inline` 自由函数（`reg32`/`reg8` 各 26 行等）
  不在「类成员」口径内，未动 —— 后续可选收益。
- 声明层面的重编译代价没变（30 个 TU），这不是第 1 步的目标，是第 2 步的。
## 6. 第 2 步（进行中）：AsmGen 拆协作对象

### 6.1 已完成：`Frame`（帧布局 + 帧记录）

新文件 `src/frame.h`（60 行）：`class Frame` 持有原来散在 AsmGen 里的帧状态 ——

| 类别 | 成员 |
|---|---|
| 布局量（5） | `out_bytes` `locals` `pad` `bytes` `cfa_off` |
| 帧记录（6） | `owned` `owned_unknown` `scan` `scan_words` `scan_mask` |
| 方法（2） | `vreg_slot_off(slot, n_slots)`、`layout(total_slots, n_saved_regs)` |

**为什么只拆到「数据 + 纯计算」**：`frame_alloc` / `emit_epilogue` / `outgoing_bytes` /
`build_frame_scan_info` 会发指令、读 `target_abi()`、`call_ptag_words()`，硬搬进去
会变成一堆反向依赖。它们留在 AsmGen，改为读 `frame_.*`。这条边界是刻意的。

AsmGen 侧只留两个一行转发（`vreg_slot_off` / `layout_frame`），**52 处字段用法**
改成 `frame_.x`，调用点写法不变。

### 6.2 模式（后面四个照抄）

1. 只搬**状态 + 不依赖发射器**的计算；需要发指令的留在 AsmGen 读 `collab_.x`；
2. AsmGen 留一行转发，避免改动大量调用点；
3. 验证用**产物等价**（12 基准输出 + 指令数），不是靠"看着没改逻辑"。

### 6.3 验证

| 项 | 结果 |
|---|---|
| 构建 | 绿 |
| 普通套件 | 369 ran ok |
| difftest | 348/0 |
| ASan | 369 ran ok、0 报告 |
| check_size | 通过 |
| 12 基准输出 | 全一致 |
| 12 基准指令数 | 除 `binary_trees` −0.27%（4.7M 指令的小程序固有噪声）外全部 ±0.000% |
### 6.4 已完成：`Emitter`（输出侧状态）

新文件 `src/emitter.h`：`class Emitter` 持有三样输出侧状态 ——

| 成员 | 说明 |
|---|---|
| `out`（MachFunc） | 结构化 Machine 层缓冲 |
| `label_id` | `.Lxxx` 的编号，每次 emit_function 归零 |
| `line_buf` | `line()` 的前导 tab 缓冲（复用容量避免每行一次分配） |

**≈391 处**用法改成 `emit_.x`，横跨 10 个文件，调用点写法不变。`pos_` 特意**没搬**：
它在别的类里也有 29 处同名使用，且语义是「当前指令在块内的下标」，不属于发射缓冲。

验证：构建绿 / 369 ran ok / difftest 348/0 / ASan 369 ran ok 0 报告 / check_size 通过；
12 基准输出全一致，指令数除 `binary_trees` +0.20%（同前，小程序的链接布局噪声）外全部 ±0.000%。

发射方法（`emit0/1/2`、`line`、`directive`、`op_*` 工厂）仍留在 AsmGen 读 `emit_.*`。
等下次把它们整体搬过来，`Emitter` 才名副其实地成为「发射器」。
### 6.5 已完成：`TagLower`（tag + 零开销异常状态）

新文件 `src/tag_lower.h`：14 个字段搬进去 ——

| 类别 | 成员 |
|---|---|
| tag（7） | `tags_enabled` `type_tags` `n_tag_words` `vreg_tag` `slot_tag` `vreg_tag_mem` `slot_tag_mem` |
| EH（7） | `eh_plans`（含 `EhPlanRec` 结构）+ 三个标签表 + 三个取用序号 |

**约 149 处**用法改成 `tag_.x`，横跨 15 个文件。`EhPlanRec` 是 TagLower 内嵌类型，
外部引用改写成 `TagLower::EhPlanRec`（只有 `codegen.cpp` 一处）。

验证：构建绿 / 369 ran ok / difftest 348/0 / ASan 369 ran ok 0 报告 / check_size 通过；
12 基准输出全一致，指令数全部 ±0.000%（`binary_trees` +0.03%，在噪声内）。

### 6.6 剩余两块（未做）

- **`RegAlloc`**：约 20 个字段，`slot_reg_` / `fp_promoted_*` / `fp_live_*` / `slot_reg_cand_`
  互相纠缠，且 `slot_reg_` 在**每条指令的操作数取值路径**上 —— 拆它需要一轮完整预算。
- **`VectPlanner`**（约 2500 行）：每个发射点都是 `AsmGen` 成员，先要定「反向引用还是
  `EmitSink` 接口」这个设计决定，做错会让 2500 行白搬。
### 6.7 已完成：`RegAlloc`（槽提升状态）

新文件 `src/reg_alloc.h`：12 个字段 ——

| 类别 | 成员 |
|---|---|
| 槽提升（6） | `slot_reg` `fp_promoted_slots` `fp_promoted_vregs` `slot_fpidx` `fp_live_in` `fp_live_out` |
| 候选与掩码（6） | `slot_reg_cand` `slot_cand_is_fp` `slot_reg_cand_idx` `fp_spill_mask` `fp_mask_valid` `fp_live_off` |

约 174 处用法改成 `ra_.x`。**这块和前几块的区别**：`slot_reg` 在每条指令的操作数
取值路径上（`slot_mop` 每次查一次），所以除了产物等价还专门看指令数 —— 全部 ±0.000%，
证明多一层成员间接没有引入可见成本。

改名时踩到一个真坑：`ra_opt.cpp` 名字不带 `codegen` 前缀，第一轮脚本没覆盖，
编译报错才发现。机械改名必须用「全 src 扫描 + 编译验证」，不能靠命名前缀猜。

### 6.8 剩余：`VectPlanner`（约 2500 行）

它和前面四块**不是一类东西**：前四块搬的是「状态」，方法留在 AsmGen；向量化器要搬的
是「方法」，而每个方法体里都是 `emit2(...)` / `src_mop(...)` / `xmm_of(...)` 这样的
AsmGen 成员调用。所以动手前必须先定一件事：

- **反向引用**（`VectPlanner` 持有 `AsmGen&`，friend 权限）—— 简单，但只是把耦合换个方向；
- **`EmitSink` 接口**（抽取 `emit*` / 操作数查询成纯虚接口）—— 干净，但要动 2500 行里
  每个发射点，且虚调用会影响热路径。

这个决定错了会让 2500 行白搬。建议下一轮**先写设计再动手**。
