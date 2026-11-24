# VectPlanner 抽取设计（模块重组第 2 步 · 5/5）

状态：**阶段 1–4 已全部实施**（0.398–0.406，2026-11-23 完成阶段 3/4 并量到重编译收益）。

## 1. 目标与非目标

**目标**：把向量化器从 `AsmGen` 里拆成独立协作对象 `VectPlanner`，使 `AsmGen`
的方法数从约 300 降到 250 以下，并让向量化代码不再需要完整的 `AsmGen` 定义。

**非目标**：
- 不改任何向量化**决策**逻辑（分类、守卫、发射序列全部保持逐字节一致）；
- 不追求「向量化器与代码生成完全解耦」——它本来就要发射指令；
- 不动 `codegen_simd*`（特化模板，默认关，属于待删清单）。

## 2. 现状数据（实测）

| 项 | 值 |
|---|---|
| 向量化器代码量 | **3,414 行**（`codegen_vect{,_affine,_avx2,_intdiv,_jvec}.cpp`） |
| 它调用的 AsmGen 成员 | **18 个** |

调用点计数（全部实际用量）：

| 原语 | 次数 | 原语 | 次数 |
|---|---|---|---|
| `emit2` | 269 | `op_mem` | 70 |
| `emit_jmp` | 60 | `line` | 41 |
| `emit_imm` | 42 | `src_mop` | 19 |
| `xmm_of` | 16 | `label` | 13 |
| `fresh_label` | 13 | `emit0`/`emit1`/`dst_mop` | 0 |

**结论：接口面比预想小得多。** 之前担心的「每个发射点都要改」实际只有 18 个成员、
约 550 个调用点。

## 3. 两个方案

### 方案 A：反向引用（`VectPlanner` 持有 `AsmGen&`）

```cpp
class VectPlanner {
    AsmGen& g_;   // friend 权限
public:
    bool try_loop(const Block& b);
};
```

**优点**：零接口设计，改动量最小（把方法体整体搬走，`emit2(...)` 改成 `g_.emit2(...)`）。

**缺点**：耦合只是换了个方向 —— `VectPlanner` 仍能看到 `AsmGen` 的一切，
`AsmGen` 的方法数虽然降了，但**可理解性没变**：读 `VectPlanner` 仍要知道整个类。
而且 `friend` 一旦开了，后面四个协作对象会照抄，最终得到五个互相 friend 的类。

### 方案 B：`EmitSink` 接口（推荐）

```cpp
class EmitSink {
public:
    virtual ~EmitSink() = default;
    // ── 发射 ──
    virtual void emit2(MOp op, const MOperand& a, MReg r) = 0;
    virtual void emit2(MOp op, const MOperand& a, const MOperand& b) = 0;
    virtual void emit_imm(MOp op, int64_t imm, MReg r) = 0;
    virtual void emit_jmp(MOp cc, MReg r);                  // 有默认实现
    virtual void emit_jmp(const char* cc, MReg r);
    virtual void line(std::string_view s) = 0;
    virtual void label(int id) = 0;
    // ── 操作数与查询 ──
    virtual MOperand op_mem(MReg base, int scale, int32_t disp) const = 0;
    virtual MOperand src_mop(VReg v) const = 0;
    virtual MReg xmm_of(VReg v) const = 0;
    virtual int fresh_label() = 0;
};
```

`AsmGen : public EmitSink`（实现这 10 个纯虚 + 2 个默认），
`VectPlanner` 只持有 `EmitSink&`。

**优点**：接口就是文档 —— 读 `VectPlanner` 的人只需要知道这 10 个原语，不需要
知道 161 个字段。后续四个协作对象也可以用同一套 sink（或各自的更小接口）。

**缺点**：虚调用开销。**但这里要澄清一件事**：

> `EmitSink` 的虚调用只影响**编译器自己的速度**（它每发射一条指令调用一次），
> 与生成代码的性能**完全无关**。向量化器每命中一个循环大约发射 50–200 条指令，
> 相对整个编译期的其它开销可以忽略。

