# 运行时

运行时按职责拆成 `runtime/rt_*.c`（引用计数/GC、异常、生成器、异步、Task/Future、fd、文件、字符串/列表/字典/切片、类/复数、math/格式化、random/os/subprocess/time、itertools/collections、json、re、调试表/traceback），共享接口在 `runtime/rt_internal.h`；各部分编成可重定位对象后用 `ld -r` 合成 `rt.o`，由 `pyadamas build` 链接进每个生成的程序，只依赖 libc 和 libm。**PyAdamas 编译器本身不链接它**（`PYADAMAS_RT_OBJ` 只是把路径编进二进制，供 `cc` 使用）。

## ABI 约定

运行时函数不是给用户调的，是给生成代码调的。约定只有三条：

1. 所有参数和返回值用 `int64_t` 或 `double` 传，指针就当 `int64_t` 用。没有按值传结构体。
2. 参数个数固定，不检查类型——类型在编译期已经确定，运行时不再打标签。
3. 出错就 `exit(1)`，不返回错误码，不抛异常。

因为生成代码把每个值都放在 8 字节栈槽里，`int64_t` 和指针天然同构；浮点用 `double` 走 xmm 寄存器，由 SysV 调用约定保证。

## 对象布局

```c
typedef struct PyAdamasRc {
    struct PyAdamasRc* next;    /* GC 注册表链 */
    struct PyAdamasRc* prev;
    int64_t rc;             /* INT64_MIN = 永生（字面量） */
    void (*dtor)(void*);    /* rc 归零：正常释放路径 */
    void (*release)(void*); /* GC 清扫：只减掉指向可达对象的引用 */
    int64_t size;           /* 负载字节数，GC 扫描范围 */
    int64_t kind;           /* STR=0 / LIST=1 / OBJ=2 / CLOSURE=3 / DICT=4 / INSTANCE=5 / RANGE=6 / STRUCT=7 */
    int64_t marked;         /* 标记-清扫用；低位是状态标志：MARK=1 / UNTRACKED=2 / YOUNG=4 */
} PyAdamasRc;                   /* 64 字节，位于负载之前，对象指针不指向它 */

/* pub：str 是码点数、bytes 是字节数；len 恒为 UTF-8 字节数。
   hash：<=7 字节时是 SSO 字符区，否则不用；chash：字典哈希缓存（0 = 未算），
   字面量在编译期填好。两者分开是因为 SSO 短串也需要哈希缓存。 */
typedef struct { int64_t pub; int64_t len; int64_t hash; const char* data; int64_t chash; } PyAdamasStr;
typedef struct {
    int64_t len; int64_t cap; int64_t* data; int64_t elem_kind;
    uint64_t* heap_bits;   /* kind 2 的逐元素「是否堆指针」位图；NULL = 不可跟踪 */
    int64_t bits_cap;      /* 位图字数（cap 会被缓冲池抬高，不能复用） */
} PyAdamasList;            /* 48 字节 */
typedef struct {
    int64_t hash; int64_t key; int64_t val;
    int32_t prev, next;            /* 插入顺序双向链表（entry 下标，-1 结束） */
} PyAdamasDictEntry;
typedef struct {
    int64_t len; int64_t cap;      /* len 在偏移 0，len()/真值判断直接读 */
    PyAdamasDictEntry* entries;        /* 条目数组；存活项由 prev/next 串成插入顺序 */
    int32_t* order;                /* 链表快照：order[i] = 第 i 个存活项下标（懒重建） */
    int32_t* index;                /* 开放寻址索引：entry 下标 + 1，0 为空，-1 墓碑 */
    int64_t icap;                  /* 索引表容量（2 的幂） */
    int64_t ord_len;               /* order 有效长度；!= len 表示需重建 */
    int64_t nent;                  /* entries 高水位（含墓碑） */
    int64_t itomb;                 /* 索引表墓碑数 */
    int64_t free_head;             /* 墓碑空闲链，-1 结束 */
    int32_t head, tail;            /* 链表头/尾 */
    int64_t key_kind;              /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;               /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;
    int64_t hash_slot, eq_slot;    /* 类 key 的 __hash__/__eq__ vtable 槽；-1 内建 */
    int64_t default_factory;       /* collections.defaultdict 工厂；0 普通 dict */
} PyAdamasDict;
/* 用户对象没有对应的 C struct：布局由编译器按类定义生成。
   负载 +0 是 vtable 指针，字段从 +8 起，每个 8 字节；隐藏的 PyAdamasRc
   头在负载前面，所以既有偏移一个都没变。 */
```

分配统一走 `rc_alloc(size, dtor, release, kind)`，返回负载指针；
`pa_rc_inc` / `pa_rc_dec` 从负载指针往前 64 字节找到头（`h->rc` 因此落在
`p - 48`）。`rc_dec` 归零时先调 `dtor`，再释放整块内存。`rc_alloc` 把
对象挂进 GC 注册表，但**不是所有堆对象都在链上**：`rc_alloc_untracked` 照常
分配后 `rc_unlink` 并置 `marked = PYADAMAS_RC_UNTRACKED`，这类对象 GC 永远
看不到、只能由 RC 精确释放（见「列表」的全非堆元组和「循环收集器」的不变量）。

### 字符串

**对外长度 `pub` 放在偏移 0**，这是刻意的：`len(s)` 编译成一条 `movq (%rax), %rax`，
`if s:` 也只是读这个字段，对 str/bytes/容器通用——改成码点语义后这两条指令一行都
不用改，`len(unknown)` 也自动正确。`pub` 对 `str` 是码点数、对 `bytes` 是字节数；
`len`（偏移 8）恒为 UTF-8 字节数。不变量：str 满足 `0 <= pub <= len`（等号当且仅当
全 ASCII），bytes 满足 `pub == len`。

`hash` 只服务 SSO：≤7 字节的短串把字符内联在这 8 字节里（留 1 字节 NUL），
`data` 指向 `&hash`，长串 `data` 指向结构体后面的字符区。字典哈希缓存在
偏移 32 的 `chash`（FNV-1a + mix64，`0` = 未算）——SSO 短串和长串都能缓存。
`data` 在偏移 24（`pub/len/hash` 之后的第四个机器字）。

字符串有两种来源，负载布局完全一致，运行时函数不需要区分：

| 来源 | 分配方式 | 生命周期 |
|---|---|---|
| 字面量 | codegen 在 `.data.rel.ro` 里静态构造，前置 `rc = INT64_MIN` | 与进程同寿，inc/dec 直接跳过 |
| 拼接 / 转换 | `rc_alloc(sizeof(PyAdamasStr) + len + 1, NULL, NULL, PYADAMAS_RC_STR)`，字符区紧跟结构体；≤7 字节只分配结构体本身，字符走 SSO | 引用计数归零时释放 |

堆分配的字符串尾部补 `\0`，方便调试时直接当 C 字符串看；但长度以 `len` 为准，内嵌 `\0` 是合法的。

<=128 字节的字符串走 freelist：按 8 字节分 16 档；小档（<=40 字节，覆盖
内联串和短堆串）每档最多缓存 512 块，其余档 64 块——`text.split()` 这类场景
全是内联短串，按 64 个的容量实测 64% 的放入直接溢出去 `free` 了。RC 归零时
不 `free`，而是回到同档池里，下一次同档分配直接复用。池块在进程退出时仍然
持有（静态根，不触发泄漏报告），总量由「16 档 × 每档上限」封顶（小档
512 块 × 5 档 + 其余档 64 块 × 11 档，百 KB 量级）。

<=7 字节的短串把字符内联在 `PyAdamasStr::hash` 的 8 字节里（留 1 字节 NUL），
`data` 指向这段内联区；长串仍用结构体后面的字符区。值仍然是裸指针，对象
本身仍走 RC（exact-RC 模式下串是 untracked leaf，不进 GC 链），所以这不是
tagged-value 级别的 SSO，只是省掉短串的尾部字符区。

串的哈希在第一次被字典使用时算好写进 `chash`（所有长度都能缓存，SSO
短串也一样）；**字面量的哈希在编译期算好**直接写进静态对象——`.data.rel.ro`
在 PIE + RELRO 下是只读的，运行时不能在上面做懒缓存。因此 `d["长键"]` 这类
重复查找不再每次重扫整串。

单码点字符串（`s[i]`、`for c in s`、`chr()`）和 `"True"/"False"/"None"`
的转换结果是**静态永生对象**：`char_tab[256]` 一次性初始化好全部 256 个槽位
（索引开启时同时登记进 RC 索引），其中 0x00–0x7F 是合法单码点 UTF-8
（`pub = 1`），取 ASCII 字符
不再分配；0x80–0xFF 的单字节序列不是合法 UTF-8，这些槽位 `pub = 0`，只是给
`pa_char_str` 的低 8 位调用留一个确定对象，str 路径不会取到。非 ASCII 码点走
`pa_cp_str`，现编 1–4 字节（CPython 对 >U+00FF 也是新对象）。

**驻留表也导出给生成代码。** `PyAdamasStr* pa_char_tab[256]` 是上表 256 个载荷
指针，由 `pa_static_strs_init()` 填好；生成的 `main` 在序言里调它（顺序在
`pa_register_literals` 之后：驻留对象要不要登记进 RC 索引取决于是否走
exact-RC，必须等索引开关定下来再初始化）。于是单字节 ASCII 的 str 字面量直接
生成 `movq pa_char_tab+N(%rip), %rax`，和 `s[i]` 返回的是**同一个指针**——
`s[i] == "0"` 这类逐字符比较退化成一次 `cmpq`，连长度检查和字节比较都不用
做（生成代码里 `str ==` / `!=` 先比指针，不等才 `call pa_str_eq`；
`pa_str_eq` 自己第一行也是 `a == b`）。bytes 字面量不参与（`pub` 语义不同）。
`pa_char_str` 收的参数是**字符编码**不是数值：`str(7)` 要传 `'7'`（0x37），
传 7 会拿到 BEL 控制字符。

字符串不可变，共享对象与 CPython 的行为一致；`s[:]`、无变化的 `strip()`、
`''.join([x])` 也按 CPython 的做法直接返回原串（inc 一次）。

构造 API 收敛成：`str_new`（默认，扫描算码点数）、`str_new_bytes`（bytes，`pub=len`）、
`str_new_ascii`（已知全 ASCII）、`str_alloc_raw` + `str_seal`/`str_seal_n`（先分配后
填充，漏 seal 会留下 `pub == -1` 哨兵）。字面量的 `pub` 在**编译期**算好写进
`.data.rel.ro`——RELRO 只读，运行期不能懒缓存。

字符串迭代有**顺序游标**（`pa_str_char_at_cursor`）：降级器为 `for ch in s` /
`enumerate` / `zip` / 推导式 / 生成器各分配一对 int64 槽，命中「紧接着上一个码点」
时 O(1)，否则退回从头扫。没有游标时 `s[i]` 是 O(i)，长串顺序迭代会退化成 O(n²)。
全 ASCII 串走快路径（码点下标即字节偏移），成本与改动前一致。

### 列表

`cap` 是已分配容量，`len` 是有效元素数，`data` 是 8 字节槽数组。元素统一占 8 字节：整数直接放，浮点放位模式（`memcpy` 转换），指针放地址。**没有运行时类型标签**——取值时按什么类型解释由编译期决定。

