// rt_obj.c —— 字符串/列表/字典/切片/对象/输出/math
#include "rt_internal.h"

/* ── 字符串 ───────────────────────────────────────────────────── */

Py23Str* py23_str_concat(Py23Str* a, Py23Str* b) {
    Py23Str* s = str_alloc(a->len + b->len);
    char* p = (char*)s->data;
    memcpy(p, a->data, (size_t)a->len);
    memcpy(p + a->len, b->data, (size_t)b->len);
    return s;
}

Py23Str* py23_str_char_at(Py23Str* s, int64_t i) {
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) py23_raise_c("IndexError", "string index out of range");
    return str_new(s->data + i, 1);
}

Py23Str* py23_str_from_i64(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return str_new(buf, n);
}

Py23Str* py23_str_from_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    return str_new(buf, (int64_t)strlen(buf));
}

Py23Str* py23_str_from_bool(int64_t v) {
    return v ? str_new("True", 4) : str_new("False", 5);
}

Py23Str* py23_str_from_none(void) { return str_new("None", 4); }

int64_t py23_str_eq(Py23Str* a, Py23Str* b) {
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}

/* 三路比较，供 `<` / `<=` / `>` / `>=` 用。
   UTF-8 的字节序与码点序一致，所以 memcmp 的结果就是 Python 的结果。 */
int64_t py23_str_cmp(Py23Str* a, Py23Str* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)n);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

/* ── 列表 ─────────────────────────────────────────────────────── */

void list_destroy(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind == 1)
        for (int64_t i = 0; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    free(l->data);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素类型
// 不可知，跳过（那些对象如果只被这个容器引用，会在同一轮清扫里被释放）。
void list_gc_release(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind != 1 || !l->data) return;
    for (int64_t i = 0; i < l->len; ++i) py23_gc_unref_child((void*)l->data[i]);
}

Py23List* py23_list_new(int64_t cap, int64_t elem_kind) {
    if (cap < 0) panic_c("negative list capacity");
    if (elem_kind < 0 || elem_kind > 2) panic_c("invalid list element kind");
    if (cap < 4) cap = 4;
    Py23List* l = (Py23List*)rc_alloc((int64_t)sizeof(Py23List), list_destroy,
                                      list_gc_release, PY23_RC_LIST);
    l->len = 0;
    l->cap = cap;
    l->elem_kind = elem_kind;
    l->data = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!l->data) panic_c("out of memory");
    return l;
}

/* 元素引用由生成代码在写入前 retain（kind 1/2 且源类型已知是堆），
   运行时不再重复 inc；kind 1 覆盖/截断/销毁时负责 release。 */
void py23_list_append(Py23List* l, int64_t v) {
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    l->data[l->len++] = v;
}

int64_t py23_list_get(Py23List* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list index out of range");
    return l->data[i];
}

void py23_list_set(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list assignment index out of range");
    int64_t old = l->data[i];
    l->data[i] = v;
    if (l->elem_kind == 1) py23_rc_dec((void*)old);
}

void py23_list_set_len(Py23List* l, int64_t n) {
    if (n < 0 || n > l->len) panic_c("invalid list length");
    if (l->elem_kind == 1)
        for (int64_t i = n; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    l->len = n;
}

/* 把 src 的元素接到 dst 末尾，供 *args 打包和切片拷贝使用。
   元素的引用按 dst 的 elem_kind 处理：kind 1 正常 inc；kind 2 只对
   已知是堆的源元素 inc（不 release，交给 GC）。 */
void py23_list_extend(Py23List* dst, Py23List* src) {
    int64_t slen = src->len;
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    if (dst == src) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        if (v && src->elem_kind == 1 &&
            (dst->elem_kind == 1 || dst->elem_kind == 2))
            py23_rc_inc((void*)v);
        py23_list_append(dst, v);
    }
    free(snap);
}

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

int list_value_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

int64_t py23_list_contains(Py23List* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return 1;
    return 0;
}

int64_t py23_str_contains(Py23Str* hay, Py23Str* needle) {
    if (needle->len == 0) return 1;
    if (needle->len > hay->len) return 0;
    for (int64_t i = 0; i + needle->len <= hay->len; ++i)
        if (memcmp(hay->data + i, needle->data, (size_t)needle->len) == 0) return 1;
    return 0;
}

