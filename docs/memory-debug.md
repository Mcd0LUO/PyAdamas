# 分配器调试开关：尾部 canary 与 guard page

> 目的：补上「ASan 抓不到生成代码越界」的盲区。`--asan` 只把 `runtime/*.c`
> 重编成插桩版运行时，**生成的汇编完全不插桩** —— masked vectorizer 写越界、
> 从错的槽读这类 bug 在 ASan 下静默通过。这两个开关用零插桩方式，在普通
> 测试里就能抓到堆越界。默认**两个都关**，关闭时运行时对象与改动前逐字节
> 相同（见 §5），所以不改变任何行为/性能。

## 1. 用法

```bash
# 尾部 canary：对象尾部多 32 字节保护带，释放时校验
PYADAMAS_CANARY=1 build/lead/pyadamas run tests/run/520_alloc_churn.py

# canary + 头字校验（size/kind 被踩也能发现）
PYADAMAS_CANARY=2 ...

# guard page：每个对象单独 mmap，对象尾部后面紧跟一页 PROT_NONE
PYADAMAS_GUARD=1 ...

# 全量套件（CANARY 内存开销小，可以全量；GUARD 只跑子集）
PYADAMAS_TEST_JOBS=16 PYADAMAS_CANARY=1 \
    bash tests/run_tests.sh build/gfix-lang build/lead/pyadamas
PYADAMAS_TEST_FILTER='[0-2]*' PYADAMAS_TEST_JOBS=16 PYADAMAS_GUARD=1 \
    bash tests/run_tests.sh build/gfixa build/lead/pyadamas
```

环境变量在**编译（链接）程序时**读取：pyadamas 编译器据此改链
`rt_allocdbg.o`（分配器调试版运行时）而不是 `rt.o`；程序运行期再读一次
决定 canary/guard 模式。同一进程内只读一次。

演示（仓库自带，三种模式各跑一遍）：

```bash
bash tests/memdebug/run_demo.sh          # 默认 build/lead/pyadamas
```

## 2. canary 模式（PYADAMAS_CANARY=1/2）

- 每个 `rc_alloc*` 对象在 payload 之后分配 32 字节保护带：前 16 字节固定
  填 `0xC5`，后 16 字节在 mode 2 里存分配时的 `size`/`kind`。
- **释放时**（RC 归零、GC 清扫、孤儿块、池/缓存兜底释放都走同一个
  `pa_dbg_free`）逐字节校验；被改写就 panic：

  ```
  pyadamas: runtime error: canary 被踩：对象 kind=0 size=104 偏移=0 期望=0xc5 实际=0x5a
  ```

  `kind` 是 `PYADAMAS_RC_*`（0=str、1=list、2=obj、4=dict、5=instance…），
  `size` 是 payload 字节数，偏移是保护带内第几个字节。
- mode 2 额外校验头字：`size`/`kind` 与分配时记录的一致、`rc` 在
  合理范围 —— 头字被踩会在释放时被点名。
- **只在释放时检查**：越界写发生后、对象被释放之前不会打断程序（也不会
  破坏 canary 之外的堆；demo 里靠释放时的 panic 证明）。对象如果在进程
  退出前没被释放，就查不到（模块级槽在退出时会释放，通常没问题）。

## 3. guard page 模式（PYADAMAS_GUARD=1）

- 每个对象单独 `mmap` 若干数据页 + 紧跟一页 `PROT_NONE`；payload 尾部
  贴近数据页末尾，**越界写撞到保护页立刻 SIGSEGV**（比 canary 更早、定位
  更准）。
- 释放时 `munmap`（同一处释放钩子；RC/GC 所有路径共用）。
- 代价：每个对象至少 2 页（8 KiB）+ 一次 mmap/mprotect/munmap 系统调用，
  内存和耗时都显著放大（只适合小规模测试/复现）。实测见 §6。
- 已知限制：payload 起点保持 16 字节对齐，所以对象末尾与保护页之间可能有
  **最多 15 字节尾隙**；只有写成片/写得够远才会撞页。想精确到「第一个越界
  字节」用 canary 模式。

## 4. 覆盖范围（分配路径清单）

所有对象都经 `runtime/rt_core.c` 的单一分配器 `rc_alloc_core`
（`rc_alloc` / `rc_alloc_ex` / `rc_alloc_untracked` / `rc_alloc_untracked_ex`
只是不同入口），所以一处钩子覆盖全部：

| 对象 | 入口 |
|---|---|
| str / bytes（含 SSO 短串） | `str_alloc_raw` → `rc_alloc` |
| list / tuple（含内联缓冲的小列表） | `pa_list_new*` → `rc_alloc_ex` |
| dict / set | `pa_dict_new` / `set_from_list` → `rc_alloc(_ex)` |
| 类实例 / 闭包 / range / struct | 生成代码 `ObjNew` / `ClosureNew` 等 → `rc_alloc` |
| 生成器帧 / 协程 / await 对象 | `rt_gen.c` / `rt_async.c` → `rc_alloc` |
| 异常值 / 异常类 | `rt_exc.c` → `rc_alloc` |
| big（limb 在块内） | `rt_big.c` → `rc_alloc_ex` |
| CPython 桥 float 盒 / 动态 JSON 的 null/bool 盒 | `rc_alloc`（无出边小对象） |
| 容器二级 malloc 缓冲（list->data、dict->entries/order/index） | **不覆盖**（普通 malloc，不经 RC 分配器） |
| 生成代码的栈 / 帧内联槽 / 静态字面量 | **不覆盖**（不在堆上） |

