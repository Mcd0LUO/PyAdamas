// rt_dict.c —— 字典与集合
//
// 布局：entries 是条目数组；存活的条目用 prev/next 串成插入顺序的双向
// 链表，删除只摘链、条目留原地当墓碑，由 free_head 空闲链复用——摊还
// O(1)，不用 memmove。index 是开放寻址的索引表，槽里是「entry 下标 + 1」，
// 0 为空槽，-1 为墓碑（探测越过继续）。查找先走 4 字节的索引表（probe 链
// 常驻缓存），命中才去读 entry。按序号遍历走 order 快照，需要时从链表懒
// 重建，顺序永远是插入顺序。
#include "rt_internal.h"

/* ── 字典 / 集合 ─────────────────────────────────────────────── */

uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

int64_t dict_hash_value(int64_t kind, int64_t v) {
    uint64_t h;
    if (kind == 1) {
        PyAdamasStr* s = (PyAdamasStr*)v;
        /* **哈希缓存：所有长度都走 chash。**
         *
         * 原来只有 len > 7 能用 hash 字段缓存 —— 短串走 SSO，字符内联在
         * hash 字段里，没有位置存。而词频统计这类场景恰恰全是短键
         * （alpha/beta/gamma…），每次查找都要重跑 FNV。
         *
         * 实测同样的 16 个键、同样内容，只把长度从 7 改成 8（触发缓存）：
         *   174.3ms -> 116.6ms（-33%）。
         *
         * 现在 chash 是独立字段（偏移 32），SSO 和堆串都能用。
         * 0 = 未算；非 0 = 已算。字面量在编译期填好。 */
        if (s->chash != 0) return s->chash;
        h = 1469598103934665603ull;
        for (int64_t i = 0; i < s->len; ++i) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ull;
        }
        h = mix64(h);
        int64_t hv = (int64_t)(h ? h : 1);
        s->chash = hv;
        return hv;
    } else if (kind == 2) {
        double d;
        memcpy(&d, &v, sizeof d);
        if (d == 0.0) v = 0;  /* 0.0 与 -0.0 同槽 */
        h = mix64((uint64_t)v);
    } else {
        h = mix64((uint64_t)v);
    }
    // 0.281：单次 mix64 已是全雪崩双射，第二次纯浪费 —— int/float 键
    // 双重 mix 占 rec_memo 运行时 17.6%。字符串路径在上面提前返回，
    // 与编译期字面量哈希（FNV + 单次 mix64）本来就一致。
    return (int64_t)(h ? h : 1);
}

/* hash() 内置函数和字典键共用同一套哈希，保证同值同哈希。 */
int64_t pa_hash_value(int64_t kind, int64_t v) { return dict_hash_value(kind, v); }

int64_t pa_hash_f64(double v) {
    int64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return dict_hash_value(2, bits);
}

