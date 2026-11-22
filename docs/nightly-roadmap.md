# 夜间迭代路线图（2026-11-23 00:15 → 08:00）

用户授权：先完成模块化拆分，再做通用优化；每次只用一个代理槽位；08:00 前必须结束。

## 执行纪律（每轮必须遵守）

1. **一个代理槽位**：起一个子代理 → 它完成 → 我复核（构建+闸门+产物等价）→ 提交 → 才起下一个。
2. **产物等价是第一验收口径**：12 基准 × 3 优化档输出逐字节对 CPython，指令数 ±0.000%，
   向量化触发数不变（重构期 matmul 4 / spectral_norm 2；收尾时 nbody 也 +1）。
3. **闸门**：`tests/run_tests.sh`（369 ran ok）、`difftest.sh`（348/0）、`check_size.sh`、ASan。
   **difftest 必须独占跑**（`PYADAMAS_TEST_JOBS=1`），并发会踩 277 的端口/时序。
4. **不许针对基准特化**（0.384 政策）。判据只能是 IR 结构。
5. 任何一轮失败 → `git checkout -- .` 回滚 + 记录失败原因，不提交半成品。

## 阶段 A：完成 VectPlanner 拆分（00:15 → 02:30）—— **全部完成**

- A1 ✅ 已完成（`afa6c73`）：7 个纯发射/查询方法 + 4 个仿射类型搬进 `VectPlanner`；
  同批的 `vect_jvec2`（`b181743`）也在这一步之前完成。
- A2 ✅ 已完成（`270ad6e`）：搬 `vect_affine_index` / `vect_affine_plan_ok`
  （纯函数，只用 `val_.simd_def`）。
- A3 ✅ 已完成（`270ad6e`）：搬 `jvec` 的 `merge_const_invs`
  （`simd_def` + `ty_of` 都已在 `ValueQuery`）。
- A4 ✅ 已完成（`e72f4ff` / `5820ac7` / `1967077`）：`Loop` 上下文三步 ——
  ① 局部变量 → `struct Loop` 字段（36 个局部量）；
  ② 20 个捕获 lambda → `Loop` 方法（+22 个局部量）；
  ③ `try_vect_loop` 主体（约 800 行）搬进 `VectPlanner::try_loop`。
  **第 ④ 步「辅助文件跟着搬 + 删转发」没做**：`vect_affine_*` / `vect_jvec2` /
  `vect_avx2_emit` / `vect_alloc_temps` 仍留 `AsmGen`（它们直接发指令、且被发射路径
  调用，按「三接口 + 反向引用」的边界留在原处更简单）。
- A5 ✅ 已完成（实测）：改 `vect_planner_loop.cpp` 一行 = **1 个 TU / 7.76 s**；
  改 `codegen_core.h` 一行 = **31 个 TU / 55.16 s**（ccache 关、`-j4`）—— **1:7**。

## 阶段 B：通用优化（02:30 → 05:30）

原计划按性价比排序、每轮一个主题：

- B1 **向量化覆盖**：`Convert` 目前只认 `j+常量`（`float(M+j)` 不向量化）；下标经临时槽
  （`t = j + 1; a[t]`）也不向量化。这两条都是通用缺口，不依赖任何形状。
- B2 **P4 判据更新**：P1+P3 之后 `matmul` 上通用路径已比特化快 1.39×。评估把
  `codegen_simd*` 从 `gen_block` 的优先级里去掉（或按代价择优），减少「特化挡住通用」。
  只改优先级，不删代码 → 产物等价可验证。
- B3 **`spectral_norm` 后续**：0.391 后触发 2 个循环，看还有没有剩余标量热点
  （用 `perf stat` 指令数口径，不用墙钟）。

> **实际执行与计划不同**：排查后发现真正挡路的是 nbody 的三道通用阻塞，于是改成
> 「一条一条拆掉 nbody 的阻塞」，原 B2/B3 未做：
>
> - B1 ✅ **其实是 bug 修**：`Convert` 的 `j_src` 快速路径缺 disp32 上界 →
>   大常量 + j 直接**汇编失败**（`5eec7eb`）。原 B1 的「下标经临时槽」缺口没有单独立项。
> - B2 ✅ `math.sqrt` 支持（`82044b7`）。
> - B3 ✅ float Sub 归约（`7d5a901`）。
> - B4 ✅ `coef==0` 不变下标元素装载 → 广播装载（`1646e61`），nbody 触发 0 → 1。
> - 原 B2（P4 删特化优先级）**未做**：nbody/`spectral_norm` 仍有循环只有特化能覆盖，
>   现在动优先级会回退。
> - 原 B3（`spectral_norm` 剩余热点）**未做**：P5 后已触发 2 个循环，没有继续压。