`PyAdamasList` 结构体（48 字节，多了 `heap_bits`/`bits_cap`）和 <=512 字节的
`data` 缓冲也走 freelist：结构体最多缓存 64 个；`data` 按 64 字节分 8 档，
每档最多 64 块。复用时 `cap` 按档容量对齐（只会变大），销毁时先 dec 元素再
回收缓冲。

**小列表合并分配**：容量 <=8 元素的列表（含定长构造快路径 `pa_list_new_filled` /
`pa_tuple_new_filled`）把 `data` 缓冲内联在「RC 头 + 结构体」同一个 malloc 块
尾部（`list_inline_pool`，64+48+64=176 字节，最多缓存 64 块），建/销各只需一次
池操作。内联缓冲是块内指针，**不能 realloc / free**：append / insert / 切片赋值
增长越过 8 时由 `list_buf_grow` 换 malloc + memcpy，内联区从此闲置；销毁时按
`data == list_inline_at(l)` 判断是否还外部缓冲，头块按 `h->size`（48+64）整块
挂回内联池，下次复用重新启用内联缓冲。GC 清扫路径（`gc_free_body`）同样不能
单独 free 块内的 `data` 指针。

`elem_kind` 是元素所有权类别，由降级器按容器静态类型算好：

| kind | 含义 | 写入时 | 覆盖 / 截断 / 销毁时 |
|---|---|---|---|
| 0 | 全部非堆类型 | 不 retain | 不 release |
| 1 | 全部堆类型 | 生成代码 retain | 运行时 release |
| 2 | unknown 或混合 | 堆→`rc_inc`，unknown→`rc_inc_unknown` | 覆盖/截断走注册表；销毁 / GC 清扫有位图时按位图判，否则 `pa_rc_dec_unknown`（整数是空操作） |
| 3 | 未定（只可能是空容器） | 第一次写入时定型并 retain | 沿用定型后的种类 |

`pa_list_new(cap, elem_kind)` 最小容量 4；请求容量 <=8 走合并分配（实际
容量 8），`append` 满了就翻倍。
`pa_list_set` / `pa_list_set_len` / `del` / 销毁都按 `elem_kind` 释放旧元素；
kind 2 的元素释放要么查全局 RC 索引（`pa_rc_dec_unknown`：先确认是堆指针
再 dec，所以整数和「看起来像指针」的字面量都不会被误减），要么走**逐元素
堆位图**。

位图 `heap_bits` 是方案 A：元素「是不是堆指针」在**写入那一刻**就知道（写入点
的静态类型），没必要事后到全局索引里反查。生成代码给每个元素多传一个
`heap` 标记（0 非堆 / 1 堆 / 2 unknown），`pa_list_append` 据此置位；一旦遇到
unknown（`heap == 2`）就把整个位图丢掉（`heap_bits = NULL`），退回索引反查的
旧路径——正确性优先。位图只给 kind 2 分配：kind 0/1 的销毁路径从不读它，
无条件分配等于给 `tuple[int, int]` 每次创建多一次 `realloc` + 清零。

目标容器原本是 kind 1、但接入了 unknown/混合元素时，运行期把 `elem_kind`
提升为 2。

kind 3 只出现在空容器上：`pa_list_prep(l, v, vk, owned)` 在第一次写入前
定型——`vk` 是值的静态种类（0/1/2），unknown 值查一次 RC 索引判断是不是
堆指针；静态种类和现有 kind 冲突时降级为 2。生成代码只在容器未定型、
种类冲突或值是 unknown 时调用它（`store_list_elem`），其余写入仍按静态
种类直接 retain。

**全非堆元组不进 GC 堆链。** 元组不可变，`elem_kind == 0` 表示元素全是非堆
值——对象没有任何出边，不可能处在引用环里，GC 完全不必看到它。codegen 对
元素个数 <=4 且全是整型机器字的 `tuple.new{a, b}` 走 `pa_tuple_new_filled`
（`n, elem_kind, a, b, c, d` 一次调用）：`elem_kind == 0` 时用
`rc_alloc_untracked` 分配——省掉 `rc_link` / `rc_unlink`、新生代登记和 GC
分配计数，RC 归零时照常由 `list_destroy` 释放 `data` 缓冲。元素全是堆引用
的元组（`elem_kind == 1`）仍走普通 `rc_alloc`（同形的 `pa_list_new_filled`），
因为出边必须被 GC 看见。实测 `bench/realistic/queue_sim.py` 每次
`simulate` 分配约 66 个 `tuple[int, int]`，`rc_link`/`rc_unlink` 合计占
profile 的 7.9%；这个提交让 `sort_struct` 快 11.9%。

### 字典 / 集合

字典是**开放寻址哈希表 + 插入顺序双向链表**：

- 存活条目用 `prev`/`next` 串成插入顺序的双向链表（`head`/`tail`），
  `index` 是 int32 开放寻址表（entry 下标 + 1，0 为空槽，-1 为墓碑），
  查找先走索引；按序号遍历（`for k in d`）走 `order` 快照；
- `order` 是链表的下标快照，删除后失效（`ord_len = -1`），下一次
  `pa_dict_key_at` 时从链表重建；顺序遍历本身是 O(n)，这层重建不改变
  摊还代价，却让按序访问保持 O(1)；
- key 的哈希/判等类别放在 `key_kind`：int/bool 机器字、str 逐字节
  （FNV-1a）、float IEEE 值（`0.0` / `-0.0` 同键）、其他按机器字；
- `key_own` / `val_own` 决定 RC：写入前生成代码 retain 一份，更新/销毁
  时运行时释放；kind 2（unknown）释放走 `pa_rc_dec_unknown`，整数是空操作；
- 更新已有 key 时保留原来的 key 对象，释放调用方新传入的 key 引用；
- **删除是摊还 O(1)**：链表摘链，entry 原地变墓碑（`hash = 0`）挂进
  `free_head` 空闲链，索引槽置 -1；不 memmove、不整表重建，插入时优先
  复用墓碑。`pop` / `popitem` / `remove` / `discard` 都走这条路径，
  `popitem` 取链表尾（LIFO）；
- 索引装载按「存活 + 墓碑」算，超过 2/3 时重建（存活项过多则翻倍）；
  entries 不搬家，遍历顺序稳定。
- set 就是 `val_own = 0`、值恒 0 的 dict，复用同一套函数和 GC。
- `default_factory` 非 0 时是 `collections.defaultdict`：`d[k]` 缺失会按工厂
  造值、插入并返回；`get` / `in` 不触发。工厂只编码内置零参类型
  （int/float/bool/str/list/dict/set/tuple），任意 callable 需要运行期
  闭包调用协议，尚未支持；`dict_copy` 会继承工厂。
- 方法：`get` 走 get/get_or；`keys`/`values`/`items` 分配新 list（items
  的元素是二元组），元素按目标 list 的 kind retain；`update` 逐项复制，
  目标的 own 类别向源靠拢（源 unknown 则目标升级为 2），给每个键值补一份
  引用，避免源临时字典释放后悬空；`copy` 复制 own 类别后逐项 retain；
  `set.add/update` 复用 dict_set/update；`set.pop` 取最后一个插入项。

### 切片

`pa_list_slice` / `pa_str_slice` 接收 `(容器, start, stop, step)`，
缺省边界用 `INT64_MIN` 哨兵，运行时按 Python `slice.indices` 归一化：

- 正/负步长、负索引、越界夹取；`step == 0` 抛 ValueError；
- list/tuple 结果新建同 `elem_kind` 的列表，kind 1/2 元素按源类别逐个 retain；
- str 结果新建字节串，按字节复制；
- 结果是新堆对象，正常走 RC（exact-RC 模式下 range/str/struct 这类叶子不进 GC 链）。

`pa_list_slice_assign(dst, start, stop, step, src)` 处理写切片：
`step == 1` 时用 `memmove` 支持长度变化（替换 / 插入 / 删除），扩展切片
要求 `src->len` 与切片长度一致；写入时按来源种类 retain 新元素、
按 `dst->elem_kind` 释放被覆盖的旧元素（源是 kind 2 时目标提升为 2），
`src == dst` 先做快照。`pa_list_extend(dst, src)` 用于 `*args` 打包和
拷贝：按来源种类 retain，目标接入了 kind 2 源时同样提升自己的 kind。

### range

```c
typedef struct { int64_t start; int64_t stop; int64_t step; int64_t len; } PyAdamasRange;
```

`pa_range_new` 只分配这 32 字节负载，元素按 `start + i * step` 现算；`len`
在构造时算好，`len()` / 下标 / `in` / 归约都不物化。`list(r)` / `tuple(r)`
才建列表。range 是不可变堆对象，没有堆字段（出边为零），exact-RC 模式下按
`rc_leaf_untracked` 当叶子处理：不进 GC 链，只由 RC 释放。

### 用户对象

对象是编译器按类定义排出来的连续内存，运行时只知道「多大」和析构器：

```c
void* pa_obj_new(int64_t size, void (*dtor)(void*), void (*gc_release)(void*));
```

调用方（生成代码）紧接着把 vtable 地址写到偏移 0。分配全零：0 同时是
`0` / `None` / 假，和栈槽「未赋值就是 None」的约定一致。

析构器由 codegen 按类布局生成，只 `rc_dec` 堆类型字段；没有堆字段的类传
`NULL`。子类析构器包含父类字段，顺序与对象布局一致。

vtable 是静态数据，在 `.data.rel.ro` 里，由 codegen 按 `IRClass::vtable` 生成。
方法槽在模块内按名字全局分配，所有类的 vtable 等长，同名覆盖写同一个槽。
vtable 前固定 5 个字：MRO 元数据指针表、类编号、vtable 自身、本类声明的槽表、
模块限定类名。`pa_isinstance` / `pa_is_named_class` 先查 RC 注册表确认是
`PYADAMAS_RC_INSTANCE`，再沿 MRO 判断；`pa_super_lookup` 用实例真实 MRO 找
after 类之后第一个声明该槽的类，供零参 `super().m()` 动态分派。

### struct 值类型

`struct` 也是 `[RC 头][payload]`，payload 按 C 布局（字段偏移与宽度由声明决定），
分配全零。字段都是 C 数据，不持有 PyAdamas 堆引用，所以析构器为空；
赋值/传参/返回是整块 `memcpy`（`pa_struct_copy` 分配新副本），不是共享。

### 闭包

闭包负载是 `[code, n_caps, cell0, cell1…]`：

```c
void* pa_closure_new(int64_t code, int64_t n_caps);
```

- `code` 是静态代码地址，不是堆指针，析构时不 dec；
- `n_caps` 后面的每个 8 字节槽是一个 cell 指针；
- 析构器逐个 `rc_dec` cell，然后由 RC 头统一释放；
- 无捕获函数的闭包在 `.data.rel.ro` 里静态构造，rc = `INT64_MIN`。

cell 不是新的运行时类型，就是一个**单元素列表** `list[T]`：读 `l[0]`、
写 `l[0] = v`，元素所有权和析构完全复用列表规则。这样闭包不需要第二套
引用计数实现。

### 生成器

生成器帧是普通堆对象（`PYADAMAS_RC_OBJ`），负载布局：