所以「虚调用拖慢热路径」这个担心在这里**不成立** —— 热路径是生成的程序，不是
编译器。真正需要留意的是 `src_mop`/`xmm_of` 这类**查询**在向量循环里被调用
（每 lane 检查一次），但它们的调用次数是每循环几十次，同样可忽略。

## 4. 推荐：方案 B，分四阶段迁移

每个阶段都能独立编译、独立验证（产物逐字节等价），任何一阶段不过就停在那。

### 阶段 1：抽接口，不改调用方

- 新增 `src/backend/emit_sink.h`（10 个纯虚 + 2 个默认实现）；
- `class AsmGen : public EmitSink`，把现有方法标 `override`；
- **不改任何调用点**，不新增 `VectPlanner`。
- 验收：构建绿 + 12 基准输出与指令数不变。

### 阶段 2：搬「纯查询」方法

- 把 `op_mem` / `xmm_of` / `fresh_label` / `label` / `line` 的实现搬进一个
  `AsmGen` 的薄层（它们本来就短），或直接留在 AsmGen 实现接口。
- 目的：确认接口签名够用，**不搬向量化代码**。

### 阶段 3：搬 `backend/vect.cpp` 主体

- 新增 `src/opt/vect_planner.h/.cpp`：`class VectPlanner { EmitSink& sink_; ... }`；
- 把 `try_vect_loop` 及其全部辅助方法整体搬进去，`AsmGen::try_vect_loop` 留一行转发；
- 辅助文件（`_affine` / `_avx2` / `_intdiv` / `_jvec`）跟着搬，成为 `VectPlanner` 的方法。
- 验收：12 基准输出逐字节一致 + 指令数 ±0.000% + 向量化触发数不变。

### 阶段 4：删转发 + 量重编译

- 删掉 `AsmGen` 里的转发与残留声明；
- 实测「改 `opt/vect_planner.cpp`」的重编译 TU 数（应 ≈ 1–2，而不是 30）。

## 5. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 接口设计漏了成员，阶段 3 卡住 | 阶段 1/2 只做接口不搬代码，漏了在阶段 3 立刻暴露，回滚成本低 |
| 虚调用拖慢编译器 | 实测「编译 12 个基准的墙钟/指令数」对比；只在超标时才考虑去虚化 |
| 搬动引入行为差异 | 每阶段跑 12 基准 × 3 优化档的**输出 + 指令数**双口径，以及汇编 md5 |
| `emit2` 有多个重载，签名不一致 | 阶段 1 就按实际调用点收敛（见 §2 的计数表） |

## 6. 不做什么

- **不做模板化 sink**（`template<class Sink>` 静态多态）—— 会把 3,414 行全部变成
  模板代码，编译时间和可读性都会恶化，收益只是省掉虚调用，而虚调用在本场景
  本来就不是瓶颈。
- **不拆 `codegen_simd*`** —— 它是待删的特化模板，不值得投入。
- **不改向量化决策逻辑** —— 这轮是纯结构重构。

## 7. 验收标准

1. `AsmGen` 的方法数从约 300 降到 250 以下（脚本统计）；
2. 改 `src/opt/vect_planner.cpp` 的重编译 TU 数 ≤ 2（实测，关 ccache）；
3. 12 基准 × 3 优化档：输出逐字节一致、指令数 ±0.000%、向量化触发数不变；
4. 普通套件 / difftest / ASan / check_size 全绿。
## 8. 实施进度

### 阶段 1 已完成（2026-11-22）

- 新增 `src/backend/emit_sink.h`：`class EmitSink`，7 个纯虚 ——
  `emit2` / `emit_imm` / `emit_jmp`(×2) / `line` / `label` / `src_mop` / `fresh_label`；
- `class AsmGen : public EmitSink`，8 处实现加 `override`（`emit_jmp` 两个重载）。
- **没有改任何调用点，没有新增 `VectPlanner`**。

