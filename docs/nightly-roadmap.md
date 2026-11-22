# 夜间迭代路线图（2026-11-23 00:15 → 08:00）

用户授权：先完成模块化拆分，再做通用优化；每次只用一个代理槽位；08:00 前必须结束。

## 执行纪律（每轮必须遵守）

1. **一个代理槽位**：起一个子代理 → 它完成 → 我复核（构建+闸门+产物等价）→ 提交 → 才起下一个。
2. **产物等价是第一验收口径**：12 基准 × 3 优化档输出逐字节对 CPython，指令数 ±0.000%，
   向量化触发数不变（matmul 4 / spectral_norm 2）。重构类改动不允许有任何功能变化。
3. **闸门**：`tests/run_tests.sh`（369 ran ok）、`difftest.sh`（348/0）、`check_size.sh`、ASan。
   **difftest 必须独占跑**（`PYADAMAS_TEST_JOBS=1`），并发会踩 277 的端口/时序。
4. **不许针对基准特化**（0.384 政策）。判据只能是 IR 结构。
5. 任何一轮失败 → `git checkout -- .` 回滚 + 记录失败原因，不提交半成品。

## 阶段 A：完成 VectPlanner 拆分（00:15 → 02:30）

- A1 ✅ 已完成：7 个纯发射/查询方法 + 4 个仿射类型（`afa6c73`）
- A2 搬 `vect_affine_index` / `vect_affine_plan_ok`（纯函数，只用 `val_.simd_def`）
- A3 搬 `jvec` 的 `merge_const_invs`（`simd_def` + `ty_of` 都已在 `ValueQuery`）
- A4 **`Loop` 上下文**（大块）：按 `docs/vect-planner-design.md` §12.3 四步走：
  ① 局部变量 → `struct Loop` 字段（纯机械、产物等价）② 21 个 lambda → `Loop` 方法
  ③ `try_vect_loop` 主体搬进 `VectPlanner::try_loop` ④ 辅助文件跟着搬 + 删转发
- A5 量重编译收益：改 `vect_planner.cpp` 应只需 1-2 个 TU。

## 阶段 B：通用优化（02:30 → 05:30）

按性价比排序，每轮一个主题：

- B1 **向量化覆盖**：`Convert` 目前只认 `j+常量`（`float(M+j)` 不向量化）；下标经临时槽
  （`t = j + 1; a[t]`）也不向量化。这两条都是通用缺口，不依赖任何形状。
- B2 **P4 判据更新**：P1+P3 之后 `matmul` 上通用路径已比特化快 1.39×。评估把
  `codegen_simd*` 从 `gen_block` 的优先级里去掉（或按代价择优），减少「特化挡住通用」。
  只改优先级，不删代码 → 产物等价可验证。
- B3 **`spectral_norm` 后续**：0.391 后触发 2 个循环，看还有没有剩余标量热点
  （用 `perf stat` 指令数口径，不用墙钟）。

## 阶段 C：收尾（05:30 → 07:00）

- C1 全量验证：普通/ASan 套件、difftest（独占）、12 基准 × 3 档、指令数对比
- C2 更新 `docs/modularization.md` / `docs/vect-planner-design.md` / `CHANGELOG.md`
- C3 生成夜间总结：完成了什么、撤回什么、剩余什么、下一步建议

## 硬约束

- 07:00 前必须停止开新工作，只做收尾；08:00 前必须全部提交并推送。
- 每轮提交都要能单独回滚（小步提交，不攒大改动）。
- 不许用墙钟判断收益（这台机器同二进制能差 2 倍），一律用 `perf stat -e instructions:u`。