/* ── 切片 ─────────────────────────────────────────────────────── */

/* 缺省边界用 INT64_MIN 当哨兵；显式写 INT64_MIN 的极端索引会被当成缺省。 */
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step,
                         int64_t* out_start, int64_t* out_stop, int64_t* out_step) {
    int has_start = start != INT64_MIN;
    int has_stop = stop != INT64_MIN;
    int has_step = step != INT64_MIN;
    if (!has_step) step = 1;
    if (step == 0) py23_raise_c("ValueError", "slice step cannot be zero");

    if (step > 0) {
        if (!has_start) start = 0;
        else {
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start > len) start = len;
        }
        if (!has_stop) stop = len;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = 0;
            if (stop > len) stop = len;
        }
    } else {
        if (!has_start) start = len - 1;
        else {
            if (start < 0) start += len;
            if (start < 0) start = -1;
            if (start >= len) start = len - 1;
        }
        if (!has_stop) stop = -1;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = -1;
            if (stop >= len) stop = len - 1;
        }
    }
    *out_start = start;
    *out_stop = stop;
    *out_step = step;
}

int64_t slice_total(int64_t start, int64_t stop, int64_t step) {
    if (step > 0) {
        if (start >= stop) return 0;
        return (stop - start + step - 1) / step;
    }
    if (start <= stop) return 0;
    return (start - stop + (-step) - 1) / (-step);
}

Py23List* py23_list_slice(Py23List* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    Py23List* out = py23_list_new(count, src->elem_kind);
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) {
        int64_t v = src->data[i];
        if (src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23Str* py23_str_slice(Py23Str* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    Py23Str* out = str_alloc(count);
    char* p = (char*)out->data;
    int64_t j = 0;
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) p[j++] = src->data[i];
    return out;
}

/* 切片赋值。src 的元素引用由本函数接管：kind 1 时对新元素 inc、
   对被移除的旧元素 dec；src == dst 时先做快照，避免边读边写。 */
void py23_list_slice_assign(Py23List* dst, int64_t start, int64_t stop, int64_t step,
                            Py23List* src) {
    int64_t lo, hi, st;
    slice_bounds(dst->len, start, stop, step, &lo, &hi, &st);
    int64_t n = slice_total(lo, hi, st);
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    int64_t slen = src->len;
    if (src == dst) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    int kind = (int)dst->elem_kind;
    if (st != 1) {
        if (slen != n) py23_raise_c("ValueError", "cannot assign sequence of different size to extended slice");
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = lo + i * st;
            int64_t v = sv[i];
            int64_t old = dst->data[idx];
            if (kind == 1 && v) py23_rc_inc((void*)v);
            else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
            dst->data[idx] = v;
            if (kind == 1 && old) py23_rc_dec((void*)old);
        }
        free(snap);
        return;
    }
    int64_t delta = slen - n;
    if (delta > 0) {
        if (dst->len + delta > dst->cap) {
            int64_t ncap = dst->cap ? dst->cap : 4;
            while (ncap < dst->len + delta) ncap *= 2;
            int64_t* nd = (int64_t*)realloc(dst->data, sizeof(int64_t) * (size_t)ncap);
            if (!nd) panic_c("out of memory");
            dst->data = nd;
            dst->cap = ncap;
        }
        memmove(&dst->data[hi + delta], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    } else if (delta < 0) {
        // [lo+slen, hi) 的旧元素会被下面的 memmove 覆盖，先释放。
        if (kind == 1)
            for (int64_t i = lo + slen; i < hi; ++i)
                if (dst->data[i]) py23_rc_dec((void*)dst->data[i]);
        memmove(&dst->data[lo + slen], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        int64_t old = dst->data[lo + i];
        if (kind == 1 && v) py23_rc_inc((void*)v);
        else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        dst->data[lo + i] = v;
        // 增长时 memmove 会把尾部元素复制到 [hi, hi+delta)，这些副本
        // 没有独立的引用计数；只有原区间 [lo, hi) 的旧元素归本列表所有。
        if (kind == 1 && old && (delta <= 0 || lo + i < hi))
            py23_rc_dec((void*)old);
    }
    free(snap);
}

Py23List* py23_str_to_list(Py23Str* s) {
    Py23List* out = py23_list_new(s->len, 1);
    for (int64_t i = 0; i < s->len; ++i)
        py23_list_append(out, (int64_t)py23_str_char_at(s, i));
    return out;
}

/* range(...) 作为值使用时实体化成整数列表。for 循环走的是专门的
   计数循环，不经过这里。 */
Py23List* py23_range_new(int64_t start, int64_t stop, int64_t step) {
    if (step == 0) py23_raise_c("ValueError", "range() arg 3 must not be zero");
    int64_t n;
    if (step > 0)
        n = stop > start ? (stop - start + step - 1) / step : 0;
    else
        n = stop < start ? (start - stop + (-step) - 1) / (-step) : 0;
    Py23List* out = py23_list_new(n, 0);
    for (int64_t i = 0, v = start; i < n; ++i, v += step)
        py23_list_append(out, v);
    return out;
}

/* ── 对象 ─────────────────────────────────────────────────────── */

/* 对象 = [vtable 指针][字段 0][字段 1]…。分配时全部清零：0 同时是
   None / 0 / 假，和栈槽的约定一致，所以「声明了但没赋值的字段」
   天然就是 None。vtable 指针由生成代码紧接着写入。
   dtor 由代码生成按类布局生成，只 dec 堆类型字段。 */
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*)) {
    if (size < 8) panic_c("object size too small");
    return rc_alloc(size, dtor, release, PY23_RC_OBJ);
}