> 注意：`--asan` 与 canary/guard 同时给时 **asan 优先**（链接 `rt_asan.o`）；
> 分配器改造与 ASan 插桩互斥，别同时开。

## 5. 为什么默认零开销

- 调试代码只在 `-DPYADAMAS_ALLOC_DEBUG=1` 编译的 `rt_allocdbg.o` 里；
  默认 `rt.o` 里钩子宏展开成**改动前逐字相同的** `malloc(sizeof(PyAdamasRc)
  + size)` / `free(h)`。
- 验证方法（本机实测）：
  - **运行时对象逐字节相同**：在 HEAD `3d8018c` 的干净 worktree 里，带补丁
    构建出的默认 `rt.o` 与把 `runtime/rt_core.c` 还原后重建的 `rt.o`
    `cmp` **逐字节相同**（`cc -O2 ... rt_core.c` 的单对象同理）。
  - **性能闸门**：`tools/checkall.sh --perf` 在 patched 构建上 PASS
    （12 基准 warn=0 fail=0，偏差在 ±0.6% 内且属于采集抖动）。
  - **生成程序逐字节相同**：在**同一个仓库路径**下，用改动前的编译器
    二进制与改动后的编译器分别 `build --rebuild -O2` 编 12 个基准，
    产物两两 `cmp` **全部相同** —— 这比「指令数相同」更强（同一份机器码，
    指令数必然相同）。注意比较必须在同一路径下做：生成程序里嵌了源文件
    绝对路径（调试信息/异常回溯用），换目录编同一份代码也会差几个字节，
    但那不是代码差异（实测换 worktree 路径时差的全是路径字符串）。
  - 直接量 `perf stat -e instructions:u` 时，同代码不同次运行会有
    ±0.1% 抖动（ASLR 改变堆地址 → malloc/分支走不同路径），所以「逐项
    相同」以**二进制相同**为准；实测 12 项差值都在该抖动范围内（见 §6）。
- 如果改成运行期 `if (canary_on)`，每次分配/释放都会多执行分支指令，
  达不到「逐项相同」；所以这里用「链接期选运行时对象」。

## 6. 实测记录（本机）

环境：`-O2 --rebuild`，`perf stat -e instructions:u` 绑核 3 轮取最小
（与 `bench/compare` 12 个基准同一口径）。

| 基准 | 改动前 | 改动后（默认关） | 生成程序 |
|---|---|---|---|
| binary_trees | 4,725,477 | 4,722,439 | 逐字节相同 |
| collatz | 81,504,558 | 81,504,564 | 逐字节相同 |
| fannkuch | 34,927,047 | 34,927,049 | 逐字节相同 |
| fasta | 5,956,376 | 5,956,375 | 逐字节相同 |
| fib | 130,640,168 | 130,640,176 | 逐字节相同 |
| mandelbrot | 44,080,496 | 44,080,498 | 逐字节相同 |
| matmul | 10,127,390 | 10,127,392 | 逐字节相同 |
| nbody | 109,040,711 | 109,040,721 | 逐字节相同 |
| nqueens | 339,789,090 | 339,789,123 | 逐字节相同 |
| sieve | 19,631,899 | 19,631,900 | 逐字节相同 |
| spectral_norm | 94,638,028 | 94,638,034 | 逐字节相同 |
| wordfreq | 251,337,105 | 251,337,138 | 逐字节相同 |

> 说明：两次 `perf` 采集之间 ±0.1% 的差值是测量抖动（同二进制重复跑也会
> 变），产物二进制相同才是「零开销」的硬证据。

套件（干净 worktree：HEAD `3d8018c` + 本补丁，同一台机器，`PYADAMAS_TEST_JOBS=16`）：

| 模式 | 范围 | 结果 | 耗时 | 峰值 RSS |
|---|---|---|---|---|
| 默认 | 全量 | 422 用例全过，0 失败 | 1:00 | 128 MB |
| `PYADAMAS_CANARY=1` | 全量 | 422 用例全过，0 失败 | 1:05 | 140 MB |
| 默认 | `[0-2]*` 子集 | 192 用例全过 | 0:27 | 127 MB |
| `PYADAMAS_GUARD=1` | `[0-2]*` 子集 | 192 用例全过 | 1:09 | 1.72 GB |

- canary 全量只比默认慢 ~8%、内存 +9%：每个对象多 32 字节保护带。
- guard 每个对象 2 页 + mmap/mprotect/munmap，**耗时 ~2.6×、峰值内存 ~13×**
  —— 只适合子集/复现，别对全量开。
- `PYADAMAS_CANARY=1` 的全量运行包含 `tests/run/520_alloc_churn.py`
  （专门的分配/释放 churn 用例），用来确认调试运行时没有误报。