```
[state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot0..]
```

- `resume` 是状态机代码地址，`gen_run` 用 `void (*)(void*, int64_t)` 调用；
- `status`：0 未开始/挂起，1 已完成（槽位已释放），2 异常逃出（槽位已在
  展开时释放），3 awaiting，4 已排队；
- `kind`：0 生成器，`PYADAMAS_CORO_MAGIC` 协程，`PYADAMAS_AGEN_MAGIC` 异步生成器；
- `waiter` 是等待本对象的协程/gather/task（持有一个引用）；
- `exc` 是待注入的 `PyAdamasException*`；
- `cur` 是当前等待的 awaitable（借用观察指针，取消时用来解绑）；
- `out` 保存 yield/await 值或协程返回值，运行时读走后清零。

```c
void* pa_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind);
int64_t pa_gen_next(void*);                  // 结束抛 StopIteration
int64_t pa_gen_next_opt(void*, int64_t* ok); // 结束写 ok=0
int64_t pa_gen_send(void*, int64_t sent);
int64_t pa_gen_send_opt(void*, int64_t sent, int64_t* ok); // yield from
int64_t pa_gen_throw(void*, PyAdamasExcValue*);
int64_t pa_gen_throw_opt(void*, int64_t exc, int64_t* ok); // yield from 转发
void    pa_gen_close(void*);
```

`gen_run` 用一层 C 级 handler 兜住逃出 resume 的异常：标记 `status=2`
后重新转抛，避免生成器对象销毁时把已经跑过的 `finally`/`__exit__`
再跑一遍；转抛前调用生成的 `gcrel`（`pa_gen_release_slots`）把帧槽
逐槽 `dec` 并清零，所以只要对象还被引用着，槽里的值也不会滞留。
`pa_gen_close` 的 finally 抛异常、`coro_step` / `agen_drive` 的异常
逃出路径同样处理。析构器（`gen_destroy`）释放帧里挂着的异常/等待者
引用，再调用 `pa_gen_finalize`：按挂起点快照执行清理并释放槽位，
清理期间抛出的异常被吞掉。`gcrel` 与 GC 清扫共用（幂等）：清扫不执行
Python 代码，逐槽读到非空值就 `dec` + 清零。

跨挂起的 handler 记录：yield/await 前 `pa_handler_suspend` 把它从全局栈
摘下（不 free），恢复时 `pa_handler_rearm` 重新压栈并记录临时值
水位，生成代码随后重新 `setjmp`；close 路径用 `pa_handler_free`
回收这些记录。

跨挂起的异常上下文：resume 入口 `pa_exc_enter` 把调用方的 `current_exc`
与 `exc_save_top` 压进运行时节点栈（两边都空时不压），再装上帧槽里
保存的那份；挂起/正常退出时 `pa_exc_leave` 把本帧的写回帧槽、换回
调用方那份。异常从 resume 逃出时，`gen_run` / `coro_step` /
`agen_drive` 按进入时记录的深度 `pa_exc_unwind` 退栈，避免 except 体
里的挂起把调用方的异常上下文带跑。

### 协程与事件循环

`async def` 复用生成器帧，`coro_step` 每次恢复后按 status 处理：

- `1`：把 `out` 里的返回值交给 waiter（或记为 `asyncio.run` 的结果）；
- `3`：`out` 是 awaitable，交给 `await_from_coro` 挂到事件循环；
- `2`：异常交给 waiter；等待者下次恢复时走注入分发，在帧内重新抛出。

awaitable 有六类：协程帧、`PyAdamasAwait` 的 sleep(1)/gather(2)/task(3)、
异步生成器驱动(4)、Future(5)、fd 等待(6)。协程/异步生成器/fd 只有一个
`waiter`；Future 和 Task 用 `waiters` 列表支持多个等待者，Future 另用
`callbacks` 列表保存
`add_done_callback` 注册的函数（完成时按注册顺序同步调用）；异步生成器
驱动对象用 `driver_sent`（asend 的值，跨事件循环持有引用）和
`driver_close`（aclose 模式）标记；内部 awaitable 完成时把结果写进帧槽
并置 `driver_await_result`，下一轮推进不再用 `driver_sent` 覆盖它；gather 把自己的引用挂在每个
子 awaitable 上，最后一个子任务完成时汇总结果。task 由 `loop_pending`
持有到完成，保存结果/异常供后续 await；`cancel()` 沿 `cur` 向下级联
（协程链/task/gather/future/sleep/fd 各有取消路径），再注入
`CancelledError`，各层 `finally` 照常执行。

异步生成器的驱动对象挂在就绪队列上一步步推进（不递归调用 resume 栈）：
拉起生成器 → 遇到 `yield` 就完成驱动对象并把值交给等待者；遇到
`await` 则把驱动对象挂到内部 awaitable 上，等它完成后把结果写回帧的
`sent`/`exc` 槽再重新入队。生成器结束抛 `StopAsyncIteration`，
`async for` 在自己的 handler 里捕获。`asyncio.run` 的循环：

```
就绪队列非空 → 取一个协程 step
否则有定时器 → 触发到期项；没有到期项就 nanosleep 到最近 deadline
两者都空且根协程未完成 → RuntimeError("event loop stalled")
```

就绪队列、定时器表、`loop_pending` 和 `loop_fds` 都是 `list` 对象
并注册为 GC 根；`sleep` 用 `CLOCK_MONOTONIC` 毫秒定时。没有就绪任务时，
有 fd 等待就调用 `poll`（超时取最近定时器），否则 `nanosleep` 到
最近 deadline。根协程的异常在清空事件循环状态后设成 pending 并
longjmp 回同步调用点。GC 清扫和进程退出不跑析构器，但会单独释放挂在
帧 / Task / Future 上的 malloc 异常对象。

### 异常

异常不参与 RC/GC：handler 记录和异常对象都是 `malloc` 的普通内存。

- **handler 记录**：`{jmp_buf env; PyAdamasHandler* prev; int64_t mark; PyAdamasFrame* frame}`，
  `env` 必须在开头，生成代码把记录指针直接交给 `setjmp`。全局
  `handler_top` 维护 LIFO 链；`mark` 是压入时刻的临时值栈水位，
  `frame` 是压入时刻的帧栈顶。
- **帧记录**：普通函数在序言压一条 `PyAdamasFrame {prev; n; void* slots[]}`
  （槽地址数组），正常返回前弹栈，全局 `pa_frame_top` 串成链。
  `raise` 的 `exc_longjmp` 先调 `pa_frame_unwind(handler_top->frame)`
  沿链释放被跳过帧里持有引用的堆槽（先清空再 `rc_dec`，可重入），
  再 `longjmp`。生成器/协程的槽在堆帧里，由帧销毁负责，不压记录。
- **异常对象**：`{char* type; char* message}`，只保存内置异常名和消息。
  `pending_exc` 是待处理异常，`current_exc` 是当前 `except` 体正在处理
  的异常；`pa_exc_accept` / `pa_exc_done` 维护这层上下文。
- **匹配**：`pa_exc_matches` 按内置异常继承表（KeyError → LookupError
  → Exception 等）判断；未知名字默认按 Exception 子类处理。
- **展开**：landing pad 先 `pa_handler_leave` 弹出记录并
  `pa_rc_cleanup(mark)`，再分派。被跳过函数帧的局部引用已在
  `pa_frame_unwind` 里释放，不再依赖保守 GC 兜底。
- **未捕获**：打印 `PyAdamas: uncaught <type>: <message>` 后 `exit(1)`。

## 函数表

`RtFn` 枚举与 `rt_name()` 都在 `src/ir_ops.h`（`ir.h` 只是把它 include 进来）。生成代码按枚举直接 `call pa_xxx`，不做符号查找。