int dict_key_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return pa_str_eq((PyAdamasStr*)a, (PyAdamasStr*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

/* 类 key：__hash__ / __eq__ 的 vtable 槽在编译期分配好，槽里是生成的
   普通方法（self 在 %rdi，__eq__ 的 other 在 %rsi）。只有类同时实现了
   两个 dunder 时调用方才会把槽号传进来。 */
static int64_t dunder_call1(void* obj, int64_t slot) {
    int64_t* vt = *(int64_t**)obj;
    int64_t fn = vt[slot];
    if (fn <= 0) return 0;
    return ((int64_t(*)(void*))(uintptr_t)fn)(obj);
}

static int64_t dunder_call2(void* a, int64_t b, int64_t slot) {
    int64_t* vt = *(int64_t**)a;
    int64_t fn = vt[slot];
    if (fn <= 0) return 0;
    return ((int64_t(*)(void*, int64_t))(uintptr_t)fn)(a, b);
}

static int64_t dict_hash_key(PyAdamasDict* d, int64_t key) {
    if (d->hash_slot >= 0) {
        int64_t h = dunder_call1((void*)key, d->hash_slot);
        return h ? h : 1;
    }
    return dict_hash_value(d->key_kind, key);
}

/* always_inline：探测循环每步都调它。新增 dict_lookup_place 后 gcc 的
   内联预算把它 outline 成公共函数（两处调用共享），wordfreq 实测每次
   探测多一层 call/ret，指令 +6.3%。强制内联恢复紧探测循环（0.285）。 */
static inline __attribute__((always_inline))
int dict_key_matches(PyAdamasDict* d, int64_t a, int64_t b) {
    if (d->eq_slot >= 0) return dunder_call2((void*)a, b, d->eq_slot) != 0;
    return dict_key_eq(d->key_kind, a, b);
}

/* 索引表里找到 key 的 entry 下标；找不到返回 -1。墓碑槽（-1）跳过。 */
static int64_t dict_lookup(PyAdamasDict* d, int64_t key, int64_t hash) {
    int64_t mask = d->icap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    for (;;) {
        int32_t slot = d->index[i];
        if (!slot) return -1;
        if (slot > 0) {
            int64_t e = (int64_t)slot - 1;
            if (d->entries[e].hash == hash && dict_key_matches(d, d->entries[e].key, key))
                return e;
        }
        i = (i + 1) & mask;
    }
}

/* 0.285：查找与可写槽一次探测完成。set 原来先 dict_lookup 找已有
   key，未命中再 dict_place_slot 从同一 hash 重新探测找空槽/墓碑 ——
   新键插入走两遍探测链。这里合并：探测路上顺带记第一个墓碑，命中
   返回 entry，未命中把可写槽写进 *place（与 dict_place_slot 逐位
   一致：优先链上第一个墓碑，否则第一个空槽）。reindex 会重建索引，
   调用方须在 reindex 后重算 place。 */
static int64_t dict_lookup_place(PyAdamasDict* d, int64_t key, int64_t hash,
                                 int64_t* place) {
    int64_t mask = d->icap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    int64_t tomb = -1;
    for (;;) {
        int32_t slot = d->index[i];
        if (!slot) {
            *place = tomb >= 0 ? tomb : i;
            return -1;
        }
        if (slot > 0) {
            int64_t e = (int64_t)slot - 1;
            if (d->entries[e].hash == hash && dict_key_matches(d, d->entries[e].key, key))
                return e;
        } else if (tomb < 0) {
            tomb = i;
        }
        i = (i + 1) & mask;
    }
}

/* 找可写槽：优先复用探测链上的第一个墓碑，没有墓碑就用第一个空槽。
   调用前保证索引表至少有一个空槽（见 pa_dict_set 的装载检查）。 */
static int64_t dict_place_slot(PyAdamasDict* d, int64_t hash) {
    int64_t mask = d->icap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    int64_t tomb = -1;
    for (;;) {
        int32_t slot = d->index[i];
        if (!slot) return tomb >= 0 ? tomb : i;
        if (slot < 0 && tomb < 0) tomb = i;
        i = (i + 1) & mask;
    }
}

/* 重建索引表：顺着插入顺序链表扫存活项，顺带清掉墓碑。 */
static void dict_reindex(PyAdamasDict* d, int64_t nicap) {
    if (nicap < 8) nicap = 8;
    if (nicap != d->icap) {
        free(d->index);
        d->index = (int32_t*)calloc((size_t)nicap, sizeof(int32_t));
        if (!d->index) panic_c("out of memory");
        d->icap = nicap;
    } else {
        memset(d->index, 0, sizeof(int32_t) * (size_t)d->icap);
    }
    d->itomb = 0;
    int64_t mask = d->icap - 1;
    for (int32_t e = d->head; e >= 0; e = d->entries[e].next) {
        int64_t s = (int64_t)((uint64_t)d->entries[e].hash & (uint64_t)mask);
        while (d->index[s]) s = (s + 1) & mask;
        d->index[s] = (int32_t)(e + 1);
    }
}

/* 把 e 从字典里摘掉：双向链表摘链、索引槽置墓碑、entry 挂空闲链。
   不碰引用计数——key/value 的释放由调用方按所有权处理。 */
static void dict_erase(PyAdamasDict* d, int64_t e) {
    int32_t prev = d->entries[e].prev;
    int32_t next = d->entries[e].next;
    if (prev >= 0)
        d->entries[prev].next = next;
    else
        d->head = next;
    if (next >= 0)
        d->entries[next].prev = prev;
    else
        d->tail = prev;
    int64_t mask = d->icap - 1;
    int64_t s = (int64_t)((uint64_t)d->entries[e].hash & (uint64_t)mask);
    int64_t steps = 0;
    while (d->index[s] != (int32_t)(e + 1)) {
        if (++steps > d->icap) panic_c("dict index corrupted");
        s = (s + 1) & mask;
    }
    d->index[s] = -1;
    ++d->itomb;
    --d->len;
    d->ord_len = -1;
    d->entries[e].hash = 0;
    d->entries[e].key = d->free_head;
    d->free_head = e;
}

/* 0.302：entry 写入探针。own==2 的键/值若是堆指针就置 saw_heap（sticky，
   只置不清）。标量只做两次比较、不写内存 —— 0.283 的 STLF 教训：不要每
   set 必写标志形成循环携带 store→load 依赖。探针与 dec_unknown 共用
   pa_unknown_is_heap（rt_core.c），判定逐位一致：写入时探针说 0 的值，
   销毁时 pa_own_dec(2,·) 必然空操作，跳过链游不会漏释放。 */
static inline __attribute__((always_inline))
void dict_note_heap(PyAdamasDict* d, int64_t key, int64_t val) {
    if (d->saw_heap) return;
    if (d->key_own == 2 && pa_unknown_is_heap(key)) { d->saw_heap = 1; return; }
    if (d->val_own == 2 && pa_unknown_is_heap(val)) d->saw_heap = 1;
}

void dict_destroy(void* p) {
    PyAdamasDict* d = (PyAdamasDict*)p;
    // 0.285：标量键值（own==0）没有 RC 可做，整条链_walk 跳过 ——
    // rec_memo 的 dict[int,int] 每轮销毁 25 项，链游是循环携带指针
    // 追逐，占运行时 11.6%。缓冲仍由 dict_pool_put 处置。
    if (d->key_own == 0 && d->val_own == 0) return;
    // 0.302：own==(2,2) 且从未存过堆值 —— 全部 entry 是标量，无 RC 可做。
    // rec_memo 的模块级 memo={} 推成 dict[unknown,unknown]，0.285 的
    // (0,0) 跳过从不命中，每轮 25 项白走链游 + 探测（dict_destroy 13%）。
    if (d->key_own == 2 && d->val_own == 2 && !d->saw_heap) return;
    for (int32_t i = d->head; i >= 0; i = d->entries[i].next) {
        PyAdamasDictEntry e = d->entries[i];
        pa_own_dec(d->key_own, e.key);
        pa_own_dec(d->val_own, e.val);
    }
    // 0.281：缓冲不在此 free —— dict_pool_put 决定保留（小 dict）
    // 还是释放（大 dict）。GC 清扫走 gc_free_body 自己 free，不经这里。
}

void dict_gc_release(void* p) {
    PyAdamasDict* d = (PyAdamasDict*)p;
    if (d->key_own == 0 && d->val_own == 0) return;
    // 0.302：同 dict_destroy —— 纯标量的 unknown dict 无引用可 unref。
    if (d->key_own == 2 && d->val_own == 2 && !d->saw_heap) return;
    for (int32_t i = d->head; i >= 0; i = d->entries[i].next) {
        PyAdamasDictEntry e = d->entries[i];
        pa_own_gc_unref(d->key_own, e.key);
        pa_own_gc_unref(d->val_own, e.val);
    }
}

PyAdamasDict* pa_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own,
                        int64_t hash_slot, int64_t eq_slot) {
    if (key_kind < 0 || key_kind > 3) panic_c("invalid dict key kind");
    if (key_own < 0 || key_own > 2 || val_own < 0 || val_own > 2)
        panic_c("invalid dict ownership kind");
    PyAdamasDict* d =
        (PyAdamasDict*)rc_alloc((int64_t)sizeof(PyAdamasDict), dict_destroy, dict_gc_release,
                            PYADAMAS_RC_DICT);
    if (d->entries) {
        // 池命中且缓冲保留（0.281）：cap/icap 是上一世高水位，零 malloc、
        // 零增长。状态重置 + index 清零即可复用；entries 陈旧内容只会被
        // 新插入覆盖（nent=0、free_head=-1、index 全 0，读不到旧值）。
        d->len = 0;
        d->ord_len = 0;
        d->nent = 0;
        d->itomb = 0;
        d->free_head = -1;
        d->head = -1;
        d->tail = -1;
        memset(d->index, 0, sizeof(int32_t) * (size_t)d->icap);
    } else {
        d->len = 0;
        d->cap = 8;
        d->icap = 8;
        d->ord_len = 0;
        d->nent = 0;
        d->itomb = 0;
        d->free_head = -1;
        d->head = -1;
        d->tail = -1;
        d->entries = (PyAdamasDictEntry*)malloc(sizeof(PyAdamasDictEntry) * 8);
        d->order = (int32_t*)malloc(sizeof(int32_t) * 8);
        d->index = (int32_t*)calloc(8, sizeof(int32_t));
        if (!d->entries || !d->order || !d->index) panic_c("out of memory");
    }
    d->key_kind = key_kind;
    d->key_own = key_own;
    d->val_own = val_own;
    d->hash_slot = hash_slot;
    d->eq_slot = eq_slot;
    d->default_factory = 0;
    // 空表无堆 entry（池命中时 payload 已被 memset 清零，这里显式归零）。
    d->saw_heap = 0;
    return d;
}

/* 调用方已经把 key/value 的「入表引用」retain 好；更新已有 key 时
   释放这份多余的新 key 引用，字典继续保留原来的 key 对象。 */
void pa_dict_set(PyAdamasDict* d, int64_t key, int64_t val) {
    // 插入与覆盖两条路径都从这里走，统一探针（覆盖时旧值已 dec，新值
    // 可能是堆；键对象沿用插入时已探针的原 key）。
    dict_note_heap(d, key, val);
    int64_t h = dict_hash_key(d, key);
    int64_t place;
    int64_t e = dict_lookup_place(d, key, h, &place);
    if (e >= 0) {
        pa_own_dec(d->key_own, key);
        pa_own_dec(d->val_own, d->entries[e].val);
        d->entries[e].val = val;
        return;
    }
    // 索引按 2/3 装载（墓碑也计入），超了就重建；存活项本身超了才扩容。
    if ((d->len + d->itomb + 1) * 3 >= d->icap * 2) {
        if ((d->len + 1) * 3 >= d->icap * 2)
            dict_reindex(d, d->icap * 2);
        else
            dict_reindex(d, d->icap);
        place = dict_place_slot(d, h);
    }
    int64_t idx;
    if (d->free_head >= 0) {
        idx = d->free_head;
        d->free_head = d->entries[idx].key;
    } else {
        if (d->nent == d->cap) {
            int64_t ncap = d->cap ? d->cap * 2 : 8;
            PyAdamasDictEntry* ne =
                (PyAdamasDictEntry*)realloc(d->entries, sizeof(PyAdamasDictEntry) * (size_t)ncap);
            int32_t* no = (int32_t*)realloc(d->order, sizeof(int32_t) * (size_t)ncap);
            if (!ne || !no) panic_c("out of memory");
            d->entries = ne;
            d->order = no;
            d->cap = ncap;
        }
        idx = d->nent++;
    }
    d->entries[idx].hash = h;
    d->entries[idx].key = key;
    d->entries[idx].val = val;
    d->entries[idx].prev = d->tail;
    d->entries[idx].next = -1;
    if (d->tail >= 0)
        d->entries[d->tail].next = (int32_t)idx;
    else
        d->head = (int32_t)idx;
    d->tail = (int32_t)idx;
    if (d->ord_len >= 0) {
        d->order[d->ord_len++] = (int32_t)idx;
    }
    ++d->len;
    if (d->index[place] < 0) --d->itomb;
    d->index[place] = (int32_t)(idx + 1);
}

/* default_factory 的编码与 lower_collections 保持一致。这里只实现内置
   零参数工厂；任意 callable 需要运行期闭包调用协议，另行处理。 */
static int64_t dict_make_default(PyAdamasDict* d) {
    switch (d->default_factory) {
        case 1: /* int */
        case 2: /* float：0.0 的位模式就是 0 */
        case 3: /* bool */
            return 0;
        case 4: /* str */
            return (int64_t)str_new("", 0);
        case 5: /* list：元素类型未知（kind 2，释放走注册表） */
            return (int64_t)pa_list_new(0, 2);
        case 6: /* dict：键值类型未知 */
            return (int64_t)pa_dict_new(3, 2, 2, -1, -1);
        case 7: /* set：复用 dict，值恒为 0 */
            return (int64_t)pa_dict_new(3, 2, 0, -1, -1);
        case 8: /* tuple：pyadamas 用 list 表示元组 */
            return (int64_t)pa_list_new(0, 2);
        default:
            pa_raise_c("TypeError", "defaultdict factory is not callable");
    }
}

void pa_dict_set_default_factory(PyAdamasDict* d, int64_t factory) {
    d->default_factory = factory;
}

int64_t pa_dict_try_get(PyAdamasDict* d, int64_t key, int64_t* out) {
    int64_t e = dict_lookup(d, key, dict_hash_key(d, key));
    if (e < 0) return 0;
    *out = d->entries[e].val;
    return 1;
}

int64_t pa_dict_get(PyAdamasDict* d, int64_t key) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e < 0) {
        if (d->default_factory) {
            /* 新值交给 dict 持有；key 是调用方借来的，入表前补引用。 */
            int64_t val = dict_make_default(d);
            pa_own_inc(d->key_own, key);
            pa_dict_set(d, key, val);
            return val; /* 和已有 key 一样返回 dict 内部的借用值 */
        }
        pa_raise_c("KeyError", "key not found");
    }
    return d->entries[e].val;
}

