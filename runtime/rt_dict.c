// rt_dict.c —— 字典与集合
//
// 布局：entries 是密集数组，按插入顺序存放 {hash, key, val}；index 是
// 开放寻址的索引表，槽里是「entry 下标 + 1」，0 表示空。查找先走 4 字节
// 的索引表（probe 链常驻缓存），命中才去读 24 字节的 entry。扩容/删除只
// 重建索引表，entries 本身不用搬家；顺序遍历直接扫 entries。
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
        Py23Str* s = (Py23Str*)v;
        h = 1469598103934665603ull;
        for (int64_t i = 0; i < s->len; ++i) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ull;
        }
    } else if (kind == 2) {
        double d;
        memcpy(&d, &v, sizeof d);
        if (d == 0.0) v = 0;  /* 0.0 与 -0.0 同槽 */
        h = mix64((uint64_t)v);
    } else {
        h = mix64((uint64_t)v);
    }
    h = mix64(h);
    return (int64_t)(h ? h : 1);
}

/* hash() 内置函数和字典键共用同一套哈希，保证同值同哈希。 */
int64_t py23_hash_value(int64_t kind, int64_t v) { return dict_hash_value(kind, v); }

int64_t py23_hash_f64(double v) {
    int64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return dict_hash_value(2, bits);
}

int dict_key_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
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

static int64_t dict_hash_key(Py23Dict* d, int64_t key) {
    if (d->hash_slot >= 0) {
        int64_t h = dunder_call1((void*)key, d->hash_slot);
        return h ? h : 1;
    }
    return dict_hash_value(d->key_kind, key);
}

static int dict_key_matches(Py23Dict* d, int64_t a, int64_t b) {
    if (d->eq_slot >= 0) return dunder_call2((void*)a, b, d->eq_slot) != 0;
    return dict_key_eq(d->key_kind, a, b);
}

/* 索引表里找到 key 的 entry 下标；找不到返回 -1。 */
static int64_t dict_lookup(Py23Dict* d, int64_t key, int64_t hash) {
    int64_t mask = d->icap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    for (;;) {
        int32_t slot = d->index[i];
        if (!slot) return -1;
        int64_t e = (int64_t)slot - 1;
        if (d->entries[e].hash == hash && dict_key_matches(d, d->entries[e].key, key))
            return e;
        i = (i + 1) & mask;
    }
}

static int64_t dict_place_slot(Py23Dict* d, int64_t hash) {
    int64_t mask = d->icap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (d->index[i]) i = (i + 1) & mask;
    return i;
}

/* 重建索引表。只读 entries 里已存的 hash，不碰 key；entries 顺序不动。 */
static void dict_reindex(Py23Dict* d, int64_t nicap) {
    if (nicap < 8) nicap = 8;
    if (nicap != d->icap) {
        free(d->index);
        d->index = (int32_t*)calloc((size_t)nicap, sizeof(int32_t));
        if (!d->index) panic_c("out of memory");
        d->icap = nicap;
    } else {
        memset(d->index, 0, sizeof(int32_t) * (size_t)d->icap);
    }
    int64_t mask = d->icap - 1;
    for (int64_t e = 0; e < d->len; ++e) {
        int64_t i = (int64_t)((uint64_t)d->entries[e].hash & (uint64_t)mask);
        while (d->index[i]) i = (i + 1) & mask;
        d->index[i] = (int32_t)(e + 1);
    }
}

void dict_destroy(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[i];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    free(d->entries);
    free(d->index);
}

void dict_gc_release(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[i];
        if (d->key_own == 1) py23_gc_unref_child((void*)e.key);
        if (d->val_own == 1) py23_gc_unref_child((void*)e.val);
    }
}

Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own,
                        int64_t hash_slot, int64_t eq_slot) {
    if (key_kind < 0 || key_kind > 3) panic_c("invalid dict key kind");
    if (key_own < 0 || key_own > 2 || val_own < 0 || val_own > 2)
        panic_c("invalid dict ownership kind");
    Py23Dict* d =
        (Py23Dict*)rc_alloc((int64_t)sizeof(Py23Dict), dict_destroy, dict_gc_release,
                            PY23_RC_DICT);
    d->len = 0;
    d->cap = 8;
    d->icap = 8;
    d->entries = (Py23DictEntry*)malloc(sizeof(Py23DictEntry) * 8);
    d->index = (int32_t*)calloc(8, sizeof(int32_t));
    if (!d->entries || !d->index) panic_c("out of memory");
    d->key_kind = key_kind;
    d->key_own = key_own;
    d->val_own = val_own;
    d->hash_slot = hash_slot;
    d->eq_slot = eq_slot;
    return d;
}