/* ── 数学 / 内置模块 ──────────────────────────────────────────── */

double py23_math_sqrt(double x) {
    if (x < 0) py23_raise_c("ValueError", "math domain error");
    return sqrt(x);
}
int64_t py23_math_floor(double x) { return (int64_t)floor(x); }
int64_t py23_math_ceil(double x) { return (int64_t)ceil(x); }
double py23_math_fabs(double x) { return fabs(x); }
double py23_math_sin(double x) { return sin(x); }
double py23_math_cos(double x) { return cos(x); }
double py23_math_tan(double x) { return tan(x); }
double py23_math_log(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log(x);
}
double py23_math_log2(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log2(x);
}
double py23_math_log10(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log10(x);
}
double py23_math_exp(double x) { return exp(x); }
double py23_math_pow(double x, double y) { return pow(x, y); }
double py23_math_fmod(double x, double y) {
    if (y == 0) py23_raise_c("ValueError", "math domain error");
    return fmod(x, y);
}

Py23List* sys_argv_list;

void py23_sys_set_args(int64_t argc, char** argv) {
    Py23List* out = py23_list_new(argc > 0 ? argc : 0, 1);
    sys_argv_list = out;
    // 列表要立刻成为 GC 根；根表元素是「槽的地址」，不是槽里的值。
    void* root_slot[1] = {(void*)&sys_argv_list};
    py23_gc_add_roots(root_slot, 1);
    for (int64_t i = 0; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        Py23Str* str = str_new(s, (int64_t)strlen(s));
        py23_list_append(out, (int64_t)str);
    }
}

Py23List* py23_sys_argv(void) {
    if (!sys_argv_list) sys_argv_list = py23_list_new(0, 1);
    return sys_argv_list;
}

void py23_sys_exit(int64_t code) { exit((int)code); }

/* ── 输出 ─────────────────────────────────────────────────────── */

void py23_print_i64(int64_t v) { printf("%lld", (long long)v); }

void py23_print_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    fputs(buf, stdout);
}

void py23_print_bool(int64_t v) { fputs(v ? "True" : "False", stdout); }

void py23_print_str(Py23Str* s) { fwrite(s->data, 1, (size_t)s->len, stdout); }

void py23_print_none(void) { fputs("None", stdout); }

void py23_print_sep(void) { fputc(' ', stdout); }

void py23_print_end(void) { fputc('\n', stdout); }

void py23_panic(Py23Str* msg) {
    py23_raise_c("RuntimeError", msg->data);
}

void py23_panic_div0(void) {
    py23_raise_c("ZeroDivisionError", "integer division or modulo by zero");
}

void py23_panic_overflow(void) {
    py23_raise_c("OverflowError", "integer division result too large for int64");
}

/* ── 数学 ─────────────────────────────────────────────────────── */

int64_t py23_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}
