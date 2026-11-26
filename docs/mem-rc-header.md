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
- 我们的 personality（`runtime/rt_personality.c`，677 行）**自采样占比是 0.00%**，
  说明它不是热点；热点是「逐帧走一遍」这个动作本身，以及 phase1/phase2 **走两遍**。

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