# 三语言经典基准（0.158 本机快照）

生成时间：2026-11-14

环境：WSL2 Debian trixie；CPython 3.13.5；g++ 14.2.0；PyAdamas release preset（生成代码 -O2）。每种实现跑 5 次取最小值；9 个用例的 PyAdamas / CPython / C++ 标准输出全部一致。Go 未安装，本表不含 Go。

| 用例 | N | CPython | PyAdamas | C++ -O2 | PyAdamas/CPython |
|---|---|---|---|---|---|
| `binary_trees` | 12 | 7.2 ms | 0.8 ms | 0.6 ms | 8.91× |
| `fannkuch` | 8 | 29.5 ms | 2.6 ms | 1.1 ms | 11.51× |
| `fasta` | 40000 | 10.9 ms | 0.8 ms | 0.8 ms | 13.26× |
| `fib` | 32 | 148.3 ms | 10.0 ms | 2.2 ms | 14.87× |
| `mandelbrot` | 350 | 178.1 ms | 6.9 ms | 5.1 ms | 25.81× |
| `matmul` | 120 | 143.6 ms | 7.4 ms | 1.1 ms | 19.36× |
| `nbody` | 50000 | 190.0 ms | 10.2 ms | 2.3 ms | 18.54× |
| `nqueens` | 12 | 388.5 ms | 45.6 ms | 32.0 ms | 8.53× |
| `spectral_norm` | 600 | 543.7 ms | 18.4 ms | 6.0 ms | 29.49× |

这是本机 WSL 快照，绝对耗时和 [RESULTS-release.md](RESULTS-release.md) 的 gcc:14 容器口径不同，只看相对倍数与趋势。mandelbrot 从 0.156 记录的同口径 12.0 ms 级别降到本机 6.9 ms，主要来自 0.156/0.157 的浮点循环优化；具体性能对比以同机 A/B 为准。

0.160 复跑（同机）：mandelbrot 26.5×、spectral_norm 30.3×、fib 15.3×、matmul 20.0×、nqueens 8.7×，与 0.158 持平；traceback 调试表不影响 release 生成代码。
