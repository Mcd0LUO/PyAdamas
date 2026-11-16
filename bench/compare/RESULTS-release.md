# 四语言经典基准对比

生成时间：2026-11-16 13:32

环境：Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.41，CPU 未识别；Python 3.13.5；go version go1.27.4 linux/amd64；g++ (Debian 14.2.0-19) 14.2.0。

每种实现跑 5 次取最小值；PyAdamas 用 release preset（`-O2`） 编译，Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 12.2 ms | 1.3 ms | 1.4 ms | 1.1 ms | 9.20× | 是 |
| `fannkuch` | 8 | 49.2 ms | 4.4 ms | 3.0 ms | 1.8 ms | 11.24× | 是 |
| `fasta` | 40000 | 18.7 ms | 1.4 ms | 1.6 ms | 1.2 ms | 13.14× | 是 |
| `fib` | 32 | 237.1 ms | 13.4 ms | 13.1 ms | 3.6 ms | 17.68× | 是 |
| `mandelbrot` | 350 | 284.5 ms | 11.8 ms | 10.1 ms | 9.0 ms | 24.16× | 是 |
| `matmul` | 120 | 226.6 ms | 15.6 ms | 2.6 ms | 2.0 ms | 14.52× | 是 |
| `nbody` | 50000 | 303.9 ms | 16.8 ms | 4.8 ms | 3.8 ms | 18.06× | 是 |
| `nqueens` | 12 | 633.5 ms | 92.3 ms | 52.4 ms | 53.4 ms | 6.86× | 是 |
| `spectral_norm` | 600 | 830.2 ms | 31.3 ms | 10.6 ms | 10.2 ms | 26.51× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
