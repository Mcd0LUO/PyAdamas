# 四语言经典基准对比

生成时间：2026-11-13 02:51

环境：Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.41，CPU 未识别；Python 3.13.5；go version go1.18 gccgo (GCC) 14.4.0 linux/amd64；g++ (GCC) 14.4.0。

每种实现跑 5 次取最小值；PyAdamas 用 dev preset（`-O0`）编译，Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 7.7 ms | 0.8 ms | 8.0 ms | 0.8 ms | 10.12× | 是 |
| `fannkuch` | 8 | 31.5 ms | 2.8 ms | 9.1 ms | 1.2 ms | 11.45× | 是 |
| `fasta` | 40000 | 11.1 ms | 0.9 ms | 15.5 ms | 0.8 ms | 12.82× | 是 |
| `fib` | 32 | 148.0 ms | 6.7 ms | 15.4 ms | 2.5 ms | 21.94× | 是 |
| `mandelbrot` | 350 | 179.9 ms | 27.3 ms | 13.2 ms | 5.5 ms | 6.60× | 是 |
| `matmul` | 120 | 147.5 ms | 7.0 ms | 8.2 ms | 1.2 ms | 21.01× | 是 |
| `nbody` | 50000 | 198.1 ms | 13.0 ms | 10.3 ms | 2.7 ms | 15.29× | 是 |
| `nqueens` | 12 | 409.1 ms | 53.8 ms | 45.2 ms | 32.4 ms | 7.61× | 是 |
| `spectral_norm` | 600 | 555.5 ms | 20.0 ms | 13.6 ms | 6.2 ms | 27.83× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
