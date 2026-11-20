# 今晚自主迭代计划（单槽位顺序）

> 用户睡前授权。基调：**单 qwen 槽位顺序执行**（收回 4 代理并行，减少早夭混乱），
> 等代理交付→严审→集成→下一轮。**不手动接管/commit 代理 WIP**，让代理自己交付。
> P3 big BigInt 已被用户停止，暂缓。

## 现状（master 0.341 / 9b4a6f9）
本会话已交付 9 版（0.333-0.341）：SIMD 数值核（特化，胜 Go/C++）/ 整数 BCE（matmul −25%）/
regalloc 别名 / 递归展开（fib −29%）/ stdlib 7 模块 / borrow（dict_wordfreq −4.5% 通用）/
P2 unknown 类型标签（pprint/isinstance 解锁）/ P1 定宽整数（i8-i64/u8-u64）。

在途：lang-gaps（P4 语言缺口，qwen，修 custom 异常类当值 bug）；cspeed（编译速度，deepseek，
5 commit 位集系列，实测编译吞吐 ~11% 提速，但 codegen_impl.h 1221 超限待修）；
simd-general（SIMD 通用化，qwen）。big-int（P3）已停。

## 今晚优先级队列（单槽位顺序）

1. **P4 语言缺口（lang-gaps）**——当前优先。修 custom 异常类当值 raise/catch bug（test 340 崩溃）
   + repr 模块前缀；完成 suppress/closing（已工作）+ file 协议/io.StringIO + traceback + dataclass。
   交付→严审（异常钉子 92/93/94/119 + ASan leak + 语料 + CPython 对拍）→集成 0.342。
2. **编译速度（cspeed 工作）**——位集系列已有 ~11% 吞吐收益。修 codegen_impl.h 超限→严审
   （asm 逐字节不变命门 + 套件 + 吞吐 A/B）→集成 0.343。若 deepseek 早夭频繁，qwen 接手收尾。
3. **SIMD 通用化（simd-general）**——特化 matcher→通用自动向量化（回应通用性关切）。
   交付→严审（向量化==标量 + 非基准循环证据 + flag-off asm 不变）→集成 0.344。
4. **P3 big BigInt + f32**——暂缓（用户已停）；若前 3 项顺利且时间充裕再议立项。

## 每轮流程（纪律）
- **等代理交付报告**（不手动接管 WIP）；早夭→send_message 恢复（保留上下文）。
- 独立复核闸门：difftest 0 + 套件 normal/ASan（JOBS=1）+ CPython 对拍 + 语料零差异 +
  ASan leak 钉子 + 相关 asm 逐字节不变 + check_size 无新增超限 + 性能 A/B 实证。
- 通过→merge --no-ff + CHANGELOG 定版 + 重建 master 复验 + SSH 推送 + 清理 worktree。
- 负面结果（收益不抵复杂度）→记录跳过，进下一轮。
- 单槽位：当前代理交付集成后才派下一个。

## 醒来可见
- 集成版本（预计 0.342 P4 / 0.343 编译速度 / 0.344 SIMD 通用化，按实际落地）。
- 各轮闸门结果 + 性能数字 + 负面结论记录（roadmap 迭代日志）。
- 剩余/待决项（P3 big、SIMD 通用化深度、file 协议/traceback/dataclass 完成度）。

## 风险
- harness 间歇早夭频繁（本会话 4 代理起步全夭过）→恢复套路；若某代理反复夭（>3 次）
  且 WIP 有效，lead 复核后接管收尾（例外，非常态）。
- 4 分支都改 codegen/typecheck，集成冲突按序解。