/* 调用方已经把 key/value 的「入表引用」retain 好；更新已有 key 时
   释放这份多余的新 key 引用，字典继续保留原来的 key 对象。 */
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e >= 0) {
        if (d->key_own == 1) py23_rc_dec((void*)key);
        if (d->val_own == 1) py23_rc_dec((void*)d->entries[e].val);
        d->entries[e].val = val;
        return;
    }
    // 索引按 2/3 装载，entries 满了才扩；索引重建只动 4 字节槽。
    if ((d->len + 1) * 3 >= d->icap * 2) dict_reindex(d, d->icap * 2);
    if (d->len == d->cap) {
        int64_t ncap = d->cap ? d->cap * 2 : 8;
        Py23DictEntry* ne =
            (Py23DictEntry*)realloc(d->entries, sizeof(Py23DictEntry) * (size_t)ncap);
        if (!ne) panic_c("out of memory");
        d->entries = ne;
        d->cap = ncap;
    }
    int64_t idx = d->len++;
    d->entries[idx].hash = h;
    d->entries[idx].key = key;
    d->entries[idx].val = val;
    d->index[dict_place_slot(d, h)] = (int32_t)(idx + 1);
}

int64_t py23_dict_get(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e < 0) py23_raise_c("KeyError", "key not found");
    return d->entries[e].val;
}

int64_t py23_dict_contains(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_key(d, key);
    return dict_lookup(d, key, h) >= 0 ? 1 : 0;
}

int64_t py23_dict_key_at(Py23Dict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    return d->entries[i].key;
}

int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    return e >= 0 ? d->entries[e].val : def;
}

int own_to_list_kind(int64_t own) {
    return own == 1 ? 1 : (own == 0 ? 0 : 2);
}

Py23List* py23_dict_keys(Py23Dict* d) {
    int kind = own_to_list_kind(d->key_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[i].key;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_values(Py23Dict* d) {
    int kind = own_to_list_kind(d->val_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[i].val;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_items(Py23Dict* d) {
    // 单元素 kind 无法同时表达 key/value 两类所有权：都为堆用 1，
    // 都非堆用 0，混合用 2（已知堆的元素 retain，销毁时不解引用，靠 GC）。
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* out = py23_list_new(d->len, 1);
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[i];
        Py23List* tup = py23_list_new(2, tk);
        if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
        py23_list_append(tup, e.key);
        if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
        py23_list_append(tup, e.val);
        py23_list_append(out, (int64_t)tup);  // tuple 的初始引用交给外层 list
    }
    return out;
}

/* dict 相等：长度相同，且 a 的每个 (key,val) 在 b 里都能找到同样的 key
   和等值的 val。顺序无关，和 CPython 一致。chain 说明值怎么比。 */
int64_t chain_dict_eq(Py23Dict* a, Py23Dict* b, const uint8_t* chain, int64_t len,
                      int64_t level) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    int kind = (level < len) ? chain[level] : 'x';
    for (int64_t i = 0; i < a->len; ++i) {
        int64_t k = a->entries[i].key;
        int64_t e = dict_lookup(b, k, dict_hash_key(b, k));
        if (e < 0) return 0;
        if (!chain_elem_eq(kind, a->entries[i].val, b->entries[e].val, chain, len, level))
            return 0;
    }
    return 1;
}

int64_t py23_dict_eq(Py23Dict* a, Py23Dict* b, Py23Str* chain) {
    return chain_dict_eq(a, b, (const uint8_t*)chain->data, chain->len, 0);
}

/* set 相等：只看键。set 复用 dict 的表（值恒 0），所以按 dict 的键来比。 */
int64_t chain_set_eq(Py23Dict* a, Py23Dict* b, const uint8_t* chain, int64_t len,
                     int64_t level) {
    (void)chain;
    (void)len;
    (void)level;
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    for (int64_t i = 0; i < a->len; ++i) {
        int64_t k = a->entries[i].key;
        if (dict_lookup(b, k, dict_hash_key(b, k)) < 0) return 0;
    }
    return 1;
}

int64_t py23_set_eq(Py23Dict* a, Py23Dict* b) {
    return chain_set_eq(a, b, 0, 0, 0);
}

void py23_dict_update(Py23Dict* dst, Py23Dict* src) {
    if (dst == src) return;
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[i];
        // 目标类型未知时也要保证源销毁后指针不悬空：源自己是堆所有权
        // 就 retain 一份。目标 own=2/0 不会 dec，这份引用会留给 GC。
        if ((dst->key_own == 1 || src->key_own == 1) && e.key)
            py23_rc_inc((void*)e.key);
        if ((dst->val_own == 1 || src->val_own == 1) && e.val)
            py23_rc_inc((void*)e.val);
        py23_dict_set(dst, e.key, e.val);
    }
}

/* 同 kind 的浅拷贝：所有权类别也复制过去，键值按需 retain。
   供 match 的 `**rest` 使用，随后再删掉显式匹配过的 key。 */
Py23Dict* py23_dict_copy(Py23Dict* src) {
    Py23Dict* out = py23_dict_new(src->key_kind, src->key_own, src->val_own,
                                  src->hash_slot, src->eq_slot);
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[i];
        if (src->key_own == 1 && e.key) py23_rc_inc((void*)e.key);
        if (src->val_own == 1 && e.val) py23_rc_inc((void*)e.val);
        py23_dict_set(out, e.key, e.val);
    }
    return out;
}