### 列表

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_list_new` | `(i64 cap, i64 elem_kind) -> PyAdamasList*` | 建空表，容量至少 4；kind 3 表示元素种类未定，kind 2 的列表在第一次 append 时分配 `heap_bits` |
| `pa_list_new_filled` | `(i64 n, i64 elem_kind, i64 a, i64 b, i64 c, i64 d) -> PyAdamasList*` | 一次调用构造 n <= 4 的定长列表（元素已按值展开在寄存器里）；不建位图 |
| `pa_tuple_new_filled` | 同上 | 同形，但 `elem_kind == 0` 时走 `rc_alloc_untracked`，不进 GC 堆链 |
| `pa_list_append` | `(PyAdamasList*, i64 v, i64 heap) -> void` | 追加，满则翻倍 `realloc`；`heap` 是元素级堆标记（0 非堆 / 1 堆 / 2 unknown），用来维护 `heap_bits`；不隐式 retain |
| `pa_list_prep` | `(PyAdamasList*, i64 v, i64 vk, i64 owned) -> void` | 空表首次写入前定型并 retain（`owned` 非 0 则只定型） |
| `pa_list_get` | `(PyAdamasList*, i64) -> i64` | 负索引先加 `len`；越界抛 IndexError；返回借用。生成代码内联快路径，只有越界才进来 |
| `pa_list_set` | `(PyAdamasList*, i64, i64) -> void` | 同上，写入；按 elem_kind 释放旧元素（kind 2 走注册表，覆盖路径不读 `heap_bits`）。生成代码内联快路径，运行期 elem_kind 决定是否释放 |
| `pa_list_set_len` | `(PyAdamasList*, i64) -> void` | 改 `len`；按 elem_kind 释放被截掉的元素 |
| `pa_list_extend` | `(PyAdamasList* dst, PyAdamasList* src) -> void` | 追加 src 全部元素；按来源种类 retain，dst 原本是 kind 1 而源是 kind 2 时提升为 2，`src == dst` 先快照 |
| `pa_list_repeat` | `(PyAdamasList*, i64) -> PyAdamasList*` | n 次重复；一次分配后按 `elem_kind` retain 追加，负次数返回空表 |
| `pa_list_sum_i64` | `(PyAdamasList*, i64 start) -> i64` | O2 归约；AVX2 4×int64 累加（按 uint64 回绕），否则标量 |
| `pa_list_sum_f64` | `(PyAdamasList*, f64 start) -> f64` | `--fast-math` + O2 时 AVX2 4 路 double 分组求和；否则标量顺序 |
| `pa_list_min_i64` / `max_i64` | `(PyAdamasList*) -> i64` | O2 归约；AVX2 compare+blend，空表抛 ValueError |
| `pa_list_slice_assign` | `(PyAdamasList*, i64, i64, i64, PyAdamasList*) -> void` | 切片赋值；step 1 支持长度变化 |
| `pa_str_to_list` | `(PyAdamasStr*) -> PyAdamasList*` | 按字节拆成 list[str]，每字符新串 |
| `pa_bytes_to_list` | `(PyAdamasStr*) -> PyAdamasList*` | 按字节拆成 list[int]（0..255），`for c in b"..."` 用 |
| `pa_range_new` | `(i64 start, i64 stop, i64 step) -> PyAdamasRange*` | 分配惰性 range（只存边界与 len）；step 0 抛 ValueError |
| `pa_range_len` / `pa_range_get` / `pa_range_contains` | `(PyAdamasRange*[, i64]) -> i64` | `len` / 下标 / `in`，越界 panic |
| `pa_range_sum_i64` / `sum_f64` / `min_i64` / `max_i64` / `any` | `(PyAdamasRange*, i64 start) -> i64` 或 `(PyAdamasRange*) -> i64/f64` | 归约，不物化 |
| `pa_range_first` / `pa_range_step` / `pa_range_stop` / `pa_range_eq` | `(PyAdamasRange*) -> i64` | 迭代边界与相等比较 |

### 字典 / 集合

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_dict_new` | `(i64 key_kind, i64 key_own, i64 val_own, i64 hash_slot, i64 eq_slot) -> PyAdamasDict*` | 空表，初始 8 槽；set 传 `val_own = 0`。`hash_slot`/`eq_slot` ≥ 0 时类键的哈希/相等走 vtable（`__hash__`/`__eq__`），-1 表示按 `key_kind` 走内建哈希 |
| `pa_dict_set` | `(PyAdamasDict*, i64 key, i64 val) -> void` | 插入/更新；更新时释放新 key 引用并 dec 旧 value |
| `pa_dict_get` | `(PyAdamasDict*, i64 key) -> i64` | 缺失抛 KeyError；defaultdict 设了工厂时先造默认值、插入再返回。返回借用 |
| `pa_dict_set_default_factory` | `(PyAdamasDict*, i64 factory) -> void` | 设置 defaultdict 的内置工厂编码（0 = 普通 dict） |
| `pa_dict_contains` | `(PyAdamasDict*, i64 key) -> i64` | `in` 用，返回 0/1 |
| `pa_dict_key_at` | `(PyAdamasDict*, i64 index) -> i64` | 按插入顺序取 key，遍历用（order 快照失效时先重建） |
| `pa_dict_val_at` | `(PyAdamasDict*, i64 index) -> i64` | 按插入顺序取 value，遍历用 |
| `pa_dict_get_or` | `(PyAdamasDict*, i64 key, i64 def) -> i64` | 缺失返回默认值 |
| `pa_dict_keys` | `(PyAdamasDict*) -> PyAdamasList*` | 新 list，按插入顺序装 key |
| `pa_dict_values` | `(PyAdamasDict*) -> PyAdamasList*` | 新 list，按插入顺序装 value |
| `pa_dict_items` | `(PyAdamasDict*) -> PyAdamasList*` | 新 list，元素是 `(key, value)` 二元组 |
| `pa_dict_update` | `(PyAdamasDict*, PyAdamasDict*) -> void` | 逐项复制；先 retain，再 dict_set |
| `pa_dict_copy` | `(PyAdamasDict*) -> PyAdamasDict*` | 同所有权类别的浅拷贝（match 的 `**rest`） |
| `pa_dict_pop` | `(PyAdamasDict*, i64 key, i64 has_default, i64 def) -> i64` | 删除并返回值；缺失且有默认值返回默认值，否则抛 KeyError |
| `pa_dict_popitem` | `(PyAdamasDict*) -> PyAdamasList*` | 删除最后插入项，返回 `(key, value)` 二元组 |
| `pa_dict_pop_last` | `(PyAdamasDict*) -> i64` | 删除最后插入项并返回 key（set.pop） |
| `pa_dict_remove` | `(PyAdamasDict*, i64 key, i64 missing_ok) -> void` | remove/discard；缺失时抛 KeyError 或按 missing_ok 忽略 |
| `pa_dict_clear` | `(PyAdamasDict*) -> void` | 释放全部 key/value 引用并清空表 |
| `pa_list_contains` | `(PyAdamasList*, i64 value, i64 cmp_kind) -> i64` | list/tuple 的 `in`；0 int / 1 str / 2 float / 3 raw |
| `pa_str_contains` | `(PyAdamasStr*, PyAdamasStr*) -> i64` | 子串查找 |
| `pa_list_slice` | `(PyAdamasList*, i64 start, i64 stop, i64 step) -> PyAdamasList*` | 新列表；kind 1 元素 retain |
| `pa_str_slice` | `(PyAdamasStr*, i64 start, i64 stop, i64 step) -> PyAdamasStr*` | 新字节串 |

### 模式匹配

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_isinstance` | `(void*, i64 class_id) -> i64` | unknown 值是不是该类或子类；查 RC 注册表，原始整数安全返回 0 |
| `pa_is_named_class` | `(void*, str* qualname) -> i64` | 按「模块.类」限定名沿 MRO 匹配，跨模块类模式用 |
| `pa_super_lookup` | `(void*, after_cid, slot, str* name) -> fnptr` | 沿实例真实 MRO 找 after 类之后的下一实现 |
| `pa_is_exc` | `(void*, str* type) -> i64` | unknown 值是不是异常值且类型匹配（含自定义异常父子关系） |
| `pa_is_seq` | `(void*) -> i64` | 是不是 list/tuple 载体（序列模式的前置检查） |
| `pa_is_dict` | `(void*) -> i64` | 是不是 dict/set 载体（映射模式的前置检查） |
| `pa_is_str` / `pa_is_complex` | `(void*) -> i64` | unknown 值上的 `str()` / `complex()` 类型模式 |
| `pa_str_eq_unknown` / `pa_complex_eq_unknown` | `(void*, lit*) -> i64` | unknown 值上的字符串/复数模式比较 |
| `pa_dict_copy` | `(PyAdamasDict*) -> PyAdamasDict*` | `**rest` 的浅拷贝，随后用 `dict_remove` 删掉已匹配 key |

静态字符串字面量 rc 为 `INT64_MIN`，不进 GC 堆链；模块入口先调用
`pa_register_literals` 把它们登记进 RC 索引，unknown 类型检查才能安全
确认「这个机器字确实指向 PyAdamasStr」。紧接着还要调
`pa_static_strs_init` 填好 256 项驻留表——单字节 ASCII 字面量在生成代码里
直接索引 `pa_char_tab`，函数体执行前这张表必须是有效的。

类模式的字段仍按编译期偏移直接 `field.get`：子类沿用父类字段前缀，所以模式类
自身的字段偏移对所有子类实例都成立。

### 异常

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_handler_push` | `() -> handler*` | 压入 handler 记录，保存临时栈水位与帧栈顶 |
| `pa_handler_leave` | `(handler*) -> void` | 弹出记录并 `rc_cleanup` 到水位 |
| `pa_handler_discard` | `(handler*) -> void` | 只弹出记录，不清临时值（提前退出用） |
| `pa_frame_unwind` | `(PyAdamasFrame* target) -> void` | 释放 target 之上所有帧记录的堆槽引用 |
| `pa_exc_matches` | `(str* type) -> i64` | 当前异常是否属于该类型（含继承） |
| `pa_exc_accept` | `() -> str*` | pending 转 current，返回消息新字符串 |
| `pa_exc_accept_value` | `() -> excval*` | pending 转 current，返回异常值（`except as e`） |
| `pa_exc_value_new` | `(str* type, str* msg) -> excval*` | 构造异常值，msg 为 NULL 表示无参数 |
| `pa_exc_value_from_exc` | `(PyAdamasException*) -> excval*` | 从控制流异常复制一个值对象 |
| `pa_exc_from_value` | `(excval*) -> PyAdamasException*` | 转成 malloc 的 pending 异常 |
| `pa_exc_value_args` | `(excval*) -> tuple*` | `e.args`（借用，args[0] 是消息） |
| `pa_exc_value_str` | `(excval*) -> str*` | `str(e)` / `print(e)` 的消息字符串 |
| `pa_raise_value` | `(excval*) -> void` | 抛异常值（noreturn） |
| `pa_raise_value_from` | `(excval*, excval*|0) -> void` | 抛异常值并设置 cause（noreturn） |
| `pa_exc_value_cause` | `(excval*) -> excval*` | `e.__cause__`，没有返回 NULL |
| `pa_exc_register` | `(str* child, str* parent) -> void` | 自定义异常类的父子关系注册 |
| `pa_exc_done` | `() -> void` | 释放 current，恢复上一层异常上下文 |
| `pa_exc_clear` | `() -> void` | 抑制并释放 pending |
| `pa_exc_reraise` | `() -> void` | 继续向外交互展开（noreturn） |
| `pa_exc_take_value` | `() -> excval*` | pending 复制成异常值并清空（finally 跨挂起保存待重抛异常） |
| `pa_exc_reraise_value_if` | `(excval*) -> void` | 非空时抛出该异常值，空则返回（finally 结束后的重抛） |
| `pa_exc_type_name` | `() -> str*` | pending 的类型名新字符串 |
| `pa_exc_message` | `() -> str*` | pending 的消息新字符串 |
| `pa_raise` | `(str* type, str* msg) -> void` | 设置 pending 并 longjmp（noreturn） |
| `pa_reraise` | `() -> void` | bare raise：重抛 current（noreturn） |
| `pa_raise_c` | `(const char*, const char*) -> void` | 运行时错误入口（noreturn） |

### 模块

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_import_module` | `(i64* ready, void (*init)(void)) -> void` | 惰性初始化：ready 为 0 时置 1 并调用模块 init |
| `pa_gc_add_roots` | `(void** roots, i64 n) -> void` | 追加根表；入口先 `gc_init`，被导入模块在 init 入口追加 |

模块全局是普通静态槽，由生成代码导出；`init` 是模块顶层生成的
`void(void)`，返回前不释放模块全局、不 shutdown。入口函数负责
`GcShutdown` / `RcShutdown`。

### 内置模块 math / sys

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_math_sqrt` 等 | `(double) -> double` | sqrt/fabs/sin/cos/tan/log/log2/log10/exp；域错误抛 ValueError |
| `pa_math_floor/ceil` | `(double) -> i64` | Python 的 floor/ceil 返回 int |
| `pa_math_pow/fmod` | `(double, double) -> double` | libm 包装；fmod 除零抛 ValueError |
| `pa_sys_set_args` | `(i64 argc, char** argv) -> void` | 生成 `main` 初始化 argv，列表立即可注册为 GC 根 |
| `pa_sys_argv` | `() -> list*` | 借用全局 argv 列表 |
| `pa_sys_exit` | `(i64 code) -> void` | 直接 `exit(code)`，noreturn |

