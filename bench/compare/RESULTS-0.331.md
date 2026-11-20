# 四语言经典基准对比

生成时间：2026-11-20 12:00

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现跑 5 次取最小值；PyAdamas 用 **build/lead（Release -O3 编译器，生成码默认 -O2）** 编译（版本 0.331 / `05680b7`；harness 按路径误标 dev/-O0，实际默认 opt_level=2），Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入。

> 注：多租户负载窗口（另有 slim 代理在构建），绝对值偏高于空闲基线，**相对比值为准**。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 19.5 ms | 5.0 ms | 4.9 ms | 3.4 ms | 3.93× | 是 |
| `fannkuch` | 8 | 60.6 ms | 10.9 ms | 10.9 ms | 4.5 ms | 5.54× | 是 |
| `fasta` | 40000 | 45.2 ms | 4.7 ms | 5.0 ms | 3.5 ms | 9.71× | 是 |
| `fib` | 32 | 403.4 ms | 19.4 ms | 16.9 ms | 7.3 ms | 20.83× | 是 |
| `mandelbrot` | 350 | 481.4 ms | 18.4 ms | 16.8 ms | 14.2 ms | 26.15× | 是 |
| `matmul` | 120 | 220.6 ms | 14.2 ms | 6.0 ms | 5.9 ms | 15.54× | 是 |
| `nbody` | 50000 | 590.6 ms | 33.2 ms | 16.1 ms | 16.3 ms | 17.81× | 是 |
| `nqueens` | 12 | 705.7 ms | 99.5 ms | 63.2 ms | 61.9 ms | 7.09× | 是 |
| `spectral_norm` | 600 | 1497.7 ms | 54.4 ms | 15.9 ms | 15.1 ms | 27.51× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