int64_t pa_dict_contains(PyAdamasDict* d, int64_t key) {
    int64_t h = dict_hash_key(d, key);
    return dict_lookup(d, key, h) >= 0 ? 1 : 0;
}

/* order 是链表快照：删除会让它失效，下一次按序号访问时重建。遍历本身
   是 O(n)，这层重建不改变摊还代价，却让按序访问保持 O(1)。 */
static void dict_ensure_order(PyAdamasDict* d) {
    if (d->ord_len == d->len) return;
    int64_t i = 0;
    for (int32_t e = d->head; e >= 0; e = d->entries[e].next) {
        if (i >= d->cap) panic_c("dict order overflow");
        d->order[i++] = e;
    }
    if (i != d->len) panic_c("dict order mismatch");
    d->ord_len = d->len;
}

int64_t pa_dict_key_at(PyAdamasDict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    dict_ensure_order(d);
    return d->entries[d->order[i]].key;
}

int64_t pa_dict_val_at(PyAdamasDict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    dict_ensure_order(d);
    return d->entries[d->order[i]].val;
}

/* d.get(k)：缺失时返回 None。pyadamas 里 None 就是 0（没有类型标签），
   所以这里返回 0——和 const.none 同一个表示。 */
int64_t pa_dict_get_opt(PyAdamasDict* d, int64_t key) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    return e >= 0 ? d->entries[e].val : 0;
}