### random / os / subprocess

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_random_new` | `(i64 seed) -> random*` | 分配 MT19937 状态；seed 为 `INT64_MIN` 时用时间 + pid 播种 |
| `pa_random_seed` | `(random*, i64 seed) -> void` | 按 CPython 的 seed(int, version=2) 重新播种 |
| `pa_random_next` | `(random*) -> f64` | `genrand_res53`，与 CPython `random()` 逐位一致 |
| `pa_random_randint` | `(random*, i64 a, i64 b) -> i64` | `a + _randbelow(b - a + 1)`；空区间抛 ValueError |
| `pa_random_choice` | `(random*, list*) -> i64` | 返回借用元素；空序列抛 IndexError |
| `pa_random_uniform` | `(random*, f64 a, f64 b) -> f64` | `a + (b - a) * random()` |
| `pa_random_shuffle` | `(random*, list*) -> void` | 原地 Fisher-Yates，与 CPython 交换顺序一致 |
| `pa_random_g*` | 同上但不带 random* | 模块级函数共用一把静态全局 RNG（不在 GC 链上） |
| `pa_os_mkdir/rmdir/remove/rename` | `(str*, ...) -> void` | 失败抛 OSError 子类（ENOENT→FileNotFoundError 等） |
| `pa_os_getcwd` | `() -> str*` | 新字符串 |
| `pa_os_cpu_count` | `() -> i64` | `_SC_NPROCESSORS_ONLN` |
| `pa_os_listdir` | `(str*) -> list*` | 新 list[str]，按 `os.listdir` 顺序 |
| `pa_os_getenv` | `(str* name, str* def) -> str*` | 缺失返回 `def`；编译器默认传空串 |
| `pa_os_path_join` | `(list*) -> str*` | posixpath.join 语义；参数是字符串列表 |
| `pa_os_path_exists/isdir/isfile` | `(str*) -> i64` | access/stat，返回 0/1 |
| `pa_os_path_basename/dirname/normpath/abspath/expanduser` | `(str*) -> str*` | posixpath 对应函数 |
| `pa_os_path_splitext` | `(str*) -> list*` | `(root, ext)` 二元组（list 表示） |
| `pa_subprocess_run` | `(list* argv, i64 capture, i64 text, f64 timeout, i64 check) -> proc*` | fork/execvp + 管道捕获；timeout < 0 表示不超时；超时 SIGKILL 后抛 TimeoutExpired；check 且 rc≠0 抛 CalledProcessError |
| `pa_subprocess_returncode` | `(proc*) -> i64` | 正常退出取退出码，被信号终止取负信号号 |
| `pa_subprocess_stdout/stderr` | `(proc*) -> str*` | 新字符串；未捕获时为空串 |

### time / file / itertools / collections / json

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_time_time` | `() -> f64` | `CLOCK_REALTIME`，秒 |
| `pa_time_monotonic` | `() -> f64` | `CLOCK_MONOTONIC`，秒 |
| `pa_time_perf_counter` | `() -> f64` | 单调高精度时钟，秒 |
| `pa_time_sleep` | `(f64) -> void` | `nanosleep`；负值按 0 |
| `pa_file_open` | `(str* path, i64 fd, str* mode, i64 buffering, i64 newline, i64 closefd) -> file*` | `fd >= 0` 时包装已有描述符；文本模式默认 UTF-8 |
| `pa_file_config` | `(file*, str* enc, str* errors) -> void` | 设置编码/错误策略 |
| `pa_file_read/readline/readlines` | `(file*, i64 n/hint) -> str*/list*` | 文本读取；`readlines` 返回 list[str] |
| `pa_file_write/writelines` | `(file*, str*/list*) -> i64/void` | 写入；返回写入字节数 |
| `pa_file_seek/tell/truncate` | `(file*, i64, ...) -> i64` | 定位/截断 |
| `pa_itertools_chain` | `(list*, list*, i64 elem_kind) -> list*` | 拼接两个 list；`islice` 在降级期展开，无运行时入口 |
| `pa_itertools_repeat` | `(i64 v, i64 n, i64 elem_kind) -> list*` | n 个 v |
| `pa_collections_counter` | `(list*, i64 key_kind, i64 key_own) -> dict*` | 计频，返回 dict |
| `pa_collections_most_common` | `(dict*, i64 n) -> list*` | `(key, count)` 按 count 降序，稳定 |
| `pa_json_dumps` | `(i64 v, str* desc) -> str*` | 按编译期类型描述符序列化 |
| `pa_json_loads` | `(str* text, str* desc) -> i64` | 按描述符解析；类型不匹配抛 ValueError |

### struct / datetime / pathlib / deque

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_struct_new` | `(i64 size) -> void*` | 分配 `[RC 头][payload]`，字段清零 |
| `pa_struct_copy` | `(void* src, i64 size) -> void*` | 分配新副本并整块 memcpy |
| `pa_date_new/pa_datetime_new` | `(i64 y, i64 mo, i64 d[, i64 h, i64 mi, i64 s]) -> datetime*` | 构造；日期按实际月份天数校验（含闰年） |
| `pa_datetime_now` | `(i64 is_date) -> datetime*` | `date.today()` / `datetime.now()` |
| `pa_datetime_field` | `(dt*, i64 idx) -> i64` | year/month/day/hour/minute/second/weekday |
| `pa_datetime_isoformat/strftime` | `(dt*, ...) -> str*` | ISO 文本 / `strftime` |
| `pa_datetime_replace` | `(dt*, list* fields) -> datetime*` | 替换字段，返回新对象 |
| `pa_datetime_add_delta/diff` | `(dt*, td*, i64 sub)` / `(dt*, dt*)` | 日期 ± timedelta、日期相减得 timedelta |
| `pa_datetime_cmp` | `(dt*, dt*) -> i64` | 按值比较 |
| `pa_timedelta_new` | `(i64 days, i64 seconds, i64 microseconds) -> timedelta*` | 规范化成 days/seconds/microseconds |
| `pa_timedelta_field` | `(td*, i64 idx) -> i64` | 0 days / 1 seconds / 2 microseconds |
| `pa_timedelta_total_seconds` | `(td*) -> f64` | `total_seconds()` |
| `pa_timedelta_cmp` | `(td*, td*) -> i64` | -1 / 0 / 1 |
| `pa_timedelta_add/sub` | `(td*, td*) -> td*` | 加减，返回新对象 |
| `pa_timedelta_mul/neg` | `(td*, i64) -> td*` / `(td*) -> td*` | 乘整数、取负 |
| `pa_timedelta_floordiv_int/truediv_int` | `(td*, i64) -> td*` | 除以整数 |
| `pa_timedelta_floordiv_td/truediv_td` | `(td*, td*) -> i64` / `f64` | timedelta 相除 |
| `pa_timedelta_str` | `(td*) -> str*` | `str(timedelta)`，与 CPython 对齐 |
| `pa_path_new/str/join` | `(str*) -> path*` / `(path*) -> str*` / `(path*, str*) -> path*` | 构造、转字符串、`/` 拼接 |
| `pa_path_name/stem/suffix/parent/parts` | `(path*) -> str*/path*/list*` | 取路径成分 |
| `pa_path_exists/is_file/is_dir/is_absolute` | `(path*) -> i64` | 查文件系统 / 绝对路径判断 |
| `pa_path_read_text/write_text` | `(path*) -> str*` / `(path*, str*) -> i64` | 文本读写 |
| `pa_path_read_bytes/write_bytes` | `(path*) -> bytes*` / `(path*, bytes*) -> i64` | 字节读写（0.144） |
| `pa_path_iterdir/glob/mkdir` | `(path*, ...) -> list*/i64` | 列目录、通配、建目录 |
| `pa_path_unlink/rmdir/with_suffix` | `(path*, ...) -> i64/path*` | 删除、替换后缀 |
| `pa_path_relative_to/resolve/absolute/touch` | `(path*, ...) -> path*/i64` | 路径运算与建空文件（0.144） |
| `pa_deque_new/from_list` | `(i64 maxlen) -> deque*` / `(list*, i64 maxlen) -> deque*` | 构造；maxlen 0 表示无界 |
| `pa_deque_append/appendleft` | `(dq*, v, ...) -> void` | 两端入队，按元素所有权 retain |
| `pa_deque_pop/popleft` | `(dq*) -> v` | 两端出队 |
| `pa_deque_len/maxlen/get/set` | `(dq*, ...) -> i64/v` | 长度、容量、下标读写 |
| `pa_deque_rotate/clear/extend/extendleft` | `(dq*, ...) -> void` | 轮转、清空、批量追加 |
| `pa_deque_to_list` | `(dq*) -> list*` | 物化成 list |
| `pa_deque_remove/count/index/insert/reverse/copy` | `(dq*, ...) -> ...` | 0.144 补齐的序列方法 |

### re

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_re_compile` | `(str* pat, i64 flags) -> regex*` | 编译 Pattern；语法错误抛 ValueError |
| `pa_re_search_str` | `(str* pat, str* text, i64 flags, i64 mode) -> match*` | mode 0 search / 1 match / 2 fullmatch；无匹配返回 0 |
| `pa_re_search` | `(regex*, str* text, i64 mode) -> match*` | Pattern 方法；无匹配返回 0 |
| `pa_re_findall_str` / `pa_re_findall` | `(...) -> list*` | 返回 `list[str]`，元素是整段匹配 |
| `pa_re_sub_str` / `pa_re_sub` | `(...) -> str*` | 替换；`\1`..`\9` 引用分组 |
| `pa_re_split_str` / `pa_re_split` | `(...) -> list*` | 按匹配切分，不含捕获组 |
| `pa_re_escape` | `(str*) -> str*` | 转义正则元字符与空白 |
| `pa_re_group` / `groups` / `start` / `end` / `span` | `(match*, i64 n) -> ...` | Match 方法；未匹配分组 `group` 返回空串，`start`/`end` 返回 -1 |

### 对象

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_obj_new` | `(i64 size, dtor, gc_release) -> void*` | `rc_alloc`，全零；`size < 8` panic |

### 生成器

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_gen_new` | `(i64 n_slots, i64 resume, i64 release, i64 kind) -> void*` | 分配堆帧；kind=1 表示协程 |
| `pa_gen_next` | `(void*) -> i64` | 恢复并取下一个值；结束抛 StopIteration |
| `pa_gen_next_opt` | `(void*, i64* ok) -> i64` | for 循环用：结束写 `ok=0`，不抛异常 |
| `pa_gen_send` | `(void*, i64) -> i64` | 把值送到 `x = yield v` |
| `pa_gen_send_opt` | `(void*, i64, i64* ok) -> i64` | `yield from` 用：转发 send，结束写 `ok=0`；非生成器帧报 TypeError |
| `pa_gen_throw` | `(void*, excval*) -> i64` | `gen.throw(exc)`：异常转成帧内注入并恢复；已结束则把异常抛给调用方 |
| `pa_gen_throw_opt` | `(void*, i64 exc, i64* ok) -> i64` | `yield from` 的 throw 转发：子生成器结束写 `ok=0` 并返回它的 return 值；没接住则异常在外层挂起点抛出 |
| `pa_gen_close` | `(void*) -> void` | 跑挂起点清理；异常向外传 |
| `pa_gen_finalize` | `(void*) -> void` | 引用计数归零时清理；异常吞掉 |
| `pa_handler_suspend` | `(void*) -> void` | yield/await：把 handler 记录从全局栈摘下 |
| `pa_handler_rearm` | `(void*) -> void` | 恢复：重压记录并更新临时值水位与帧栈顶 |
| `pa_handler_free` | `(void*) -> void` | close：释放挂起的记录 |
| `pa_coro_rethrow` | `(void*) -> void` | 在 resume 帧里抛出注入异常（noreturn） |
| `pa_asyncio_run` | `(void* coro) -> i64` | 跑事件循环到根协程结束，返回结果或抛异常 |
| `pa_asyncio_sleep` | `(double) -> void*` | 创建毫秒定时 awaitable |
| `pa_asyncio_gather` | `(list*, i64) -> void*` | 创建 gather awaitable（不接管参数列表）；第二参数是结果元素的 RC 类别（0 裸值 / 1 堆引用 / 2 unknown） |
| `pa_asyncio_create_task` | `(void* coro, i64 kind) -> void*` | 立即调度协程，返回 task awaitable；`kind` 是结果元素的 RC 类别（0 裸值 / 1 堆引用 / 2 unknown）；Task 可被多个协程同时 await |
| `pa_asyncio_task_done` | `(void*) -> i64` | 任务是否结束 |
| `pa_asyncio_task_cancel` | `(void*) -> i64` | 请求取消；解绑当前 awaitable 并注入 CancelledError |
| `pa_asyncgen_anext` | `(void*) -> void*` | 创建异步生成器的 `__anext__` 驱动对象 |
| `pa_asyncgen_asend` | `(void*, i64) -> void*` | `asend(v)` 驱动对象，v 的引用转交驱动 |
| `pa_asyncgen_aclose` | `(void*) -> void*` | `aclose()` 驱动对象，走 close 分发或注入 GeneratorExit |
| `pa_asyncgen_athrow` | `(void*, excval*) -> void*` | `athrow(exc)` 驱动对象，在挂起点注入异常 |
| `pa_asyncio_future_new` | `() -> void*` | 创建 Future（多等待者、回调列表） |
| `pa_future_set_result` | `(void*, i64) -> void` | 设置结果并唤醒全部等待者 |
| `pa_future_set_exception` | `(void*, str*) -> void` | 设置失败（类型 `Exception`、消息字符串），唤醒等待者并跑回调 |
| `pa_future_add_callback` | `(void*, i64 fn) -> void` | 注册完成回调；已完成则立即同步调用 |
| `pa_call1` | `(void* fn, i64 arg) -> void` | 跳板：按闭包 ABI 调用单参数 PyAdamas 函数值 |
| `pa_future_result` | `(void*) -> i64` | 取结果；未完成抛 RuntimeError，失败抛异常副本 |
| `pa_future_exception` | `(void*) -> str*` | 失败消息；成功返回 NULL |
| `pa_future_cancel/done/cancelled` | `(void*) -> i64/bool` | Future 状态 |
| `pa_asyncio_task_result/exception/cancelled` | `(void*) -> i64/str*` | Task 结果与状态 |
| `pa_asyncio_wait_fd` | `(i64 fd, i64 write) -> void*` | 创建 fd 等待对象，事件循环用 poll 唤醒 |

