# 异常抛出路径的性能现状（2026-11-27）

> 状态：**只有测量与方案，没有实现**。写下来是为了下一次有人想动这块时不必重新测一遍。

## 1. 现象

`/src/luo-proj/excbench/pya/main.py`：10 万次「真的抛出并接住」。

| 机制 | 时间 |
|---|---|
| pya 表驱动（默认，零开销异常） | **235–239 ms** |
| pya `--no-zero-cost-exc`（setjmp 回退） | **33–61 ms** |
| CPython | 57–72 ms |
| Go（error 返回值） | 0 ms |

**正常路径**（不抛）两边都是 0–2 ms——「零开销」这个设计本身没问题，慢的只有抛出路径。

## 2. profile（指令级）

```
$ perf record -e cycles:u /tmp/ex_pya
    79.52%  pa_eh_throw
            --77.79%--_Unwind_RaiseException
                        --24.76%-- (libgcc 内部，无符号)
```

- `pa_eh_throw` 自身很干净：一次 wrapper 分配、几个字段赋值、一次 `_Unwind_RaiseException`。
- 时间几乎全在 **libgcc 的展开器**里：DWARF CFI 逐帧解析 + 每个 FDE 调用一次 personality。
- 我们的 personality 与 LSDA 解析合计约 **7%**（`pa_personality` 3.47% + `pa_read_encoded` 3.58%，
  `perf record -F 12000 -e cycles:u` 同口径）。**更正**：本文早期版本写「自采样 0.00%」是按
  `-g` 调用树的口径读的，那口径会把被内联的解析记到调用方，不能用来判断占比。
- 结论不变：**热点是 libgcc 的展开器（71%）**，不是我们的 personality。

## 3. 可选方向（按风险从低到高）

### A. 同一帧内直接跳 landing pad（推荐先做这个）
`raise` 在**同一个函数**的 `try` 里时，理论上不需要展开：直接跳到该 try 的 landing pad，
把异常对象塞进槽即可。这是绝大多数 `try/except` 的形态。

- 需要 lowering 知道「当前 raise 点属于哪个 try 区域、该区域的 landing pad 在哪」；
- 仍需处理中间的 `finally`、以及 handler 的执行顺序（不能跳过清理）；
- 收益上限：把 235ms 拉到接近 setjmp 的 33ms（即 **7×**）。

### B. 在一次 throw 内缓存 LSDA 查询
phase1（找 handler）与 phase2（清理）会遍历同一串帧。若把「IP → landing pad/filter」
缓存在 `pa_eh_throw` 的栈上（每帧一次），可省掉一半解析。

- 改动集中在 `runtime/rt_personality.c` + `rt_eh.c` 的 wrapper；
- 收益不确定（未见 profile 显示 personality 是大头，但它是 libgcc 匿名帧之外的必然成本）；
- 风险：缓存键必须含「帧的 CFA/SP」，增量标记/信号等边角要小心。

### A 的可行性（读了 `src/lower/exc.cpp` 之后，**未实现**）

**可以做，但不是小改**。三个具体障碍：

1. **没有「当前 try 区域」的跟踪**。`lower_try` 把区域登记进 `st_->fn->eh_regions`（`exc.cpp:984`），
   但 `lower_raise` 完全不知道自己处在哪个 try 体内——`eh_regions` 里只有 `lp_block` / `end_block`。
   要加一个 Lowerer 侧的栈，在 try 体降级前后 push/pop。
2. **landing pad 的入口协议绑定了展开包装**。LP 头是 `EhLandingPad` + `pa_eh_land`，从 `%rax`
   取的是 `_Unwind_Exception` 包装；直接跳过去的话没有包装。所以需要一个**第二条入口**
   （`land_direct`）：跳过 `EhLand`，从约定槽位取异常对象，再汇入现有的 handler 分发链。
3. **`pa_eh_depth` 记账**。LP 里那句 `EhLeave` 是为了退表驱动展开区；直接跳路径没进过展开，
   但 `EhEnter`（`EhBegin`）已经记过一次，所以仍然要配对退出——这条要是漏了，后面别的 try
   会误判成「在表驱动区里」（`exc.cpp:1000` 附近有注释记录过这个坑）。

