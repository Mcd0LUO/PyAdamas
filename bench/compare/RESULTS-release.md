# 四语言经典基准对比

生成时间：2026-11-13 03:12

环境：Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.41，CPU 未识别；Python 3.13.5；go version go1.18 gccgo (GCC) 14.4.0 linux/amd64；g++ (GCC) 14.4.0。

每种实现跑 5 次取最小值；PyAdamas 用 release preset（`-O2`） 编译，Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 7.8 ms | 0.7 ms | 9.3 ms | 0.8 ms | 10.58× | 是 |
| `fannkuch` | 8 | 32.2 ms | 2.8 ms | 9.1 ms | 1.1 ms | 11.68× | 是 |
| `fasta` | 40000 | 11.8 ms | 0.8 ms | 9.0 ms | 0.8 ms | 14.95× | 是 |
| `fib` | 32 | 154.8 ms | 7.6 ms | 9.3 ms | 2.6 ms | 20.48× | 是 |
| `mandelbrot` | 350 | 192.7 ms | 28.5 ms | 15.1 ms | 5.6 ms | 6.76× | 是 |
| `matmul` | 120 | 152.9 ms | 8.0 ms | 8.9 ms | 1.2 ms | 19.07× | 是 |
| `nbody` | 50000 | 199.0 ms | 10.8 ms | 10.8 ms | 2.3 ms | 18.49× | 是 |
| `nqueens` | 12 | 407.5 ms | 54.2 ms | 44.2 ms | 32.0 ms | 7.52× | 是 |
| `spectral_norm` | 600 | 566.8 ms | 21.0 ms | 15.0 ms | 6.2 ms | 26.95× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