### 引用计数

**生成代码已经不再 `call pa_rc_inc` / `pa_rc_dec` 了。** `gen_call_rt` 对
`RcInc` / `RcDec` / `RcMark` / `RcCleanup` 四个 RtFn 有专用发射，结构照抄
`rt_internal.h` 里的 `pa_rc_inc_fast` / `pa_rc_dec_fast`：只有慢路径才
`call pa_rc_inc_slow` / `pa_rc_dec_slow`。inc 的内联判据是 NULL、永生字面量
（`rc == INT64_MIN`，直接跳过）、`rc <= 0`（走慢路径报错）；dec 是 NULL、
`pa_gc_phase_fast != 0`（收集中只登记边，不能重入释放）、永生、以及
`rc <= 1`（归零必须走慢路径真正释放）。统计模式只在运行时函数的内联版
（`*_fast`）里当慢路径判据，生成代码不查它。实测这四个函数在
queue_sim / str_build 的 perf 采样里分别占 8.2%/11.4%（cleanup）、
4.0%/3.0%（inc）、4.2%/3.7%（dec），而函数体本身只有 3~6 条指令——call/ret +
被调用者保存的开销比它们干的活还大。

`pa_rc_mark` 内联得更彻底：它就是 `return temp_len;`，生成代码一条
`movq temp_len(%rip), %rax` 顶掉一次 call。`pa_rc_cleanup` 只内联「水位没变就
直接返回」这条最常见的出口，其余整体交给原函数。慢路径落回运行时函数时
**必须**补 `spill_fp_slots` / `reload_fp_slots`：xmm 全是 caller-saved，慢路径
可能触发析构函数，不落盘就会被冲掉。

运行时自己的 `.c` 里也走内联：头文件把 `pa_rc_inc` / `pa_rc_dec` 宏成
`*_fast`，只有 `rt_core.c`（`PA_RC_NO_INLINE`）提供外部实体符号——C 的
`static inline` 不产生外部符号，所以生成代码调的那两个符号必须单独定义一次。
快路径只碰 `h->rc` 和两个全局别名，编译器能把整个判断序列摊在调用点。

**临时值栈**：全局 `void** temp_stack; int64_t temp_len, temp_cap;`。
`pa_rc_temp(p)` 入栈但**不增加计数**——栈接管的是产生者本来就持有的那一个
引用（永生对象直接跳过，不入栈）；`pa_rc_mark()` 直接 `return temp_len;`，
`pa_rc_cleanup(mark)` 把水位弹到 mark 并逐个 `dec`，水位相等时一次比较就返回
（实测 65.6 万次 cleanup 里至少 10 万次是空操作），mark 越界才 panic。
`pa_rc_temp_take(p)` 把栈上那个新值**取走**、所有权转给槽/字段/返回值，之后
cleanup 不再释放它：先看栈顶（容器构造时「刚 push 就 take」是主流模式），
不中再从 `temp_len-2` 往前找，命中就与栈顶交换。`pa_rc_temp_unknown(v)` 是
unknown 值的版本：先查 RC 索引确认是堆对象，才入栈。

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_rc_inc` | `(void*) -> void` | 引用 +1；空指针和永生字面量跳过（运行时内部走 `pa_rc_inc_fast` 内联） |
| `pa_rc_inc_slow` | `(void*) -> void` | 慢路径：统计计数、NULL、`rc <= 0` 报错 |
| `pa_rc_inc_unknown` | `(i64) -> i64` | unknown 值按堆注册表保守 retain；不是堆指针就什么也不做（先按 `< 65536` 排除小整数） |
| `pa_rc_dec_unknown` | `(i64) -> void` | unknown 值按堆注册表保守释放；不是堆指针就什么也不做 |
| `pa_rc_dec` | `(void*) -> void` | 引用 -1；归零时调 `dtor` 并释放整块（运行时内部走 `pa_rc_dec_fast` 内联） |
| `pa_rc_dec_slow` | `(void*) -> void` | 慢路径：统计、GC 期间只登记边、归零时 `rc_unlink` + `dtor` + 回收（untracked 对象跳过 `rc_unlink`） |
| `pa_rc_temp` | `(void*) -> void` | 表达式临时值入栈，接管产生者的引用（不 inc；永生对象不入栈） |
| `pa_rc_temp_unknown` | `(i64) -> void` | unknown 值的入栈版本：查注册表确认是堆对象才入栈 |
| `pa_rc_temp_take` | `(void*) -> void` | 把临时栈上的新值取走，所有权转给槽/字段/返回值 |
| `pa_rc_mark` | `() -> i64` | 返回临时值栈当前水位（生成代码内联成一条 load） |
| `pa_rc_cleanup` | `(i64 mark) -> void` | 释放到指定水位；水位不变直接返回，越界 panic |
| `pa_rc_stats_init` | `() -> void` | 入口按 `PYADAMAS_RC_STATS` 打开统计，退出时打印 `[rc] ...` |
| `pa_rc_shutdown` | `() -> void` | 入口函数退出前把栈里残留的临时值逐个 `dec` 再释放栈存储（exact-RC 下残留的 untracked leaf 只能靠 RC 释放，直接 free 会永久泄漏） |
| `pa_register_literals` | `(void** objs, i64 n) -> void` | 把模块的静态字符串字面量登记进 RC 索引（只登记，不挂 GC 链） |
| `pa_static_strs_init` | `() -> void` | 填好 256 项驻留表 `pa_char_tab`；生成的 `main` 在序言里调，单字节字面量直接索引它 |
| `pa_closure_new` | `(i64 code, i64 n_caps) -> closure*` | 分配闭包负载并挂 cell 析构器 |

### 循环收集

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_gc_init` | `(void** roots, i64 n, void* stack_top) -> void` | 注册全局根表与栈顶，生成的 `main` 调一次；顺带按 `PYADAMAS_GC_STATS` / `PYADAMAS_GC_CONSERVATIVE` 打开诊断与收窄回退（各读一次环境变量） |
| `pa_gc_collect` | `() -> void` | 由 `rc_alloc` 在分配量超过阈值时自动触发（minor 初始 2MB / major 初始 8MB）；大集合 major 的清扫由后续分配推进 |
| `pa_gc_unref_child` | `(void*) -> void` | GC 清扫用：对象可达时把引用计数减一 |
| `pa_gc_shutdown` | `() -> void` | 程序退出，释放链上所有剩余堆对象（含环）；untracked 对象不在链上，必须此前由 RC 归零（入口的 shutdown 顺序负责） |

### 字符串

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_str_concat` | `(str*, str*) -> str*` | 新建对象，两次 `memcpy` |
| `pa_str_repeat` | `(str*, i64) -> str*` | n 次重复；一次分配 + `memcpy`，负次数返回空串 |
| `pa_str_char_at` | `(str*, i64) -> str*` | 负索引先加 `pub`（码点数）；越界抛 IndexError；ASCII 返回驻留表单字节串（不分配），非 ASCII 现编 1–4 字节 |
| `pa_char_str` | `(i64) -> str*` | 按**字符编码**取 256 项驻留表（`c & 0xff`，不是数值）；0x00–0x7F 才是合法单字节串，其余槽位 `pub = 0` |
| `pa_str_from_i64` | `(i64) -> str*` | 手写十进制，不经过 `snprintf`；0..9 直接返回驻留的数字字符（`pa_char_str('0' + v)`，零分配） |
| `pa_str_any` | `(i64) -> str*` | unknown 值：堆字符串原样返回并补引用，否则按整数格式化 |
| `pa_str_from_f64` | `(double) -> str*` | CPython `repr` 规则，见下 |
| `pa_str_from_bool` | `(i64) -> str*` | 静态驻留的 `"True"` / `"False"` |
| `pa_str_from_none` | `() -> str*` | 静态驻留的 `"None"` |
| `pa_find_bytes` | `(const char*, i64, const char*, i64, i64) -> i64` | `memchr` 找首字节 + `memcmp` 校验；`find`/`count`/`replace`/`split`/`in` 共用 |
| `pa_str_eq` | `(str*, str*) -> i64` | 同一对象先命中（`a == b`）；再比长度、单字节/批量字节；返回 0/1 |
| `pa_str_cmp` | `(str*, str*) -> i64` | 三路比较，返回 -1/0/1；<=8 字节走内联循环，更长 `memcmp` |
| `pa_str_capwords` | `(str*) -> str*` | 单遍 `string.capwords`：按 ASCII 空白切词、词首大写/其余小写，只分配一个输出串 |
| `pa_percent_format` | `(str* pat, list* args, str* desc, i64 is_bytes) -> str*` | printf 风格 `%`；`desc` 每个字节描述实参域（`i` int、`b` bool、`f` float、`s` str/bytes、`n` None），非法组合抛 TypeError |

`pa_str_cmp` 用 `memcmp`，前提是 **UTF-8 的字节序与码点序一致**（这是 UTF-8 的设计性质），所以结果与 Python 的字符串比较相同。

### 哈希 / Base64

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_sha256_digest` | `(str*) -> str*` | SHA-256，返回 32 字节摘要（`bytes` 与 `str` 共用表示） |
| `pa_sha256_hex` | `(str*) -> str*` | SHA-256 小写十六进制，64 字符 |
| `pa_b64encode` | `(str*, i64 urlsafe) -> str*` | b64 编码；`urlsafe` 非 0 用 `-_` 字母表，不补 `=` |
| `pa_b64decode` | `(str*) -> str*` | 同时接受标准与 URL-safe 字母表；非法字符抛 `ValueError` |

