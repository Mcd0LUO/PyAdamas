# bench

逐个内置函数的性能对比：同一份源码分别喂给 **PyAdamas**（编译成原生二进制）
和 **CPython 3.13**，比墙钟时间。

## 跑法

在容器里跑（仓库挂在 `/w`）：

```bash
cd /w && python3 bench/run_bench.py                      # 全量
cd /w && python3 bench/run_bench.py 13_sum 32_sorted     # 只跑指定用例
cd /w && python3 bench/run_bench.py --repeat 5 --target 0.4

# 出表：Markdown 快照 + 原始数据
cd /w && python3 bench/run_bench.py --md bench/RESULTS.md --json bench/report.json

# 不重跑，用已有结果重新出表（改了表格格式时用）
cd /w && python3 bench/run_bench.py --from-json bench/report.json --md bench/RESULTS.md
```

表格直接打在 stdout 上。`RESULTS.md` 是一次实测快照，机器相关，别当基准线看。

## 加一个用例

复制 `bench/builtins/` 下现成的 `.py`，改三处：文件名（`<序号>_<内置名>.py`）、
第一行的说明（`# <名字> —— <说明>`，驱动会把它写进结果表）、以及 `N = `
那一行后面的初值（驱动会按 CPython 的实测时间把它改写掉）。正文要**两端
都跑得通**（CPython 也要能跑），否则驱动会直接跳过这个用例。

## 用例怎么设计的

`bench/builtins/<序号>_<内置名>.py` 每个文件测一个内置函数，第一行是
`N = <整数>`（驱动脚本会按实测时间改写它）。

三条规则，都是为了让「倍数」有意义：

1. **参数必须随循环变化。** 写 `len(S)`（`S` 是常量）的话，PyAdamas 的 LICM
   会把整个调用提到循环外，测出来的是「优化器有多强」，不是「`len` 有多贵」。
   所以取值一律走 `S[i % 64]` 这种下标，或者直接用循环变量。
2. **容器类内置先改写再消费。** `sum(T)`（`T` 是常量）同样会被提出来。
   用例里每轮先 `T[i % 64] = i` 再调用，`T` 被改了就提不动了。
3. **结果要打印出来。** 末尾 `print(total)`，避免整段被当成死代码删掉。

驱动脚本还会用 `2N` 再跑一次 PyAdamas：耗时没有跟着翻倍，就说明循环体被优化
掉了（提出 / 常量折叠 / DCE），这时候倍数只能当「优化器收益」看，不能当
「内置函数成本」看。表格里的「线性」列就是这个检查的结果。

输出也顺带比一遍（行数 + 最后一行），确认两边跑的是同一件事。`hash` 和
`id` 的数值语义本来就和 CPython 不同，只比速度。

## 已知口径

- 计时取多次运行的**最小值**：噪声只会让时间变长，不会变短。
- 「净加速」扣掉了进程启动开销（`00_baseline` 用例实测：CPython 约 8ms，
  PyAdamas 约 0.8ms）。原始倍数也在 `report.json` 里（`speedup_raw`）。
- PyAdamas 侧只算**运行**时间，不含编译时间；编译是静态编译，不参与每次运行。
- CPU 频率调节、容器负载都会影响结果，量级看就好。
- 用例里的 `N` 是**按 CPython 的实测时间校准**出来的，所以同一个用例在不同
  机器上 `N` 不一样，比的是倍数不是绝对时间。

## 四语言经典基准（`bench/compare/`）

同一份算法用 **Python / Go / C++** 各写一遍（Python 那份同时喂 CPython 和
PyAdamas），比墙钟时间并核对输出。

```bash
# 全量，每种实现每轮 9 次，3 轮后按轮取最小值合并（多租户机器的唯一稳口径）
PYADAMAS_BIN=$PWD/build/lead/pyadamas python3 bench/compare/run_compare.py \
    --repeat 9 --json /tmp/r1.json
PYADAMAS_BIN=$PWD/build/lead/pyadamas python3 bench/compare/run_compare.py \
    --merge /tmp/r1.json /tmp/r2.json /tmp/r3.json \
    --md bench/compare/RESULTS.md --json bench/compare/report.json
```

加一个用例：在同目录写 `<名字>.py` / `.go` / `.cpp` 三份，**三份的 `N` 必须相同**
（驱动读 `N = <int>` / `const N = <int>` / `static const int N = <int>;`），
输出必须逐字节一致 —— 不一致的行会在表里标 **否**。

两条口径：

- PyAdamas 每次 `--rebuild` 强制真编译。modcache 的键不含编译器版本，命中旧
  产物会量出和上次一模一样的二进制；
- 单轮绝对值能差 2 倍，所以结果一律「多轮取最小再合并」；表里同时给
  「相比 CPython」与「相比 Go/C++ 中的较快者」两列，后者才是真实水平。

