# tools —— 开发基础设施

这些脚本不参与编译器构建，只服务「改代码 -> 验证 -> 量收益」这个循环。

## difftest.sh —— 差分测试（并行）

同一份源码在 `-O0` / `-O2` 下输出必须一致。优化遍引入静默错误
（野指针、越界、算错）时立刻抓出来。

**已并行化**：原来 188 用例串行跑 63s，现在 32 核并行约 5.6s。

```bash
bash tools/difftest.sh                        # 全部（默认 nproc 并发）
PYADAMAS_TEST_JOBS=8 bash tools/difftest.sh   # 指定并发度
PYADAMAS_TEST_FILTER='3[0-4]*' bash tools/difftest.sh   # 只跑匹配的用例
bash tools/difftest.sh build/release/pyadamas # 指定编译器
```

每个用例用独立临时目录，并行时不会互相覆盖产物。

## abbench.sh —— A/B 性能对比

对比「当前工作区」与「某个 git 版本」的性能。固定了三件事，都是踩过坑的：

1. **独立 worktree 构建基线** —— 绝不碰当前 build 目录；
2. **交替测量**（A,B,A,B...）—— 抵消系统漂移。曾经因为「先测完全部 A
   再测全部 B」，把系统变慢误判成 17% 回归；
3. **报中位数** —— 最小值容易被单次幸运值带偏。

另外每个用例都带 `--rebuild`：modcache 的键不含编译器版本，换了编译器
但源码没变时会命中旧产物，量出完全相同的二进制（曾经连续几轮 A/B 拿到
假数据）。

```bash
bash tools/abbench.sh                          # 当前 vs HEAD，全部基准
bash tools/abbench.sh 575d4eb                  # 当前 vs 指定提交
bash tools/abbench.sh HEAD realistic 11        # 只跑真实基准，11 轮
```

输出示例（自检：当前 vs HEAD 代码相同时应接近 0%）：

```
用例                     基线     当前    变化 p25变化
dict_wordfreq         158.24ms  158.32ms   +0.1%   -0.7%
obj_dispatch          287.43ms  288.17ms   +0.3%   +3.7%
总计: 1344.68ms -> 1349.90ms  +0.4%
```

噪声在 ±2% 内。超过这个幅度才算真实变化。

## irdump.cpp —— 枚举编号对照

把 IR 日志里的数字对回名字。**调试优化遍时最耗时的就是这个动作**，
实测踩过的坑：

- 日志打出 `op=36`，不知道那是 `call.rt`（`IrOp` 是 `uint8_t` 枚举，
  日志里只有数值）；
- 日志打出 `rt=58`，以为是 `RcMark`，实际是 `pa_rc_inc`（`RtFn` 有 528 项，
  靠数枚举位置必然出错）；
- 块号连续不等于 CFG 连续（内联后 `for.body` 会 branch 到很远的块），
  只看块号会漏掉循环体里的指令。

```bash
g++ -std=c++23 -I src tools/irdump.cpp -o /tmp/irdump

/tmp/irdump ops            # 全部 IrOp（60 项）
/tmp/irdump rt             # 全部 RtFn（528 项）
/tmp/irdump op 36          # 查 IrOp 36 附近
/tmp/irdump rt 58 79       # 查 RtFn 58 / 79 附近
```

```
$ /tmp/irdump op 36
=== IrOp: 36 附近 ===
  35  call
  36  call.rt   <== 命中
  37  call.extern

$ /tmp/irdump rt 58
=== RtFn: 58 附近 ===
  57  pa_panic_overflow
  58  pa_rc_inc   <== 命中
  59  pa_rc_dec
```

不放进 CMake 是刻意的：它是开发期辅助工具，不该影响主构建的依赖图。

## 环境注意

WSL 在内存压力下会**整个服务失去响应**（所有命令返回
`Wsl/Service/0x8007274c`，连 `echo` 都不行）。`.wslconfig` 里必须有 swap：

```ini
[wsl2]
memory=5GB
swap=8GB
```

只有 `memory=6GB` 没有 swap 时，WSL 顶满后宿主机只剩 1.4GB，服务直接卡死，
必须 `wsl --shutdown` 才能恢复。