int64_t pa_dict_get_or(PyAdamasDict* d, int64_t key, int64_t def) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    return e >= 0 ? d->entries[e].val : def;
}

int own_to_list_kind(int64_t own) {
    return own == 1 ? 1 : (own == 0 ? 0 : 2);
}

PyAdamasList* pa_dict_keys(PyAdamasDict* d) {
    int kind = own_to_list_kind(d->key_own);
    PyAdamasList* out = pa_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = pa_dict_key_at(d, i);
        pa_own_inc(kind, v);
        pa_list_append(out, v, kind);  /* 0.303：种类即堆性 */
    }
    return out;
}

PyAdamasList* pa_dict_values(PyAdamasDict* d) {
    int kind = own_to_list_kind(d->val_own);
    PyAdamasList* out = pa_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = pa_dict_val_at(d, i);
        pa_own_inc(kind, v);
        pa_list_append(out, v, kind);  /* 0.303 */
    }
    return out;
}

PyAdamasList* pa_dict_items(PyAdamasDict* d) {
    // 单元素 kind 无法同时表达 key/value 两类所有权：都为堆用 1，
    // 都非堆用 0，混合用 2（销毁时按注册表保守释放）。
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    PyAdamasList* out = pa_list_new(d->len, 1);
    // RAII：循环内每个 pa_list_new(tup) 都是 GC 分配点，out 只在 C 局部
    // 变量里，必须挂根 —— 作用域守卫管所有出口，不再手工配对 unpin。
    PA_PIN_SCOPE(out);
    for (int32_t i = d->head; i >= 0; i = d->entries[i].next) {
        PyAdamasDictEntry e = d->entries[i];
        PyAdamasList* tup = pa_list_new(2, tk);
        /* 0.303：tk==2 的短命元组用 heap2（不建位图，销毁走注册表）——
           按 own 传 0/1 会让 kind2 元组在首元素时分配位图，items 每对
           一次 alloc/free churn（dict_wordfreq 实测 +4.7%）。tk 0/1 时
           传准确种类，免探针。 */
        int th = tk == 2 ? 2 : tk;
        pa_own_inc(d->key_own, e.key);
        pa_list_append(tup, e.key, th);
        pa_own_inc(d->val_own, e.val);
        pa_list_append(tup, e.val, th);
        pa_list_append(out, (int64_t)tup, 1);  // tuple 的初始引用交给外层 list（0.303：堆提示 1）
    }
    PA_PIN_RELEASE(out);
    return out;
}

