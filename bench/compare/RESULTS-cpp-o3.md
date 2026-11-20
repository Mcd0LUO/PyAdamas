# 四语言经典基准对比

生成时间：2026-11-20 18:42

环境：Linux-7.0.0-38-generic-x86_64-with-glibc2.43，CPU 未识别；Python 3.14.4；go version go1.26.10 linux/amd64；g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0。

每种实现跑 5 次取最小值；PyAdamas 用 build/lead（生成码 -O2）+ **SIMD 默认开 + LOOP_OPT=1 + RA_OPT=1**（0.336）；Go 默认优化；**C++ 用 `g++ -O3`**（本轮指定，wrapper 替换 -O2→-O3）。编译时间不计入。

> 注：多租户负载窗口（devirt 代理并行），绝对值波动，**同轮相对比值为准**。

| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | 输出一致 |
|---|---|---|---|---|---|---|---|
| `binary_trees` | 12 | 18.2 ms | 2.3 ms | 3.6 ms | 3.6 ms | 7.94× | 是 |
| `fannkuch` | 8 | 57.5 ms | 9.7 ms | 9.5 ms | 5.4 ms | 5.96× | 是 |
| `fasta` | 40000 | 46.8 ms | 4.5 ms | 5.6 ms | 4.2 ms | 10.41× | 是 |
| `fib` | 32 | 403.4 ms | 19.7 ms | 16.9 ms | 16.5 ms | 20.49× | 是 |
| `mandelbrot` | 350 | 466.8 ms | 26.0 ms | 17.1 ms | 14.2 ms | 17.98× | 是 |
| `matmul` | 120 | 206.1 ms | 4.2 ms | 9.3 ms | 7.1 ms | 49.52× | 是 |
| `nbody` | 50000 | 583.5 ms | 11.2 ms | 13.0 ms | 14.7 ms | 52.32× | 是 |
| `nqueens` | 12 | 694.1 ms | 90.9 ms | 62.9 ms | 62.6 ms | 7.64× | 是 |
| `spectral_norm` | 600 | 1482.9 ms | 12.8 ms | 15.1 ms | 14.2 ms | 115.61× | 是 |

用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、matmul（整数矩阵乘）、nqueens（N 皇后回溯）、mandelbrot（迭代计数）、fib（朴素递归）、fannkuch（全排列翻转）、binary_trees（二叉树分配）、fasta（伪随机序列生成）。