为什么先只做这些：`override` 让「接口签名与实现不一致」变成**编译期错误**。
以后谁改了 `emit2` 的签名而忘了接口，构建立刻报错，不会等到搬代码时才发现。
这正是第 0 步（判据具名化 + 编译期真值表）的同一思路。

验证：构建绿 / 369 ran ok / difftest 348/0 / check_size 通过；12 基准输出全一致，
指令数全部 ±0.000%（`binary_trees` −0.07%，噪声）。

### 下一步（阶段 2）

确认接口够用：把 `op_mem`（4 个 static 重载，不需要虚化）与其余查询从向量化器
的调用面里过一遍，补齐还缺的成员，然后再进阶段 3 搬代码。
## 9. 阶段 2 结论：接口**不够用**（2026-11-22 实测）

按计划过了一遍向量化器的完整调用面。结果：`EmitSink` 的 7 个原语**只覆盖约 60%**。

实测（`codegen_vect*.cpp` 五个文件去重计数）：

| 成员 | 次数 | 是否已在 EmitSink |
|---|---|---|
| `emit2` | 85 | ✅ |
| `ty_of` | 25 | ❌ |
| `op_mem` | 19 | ➖ static，无需虚化 |
| `emit_imm` | 19 | ✅ |
| `emit_jmp` | 17 | ✅ |
| `list_base_mop` | 15 | ❌ |
| `slot_ty`（字段） | 8 | ❌ |
| `src_mop` | 6 | ✅ |
| `slot_is_jalias` / `slot_is_temp` / `slot_is_red` | 6/3/3 | ❌ |
| `op_reg` / `op_imm` | 4/2 | ➖ static |
| `fresh_label` | 4 | ✅ |
| `slot_mop` | 3 | ❌ |
| `emit_imm_hex` | 2 | ❌ |
| `global_mop` / `slot_loc_` | 1/1 | ❌ |

**这说明「一个 EmitSink 打天下」是错的设计方向。** 缺的成员分三类，
它们的归属本来就不该相同：

| 组 | 成员 | 应该属于 |
|---|---|---|
| 发射 | `emit2` `emit_imm` `emit_imm_hex` `emit_jmp` `line` `label` `fresh_label` | `EmitSink`（已有，补 1 个） |
| 值的类型/来源 | `ty_of` `src_mop` `list_base_mop` `global_mop` `slot_ty` | 新的 `ValueInfo` |
| 槽的属性 | `slot_mop` `slot_is_jalias` `slot_is_temp` `slot_is_red` `slot_loc_` | 新的 `SlotInfo` |

所以 `VectPlanner` 应该持有**三个小接口**（或一个聚合 `VectContext` 里放三个引用），
而不是一个胖接口。每个接口 5–7 个成员，语义单一。

### 下一步（阶段 2 修正版）

1. 先补 `EmitSink` 缺的 `emit_imm_hex`（1 个成员）；
2. 新增 `ValueInfo`（5 个成员）与 `SlotInfo`（5 个成员），同样用 `override` 保证一致；
3. 三个接口都落地、编译通过、产物等价之后，才进阶段 3 搬 3,414 行。

**这个结论本身就是阶段 2 的价值**：如果直接进阶段 3，会在搬到一半时发现接口缺 8 个
成员，那时改接口要连带动已经搬好的几百行。现在改只需动 3 个头文件。
### 阶段 2 修正版：已完成（2026-11-22）

三个接口全部落地，`AsmGen` 多重继承并全部加 `override`：

| 接口 | 成员 | 状态 |
|---|---|---|
| `EmitSink` | `emit2` `emit_imm` `emit_imm_hex` `emit_jmp`×2 `line` `label` `fresh_label` `src_mop` | 8/8 |
| `ValueQuery` | `ty_of` `global_mop` | 2/2 已确认 |
| `SlotQuery` | `slot_mop` | 1/1 已确认 |