/* dict 相等：长度相同，且 a 的每个 (key,val) 在 b 里都能找到同样的 key
   和等值的 val。顺序无关，和 CPython 一致。chain 说明值怎么比。 */
int64_t chain_dict_eq(PyAdamasDict* a, PyAdamasDict* b, const uint8_t* chain, int64_t len,
                      int64_t level) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    int kind = (level < len) ? chain[level] : 'x';
    for (int32_t i = a->head; i >= 0; i = a->entries[i].next) {
        PyAdamasDictEntry ae = a->entries[i];
        int64_t e = dict_lookup(b, ae.key, dict_hash_key(b, ae.key));
        if (e < 0) return 0;
        if (!chain_elem_eq(kind, ae.val, b->entries[e].val, chain, len, level))
            return 0;
    }
    return 1;
}

int64_t pa_dict_eq(PyAdamasDict* a, PyAdamasDict* b, PyAdamasStr* chain) {
    return chain_dict_eq(a, b, (const uint8_t*)chain->data, chain->len, 0);
}

/* set 相等：只看键。set 复用 dict 的表（值恒 0），所以按 dict 的键来比。 */
int64_t chain_set_eq(PyAdamasDict* a, PyAdamasDict* b, const uint8_t* chain, int64_t len,
                     int64_t level) {
    (void)chain;
    (void)len;
    (void)level;
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    for (int32_t i = a->head; i >= 0; i = a->entries[i].next) {
        int64_t k = a->entries[i].key;
        if (dict_lookup(b, k, dict_hash_key(b, k)) < 0) return 0;
    }
    return 1;
}

int64_t pa_set_eq(PyAdamasDict* a, PyAdamasDict* b) {
    return chain_set_eq(a, b, 0, 0, 0);
}