- ASan 交叉验证：`PYADAMAS_TEST_FILTER='520_*' PYADAMAS_TEST_ASAN=1` 通过
  （调试运行时本身不含 ASan 插桩，二者不叠加）。
- 性能闸门：`tools/checkall.sh --perf`（已提交的工具，12 基准指令数）在
  patched 编译器的干净构建上 **PASS，warn=0 fail=0**（最大偏差 -0.57%，
  其余 ≤0.02%，属于基线采集抖动口径）。

## 7. demo：证明真的抓得到

`tests/memdebug/` 下四个文件（**不进常规套件**）：

- `oob.c`：`pa_memdbg_stomp(char* p, long off, long n)` 从 p+off 起写 n 个
  `0x5A`（故意越界）；
- `memdbg.cpya`：`cimport` 声明，把 `str` 的字符区指针交给 C（Python 层
  没有合法越界手段，只能借 cimport）；
- `demo.py`：构造 len=63 的堆字符串（payload=104 字节，`data[63]` 是结尾
  NUL，**`data[64]` 正好是对象尾部之后第一个字节**），按模式选择写 8 字节
  （踩 canary）或 40 字节（写穿 guard 尾隙撞保护页）；
- `run_demo.sh`：三段式验证脚本。

`bash tests/memdebug/run_demo.sh` 的输出（实测）：

```
=== 1) 默认（两个开关都关）：越界写是盲区 ===
exit=0（越界写没有被打断；具体是否崩坏取决于堆布局）

=== 2) PYADAMAS_CANARY=1：释放时校验尾部 canary ===
pyadamas: runtime error: canary 被踩：对象 kind=0 size=104 偏移=0 期望=0xc5 实际=0x5a
len 63
still alive after oob write
first chars AAA
exit=1
CANARY OK：抓到越界写

=== 3) PYADAMAS_GUARD=1：对象尾部紧跟 PROT_NONE 页 ===
exit=139（139 = SIGSEGV，越界写当场触发）
GUARD OK：抓到越界写
```

## 8. 实现位置

| 位置 | 内容 |
|---|---|
| `runtime/rt_core.c` 顶部 `PYADAMAS_ALLOC_DEBUG` 段 | canary/guard 分配器与释放校验；`PA_DBG_MALLOC/PA_DBG_FREE/PA_DBG_HEAD_INIT` 钩子宏 |
| `runtime/rt_core.c` 的 `rc_alloc_core` / `pa_rc_dec_slow` / `rc_leaf_free` / `gc_free_body` / 5 个 `*_pool_put` | 分配与**所有**释放路径接钩子；调试版不复用池块 |
| `CMakeLists.txt` 的 `pyadamas_rt_debug` | 用 `-DPYADAMAS_ALLOC_DEBUG=1 -O2 -g` 重编 runtime，合成 `rt_allocdbg.o` |
| `src/driver/build.cpp` | 环境变量选择链接 `rt_allocdbg.o`；缺失时报错；工具链指纹并入调试版摘要 |

## 9. 案例：保守 GC 模式的 heap-use-after-free（3.10，未修）

`PYADAMAS_GC_CONSERVATIVE=1` 下 `tests/run/161_gc_conservative_equiv.py` 段错误。ASan 给出精确栈：

```
READ of size 8 at ... (heap-use-after-free)
  #0 pa_rc_inc_fast            runtime/rt_internal.h:872
  #1 pa_any_from_f64           runtime/rt_any.c:158
  #2 pyadamas_fn_4churn_4      161_gc_conservative_equiv.py:38

freed by:
  #0 free
  #1 gc_free_body              runtime/rt_core.c:3261
  #2 gc_collect                runtime/rt_core.c:3431
  #3 rc_alloc_core             runtime/rt_core.c:1852
  #4 pa_list_new               runtime/rt_list.c:177
```

即：**分配触发的 gc_collect 回收了一个随后立刻被 `pa_any_from_f64` 增加 RC 的对象**。

已排查：运行时**有**寄存器扫描（`gc_scan_registers` 用 `_setjmp` 扫 `jmp_buf`，见 `rt_core.c:2782`），
保守模式也会退化成整帧扫描（`f->scan_mask && !gc_conservative` 为假时走全帧）。
所以不是「没扫寄存器」这么简单，更可能是：

- 保守扫描的**根集合**与实际存活引用不一致（例如某个中间对象只被「正在构造的容器」持有，而构造顺序让它在扫描点不可见）；
- 或 `gc_collect` 在 `rc_alloc_core` 内部触发时，**当前分配中的对象**尚未挂上堆链/索引，却被当成不可达。

下一步建议：在 `gc_free_body` 里打印被释放对象的 kind/地址，与 `pa_any_from_f64` 里那个指针比对，确认是不是「同一个对象的 RC 头被当成 float 盒」；
再把 `gc_collect` 的触发点从 `rc_alloc_core` 内部挪到「分配完成、对象已挂链之后」，看崩溃是否消失（这是最小可疑点）。

现状：**模式是 opt-in，默认关闭**；在修好前不要用它做等价性验证（`161_gc_conservative_equiv` 现在是已知红）。

