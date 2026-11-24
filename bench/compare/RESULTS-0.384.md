# 四语言经典基准对比

生成时间：2026-11-22 13:59

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现 2 轮 × 每轮 9 次，取所有采样的最小值；PyAdamas 用 `/src/luo-proj/pyadamas/build/lead/pyadamas` 编译（生成码 `-O2`，每次 `--rebuild` 强制真编译），Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入；多租户机器上单轮绝对值能差 2 倍，所以这里按轮取最小值再合并。

**这里的 PyAdamas 是默认管线**：通用自动向量化（`src/backend/vect.cpp`）默认开；照着基准循环形状写的特化 SIMD（`src/codegen_simd*.cpp`）自 0.384 起**移出默认管线**，只有 `PYADAMAS_SIMD=1` 才开。特化整套关掉后 `matmul`/`spectral_norm`/`nbody` 各慢 4.5× 左右，其余用例在噪声内；量化与泛化计划见 `docs/vectorization-generalization.md`。换句话说：这张表没有用靶子上的特化，`sieve`/`wordfreq` 这类写法与它们在同一口径下比较。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | PyAdamas/最快编译实现 | 输出一致 |
|---|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 18.0 ms | 2.1 ms | 4.2 ms | 3.5 ms | 8.49× | 1.66× | 是 |
| `collatz` | 50000 | 481.2 ms | 40.4 ms | 15.4 ms | 12.6 ms | 11.91× | 0.31× | 是 |
| `fannkuch` | 8 | 57.3 ms | 10.4 ms | 5.9 ms | 4.2 ms | 5.49× | 0.40× | 是 |
| `fasta` | 40000 | 31.8 ms | 2.0 ms | 4.5 ms | 4.2 ms | 16.12× | 2.12× | 是 |
| `fib` | 32 | 401.4 ms | 19.2 ms | 16.6 ms | 6.6 ms | 20.86× | 0.34× | 是 |
| `mandelbrot` | 350 | 466.2 ms | 18.2 ms | 17.2 ms | 14.0 ms | 25.57× | 0.77× | 是 |
| `matmul` | 120 | 205.2 ms | 14.2 ms | 5.1 ms | 4.5 ms | 14.50× | 0.31× | 是 |
| `nbody` | 50000 | 577.4 ms | 32.9 ms | 11.1 ms | 14.8 ms | 17.55× | 0.34× | 是 |
| `nqueens` | 12 | 693.6 ms | 98.7 ms | 62.9 ms | 62.2 ms | 7.02× | 0.63× | 是 |
| `sieve` | 500000 | 299.2 ms | 29.3 ms | 6.0 ms | 6.0 ms | 10.23× | 0.21× | 是 |
| `spectral_norm` | 600 | 1487.3 ms | 53.5 ms | 15.8 ms | 19.1 ms | 27.79× | 0.29× | 是 |
| `wordfreq` | 200000 | 172.7 ms | 35.4 ms | 23.6 ms | 17.0 ms | 4.88× | 0.48× | 是 |

共 12 个用例。对比 CPython：全部更快；对比 Go/C++ 中的较快者：2 个明显更快、10 个明显更慢、0 个落在 ±5% 噪声内。

明显偏慢：`collatz`、`fannkuch`、`fib`、`mandelbrot`、`matmul`、`nbody`、`nqueens`、`sieve`、`spectral_norm`、`wordfreq`

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）、collatz（Collatz 步数统计）、sieve（埃氏筛计数）、wordfreq（LCG 造词 + dict 计数）。