void pa_dict_update(PyAdamasDict* dst, PyAdamasDict* src) {
    if (dst == src) return;
    // 所有权类别向源靠拢：两边不一致时统一升级为 2（unknown/混合），
    // 保证目标既有的键值也不会被按错误类别释放——kind 2 对整数是空操作。
    // 0.302：既有 entry 是按旧 own retain 的。1->2 升级时表里已持有从未
    // 探针过的堆引用（own==1 路径不探针），必须保守置 saw_heap，否则销毁
    // 跳过链游会漏释放。0->2 升级表里只有标量，无需置位。src 的新 entry
    // 走 pa_dict_set 探针。空表（len==0，墓碑不在链上）无需置位。
    int ok = dst->key_own, ov = dst->val_own;
    if (src->key_own != dst->key_own) dst->key_own = 2;
    if (src->val_own != dst->val_own) dst->val_own = 2;
    if (dst->len && ((dst->key_own == 2 && ok == 1) || (dst->val_own == 2 && ov == 1)))
        dst->saw_heap = 1;
    int kr = dst->key_own;
    int vr = dst->val_own;
    for (int32_t i = src->head; i >= 0; i = src->entries[i].next) {
        PyAdamasDictEntry e = src->entries[i];
        pa_own_inc(kr, e.key);
        pa_own_inc(vr, e.val);
        pa_dict_set(dst, e.key, e.val);
    }
}

/* 同 kind 的浅拷贝：所有权类别也复制过去，键值按需 retain。
   供 match 的 `**rest` 使用，随后再删掉显式匹配过的 key。 */
/* ── set 的原地集合运算（`s &= t` / `s -= t` / `s ^= t`）─────────────
   set 复用 dict（值恒 0），所以按键集合做交/差/对称差。遍历自己的键，
   命不命中 t 决定去留；删除走 pa_dict_remove（同 del d[k]）。
   键列表是快照（pa_dict_keys 新建），删除期间遍历安全。 */
void pa_set_intersect_update(PyAdamasDict* s, PyAdamasDict* t) {
    if (s == t) return;
    PyAdamasList* ks = pa_dict_keys(s);
    if (!ks) return;
    pa_gc_pin(ks);
    for (int64_t i = 0; i < ks->len; ++i) {
        int64_t k = ks->data[i];
        if (!pa_dict_contains(t, k)) pa_dict_remove(s, k, 1);
    }
    pa_gc_unpin(ks);
    pa_rc_dec(ks);
}

void pa_set_difference_update(PyAdamasDict* s, PyAdamasDict* t) {
    if (s == t) {
        pa_dict_clear(s);
        return;
    }
    PyAdamasList* ks = pa_dict_keys(s);
    if (!ks) return;
    pa_gc_pin(ks);
    for (int64_t i = 0; i < ks->len; ++i) {
        int64_t k = ks->data[i];
        if (pa_dict_contains(t, k)) pa_dict_remove(s, k, 1);
    }
    pa_gc_unpin(ks);
    pa_rc_dec(ks);
}

void pa_set_symdiff_update(PyAdamasDict* s, PyAdamasDict* t) {
    if (s == t) {
        pa_dict_clear(s);
        return;
    }
    /* 趟一遍 t 就够：交集项从 s 删掉，差集项补进 s。s 独有的键不动
       —— 这正好是对称差（不能先删交集再从 t 补差集：补的时候那些键
       已经不在 s 里了，会被当成差集重新加回来）。 */
    PyAdamasList* ks = pa_dict_keys(t);
    if (!ks) return;
    pa_gc_pin(ks);
    int ko = (int)s->key_own;
    for (int64_t i = 0; i < ks->len; ++i) {
        int64_t k = ks->data[i];
        if (pa_dict_contains(s, k)) {
            pa_dict_remove(s, k, 1);
        } else {
            pa_own_inc(ko, k);
            pa_dict_set(s, k, 0);
        }
    }
    pa_gc_unpin(ks);
    pa_rc_dec(ks);
}

/* ── set 的非原地集合运算（`s | t` / `s & t` / `s - t` / `s ^ t`）──────
   返回新 set，左右操作数不动。set 复用 dict（值恒 0）。 */
PyAdamasDict* pa_set_union(PyAdamasDict* a, PyAdamasDict* b) {
    PyAdamasDict* out = pa_dict_copy(a);
    pa_gc_pin(out);
    pa_dict_update(out, b);
    pa_gc_unpin(out);
    return out;
}

PyAdamasDict* pa_set_intersection(PyAdamasDict* a, PyAdamasDict* b) {
    PyAdamasDict* out = pa_dict_new(a->key_kind, a->key_own, 0, a->hash_slot, a->eq_slot);
    pa_gc_pin(out);
    PyAdamasList* ks = pa_dict_keys(a);
    pa_gc_pin(ks);
    int ko = (int)a->key_own;
    for (int64_t i = 0; i < ks->len; ++i) {
        int64_t k = ks->data[i];
        if (!pa_dict_contains(b, k)) continue;
        pa_own_inc(ko, k);
        pa_dict_set(out, k, 0);
    }
    pa_gc_unpin(ks);
    pa_rc_dec(ks);
    pa_gc_unpin(out);
    return out;
}

