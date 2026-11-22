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
- [ ] **第 1 步 · 拆头文件**：`codegen_impl.h` → core / regalloc / frame / tag 四个头；
      内联体挪回 `.cpp`。验收：改 regalloc 相关头后重编译 TU ≤ 8（现在 33）。
- [ ] **第 2 步 · AsmGen 拆组合对象**：按耦合从低到高提取 `Emitter`、`Frame`、
      `RegAlloc`、`TagLower`、`VectPlanner`；每步让 AsmGen 留一行转发，调用点改完再删。
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
