# 内存效率：RC 头 64 字节的构成与压缩评估（2026-11-27）

> 状态：**测量与评估，未实现**。结论：**不要按「只打包 size/kind/marked」做 48 字节**，两个硬冲突见 §3。

## 1. 现状

`PyAdamasRc` = 64 字节（`runtime/rt_internal.h`）：

```c
next, prev;            // 16  堆链，rc_unlink 靠它做 O(1) 摘除
int64_t rc;            // 8
void (*dtor)(void*);   // 8
void (*release)(void*);// 8
int64_t size;          // 8   GC 记账
int64_t kind;          // 8
int64_t marked;        // 8   标志位 **+ young 链表下标**
```

`_Static_assert(sizeof(PyAdamasRc) == 64)` 必须与 `src/backend/codegen.cpp` 里**预置字面量头**的
三处（`codegen.cpp:157`/`196`/`392`，str / big / 闭包）一致——漏一处就是字面量对象头部错位。

## 2. 开销占比（实测）

gcbench cycles 200K：`Maximum resident` **47.8 MB**（3 次：47,800 / 47,868 / 47,804 KB）。

每个 2 元素 list 的块构成：`header(64) + PyAdamasList(48) + 内联缓冲(8×8=64) = 176B`
（`PYADAMAS_LIST_INLINE_CAP` 是 **8**，见 `runtime/rt_internal.h:294`；glibc chunk ≈192）。
头部占 **36%**；内联缓冲占 36%——两者相当。

**推论**：把 `INLINE_CAP` 从 8 调到 4（2 元素 list 完全够用）能省 32B/对象 ≈ 6.4MB ≈ **RSS −13%**，
代价是「超过 4 元素的小 list」要多一次 realloc。这条比压缩头更容易验证，已在单独的实验里量化。

## 3. 两个硬冲突（2026-11-28 复核）

### 3.1 `marked` 不是纯标志位，它存着 young 链表的**下标**

```c
h->marked = ((int64_t)young_len << 8) | PYADAMAS_RC_YOUNG;   // rt_core.c:328
size_t i = (size_t)(h->marked >> 8);                        // :333  删除时按下标回填
```

低 8 位是标志（YOUNG/DEFER/UNTRACKED/LEAF/CONS/MARK），**其余位是下标**——这是 young 代
O(1) 摘除的基础。young 代长度自适应且无上限（`:3098`、`:3596`），gcbench cycles 一代就 20 万对象。
压到 u16（8 位下标）会让 `young_del` 下标回绕 → `young_buf[i] != h` 判断失败 → **已 free 的对象留在
young_buf**，下次 GC 拷进扫描集解引用 → 静默 UAF。

### 3.2 即使做到 48 字节，RSS 也只降 6.7%

每块省 16B → 3.2MB → **47.8 → 44.6 MB（−6.7%）**。而验收线是 ≥8%（需 ≥19B/对象）。
**只有把 `dtor`/`release` 也去掉（32 字节头）才能到 ≈−13%**——那是「第二步」。

## 4. 建议

1. **不要单独做 48 字节**。把「RC 头压缩」合并到**第二步（dtor/release 表化 → 32 字节）**一次立项：
   那时既腾出位给 `marked`/young 下标，RSS 也能到 −13%，8% 的验收才有意义。
2. 只想降 gcbench RSS 的另一条路：缩 list 内联块 / pool 参数（不动头布局），需单独量化。

## 5. 改头布局时的清单（给真要动手的人）

- `->size` 读写点 **35** 处、`->kind` **110** 处（14 文件）、`->marked` **52** 处；
- codegen 预置字面量头 **3** 处 + 1 条 `_Static_assert`；
- `PYADAMAS_RC_HDR = sizeof(...)` 会自动跟随，无需改；
- 验收：全套件 598 + **全量 difftest 541-0** + **ASan 单例 ≥3 个（0 报错）** + gcbench RSS + 12 基准不退化。