PyAdamasDict* pa_set_difference(PyAdamasDict* a, PyAdamasDict* b) {
    PyAdamasDict* out = pa_dict_copy(a);
    pa_gc_pin(out);
    pa_set_difference_update(out, b);
    pa_gc_unpin(out);
    return out;
}

PyAdamasDict* pa_set_symdiff(PyAdamasDict* a, PyAdamasDict* b) {
    PyAdamasDict* out = pa_dict_copy(a);
    pa_gc_pin(out);
    pa_set_symdiff_update(out, b);
    pa_gc_unpin(out);
    return out;
}

PyAdamasDict* pa_dict_copy(PyAdamasDict* src) {
    PyAdamasDict* out = pa_dict_new(src->key_kind, src->key_own, src->val_own,
                                  src->hash_slot, src->eq_slot);
    // 键是类实例时 pa_dict_set 会调用户 __hash__/__eq__（还能再分配）；
    // out 只在本帧 C 局部变量里，用 RAII 守卫挂根。
    PA_PIN_SCOPE(out);
    out->default_factory = src->default_factory;
    // 0.302：保守传播 sticky 标志（逐 entry 的 pa_dict_set 也会探针）。
    out->saw_heap = src->saw_heap;
    for (int32_t i = src->head; i >= 0; i = src->entries[i].next) {
        PyAdamasDictEntry e = src->entries[i];
        pa_own_inc(src->key_own, e.key);
        pa_own_inc(src->val_own, e.val);
        pa_dict_set(out, e.key, e.val);
    }
    PA_PIN_RELEASE(out);
    return out;
}

/* dict(iterable)：元素是 (key, value) 二元组（PyAdamas 里 tuple 也是 list）。 */
PyAdamasDict* pa_dict_from_pairs(PyAdamasList* src, int64_t key_kind, int64_t key_own,
                                 int64_t val_own, int64_t hash_slot, int64_t eq_slot) {
    PyAdamasDict* d = pa_dict_new(key_kind, key_own, val_own, hash_slot, eq_slot);
    /* 同 pa_dict_copy：用户 __hash__ 可能在循环内分配，d 只在 C 局部变量里。 */
    pa_gc_pin(d);
    for (int64_t i = 0; i < src->len; ++i) {
        PyAdamasList* pair = (PyAdamasList*)(uintptr_t)src->data[i];
        if (!pair || pair->len != 2) {
            // 抛异常会跳过临时栈接管：d 泄漏一张空表，可以接受。
            pa_raise_c("ValueError", "dictionary update sequence element has wrong length");
        }
        int64_t k = pair->data[0];
        int64_t v = pair->data[1];
        pa_own_inc(key_own, k);
        pa_own_inc(val_own, v);
        pa_dict_set(d, k, v);
    }
    pa_gc_unpin(d);
    return d;
}

PyAdamasDict* pa_set_from_list(PyAdamasList* src, int64_t key_kind, int64_t key_own,
                             int64_t hash_slot, int64_t eq_slot) {
    // 列表元素种类可能比声明的 key_own 更保守（推导不出来时是 2，未定型是
    // 3），取较大者；3 按 2 处理。
    int sk = src->elem_kind == 3 ? 2 : (int)src->elem_kind;
    if (sk > key_own) key_own = sk;
    PyAdamasDict* d = pa_dict_new(key_kind, key_own, 0, hash_slot, eq_slot);
    /* 用户 __hash__ 可能在循环内分配，d 只在 C 局部变量里。 */
    pa_gc_pin(d);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        pa_own_inc(key_own, k);
        pa_dict_set(d, k, 0);  // 重复 key 时 dict_set 会退掉这份多余引用
    }
    pa_gc_unpin(d);
    return d;
}

/* 找到并移除 key；返回 1 时 val_out 是原值。字典对 key/val 的引用：
   key 立即释放（调用方传的是借用），value 的所有权转交给调用方
   （pop 返回给 Python 层，由临时栈接管）。order 用末尾项填洞、entry 留
   墓碑，删除是摊还 O(1)，不再 memmove 和全表重建。 */
int dict_take(PyAdamasDict* d, int64_t key, int64_t* val_out) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e < 0) return 0;
    *val_out = d->entries[e].val;
    pa_own_dec(d->key_own, d->entries[e].key);
    dict_erase(d, e);
    return 1;
}

int64_t pa_dict_pop(PyAdamasDict* d, int64_t key, int64_t has_default, int64_t def) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (has_default) return def;
        pa_raise_c("KeyError", "key not found");
    }
    return v;
}