必须排除的形态（否则语义会变）：嵌套 try（内层 raise 要先进内层 LP）、try 里有 `finally`、
try 里有 `with`（都要跑清理）、`except SomeClass`（匹配要运行期比较）、`as e` 绑定、
`raise ... from ...`、在 finally/handler 体内 raise、可挂起函数（生成器/协程）。

**最小安全子集**：`raise` 直接位于 `try:` 体、该 try 只有一个裸 `except:`（或 `except Exception`）、
中间没有 finally/with、不在挂起函数里。估算 80–150 行（`exc.cpp` + Lowerer 状态 + `land_direct`），
外加一组「直接跳」与「正常展开」输出必须逐字节一致的对拍用例。

### B 的实测结论（2026-11-28，已实现后回退）

32 项 `(CFA, IP) → (landing pad, filter, kind)` 静态缓存，phase2 直接命中：

| 指标 | 修前 | 修后 |
|---|---|---|
| 指令数 | 1,619,547,710 | 1,595,854,780（**−1.46%**） |
| cycles:u（绑核 5 次中位） | 624.0M | 624.4M（**±0%**） |
| 墙钟（绑核 7 轮中位） | 223 ms | 222 ms（噪声内） |
| `pa_read_encoded` 占比 | 3.58% | 2.09%（−42%，缓存确实生效） |

**结论：LSDA 二次解析确实砍掉了一半，但总时间一动不动** —— 这部分工作原本被摊在 libgcc
指针追逐的停顿里，乱序执行把它藏掉了。libgcc 自己的占比反而从 71.22% 升到 75.92%。
实现已全部还原（`git status` 干净），不留复杂度。

**因此：方案 B 不值得做。** 唯一有量级收益的是方案 A。

### 分配侧（展开包装池化）的实测结论（同轮，已实现后回退）

profile 里 `malloc` 2.97% + `cfree` 约 2.19%，于是试了「64 项 LIFO 池 + 归还时清零」：

| 指标 | 修前 | 池化后 |
|---|---|---|
| 墙钟中位（绑核 40，7 轮交错） | 223 ms | **226 ms（+1.3%，更慢）** |
| cycles:u | 622.7 M | **633.9 M（+1.8%，更慢）** |
| 指令数 | 1,619,554,860 | 1,626,654,770（+0.44%） |

**为什么注定没有收益**：每次 throw 的堆操作有四份——展开包装（80B）、异常结构体、type 名串、
message 串，还有 traceback 捕获；池化只覆盖第一份，约占分配成本的 1/4 ≈ **1%**，
而 glibc 的 tcache 下「malloc 一个刚 free 的 80B 块」本来就只有 ~40 cycles。
归还侧的清零 + 分支反而多出 71 条指令/throw。

**结论：分配侧是零头，不要再往这里投入。** 71% 在 libgcc 展开器，量级收益只有方案 A 或方案 C。

### C. 明确文档化 `--no-zero-cost-exc`（零成本）
热路径上频繁真的抛异常时，编译期开关已经能回退到 setjmp（33ms，与 CPython 持平）。
README 里已经写了这条建议；如果再没有 A/B 的实现，这至少是**当下正确的答案**。

## 4. 重测方法（照抄即可）

```bash
cd /src/luo-proj/pyadamas
./build/lead/pyadamas build -O2 -o /tmp/ex_pya -I /src/luo-proj/excbench/pya /src/luo-proj/excbench/pya/main.py
for i in 1 2 3; do /usr/bin/time -f '%e s' /tmp/ex_pya; done
perf record -e cycles:u -o /tmp/perf_ex.data /tmp/ex_pya && perf report -i /tmp/perf_ex.data --stdio | head -20
```

⚠️ 不要用 `perf stat` 的 instructions 做唯一判据：这条路径的差异主要在**控制流/缓存**，
指令数变化不大（235ms 与 33ms 的指令数只差 2×）。