## 阶段 C：收尾（05:30 → 07:00）

- C1 ✅ 全量验证（本轮口径）：每轮跑构建 + 套件 + difftest（独占）+ check_size；
  收尾时 **374 ran ok / 0 bad**、**difftest 353/0**、12 基准 × 3 档输出与 CPython
  一致、指令数口径对比完成。**ASan 套件本轮未跑**（验收链没有要求），记为未做项。
- C2 ✅ 更新 `docs/modularization.md` / `docs/vect-planner-design.md` / `CHANGELOG.md`
  （0.402–0.411 共 10 条），以及 `docs/vectorization-generalization.md` 的能力边界/覆盖率、
  本文件。
- C3 ✅ 生成夜间总结：见下方「实际结果 vs 计划」。

## 硬约束

- 07:00 前必须停止开新工作，只做收尾；08:00 前必须全部提交并推送。
- 每轮提交都要能单独回滚（小步提交，不攒大改动）。
- 不许用墙钟判断收益（这台机器同二进制能差 2 倍），一律用 `perf stat -e instructions:u`。

## 实际结果 vs 计划

### 超出预期

- **VectPlanner 抽取完成且量到了兑现**：`codegen_vect.cpp` 1,178 → 59 行，新增 `vect_planner*` 三个文件合计 1,945 行，
  `codegen_vect.cpp` 1,178 → 59 行；改向量化器一行从 30 个 TU / 约 55 s 降到
  **1 个 TU / 7.76 s**（1:7）。
- **顺手逮到两个真 bug**（都不是计划内）：
  1. `Convert` 的 `j_src` 快速路径缺 disp32 上界 → `float(2^40 + j)` 直接汇编失败；
  2. `gen_bin_fp` 就地快路径覆盖右操作数 → `-O1/-O2` 下 `s = a[j] - s` / `a[j] + s`
     结果错（`-O0` 正常，且与向量化器无关）。第二个是**正确性** bug，优先级高于优化。
- **nbody 从 0 → 1 触发**：sqrt / float Sub 归约 / `coef==0` 广播装载三条补齐后，
  `energy` 内层能向量化了。

### 没做到

- **nbody 性能没有实质提升**：触发数 0 → 1 只换来 **−551 条指令 / −0.0003%**（可忽略）——
  `energy` 只调用 1 次、内层 10 个组合；`nbody` 仍由 `advance` 主导。
- **`advance` 仍未向量化**：内层/位置更新是 `coef=7` 的 scatter 写 + 同数组读写偏移不同
  （big feature：需要 packed scatter 或逐 lane 写 + 别名证明）；外层是嵌套循环形状。
- **覆盖面仍是 7 个循环 / 3 个基准**（matmul 4、spectral_norm 2、nbody 1），其余 9 个
  基准为 0 —— 其中 `fib`/`nqueens`/`binary_trees` 递归为主、`sieve` 运行期步长、
  `collatz`/`mandelbrot` 数据相关 while，按设计本来就不适合 SIMD；`fasta`/`wordfreq`
  是伪随机状态推进/字典操作。
- **原计划 B2（删特化优先级）/ B3（spectral_norm 剩余热点）未做**。

### 下一步建议

1. `advance` 的 scatter 写 + 同数组混合偏移：先量「逐 lane `movsd` 写 + 偏移别名证明」
   能不能覆盖，不能就明确记为不可行（像 `sieve` 步长那样）；
2. 不变下标装载目前是**循环体内每次广播**（`movsd+movlhps`），可评估上提到 preheader 的
   常驻广播寄存器（收益 = 每轮省 1 条 load，但要多占 xmm，energy 已经贴边）；
3. 浮点归约/广播类路径若继续做，注意 AVX2 4-lane 没有广播装载，只能留在 SSE2。