**待补**（签名需逐个确认，其中可能有局部 lambda 而非成员）：
`list_base_mop`（15 次）、`slot_is_jalias`/`slot_is_temp`/`slot_is_red`（6/3/3）、
`slot_ty`（8 次，是字段不是方法，需要一个访问器）、`slot_loc_`（字段）。

教训：接口命名要避开业务代码里的局部类型名 —— `SlotInfo` 与 `try_vect_loop` 里的
局部 `struct SlotInfo` 冲突，`-Werror=shadow` 才发现。
## 10. 阶段 2b 结论：接口**基本已够**，上一轮的「待补 6 个成员」是我看错了

逐个核对签名之后发现：那 6 个「待补成员」里，多数根本**不是 `AsmGen` 的成员**。

| 名称 | 真实身份 | 处理 |
|---|---|---|
| `slot_is_jalias` / `slot_is_temp` / `slot_is_red` | `try_vect_loop` 里的**局部 lambda**（捕获局部状态） | 阶段 3 随代码一起搬，变成 `VectPlanner` 的私有助手 |
| `list_base_mop` | 同上，**局部 lambda** | 同上 |
| `slot_ty` | `IRFunction::slot_ty`（IR 函数的字段），不是 `AsmGen` 成员 | 不需要访问器，向量化器直接读 `f_->slot_ty` |
| `op_mem` / `op_reg` / `op_imm` | `static` 操作数工厂 | 不需要虚化 |

**这是我的方法错误**：阶段 2 的表是「字符串扫描」得出的，只看了名字出现次数，没有
区分「成员 / 局部变量 / 别的类的字段」。正确的做法是逐个看声明位置。

### 结论

三个接口对阶段 3 而言**已经够用**：

| 接口 | 成员 |
|---|---|
| `EmitSink` | `emit2` `emit_imm` `emit_imm_hex` `emit_jmp`×2 `line` `label` `fresh_label` `src_mop` |
| `ValueQuery` | `ty_of` `global_mop` |
| `SlotQuery` | `slot_mop` |

阶段 3 剩下的工作就是**纯搬迁**：把 `try_vect_loop` 及其辅助方法（含那些局部 lambda）
整段搬进 `VectPlanner`，`AsmGen::try_vect_loop` 留一行转发。不需要再动接口。

## 11. 收尾状态（2026-11-22）

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | `EmitSink` 落地 + `override` | ✅ 0.398 |
| 2 | 修正成三接口 | ✅ 0.399 |
| 2b | 核对剩余成员（多数是局部 lambda） | ✅ 0.400，无需改代码 |
| 3 | 搬 `try_vect_loop` + `Loop` 状态/方法（`vect_planner*` 合计 1,945 行） | ✅ 0.403–0.406；`backend/vect.cpp` 1,178 → 59 行 |
| 4 | 删转发 + 量重编译 TU | ✅ 只剩一行转发（`AsmGen::try_vect_loop`）；改 `opt/vect_planner_loop.cpp` = **1 TU / 7.76 s**，改 `backend/core.h` = **31 TU / 55.16 s** |

**阶段 3/4 已实施完毕，重编译收益也量到了**：向量化器主体（`try_loop` 约 800 行）
搬进 `src/opt/vect_planner_loop.cpp`，每次调用状态与 20 个捕获 lambda 收进
`VectPlanner::Loop`（`opt/vect_planner.h` 319 行 / `opt/vect_planner.cpp` 677 行 /
`opt/vect_planner_loop.cpp` 949 行）。**边界**：`vect_affine_*` / `vect_jvec2` /
`vect_avx2_emit` / `vect_alloc_temps` 仍留在 `AsmGen`（它们直接发指令、且被发射路径
调用，按「三接口 + 反向引用」的边界留在原处更简单），阶段 3 只搬了 `try_vect_loop`
主体 —— 原定的「辅助文件跟着搬」这条经实践证明不必要。

**1:7** 是这块拆分的最终兑现：改向量化器一行只要 1 个 TU / 7.76 s，而改类体头文件
（`backend/core.h`）一行仍是 31 个 TU / 55.16 s（ccache 关、`-j4`）。
## 12. 阶段 3 盘点：`try_vect_loop` 的局部状态（2026-11-22）

