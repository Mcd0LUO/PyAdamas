# 四语言经典基准对比

生成时间：2026-11-20 18:32

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现跑 5 次取最小值；PyAdamas 用 build/lead（Release 编译器，生成码 -O2）+ **优化门控全开**（`PYADAMAS_SIMD=1 PYADAMAS_LOOP_OPT=1 PYADAMAS_RA_OPT=1`，版本 0.336）编译，Go 默认优化，C++ `g++ -O2`。编译时间不计入。

> 注：devirt 代理并行运行，多租户负载窗口，绝对值偏高/波动，**相对比值仅供参考**。门控默认关，需显式开启才得此性能。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 18.5 ms | 2.4 ms | 4.9 ms | 3.8 ms | 7.73× | 是 |
| `fannkuch` | 8 | 71.8 ms | 13.2 ms | 9.6 ms | 6.3 ms | 5.42× | 是 |
| `fasta` | 40000 | 46.0 ms | 2.1 ms | 5.4 ms | 3.6 ms | 21.83× | 是 |
| `fib` | 32 | 403.7 ms | 32.3 ms | 16.6 ms | 6.5 ms | 12.49× | 是 |
| `mandelbrot` | 350 | 479.0 ms | 18.2 ms | 16.7 ms | 14.2 ms | 26.31× | 是 |
| `matmul` | 120 | 211.1 ms | 5.9 ms | 7.4 ms | 7.4 ms | 36.05× | 是 |
| `nbody` | 50000 | 590.3 ms | 13.3 ms | 16.0 ms | 12.6 ms | 44.49× | 是 |
| `nqueens` | 12 | 692.5 ms | 91.3 ms | 63.5 ms | 62.5 ms | 7.59× | 是 |
| `spectral_norm` | 600 | 1503.8 ms | 25.8 ms | 15.3 ms | 26.2 ms | 58.20× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
