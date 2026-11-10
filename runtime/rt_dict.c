// rt_dict.c —— 字典与集合
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

int64_t dict_probe(Py23DictEntry* entries, int64_t cap, int64_t kind,
                          int64_t key, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) {
        if (entries[i].hash == hash && dict_key_eq(kind, entries[i].key, key)) return i;
        i = (i + 1) & mask;
    }
    return -1;
}

int64_t dict_empty_slot(Py23DictEntry* entries, int64_t cap, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) i = (i + 1) & mask;
    return i;
}

void dict_resize(Py23Dict* d, int64_t ncap) {
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)ncap, sizeof(Py23DictEntry));
    int64_t* no = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
    if (!ne || !no) panic_c("out of memory");
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        int64_t mask = ncap - 1;
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        no[i] = slot;
    }
    free(d->entries);
    free(d->order);
    d->entries = ne;
    d->order = no;
    d->cap = ncap;
}

void dict_destroy(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    free(d->entries);
    free(d->order);
}

void dict_gc_release(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_gc_unref_child((void*)e.key);
        if (d->val_own == 1) py23_gc_unref_child((void*)e.val);
    }
}

Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own) {
    if (key_kind < 0 || key_kind > 3) panic_c("invalid dict key kind");
    if (key_own < 0 || key_own > 2 || val_own < 0 || val_own > 2)
        panic_c("invalid dict ownership kind");
    int64_t cap = 8;
    Py23Dict* d =
        (Py23Dict*)rc_alloc((int64_t)sizeof(Py23Dict), dict_destroy, dict_gc_release,
                            PY23_RC_DICT);
    d->len = 0;
    d->cap = cap;
    d->entries = (Py23DictEntry*)calloc((size_t)cap, sizeof(Py23DictEntry));
    d->order = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!d->entries || !d->order) panic_c("out of memory");
    d->key_kind = key_kind;
    d->key_own = key_own;
    d->val_own = val_own;
    return d;
}

/* 调用方已经把 key/value 的「入表引用」retain 好；更新已有 key 时
   释放这份多余的新 key 引用，字典继续保留原来的 key 对象。 */
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot >= 0) {
        if (d->key_own == 1) py23_rc_dec((void*)key);
        if (d->val_own == 1) py23_rc_dec((void*)d->entries[slot].val);
        d->entries[slot].val = val;
        return;
    }
    if ((d->len + 1) * 10 >= d->cap * 7) dict_resize(d, d->cap * 2);
    slot = dict_empty_slot(d->entries, d->cap, h);
    d->entries[slot].hash = h;
    d->entries[slot].key = key;
    d->entries[slot].val = val;
    d->order[d->len++] = slot;
}

int64_t py23_dict_get(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) py23_raise_c("KeyError", "key not found");
    return d->entries[slot].val;
}

int64_t py23_dict_contains(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    return dict_probe(d->entries, d->cap, d->key_kind, key, h) >= 0 ? 1 : 0;
}

int64_t py23_dict_key_at(Py23Dict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    return d->entries[d->order[i]].key;
}

int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    return slot >= 0 ? d->entries[slot].val : def;
}

int own_to_list_kind(int64_t own) {
    return own == 1 ? 1 : (own == 0 ? 0 : 2);
}

Py23List* py23_dict_keys(Py23Dict* d) {
    int kind = own_to_list_kind(d->key_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].key;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_values(Py23Dict* d) {
    int kind = own_to_list_kind(d->val_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].val;
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
        Py23DictEntry e = d->entries[d->order[i]];
        Py23List* tup = py23_list_new(2, tk);
        if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
        py23_list_append(tup, e.key);
        if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
        py23_list_append(tup, e.val);
        py23_list_append(out, (int64_t)tup);  // tuple 的初始引用交给外层 list
    }
    return out;
}

void py23_dict_update(Py23Dict* dst, Py23Dict* src) {
    if (dst == src) return;
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[src->order[i]];
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
    Py23Dict* out = py23_dict_new(src->key_kind, src->key_own, src->val_own);
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[src->order[i]];
        if (src->key_own == 1 && e.key) py23_rc_inc((void*)e.key);
        if (src->val_own == 1 && e.val) py23_rc_inc((void*)e.val);
        py23_dict_set(out, e.key, e.val);
    }
    return out;
}

Py23Dict* py23_set_from_list(Py23List* src, int64_t key_kind, int64_t key_own) {
    Py23Dict* d = py23_dict_new(key_kind, key_own, 0);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        if (key_own == 1 && k) py23_rc_inc((void*)k);
        py23_dict_set(d, k, 0);  // 重复 key 时 dict_set 会退掉这份多余引用
    }
    return d;
}

/* 删除用「移出 order + 全表重建」：线性探查没有墓碑，直接挖空会截断
   后续 key 的探查链。重建是 O(n)，删除本来也不在热路径上。 */
void dict_rebuild(Py23Dict* d) {
    Py23DictEntry* old = d->entries;
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)d->cap, sizeof(Py23DictEntry));
    if (!ne) panic_c("out of memory");
    int64_t mask = d->cap - 1;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = old[d->order[i]];
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        d->order[i] = slot;
    }
    free(old);
    d->entries = ne;
}

/* 找到并移除 key；返回 1 时 val_out 是原值。字典对 key/val 的引用：
   key 立即释放（调用方传的是借用），value 的所有权转交给调用方
   （pop 返回给 Python 层，由临时栈接管）。 */
int dict_take(Py23Dict* d, int64_t key, int64_t* val_out) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) return 0;
    *val_out = d->entries[slot].val;
    int64_t oi = -1;
    for (int64_t i = 0; i < d->len; ++i)
        if (d->order[i] == slot) {
            oi = i;
            break;
        }
    if (oi < 0) panic_c("dict order corrupted");
    if (d->key_own == 1) py23_rc_dec((void*)d->entries[slot].key);
    for (int64_t i = oi; i + 1 < d->len; ++i) d->order[i] = d->order[i + 1];
    --d->len;
    dict_rebuild(d);
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
    int64_t slot = d->order[d->len - 1];
    Py23DictEntry e = d->entries[slot];
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
    dict_rebuild(d);
    return tup;
}

/* set.pop()：移除并返回最后一个插入项（py23 的确定性选择）。 */
int64_t py23_dict_pop_last(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "pop from an empty set");
    int64_t slot = d->order[d->len - 1];
    int64_t key = d->entries[slot].key;
    d->len--;
    dict_rebuild(d);
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
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    d->len = 0;
    memset(d->entries, 0, sizeof(Py23DictEntry) * (size_t)d->cap);
}