Py23Dict* py23_set_from_list(Py23List* src, int64_t key_kind, int64_t key_own,
                             int64_t hash_slot, int64_t eq_slot) {
    Py23Dict* d = py23_dict_new(key_kind, key_own, 0, hash_slot, eq_slot);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        if (key_own == 1 && k) py23_rc_inc((void*)k);
        py23_dict_set(d, k, 0);  // 重复 key 时 dict_set 会退掉这份多余引用
    }
    return d;
}

/* 找到并移除 key；返回 1 时 val_out 是原值。字典对 key/val 的引用：
   key 立即释放（调用方传的是借用），value 的所有权转交给调用方
   （pop 返回给 Python 层，由临时栈接管）。entries 保持插入顺序：
   后面的项前移一格，然后重建索引。删除不在热路径上。 */
int dict_take(Py23Dict* d, int64_t key, int64_t* val_out) {
    int64_t h = dict_hash_key(d, key);
    int64_t e = dict_lookup(d, key, h);
    if (e < 0) return 0;
    *val_out = d->entries[e].val;
    if (d->key_own == 1) py23_rc_dec((void*)d->entries[e].key);
    --d->len;
    if (e < d->len)
        memmove(&d->entries[e], &d->entries[e + 1],
                sizeof(Py23DictEntry) * (size_t)(d->len - e));
    dict_reindex(d, d->icap);
    return 1;
}

int64_t py23_dict_pop(Py23Dict* d, int64_t key, int64_t has_default, int64_t def) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (has_default) return def;
        py23_raise_c("KeyError", "key not found");
    }
    return v;
}

Py23List* py23_dict_popitem(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "popitem(): dictionary is empty");
    Py23DictEntry e = d->entries[d->len - 1];
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* tup = py23_list_new(2, tk);
    if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
    py23_list_append(tup, e.key);
    if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
    py23_list_append(tup, e.val);
    d->len--;
    if (d->key_own == 1) py23_rc_dec((void*)e.key);
    if (d->val_own == 1) py23_rc_dec((void*)e.val);
    dict_reindex(d, d->icap);
    return tup;
}

/* set.pop()：移除并返回最后一个插入项（py23 的确定性选择）。 */
int64_t py23_dict_pop_last(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "pop from an empty set");
    int64_t key = d->entries[d->len - 1].key;
    d->len--;
    dict_reindex(d, d->icap);
    return key;  /* 所有权转交给调用方 */
}

void py23_dict_remove(Py23Dict* d, int64_t key, int64_t missing_ok) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (!missing_ok) py23_raise_c("KeyError", "key not found");
        return;
    }
    if (d->val_own == 1) py23_rc_dec((void*)v);
}

void py23_dict_clear(Py23Dict* d) {
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[i];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    d->len = 0;
    memset(d->index, 0, sizeof(int32_t) * (size_t)d->icap);
}