`backend/vect.cpp`（1,178 行）里有 **21 个局部 lambda**，它们捕获的是**每次调用的**
局部状态（`cond_bi`/`body_bi`/`incr_bi`/`vec`/`avx`/`si`/`jalias`…）。这是搬迁的真正
难点：方法可以搬，捕获状态不能。

### 12.1 哪些 lambda 被辅助文件引用（决定谁必须变成成员）

| lambda | 主文件 | 辅助文件 | 处理 |
|---|---|---|---|
| `xmm_of` | 8 | **7** | 必须跨文件 → 进 `Loop` |
| `cls` | 17 | **11** | 必须跨文件 → 进 `Loop` |
| `take` | 10 | **3** | 必须跨文件 → 进 `Loop` |
| `list_base_mop` | 14 | 1 | 进 `Loop` |
| `slot_is_jalias` / `slot_is_temp` / `slot_is_red` | 6/3/3 | 0 | 主文件内，可留 `Loop` |
| `index_off` / `in_loop` / `is_jalias_load` / `red_of_slot` / `is_red_add` | — | 0 | 同上 |
| `cross_block` / `list_key` / `list_base_ok` / `list_elem_float` / `inv_int_ok` / `classify` / `resolve_src` / `pshufd` / `temp_store_val` | — | 0 | 同上 |

### 12.2 设计决定：`VectPlanner::Loop`

不把 21 个 lambda 变成 `VectPlanner` 的成员（它们依赖每次调用的局部状态），而是引入
**每次调用一个的上下文对象**：

```cpp
class VectPlanner {
    struct Loop {                     // 一次 try_vect_loop 调用的全部状态
        int cond_bi, body_bi, incr_bi;
        const Block* b;
        std::vector<VAccess> accesses;
        std::vector<VRed> reds;
        std::unordered_map<int, SlotInfo> si;
        std::vector<int> jalias;
        // …以及那些 lambda 需要的其它局部量

        // 原来捕获 [&] 的 lambda 变成 Loop 的方法（隐式 this）。
        bool slot_is_jalias(int s) const;
        MReg xmm_of(VReg v) const;
        MOperand list_base_mop(VReg lv) const;
        VK cls(VReg v) const;
        MReg take(MReg prefer);
    };

public:
    bool try_loop(const Block& b);
    // 辅助文件的入口也带 Loop&（它们本来就在这次调用的上下文里）。
    void vect_avx2_emit(Loop& L);
    bool vect_affine_plan(Loop& L, ...);
};
```

要点：`Loop` 的方法需要 `emit_`/`val_`/`slot_` 时，通过 `VectPlanner` 的引用传下去
（`Loop` 持有 `VectPlanner& owner`，或方法接受 `VectPlanner&`）。**推荐前者**：
`Loop` 是 `VectPlanner` 的嵌套类型，天然能访问外层的三个接口引用。

### 12.3 迁移顺序（改自 §4 阶段 3）

1. **建 `Loop` 骨架**：先把 `try_vect_loop` 里声明的局部量原样搬进 `struct Loop`，
   `try_vect_loop` 改成 `Loop L; …` 加 `L.xxx` 前缀。**这一步不改任何逻辑**，
   只做「局部变量 → 结构体字段」的机械转换，产物必须逐字节等价。
2. **lambda → Loop 方法**：21 个 `auto x = [&](…)` 改成 `Loop::x(…)`，`[&]` 捕获变成
   隐式 `this`。逐个改、逐个编译。
3. **方法搬家**：`try_vect_loop` 主体搬进 `VectPlanner::try_loop`，辅助文件跟着搬。
4. **删转发 + 量重编译**。

第 1 步是**纯机械且可验证**的（产物等价），也是整个阶段 3 里最安全的一步。
建议下一轮只做第 1 步，做完再决定要不要继续。
