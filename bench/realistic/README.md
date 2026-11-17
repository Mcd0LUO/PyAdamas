# bench/realistic —— 真实程序形态的长跑基准

`bench/compare/` 的 9 个经典基准全是**数值 / 数组密集**循环，覆盖不到
真实程序最常见的形态：字典、对象分派、字符串构建、递归、排序、事件队列。

这 6 个用例补齐那部分。每个都有 `.py` 和 `.go` 两份实现，同一份算法、
同样的数据规模，输出逐字节一致（可交叉验证）。

## 用例

| 文件 | 覆盖 | 典型瓶颈 |
|---|---|---|
| `dict_wordfreq` | 字典计数 + 排序 | 哈希表、`dict.get` |
| `obj_dispatch` | 类继承 + 虚方法分派 | 方法调用、对象布局 |
| `str_build` | 字符串拼接 / 切片 | 字符串分配与拷贝 |
| `rec_memo` | 递归 + 记忆化 | 调用开销、小字典 |
| `sort_struct` | 元组排序聚合 | 元组构造、`list.sort` |
| `queue_sim` | 事件队列 | 列表增删、元组分配 |

## 跑法

```bash
# PyAdamas
for f in bench/realistic/*.py; do
  ./build/release/pyadamas build -O2 "$f" -o /tmp/$(basename "$f" .py)
  taskset -c 0 /tmp/$(basename "$f" .py)
done

# Go
for f in bench/realistic/*.go; do
  go build -o /tmp/$(basename "$f" .go)_go "$f"
  taskset -c 0 /tmp/$(basename "$f" .go)_go
done

# CPython
for f in bench/realistic/*.py; do python3 "$f"; done
```

## 规模口径

每个文件的 `N` 已校准到 PyAdamas 跑 **250-290ms**（`taskset -c 0`，取最小值）。
Go 因为快很多，同一份 N 下只跑 20-250ms。

## 已知坑

1. **`queue_sim` 的初版有缺陷**：`pending` 从不清空，`while i < len(pending)`
   每轮扫全表 → O(steps²)，测出来的是「平方级扫描 + 索引访问」的复合成本，
   不是队列语义。现在改成真正出队，队列长度稳定在 O(1)。
2. **`str_build` 用 `"".join(parts)`**：Go 侧对应 `strings.Builder`。两者
   语义等价但实现路径不同，比较时注意口径。
3. **`sort_struct` 的元组是堆对象**：Python 语义要求 `(a, b, c)` 是独立对象，
   Go 的 `Rec{a,b,c}` 是 slice 里的值类型。这个差异是**语言语义决定**的，
   不是实现缺陷 —— 但 PyAdamas 可以把「小定长元组」做得更便宜（见下）。

## 这组基准发现了什么

（2026-11-17，提交 `0.266` 前后）

| 基准 | 修复前 | 修复后 | 收益 |
|---|---|---|---|
| `sort_struct` | 288.6ms | 167.4ms | **-42.0%** |
| `queue_sim` | 254.7ms | 213.8ms | **-16.0%** |
| `dict_wordfreq` | 255.4ms | 249.2ms | -2.4% |
| `str_build` | 272.9ms | 268.7ms | -1.5% |
| 总计 | 1587.6ms | 1417.8ms | **-10.7%** |

根因是两处元组/列表构造浪费：

1. **`heap_bits` 无条件分配**：`pa_list_append` 在 `len==0` 时无条件
   `list_bits_reserve`，但位图只在 `elem_kind == 2`（unknown 元素）时被读。
   `tuple[int, int]` 是 kind=0，那份位图从头到尾没人看。
2. **元组构造要 N+1 次函数调用**：`tuple.new{a,b}` 编译成 `pa_list_new` +
   2 次 `pa_list_append`。新增 `pa_list_new_filled`，一次调用填完，
   并跳过位图。

两处都是**纯实现问题，不涉及语言语义**。