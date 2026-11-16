# 四语言经典基准对比

生成时间：2026-11-16 12:59

环境：Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.41，CPU 未识别；Python 3.13.5；go version go1.27.4 linux/amd64；g++ (Debian 14.2.0-19) 14.2.0。

每种实现跑 5 次取最小值；PyAdamas 用 release preset（`-O2`） 编译，Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 11.6 ms | 1.4 ms | 1.4 ms | 1.1 ms | 8.37× | 是 |
| `fannkuch` | 8 | 48.7 ms | 4.4 ms | 3.2 ms | 1.8 ms | 11.17× | 是 |
| `fasta` | 40000 | 17.8 ms | 1.4 ms | 1.6 ms | 1.2 ms | 12.71× | 是 |
| `fib` | 32 | 235.3 ms | 13.4 ms | 13.1 ms | 3.8 ms | 17.55× | 是 |
| `mandelbrot` | 350 | 281.8 ms | 11.7 ms | 9.8 ms | 8.9 ms | 24.04× | 是 |
| `matmul` | 120 | 222.0 ms | 15.1 ms | 2.7 ms | 1.8 ms | 14.71× | 是 |
| `nbody` | 50000 | 294.9 ms | 16.7 ms | 4.8 ms | 3.9 ms | 17.62× | 是 |
| `nqueens` | 12 | 629.9 ms | 92.3 ms | 52.7 ms | 53.5 ms | 6.83× | 是 |
| `spectral_norm` | 600 | 828.4 ms | 30.4 ms | 10.6 ms | 10.2 ms | 27.22× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