### 输出

| 函数 | 签名 | 语义 |
|---|---|---|
| `pa_print_i64` | `(i64)` | `%lld` |
| `pa_print_f64` | `(double)` | 走 `format_double` |
| `pa_print_bool` | `(i64)` | `True` / `False` |
| `pa_print_str` | `(str*)` | 原样写出 `len` 个字节，不加换行 |
| `pa_print_none` | `()` | `None` |
| `pa_print_sep` | `()` | 一个空格 |
| `pa_print_end` | `()` | 一个换行 |

`print(sep=..., end=...)` 复用 `pa_print_str`——自定义分隔符就是「原样写一个字符串」，没必要单开函数。

### panic

| 函数 | 触发场景 | 输出 |
|---|---|---|
| `pa_panic` | 用户级 panic / 剩余内部错误 | 抛 `RuntimeError`；未捕获时 `PyAdamas: uncaught ...` |
| `pa_panic_div0` | 整数除以 / 模以 0 | 抛 `ZeroDivisionError` |
| `pa_panic_overflow` | `INT64_MIN / -1` | 抛 `OverflowError` |

这些错误在有 handler 时可以被 `except` 捕获；没有 handler 时先经
`pa_print_traceback()` 打印函数级 `Traceback (most recent call last)`，再打印
`pyadamas: uncaught <type>: <message>` 并 `exit(1)`。它用 libgcc 的
`_Unwind_Backtrace` 逐帧取返回地址（0.163 起；0.160–0.162 是手走 rbp 链，
因此整个运行时都得 `-fno-omit-frame-pointer`），只有命中 codegen 注册的
PC → (函数, 行号) 表的帧才打印；没有表（旧产物/无调试信息）时只打印消息。
展开器靠 `.eh_frame`/`.eh_frame_hdr` 找 FDE，`link_program` 显式带
`-Wl,--eh-frame-hdr`。生成器/协程的堆帧不在调用栈上：`coro_step` /
`gen_run` / 异步生成器驱动在异常逃出时用 `pa_trace_attach` 把 resume 帧挂到
异常上，未捕获时追加打印（仅函数名，没有挂起行号）。内部不变量错误和 OOM
仍走 `panic_c` 直接退出，不打印回溯。

### 不走枚举的辅助函数

| 函数 | 调用方 | 说明 |
|---|---|---|
| `pa_debug_register` | 模块入口 | 注册 codegen 发出的 PC → (函数, 行号) 表 |
| `pa_print_traceback` | 未捕获异常路径 | 用 `_Unwind_Backtrace` 逐帧查表，打印函数级 `Traceback (most recent call last)` |
| `pa_trace_attach` | 生成器/协程异常逃出 | 把 resume 帧（函数名/行）挂到异常的 trace 链表 |
| `pa_trace_print` | 未捕获异常路径 | 追加打印协程/生成器逻辑帧 |
| `pa_pow_i64` | `gen_bin_int` 的 `Pow` | 快速幂；**负指数返回 0**（CPython 返回 float） |
| `pa_abs_i64` | `abs(int)` | 整数绝对值，回绕语义 |
| `pa_str_eq` / `pa_str_cmp` | `gen_cmp` | 参数形态固定，没必要进枚举 |
| `pa_static_strs_init` | 生成的 `main` 序言 / 模块 init | 填好 `pa_char_tab` 与 `"True"/"False"/"None"` 驻留对象；必须晚于索引开关（驻留对象要不要登记进 RC 索引取决于它） |
| `floor` / `fmod` / `pow` | libm | 浮点 `//`、`%`、`**` 直接用 libm |

## 浮点转字符串

`format_double` 是运行时里最长的一段，规矩跟 CPython 的 `repr` 一致：

1. 从精度 1 往上试，找**最短的、能 `strtod` 读回原值**的数字串。
2. 按小数点位置决定排版：`decpt` 在 `-4 < decpt <= 16` 用定点，否则科学计数法（指数至少两位、带符号）。
3. 整数值补 `.0`。

两个反例说明为什么不能偷懒：

- 直接用 `%g`：`%g` 是「指数 ≥ 精度」就切科学计数法，而最短数字串精度常常只有一两位，于是 `10.0` 被排成 `1e+01`。
- 精度从 17 往下找：会停在「第一个失败的精度之上」，多吐末位数字（`-84.992391262976952` 而不是 `-84.99239126297695`）。

## 内存模型

**引用计数。** 每个堆对象（字符串、列表、元组、字典、集合、类实例、闭包）带隐藏的
`PyAdamasRc` 头；所有权约定：

- 槽 / 全局 / 字段 / kind 1 列表元素持有引用；
- `load` / `field.get` / `list.get` 只借用，不增加计数；
- 函数形参在入口统一 retain 一份，出口释放；
- 返回值在返回处 retain，调用方拿到的是新引用；
- 每个新堆值带着 `rc=1` 进临时值栈，语句 / 循环 / 函数边界按 mark 清理。
- **新值移动**：刚产生、还没被别名的堆值写入槽 / 全局 / 字段 / 容器元素，
  或直接作为返回值时，降级器用 `pa_rc_temp_take` 把临时栈上的引用直接转给
  目标，省掉「retain 一次、cleanup 再 dec 一次」；写入后被别名或再次读取时
  不再移动，按普通 retain 处理。`list` / `tuple` / `dict` / `set` 字面量与
  推导式在构造成功后同样转移元素引用。

临时值栈让降级器不用做活跃性分析：就算某个分支、`continue`、`break`
跳过一条清理路径，下一层边界也会兜底。代价是纯堆操作会多几条指令
（mark/cleanup/inc/dec 的快路径都已内联，见「引用计数」）；完全不含临时
堆值的语句、循环和函数会把 mark/cleanup 在降级期删掉，所以整数代码的
生成结果与 0.6.1 完全相同。

引用计数处理不了两类东西：**环**和**类型看不见的引用**。它们交给下面的
循环收集器：

- 对象自环、相互递归闭包形成的 cell 环：分配量超过新生代阈值（初始 2MB）
  触发 minor（trial-deletion），超过 major 阈值（初始 8MB，随后按老生代存活
  量自适应）触发 major（标记-清扫）；进程退出时也会清空整个堆。
- `unknown` 容器元素、unknown 字段/全局：写入时用 `pa_rc_inc_unknown`
  （或已知堆类型时 `pa_rc_inc`）持有，覆盖/销毁时用 `pa_rc_dec_unknown`
  释放。注册表先确认机器字确实指向堆对象，因此整数和静态字面量都不会
  被误减；容器 kind 2、`field_heap == 2`、全局的 `maybe_unknown` 都走
  这条路径。剩余不精确的地方是 unknown 类型的**局部槽和临时值**：它们
  在写入时保守 retain，出口不释放，靠 GC 兜底。
- 注册表维护一份开放寻址索引（墓碑复用、按存活数决定扩容或原地压缩），
  索引本身只随存活对象数增长。
- 唯一的「回收静默性」：没有 `__del__`/弱引用，对象什么时候真正释放
  不可观察；GC 是单线程的分代收集，没有并发收集。major 的**清扫**已经按
  集合规模分片（集合 >= `GC_INCR_MIN_SET` = 20000 个对象时，清扫摊到后续
  每次 `rc_alloc` 上按预算推进，见「循环收集器」），标记仍一次性做完。

静态类型的好处是 RC 插桩位置是确定的：赋值、传参、返回、容器写入都是
明确的增减引用点。GC 只是兜底，不替代 RC。

**异常路径**：longjmp 跳过普通函数帧时不执行 `release_slots`，改由
`pa_frame_unwind` 按帧记录逐槽释放并清零；槽地址最低两位是标志：bit0
表示「可能非堆」（走 `pa_rc_dec_unknown`），bit1 表示「只给 GC 扫描的
借来引用」（异常展开跳过，不释放）。帧记录同时是 GC 的影子根表（见
下文「循环收集器」）。handler 记录本身保存了受保护区的临时值栈水位，
landing pad 会把这段临时值精确清掉；异常路径不会泄漏已 retain 的局部
引用或临时表达式。生成器/协程的槽在堆帧里，由帧的 gcrel 释放。

## 循环收集器

引用计数会立刻释放不成环的垃圾，所以收集器只需要处理环。对象按「是否在
当前收集集合」分代：上次收集后分配、还没晋升的在新生代（`young_buf`），
其余存活对象是老生代。`alloc_since_gc` 超过 2MB 触发 minor（集合 = 新生
代），`alloc_since_major` 超过「老生代翻倍 / 8MB」触发 major（集合 =
整堆）。

**不在链上的对象不参与收集。** `rc_alloc_untracked` 分出去的对象（全非堆
元组；exact-RC 模式下连字符串 / range / struct 这类叶子也走
`rc_leaf_untracked`）不在 GC 堆链上，GC 既不扫描也不清扫，只由 RC 归零时
`dtor` 释放。这条捷径的正确性来自一个不变量：**untracked 对象没有任何
出边（或出边只指向同样 untracked 的叶子），因此不可能参与引用环**——
环一旦形成就必须由 GC 回收，所以只有能证明这一点的对象才走 untracked。

**minor（trial-deletion）**：

1. 试探减计数：对集合内每个对象执行 `release` 回调，把每条出边从子对象
   的 `rc` 减掉。减完 `rc > 0` 的对象被集合外引用，是根——老对象、全局、
   临时栈、堆槽的引用都在真实 `rc` 里，不需要写屏障。
2. 标记：从根出发沿出边走一遍。unknown 局部槽这类「借来的」引用不计入
   `rc`，由帧记录登记后交给 GC：只扫登记过的槽地址、含 unknown vreg 的
   帧的临时区，以及 `setjmp` 捕获的 callee-saved 寄存器；整段真实栈不再
   扫描。exact-RC 模式（整程序无 unknown）下所有引用都归槽/临时值/容器
   所有，直接跳过扫栈。
3. 恢复：标记对象再走一遍 `release`，把出边计数加回去并晋升为老生代。
4. 清扫：垃圾对象先走 `release`，把指向老生代的边攒进 pending；释放完本
   回合对象后再按正常 RC 处理这些边，避免清扫期重入释放。

**major（标记-清扫）**：集合是整堆，根是全局根表、临时值栈、帧记录登记
的地址和 callee-saved 寄存器；标记同步做完，然后清扫不可达对象。大集合
（>= `GC_INCR_MIN_SET` = 20000 个对象）的清扫分片：`gc_collect` 只记状态，
真实清理由后续每次 `rc_alloc` 推进 `gc_incr_step`（每次 `GC_SWEEP_BUDGET`
个对象）。分片期间 `gc_phase` 保持非 0，`pa_rc_dec` 只登记 `gc_pend`、
不立即释放——否则清扫中重入释放会撞上还在队列里的指针。小集合直接同步清完：
分片会打断分配路径的缓存局部性，实测反而更慢。地址表按「本轮集合」建，minor
时只有新生代那么大，比给整堆建表便宜。

