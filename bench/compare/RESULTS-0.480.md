# 四语言经典基准对比

生成时间：2026-11-24 21:23

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现 跑 5 次取最小值；PyAdamas 用 `/src/luo-proj/pyadamas/build/lead/pyadamas` 编译（生成码 `-O2`，每次 `--rebuild` 强制真编译），Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入；多租户机器上单轮绝对值能差 2 倍，所以这里按轮取最小值再合并。

**这里的 PyAdamas 是默认管线**：通用自动向量化（`src/backend/vect.cpp` + `backend/masked.cpp`）默认开，5/12 个基准命中（matmul -86.5%、spectral_norm -59.6%、sieve -51.9%、nbody -45.2%、mandelbrot -36.3%，口径见 `docs/vectorization-coverage.md`）。照着基准循环形状写死的特化 SIMD 模板（`PYADAMAS_SIMD`）已在 0.480 **整段删除** —— 它们对当前 IR 一次都没再匹配上。换句话说：这张表没有用靶子上的特化。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | PyAdamas/最快编译实现 | 输出一致 |
|---|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 31.8 ms | 2.2 ms | 5.1 ms | 3.7 ms | 14.40× | 1.67× | 是 |
| `collatz` | 50000 | 481.8 ms | 18.5 ms | 15.6 ms | 12.3 ms | 26.09× | 0.67× | 是 |
| `fannkuch` | 8 | 70.8 ms | 7.5 ms | 5.7 ms | 4.7 ms | 9.46× | 0.62× | 是 |
| `fasta` | 40000 | 31.8 ms | 4.0 ms | 5.1 ms | 3.2 ms | 7.97× | 0.79× | 是 |
| `fib` | 32 | 404.7 ms | 33.0 ms | 23.4 ms | 10.4 ms | 12.26× | 0.32× | 是 |
| `mandelbrot` | 350 | 478.4 ms | 15.5 ms | 29.2 ms | 20.4 ms | 30.92× | 1.32× | 是 |
| `matmul` | 120 | 205.4 ms | 4.3 ms | 6.3 ms | 3.8 ms | 47.90× | 0.88× | 是 |
| `nbody` | 50000 | 589.5 ms | 16.1 ms | 18.8 ms | 16.7 ms | 36.71× | 1.04× | 是 |
| `nqueens` | 12 | 702.8 ms | 98.1 ms | 75.0 ms | 75.0 ms | 7.17× | 0.77× | 是 |
| `sieve` | 500000 | 318.7 ms | 7.0 ms | 6.6 ms | 9.3 ms | 45.83× | 0.95× | 是 |
| `spectral_norm` | 600 | 1484.2 ms | 26.0 ms | 16.1 ms | 14.0 ms | 57.11× | 0.54× | 是 |
| `wordfreq` | 200000 | 187.4 ms | 40.5 ms | 40.1 ms | 29.1 ms | 4.63× | 0.72× | 是 |

共 12 个用例。对比 CPython：全部更快；对比 Go/C++ 中的较快者：2 个明显更快、8 个明显更慢、2 个落在 ±5% 噪声内。

明显偏慢：`collatz`、`fannkuch`、`fasta`、`fib`、`matmul`、`nqueens`、`spectral_norm`、`wordfreq`

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）、collatz（Collatz 步数统计）、sieve（埃氏筛计数）、wordfreq（LCG 造词 + dict 计数）。

## 校验与附注（0.480 复核）

- 编译器：`build/lead/pyadamas`（HEAD `44ca045`，0.480）；生成码 `-O2`，每例 `--rebuild`。
- 12 例输出与 CPython 逐字节一致（`run_compare.py` 自带比对，表末列「输出一致」）。
- 12 个 `-O2` 产物的用户态指令数见 `tools/perf_baseline.txt`（0.480 刷新）。
- **Codon 九例（7 例可比）**：`python3 tools/codon_cmp.py --rounds 5 --out .ref_proj/benchmarks/pyadamas-0.480-vs-codon-0.20.7.json`
  （同一份源码 `codon build -release`，绑核、交替 5 轮取最小，两侧输出都要求与 CPython 一致）：

| 用例 | PyAdamas | Codon | PA/Codon |
|---|---|---|---|
| fib | 21.6 ms | 16.0 ms | 1.352 |
| mandelbrot | 12.9 ms | 22.7 ms | 0.570 |
| matmul | 6.5 ms | 12.9 ms | 0.509 |
| nbody | 18.2 ms | 17.6 ms | 1.030 |
| nqueens | 87.1 ms | 101.2 ms | 0.861 |
| spectral_norm | 17.5 ms | 20.8 ms | 0.840 |
| obj_dispatch | **291.9 ms** | **113.6 ms** | **2.570** |

几何平均 **PA/Codon = 0.960（n=7）**。`binary_trees` / `fannkuch` 用 Codon 0.20.7 编不过
（本仓源码的泛型标注不兼容：`'str' does not match expected type 'Node[str,str]'`），不可比。
`obj_dispatch` 仍是唯一显著落后项（2.57×）：默认管线不开 RC 借用
（`PYADAMAS_BORROW_OPT=1` 时 300.59→202.24 ms，对 Codon 1.78×）；该开关按
[docs/experiment-switches.md](../../docs/experiment-switches.md) 的决定保持 opt-in。
