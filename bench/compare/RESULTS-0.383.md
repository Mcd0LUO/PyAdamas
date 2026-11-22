# 四语言经典基准对比

生成时间：2026-11-22 13:47

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现 3 轮 × 每轮 9 次，取所有采样的最小值；PyAdamas 用 `/src/luo-proj/pyadamas/build/lead/pyadamas` 编译（生成码 `-O2`，每次 `--rebuild` 强制真编译），Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入；多租户机器上单轮绝对值能差 2 倍，所以这里按轮取最小值再合并。

**默认管线里浮点循环的向量化是按 IR 形状模板做的**（reduction / int map / nbody pair，`src/codegen_simd*.cpp`，默认开），通用自动向量化（`src/codegen_vect.cpp`）默认关。关掉特化 SIMD 后 `matmul`/`spectral_norm`/`nbody` 慢 2–4×，其余用例在噪声内 —— 实测见 `bench/compare/SIMD-AB.md`。表里最漂亮的三行正是形状模板承包的；`sieve`/`wordfreq` 这种没被覆盖的写法才是可外推的水平。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | PyAdamas/最快编译实现 | 输出一致 |
|---|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 18.1 ms | 2.1 ms | 4.2 ms | 3.3 ms | 8.76× | 1.60× | 是 |
| `collatz` | 50000 | 482.3 ms | 28.4 ms | 15.2 ms | 11.5 ms | 17.01× | 0.40× | 是 |
| `fannkuch` | 8 | 57.2 ms | 8.0 ms | 6.9 ms | 3.3 ms | 7.14× | 0.41× | 是 |
| `fasta` | 40000 | 31.6 ms | 2.0 ms | 4.8 ms | 4.2 ms | 15.62× | 2.09× | 是 |
| `fib` | 32 | 402.8 ms | 19.2 ms | 16.6 ms | 6.6 ms | 21.00× | 0.35× | 是 |
| `mandelbrot` | 350 | 465.3 ms | 18.0 ms | 16.6 ms | 14.7 ms | 25.80× | 0.82× | 是 |
| `matmul` | 120 | 203.9 ms | 2.7 ms | 7.1 ms | 6.4 ms | 76.90× | 2.42× | 是 |
| `nbody` | 50000 | 575.4 ms | 8.4 ms | 9.9 ms | 7.6 ms | 68.74× | 0.91× | 是 |
| `nqueens` | 12 | 690.3 ms | 99.0 ms | 62.8 ms | 62.0 ms | 6.98× | 0.63× | 是 |
| `sieve` | 500000 | 301.4 ms | 16.7 ms | 5.7 ms | 9.1 ms | 18.10× | 0.34× | 是 |
| `spectral_norm` | 600 | 1484.9 ms | 14.2 ms | 15.1 ms | 12.5 ms | 104.69× | 0.88× | 是 |
| `wordfreq` | 200000 | 173.1 ms | 35.2 ms | 23.2 ms | 16.5 ms | 4.91× | 0.47× | 是 |

共 12 个用例。对比 CPython：全部更快；对比 Go/C++ 中的较快者：3 个明显更快、9 个明显更慢、0 个落在 ±5% 噪声内。

明显偏慢：`collatz`、`fannkuch`、`fib`、`mandelbrot`、`nbody`、`nqueens`、`sieve`、`spectral_norm`、`wordfreq`

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）、collatz（Collatz 步数统计）、sieve（埃氏筛计数）、wordfreq（LCG 造词 + dict 计数）。