收集现场用全局 `gc_phase` 把 `pa_rc_dec` 分流：trial/restore 只调整集合内
对象的计数，清扫把集合外的边登记到 pending，都不在 GC 内部触发释放。生成
器帧的槽释放走 `pa_gen_slot_release`，只有清扫阶段才清零槽，trial/restore
不会破坏生成器状态。

保守扫描的代价是「误报只会延迟回收」：登记区或寄存器里的整数如果恰好
等于某个堆地址，会把它当根留下；不会把可达对象误判成垃圾。误报面已经从
「整段真实栈」缩小到「登记过的 unknown 槽 + 可能装指针的帧的临时区」，
纯类型化帧不参与扫描；临时区里「不可能装指针的字」也不再扫（见下一节），
不再整段保守扫描。

### GC 诊断与临时区扫描收窄

**诊断开关**：`PYADAMAS_GC_STATS=1` 打开 GC 诊断。入口 `pa_gc_init` 只读
一次环境变量、缓存成全局 int，之后分配 / RC 热路径没有任何额外读写；关闭
时 GC 仍走原来的单遍扫描路径。每轮收集往 stderr 打三行，退出时打汇总：

```
[gcstats] #3 minor 0.412ms set=1200 freed=1190 live_objs=2345 live_bytes=456789
[gcstats]   roots frames=4 slots=7 temp_words=12/40 reg_words=25 mask=on
[gcstats]   cons_kept 3 objs / 240 bytes (temp=2/160 slot=1/80 reg=0/0)
```

- `set` / `freed` / `live_objs` / `live_bytes`：本轮集合大小、清扫掉的对象
  数、收集后链上存活对象数与堆字节数；
- `roots`：登记帧数、登记槽数、临时区实际扫描字数 / 整段字数、寄存器扫描
  字数（`setjmp` 存下的 callee-saved 寄存器，按机器字整段扫）、位图是否
  生效（`on` / `none` / `off(conservative)`）；
- `cons_kept`：**没有精确根能到达、只在保守扫描阶段才被标活的对象**。诊断
  模式先把全局、临时栈、已知堆槽这些精确根扫完并排空工作栈，再扫临时区 /
  unknown 槽 / 寄存器；第二段新标上的对象就是「只靠保守根维持」的那批——
  这是误报保留的直接度量，按临时区 / unknown 槽 / 寄存器分开计。它衡量
  的是「整段扫描多留了多少垃圾」，不是「程序有没有 bug」。

**临时区扫描收窄**：`PyAdamasFrame` 增加第 5 个头字 `scan_mask`（临时区
位图：bit i = 1 表示第 i 个字可能装指针；NULL 表示整段扫描）。生成代码按
vreg 的静态类型算位图：

- 只有「证明可能装指针」的字才置 1：`unknown` / 无类型，以及
  `Optional[unknown]`、`BoundMethod`、非 C 结构体指针（有指针机器表示但
  静态上说不清谁持有）；一个 vreg 槽被多个 vreg 复用时按字取并集。
- 没有这样的字时 `scan_words = 0`：这一帧的临时区完全跳过（纯
  int/float/bool 帧、只含已类型化堆引用的帧）。
- int / bool / float / None / Optional[标量] 的机器表示不可能是堆指针，不
  置位；类型化堆引用（str/list/dict/类实例……）也不置位——它们要么是 owned
  （临时栈 / 槽 / 容器持有，GC 从精确根可达），要么是 borrowed 而持有者
  本身从根可达。这条前提本来就是「纯类型化帧整帧不扫」的既有依据，收窄
  只是把它用到含 unknown 的帧上。
- 兜底：收窄不到一半时不发位图、做回整段扫（每字判断的成本反而更贵）；
  vreg 槽号与 vreg 表对不上时回整段；生成器 / 协程帧不参与（槽在堆帧里，
  不走帧记录）；名字槽仍按精确地址登记，不受位图影响。
  `PYADAMAS_GC_CONSERVATIVE=1` 时忽略位图，完全退回整段保守扫描——回归
  验证和线上回退都用它。

收窄只删「临时区里不可能装指针的字」，从不删已登记的槽和寄存器扫描；方向
永远是多扫而不是少扫：少扫一个真正含指针的字就是提前释放（UAF），比多留
一个垃圾严重得多。

**实测**（本机 Debug 构建，`PYADAMAS_GC_STATS=1`）：

| 场景 | 整段扫描 | 位图收窄 |
|---|---|---|
| `tests/run/13_gc.py` 每轮临时区字数 | 35 | 3 |
| `tests/run/151_gc_generations.py` | 33 | 1 |
| `tests/run/92_exc_inline_rc.py` | 42 | 1 |
| `tests/run/160_gc_scan_narrow.py` | 16 | 3 |
| `tests/run/161_gc_conservative_equiv.py` | 20 | 3 |
| `tests/run` 全部用例合计（163 个用例、143 轮收集） | 2901 字 | 403 字（13.9%） |
| 全量保守保留量合计（同 143 轮） | 92 个 / 8144 字节 | 0 |
| 全量用例（`run_tests.sh`） | 179 pass / 0 fail | 179 pass / 0 fail |
| 16MB `RLIMIT_AS` 压力程序 | 通过，峰值 RSS ≈ 8.0MB | 通过，峰值 RSS ≈ 8.0MB |

163 个 `tests/run` 用例里只有 5 个分配量过 2MB 阈值、真的触发收集，其余
没有 GC 轮次。整段扫描下有两个用例出现误报保留：
`151_gc_generations.py`（6 个 / 576 字节）和 `160_gc_scan_narrow.py`
（86 个 / 7568 字节）；后者用 `id()` 把「恰好等于堆地址的整数」放进临时区，
整段扫描会把这类整数当根、把成片的垃圾环钉在堆上。位图收窄后这些字不进
扫描，两个用例的保守保留量都归零，输出逐字节一致（`160` 是 `5340`，
`151` 是 `288890 / 1 / 1 / 1`）。

验证命令（`build/zgc` 是 M4 的独立构建目录，正式口径用 `build/dev`）：

```bash
cmake -S . -B build/zgc -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/zgc -j4
PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh build/zgc build/zgc/pyadamas
PYADAMAS_GC_CONSERVATIVE=1 PYADAMAS_TEST_JOBS=4 \
    bash tests/run_tests.sh build/zgc build/zgc/pyadamas
./build/zgc/pyadamas build -o /tmp/stress stress.py
python3 -c 'import resource,subprocess
def lim(): resource.setrlimit(resource.RLIMIT_AS,(16<<20,16<<20))
p=subprocess.run(["/tmp/stress"],preexec_fn=lim)
print(p.returncode, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)'
```

注意 `PyAdamasFrame` 的头从 4 个字变成 5 个字：运行时和生成代码必须一起
重编。

## 加一个运行时函数

1. `src/ir_ops.h`：`RtFn` 加枚举项 + 注释写签名
2. `src/ir_ops.h` 的 `rt_name()`：加 `case` 返回符号名
3. `src/codegen_call.cpp` 的 `gen_call_rt`：按参数形态把实参搬进 `%rdi/%rsi/%rdx/%xmm0`（`-Wswitch` 会强制你加）
4. `runtime/rt_*.c`（先看 `rt_internal.h` 选文件）：实现，函数名与第 2 步一致，参数用 `int64_t` / `double`
5. `docs/runtime.md`：函数表加一行

参数超过 3 个、或者要传浮点时，第 3 步最容易写错——照抄相邻的 `case`，别自己推。

## 未来

- **异常对象接入 RC/GC**：现在是 malloc 的轻量对象；如果以后要支持
  `args`、traceback 或自定义异常类，需要把异常对象改成正常的堆类型。
- **更精确的根**：帧记录已经把 unknown 槽的扫描收窄到登记地址，临时区
  也按静态类型位图收窄（见「GC 诊断与临时区扫描收窄」）。剩下的空间是
  活跃性：位图按「字」收、不按「时刻」收，不活跃的 unknown 字仍会被扫，
  这需要栈映射 / 活跃性分析。增量**清扫**已经落地（大集合 major 的清扫
  按分配点分片），但**标记**仍是一次做完的：RC 的释放时机不受 GC 控制，
  标记期间对象可能被 RC 释放、标记队列里还留着指向它的指针（登记
  `gc_pend` 延后释放也挡不住收尾后才归零的对象，实测
  `174_gc_construct_roots` 漏 21KB）。要真正并发标记，得先让所有释放都
  经过 GC 的灰对象协议——那是另一套所有权模型。
- **unknown 局部槽**：容器元素、字段和全局已经用 RC 索引精确释放
  （`pa_rc_dec_unknown`），表达式临时值也由 `pa_rc_temp_unknown` 持有并
  释放；只有 unknown 类型的局部槽仍是借来的引用（不计入 rc），靠保守扫栈
  兜底。彻底解决需要给值加运行时标签，或者把 unknown 从语言里消掉。

## 0.271–0.300 运行时改动（补记）

- **dict 缓冲池化（0.281）**：cap≤64、icap≤128 的小 dict，三块缓冲（entries/order/index）随头一起保留在池节点数组栈（256 槽）里——缓冲指针寄存在池节点而不是 payload（payload 会被 `rc_alloc_core` 的 memset 清零）；分配命中后 memset 之后写回，状态重置 + index 清零，**零 malloc、零 reindex**（缓冲已是上一世高水位）。大 dict 缓冲直接 free 不保留，防池囤内存；GC 清扫路径（`gc_free_body`）自己 free 缓冲、不经池；shutdown drain 连缓冲一起 free。
- **int/float 键单次 mix64（0.281）**：原哈希 mix64 两次（单值路径一次 + 公共尾部又一次），单次 mix64 已是全雪崩双射，第二次纯浪费；字符串路径提前返回不受影响，与编译期字面量哈希（FNV + 单次 mix64）一致，同值同哈希不变。
- **`pa_dict_try_get`（0.284）**：`(PyAdamasDict*, i64 key, i64* out) -> i64`；命中写借用值到 *out 返回 1，缺失返回 0 不写 out、不创建 default——与 contains 守卫下的 get 逐位等价；defaultdict 的缺省创建只发生在真 get。
- **dict set 单探测 `dict_lookup_place`（0.285）**：探测路上顺带记第一个墓碑，命中返回 entry，未命中把可写槽写进 out 参（与 `place_slot` 逐位一致）；reindex 触发后重算 place；新键插入从两遍探测降为一遍，bump 未命中路径从三探降到两探。
- **标量 dict 销毁跳过 entry 链游（0.285）**：`dict_destroy` / `dict_gc_release` / `pa_dict_clear` 在 `key_own==0 && val_own==0` 时跳过整条 entry 链游（无 RC 可做）。
- **`pa_rc_temp_take` 永生早退（0.286）**：永生值从不入临时栈（`pa_rc_temp` 跳过），take 碰到永生值直接返回，与原全栈扫描语义等价。
- **str 下标 ASCII 快路径（0.287）**：ASCII 字符命中 256 项驻留表（零分配、永生），生成代码内联查 `pa_char_tab`，省掉 `pa_str_char_at` + `pa_char_str` 两层调用；负索引/越界/非 ASCII 仍落回 `pa_str_char_at`。