PyAdamasList* pa_dict_popitem(PyAdamasDict* d) {
    if (d->len == 0) pa_raise_c("KeyError", "popitem(): dictionary is empty");
    int64_t e = (int64_t)d->tail;
    PyAdamasDictEntry ent = d->entries[e];
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    PyAdamasList* tup = pa_list_new(2, tk);
    pa_own_inc(d->key_own, ent.key);
    pa_list_append(tup, ent.key, 2);
    pa_own_inc(d->val_own, ent.val);
    pa_list_append(tup, ent.val, 2);
    pa_own_dec(d->key_own, ent.key);
    pa_own_dec(d->val_own, ent.val);
    dict_erase(d, e);
    return tup;
}

/* set.pop()：移除并返回最后一个插入项（pyadamas 的确定性选择）。 */
int64_t pa_dict_pop_last(PyAdamasDict* d) {
    if (d->len == 0) pa_raise_c("KeyError", "pop from an empty set");
    int64_t e = (int64_t)d->tail;
    int64_t key = d->entries[e].key;
    dict_erase(d, e);
    return key;  /* 所有权转交给调用方 */
}

void pa_dict_remove(PyAdamasDict* d, int64_t key, int64_t missing_ok) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (!missing_ok) pa_raise_c("KeyError", "key not found");
        return;
    }
    pa_own_dec(d->val_own, v);
}

void pa_dict_clear(PyAdamasDict* d) {
    // 0.302：纯标量的 unknown dict 同样跳过链游（判定同 dict_destroy）。
    if (!(d->key_own == 0 && d->val_own == 0) &&
        !(d->key_own == 2 && d->val_own == 2 && !d->saw_heap)) {
        for (int32_t i = d->head; i >= 0; i = d->entries[i].next) {
            PyAdamasDictEntry e = d->entries[i];
            pa_own_dec(d->key_own, e.key);
            pa_own_dec(d->val_own, e.val);
        }
    }
    // 清空后是空表：sticky 标志归零，后续写入重新探针。
    d->saw_heap = 0;
    d->len = 0;
    d->ord_len = 0;
    d->nent = 0;
    d->itomb = 0;
    d->free_head = -1;
    d->head = -1;
    d->tail = -1;
    memset(d->index, 0, sizeof(int32_t) * (size_t)d->icap);
}

/* `d[k] = d.get(k, def) + delta` 的融合版本。
 *
 * 这是词频统计 / 计数器最经典的写法，IR 里是三条指令：
 *     %a = call.rt pa_dict_get_or(d, k, def)
 *     %b = bin.add %a, delta
 *     call.rt pa_dict_set(d, k, %b)          (+ 两次 rc_inc)
 *
 * 每次 dict 操作都要算一次哈希 + 一次开放寻址探测。同一个 key 连续做
 * get 和 set，两次哈希完全是重复劳动（chash 缓存之后哈希本身便宜了，
 * 但 dict_lookup 的探测仍要做两遍）。
 *
 * 这里合成一次：算一次哈希、做一次查找，命中就原地改值，未命中就插入。
 *
 * **语义等价性**（必须逐条对上 pa_dict_get_or + pa_dict_set 的组合）：
 *   - 命中：新值 = 旧值 + delta，写回。旧值的引用要 dec（dict 不再持有它）；
 *     新值 val 由调用方持有并转移给 dict，**不再 inc**（省掉那对 inc/dec）。
 *   - 未命中：新值 = def + delta，插入。key 由调用方转移给 dict。
 *   - val 的堆标记沿用 d->val_own；key 沿用 d->key_own。
 *
 * 与分开调用的唯一行为差异是**求值顺序**：分开时 get 与 set 之间若有
 * 副作用（比如 delta 表达式抛异常）能看到中间状态。这里 delta 已经是
 * 算好的值，def 也是常量，没有可观测的中间状态。
 */
void pa_dict_bump(PyAdamasDict* d, int64_t key, int64_t def, int64_t delta) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e >= 0) {
        int64_t old = d->entries[e].val;
        /* 加法用整数语义 —— 与 IR 里 bin.add 在 unknown 类型上的处理一致
         * （gen_bin_int 直接 addq，不做运行时类型分派）。 */
        int64_t nv = old + delta;
        // 0.302：bump 命中路径直接写 val，不经 pa_dict_set —— 单独探针。
        if (!d->saw_heap && d->val_own == 2 && pa_unknown_is_heap(nv)) d->saw_heap = 1;
        pa_own_dec(d->val_own, old);
        d->entries[e].val = nv;
        return;
    }
    /* **新键要由 dict 持有引用。** 融合前的 IR 是
         RcInc(key); DictSet(d, key, val)
       融合把这两条都删了，所有权必须在这里补上，否则动态构造的键
       （b1 的日志字段、b5 的邮箱等）插入后立刻被调用方释放，留下
       悬空键指针，后续一次 rc_inc 就打到已释放对象。 */
    pa_own_inc(d->key_own, key);
    pa_dict_set(d, key, def + delta);
}