// rt_class.c —— 对象布局与类/类型运行期检查
#include "rt_internal.h"

/* ── 对象 ─────────────────────────────────────────────────────── */

/* 对象 = [vtable 指针][字段 0][字段 1]…。分配时全部清零：0 同时是
   None / 0 / 假，和栈槽的约定一致，所以「声明了但没赋值的字段」
   天然就是 None。vtable 指针由生成代码紧接着写入。
   dtor 由代码生成按类布局生成，只 dec 堆类型字段。 */
void* pa_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*)) {
    if (size < 8) panic_c("object size too small");
    return rc_alloc(size, dtor, release, PYADAMAS_RC_INSTANCE);
}

/* 值类型 struct：payload 与 C 结构体二进制一致，前面只有隐藏的 RC 头。
   字段只有标量，没有需要递归释放的堆引用，所以 dtor/release 为空。
   kind 独立于 INSTANCE：isinstance() 不会把 payload 当成 vtable。 */
void* pa_struct_new(int64_t size) {
    if (size < 1) panic_c("struct size must be positive");
    return rc_alloc(size, NULL, NULL, PYADAMAS_RC_STRUCT);
}

void* pa_struct_copy(void* src, int64_t size) {
    void* dst = pa_struct_new(size);
    memcpy(dst, src, (size_t)size);
    return dst;
}

/* 类模式的运行期类型检查。只认用户类实例；原始整数、str、list 等
   都不是类实例。查注册表保证对任意 unknown 机器字都安全。
   vtable 前 7 个字：MRO 元数据指针表、类编号、vtable、自身槽表、类名、
   动态属性槽偏移、__eq__ 槽号（unknown 域相等用，见 rt_any.c）。 */
static const int64_t* inst_vtable(void* obj) {
    if (!obj) return NULL;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h)) return NULL;
    if (h->kind != PYADAMAS_RC_INSTANCE) return NULL;
    return *(int64_t**)obj;
}

int64_t pa_isinstance(void* obj, int64_t class_id) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return 0;
    const int64_t* meta = vt - 7;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    if (!mro) return 0;
    int64_t n = mro[0];
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (m && m[1] == class_id) return 1;
    }
    return 0;
}


/* unknown 域 dunder 分派：元数据第 7 个字 -> [__eq__, __lt__, __hash__, -1]。
   which 越界/不是实例/没有实现都返回 NULL；调用方按签名 cast 后直接调用。
   调用点是 C 运行时 → 编译方法，方法内的 raise 走展开器（ASan 全量覆盖）。 */
void* pa_instance_dunder_fn(void* obj, int64_t which) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt || which < 0 || which > 2) return NULL;
    const int64_t* tab = (const int64_t*)(uintptr_t)(vt - 7)[6];
    if (!tab) return NULL;
    int64_t slot = tab[which];
    if (slot < 0) return NULL;
    return (void*)(uintptr_t)vt[slot];
}

/* 实例的运行期类编号；不是类实例返回 -1。读类属性时用它按实际类型
   选择存储槽——静态类型可能是基类，值却是子类。 */
int64_t pa_obj_class_id(void* obj) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return -1;
    return (vt - 7)[1];   /* 元数据第 1 个字是类编号 */
}

/* 跨模块类模式：类编号只在模块内唯一，用「模块名.类名」比较。 */
int64_t pa_is_named_class(void* obj, PyAdamasStr* qualname) {
    if (!qualname) return 0;
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return 0;
    const int64_t* meta = vt - 7;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    if (!mro) return 0;
    int64_t n = mro[0];
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        const char* cn = m ? (const char*)(uintptr_t)m[4] : NULL;
        if (!cn) continue;
        size_t l = strlen(cn);
        if (l == (size_t)qualname->len &&
            memcmp(cn, qualname->data, l) == 0)
            return 1;
    }
    return 0;
}

/* ── 运行期类对象 ───────────────────────────────────────────── */

static PyAdamasClass* as_class(void* p) {
    if (!p) return NULL;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (!rc_index_has(h)) return NULL;
    PyAdamasClass* c = (PyAdamasClass*)p;
    return c->magic == PYADAMAS_CLASS_MAGIC ? c : NULL;
}

static void class_destroy(void* p) {
    PyAdamasClass* c = (PyAdamasClass*)p;
    if (c->name) pa_rc_dec(c->name);
}

/* GC 清扫口径的出边归还（与 exc_value_gc_release 同模式）：字符串是
   索引叶子，不在堆链上，只靠 RC 释放。类对象若活到 GcShutdown（P4 异常类
   单例由注册表持永久引用），没有 release 回调的话 name 串永远到不了
   rc==0 → LeakSanitizer Direct leak。普通用户类单例在模块退出时经 RC
   先释放（走 class_destroy），这条回调对它们是纯加法。 */
static void class_gc_release(void* p) {
    PyAdamasClass* c = (PyAdamasClass*)p;
    if (c->name) pa_gc_unref_child(c->name);
}

void* pa_class_new(int64_t cid, int64_t ctor, int64_t* vtable, PyAdamasStr* name) {
    PyAdamasClass* c = (PyAdamasClass*)rc_alloc((int64_t)sizeof(PyAdamasClass),
                                                class_destroy, class_gc_release,
                                                PYADAMAS_RC_OBJ);
    c->magic = PYADAMAS_CLASS_MAGIC;
    c->cid = cid;
    c->ctor = ctor;
    c->vtable = vtable;
    c->name = name;  /* 接管调用方的引用 */
    return c;
}

int64_t pa_class_cid(void* cls) {
    PyAdamasClass* c = as_class(cls);
    return c ? c->cid : -1;
}

int64_t pa_class_ctor(void* cls) {
    PyAdamasClass* c = as_class(cls);
    return c ? c->ctor : 0;
}

void* pa_class_method(void* cls, int64_t slot) {
    PyAdamasClass* c = as_class(cls);
    if (!c || !c->vtable || slot < 0) return NULL;
    return (void*)(uintptr_t)c->vtable[slot];
}

PyAdamasStr* pa_class_name(void* cls) {
    PyAdamasClass* c = as_class(cls);
    if (!c || !c->name) return pa_str_none();
    /* 跨模块类对象存的是限定名，__name__ 只取最后一段。 */
    const char* p = c->name->data;
    const char* dot = NULL;
    for (const char* q = p; *q; ++q)
        if (*q == '.') dot = q;
    const char* start = dot ? dot + 1 : p;
    int64_t len = dot ? c->name->len - (int64_t)(start - p) : c->name->len;
    /* 总是返回新字符串：调用方按临时堆值管理。 */
    return str_new(start, len);
}

PyAdamasStr* pa_class_repr(void* cls) {
    PyAdamasClass* c = as_class(cls);
    const char* name = c && c->name ? c->name->data : "?";
    int64_t n = c && c->name ? c->name->len : 1;
    /* 类对象存的是限定名；repr 与 __name__ 一样只显示最后一段。
       P4（lang_gaps）例外：自定义异常类按 CPython 显示完整限定名
       （<class '__main__.MyErr'>）。判据是裸名在异常注册表里——异常类
       对象只在门控开时存在，普通用户类的 repr 口径一字不变。 */
    const char* start = name;
    for (int64_t i = 0; i < n; ++i)
        if (name[i] == '.') start = name + i + 1;
    if (start != name && pa_exc_is_registered(start))
        start = name;   /* 完整限定名 */
    n -= (int64_t)(start - name);
    const char* pre = "<class '";
    const char* post = "'>";
    int64_t plen = 8, slen = 2;
    PyAdamasStr* s = str_alloc_raw(plen + n + slen);
    char* p = (char*)s->data;
    memcpy(p, pre, (size_t)plen);
    memcpy(p + plen, start, (size_t)n);
    memcpy(p + plen + n, post, (size_t)slen);
    return str_seal(s);
}

int64_t pa_isinstance_class(void* obj, void* cls) {
    PyAdamasClass* c = as_class(cls);
    if (!c) {
        /* type(x) 之类返回的字符串：按类名比较。 */
        if (pa_is_str(cls)) {
            PyAdamasStr* s = (PyAdamasStr*)cls;
            return pa_is_named_class(obj, s);
        }
        return 0;
    }
    /* 类编号只在模块内唯一；类对象存的是限定名，统一按限定名比较，
       避免导入模块的对象 MRO 里出现同号的别的类。 */
    if (c->name) return pa_is_named_class(obj, c->name);
    if (c->cid >= 0) return pa_isinstance(obj, c->cid);
    return 0;
}

/* super()：从实例的动态 MRO 里找 after 类之后的第一个「自己声明了
   该槽」的类，返回它的实现。没有就按 AttributeError 抛。
   类编号只在模块内唯一，跨模块 super 必须按限定名定位 after 类。 */
void* pa_super_lookup(void* obj, PyAdamasStr* after_qualname, int64_t slot,
                      PyAdamasStr* name) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) {
        pa_raise_c("AttributeError", "'super' object has no attribute");
        return NULL;
    }
    const int64_t* meta = vt - 7;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    int64_t n = mro ? mro[0] : 0;
    int found = 0;
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (!m) continue;
        if (!found) {
            const char* cn = (const char*)(uintptr_t)m[4];
            if (after_qualname && cn && strlen(cn) == (size_t)after_qualname->len &&
                memcmp(cn, after_qualname->data, (size_t)after_qualname->len) == 0)
                found = 1;
            continue;
        }
        const int64_t* own = (const int64_t*)(uintptr_t)m[3];
        if (!own) continue;
        int64_t k = own[0];
        for (int64_t j = 1; j <= k; ++j) {
            if (own[j] != slot) continue;
            const int64_t* ovt = (const int64_t*)(uintptr_t)m[2];
            if (ovt) return (void*)(uintptr_t)ovt[slot];
        }
    }
    char buf[192];
    if (name && name->len >= 0 && name->len < 150) {
        snprintf(buf, sizeof buf, "'super' object has no attribute '%.*s'",
                 (int)name->len, name->data);
    } else {
        snprintf(buf, sizeof buf, "'super' object has no attribute");
    }
    pa_raise_c("AttributeError", buf);
    return NULL;
}

int64_t pa_is_exc(void* obj, PyAdamasStr* type) {
    if (!obj || !type) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    /* P4（lang_gaps）：富异常实例（vtable 对象）也按异常类名判父子。
       自定义异常类在 pa_exc_register 里登记的是裸类名，MRO 元数据存的是
       限定名——取最后一段再走 exc_is_a。门控关时异常类当值不存在，
       isinstance(x, ValueError) 的第二参是 0，到不了这条新分支。 */
    if (h->kind == PYADAMAS_RC_INSTANCE) {
        const int64_t* vt = *(const int64_t**)obj;
        if (!vt) return 0;
        const int64_t* meta = vt - 7;
        const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
        if (!mro || mro[0] < 1) return 0;
        const int64_t* m = (const int64_t*)(uintptr_t)mro[1];
        const char* cn = m ? (const char*)(uintptr_t)m[4] : NULL;
        if (!cn) return 0;
        const char* base = strrchr(cn, '.');
        base = base ? base + 1 : cn;
        char want[128];
        int64_t n = type->len;
        if (n < 0 || n >= (int64_t)sizeof(want)) return 0;
        memcpy(want, type->data, (size_t)n);
        want[n] = '\0';
        return exc_is_a(base, want) ? 1 : 0;
    }
    if (h->kind != PYADAMAS_RC_OBJ || h->size < (int64_t)sizeof(PyAdamasExcValue)) return 0;
    PyAdamasExcValue* v = (PyAdamasExcValue*)obj;
    if (v->magic != PYADAMAS_EXCVAL_MAGIC) return 0;
    char want[128];
    int64_t n = type->len;
    if (n < 0 || n >= (int64_t)sizeof(want)) return 0;
    memcpy(want, type->data, (size_t)n);
    want[n] = '\0';
    return exc_is_a(v->type, want) ? 1 : 0;
}

int64_t pa_is_seq(void* obj) {
    if (!obj) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PYADAMAS_RC_LIST ? 1 : 0;
}

int64_t pa_is_dict(void* obj) {
    if (!obj) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PYADAMAS_RC_DICT ? 1 : 0;
}

/* unknown 值上的字符串/复数模式：先按 RC 注册表确认载荷类型，再比较。 */
int64_t pa_str_eq_unknown(void* obj, PyAdamasStr* s) {
    if (!obj || !s) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_STR) return 0;
    return pa_str_eq((PyAdamasStr*)obj, s);
}

int64_t pa_complex_eq_unknown(void* obj, PyAdamasComplex* c) {
    if (!obj || !c) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ ||
        h->size < (int64_t)sizeof(PyAdamasComplex))
        return 0;
    PyAdamasComplex* x = (PyAdamasComplex*)obj;
    if (x->magic != PYADAMAS_COMPLEX_MAGIC) return 0;
    return pa_complex_eq(x, c);
}

int64_t pa_is_str(void* obj) {
    if (!obj) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PYADAMAS_RC_STR ? 1 : 0;
}

/* ── 动态属性 ─────────────────────────────────────────────────
   带动态属性的类在实例末尾留一个字，指向 dict[str, unknown]；偏移由
   vtable 元数据第 6 个字给出（0 = 没有槽）。槽在第一次 setattr 时
   惰性分配，不用动态属性的类一个字节都不多花。 */
static void** attr_slot(void* obj) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return NULL;
    int64_t off = vt[-2];  // 元数据第 6 字（dict_off）：7 字元数据块 = vt[-7..-1]，偏移取 vt[-2]
    if (off <= 0) return NULL;
    return (void**)((char*)obj + off);
}

static PyAdamasDict* attr_dict(void* obj) {
    void** s = attr_slot(obj);
    return s ? (PyAdamasDict*)*s : NULL;
}

int64_t pa_attr_has(void* obj, PyAdamasStr* name) {
    PyAdamasDict* d = attr_dict(obj);
    if (!d || !name) return 0;
    return pa_dict_contains(d, (int64_t)(uintptr_t)name);
}

int64_t pa_attr_get(void* obj, PyAdamasStr* name, int64_t dflt) {
    PyAdamasDict* d = attr_dict(obj);
    if (!d || !name) return dflt;
    return pa_dict_get_or(d, (int64_t)(uintptr_t)name, dflt);
}

/* vars() 合并用：返回实例的动态属性表（借用，可能 NULL）。 */
PyAdamasDict* pa_attr_dict_view(void* obj) {
    return attr_dict(obj);
}

/* for x in unknown / list(unknown)：按运行期种类物化成 list[unknown]。
   list/tuple 原样 +1 返回（零拷贝）；str 拆码点；dict/set 取键。
   返回的列表带一份引用（调用方 push_temp 接管）。bytes 与 str 同形
   （无运行期标签），按 str 口径拆——文档化边界。
   放在 rt_class.c（与动态属性同族），不动 rt_any.c（P3 并行在改）。 */
PyAdamasList* pa_any_iter_list(int64_t v) {
    if (!pa_unknown_is_heap(v)) {
        if (v == 0) pa_raise_c("TypeError", "'NoneType' object is not iterable");
        pa_raise_c("TypeError", "'int' object is not iterable");
    }
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_LIST:
            pa_rc_inc((void*)(uintptr_t)v);
            return (PyAdamasList*)(uintptr_t)v;
        case PYADAMAS_RC_STR:
            return pa_str_to_list((PyAdamasStr*)(uintptr_t)v);
        case PYADAMAS_RC_DICT:
            return pa_dict_keys((PyAdamasDict*)(uintptr_t)v);
        default:
            pa_raise_c("TypeError", "object is not iterable");
            return NULL;
    }
}


/* P2（0.340，PYADAMAS_TYPE_TAGS）：带 tag 的迭代物化。tag 有精确信息时
   免探针（关键收益：**未被 RC 索引跟踪的标量元组**也能迭代 —— 旧的
   pa_any_iter_list 靠探针判堆，标量元组探针落空后报 'int' not iterable）。
   tag 6/7（None/bool）与非堆标签按 CPython 口径报 TypeError；UNK 回退
   旧探针路径。 */
PyAdamasList* pa_any_iter_list_t(int64_t v, int64_t tag) {
    switch (tag) {
        case PA_TAG_TUPLE: {
            pa_rc_inc((void*)(uintptr_t)v);
            return (PyAdamasList*)(uintptr_t)v;
        }
        case PA_TAG_SET:
            return pa_dict_keys((PyAdamasDict*)(uintptr_t)v);
        case PA_TAG_STR:
        case PA_TAG_BYTES:
            return pa_str_to_list((PyAdamasStr*)(uintptr_t)v);
        case PA_TAG_HEAP: {
            PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
            switch (h->kind) {
                case PYADAMAS_RC_LIST:
                    pa_rc_inc((void*)(uintptr_t)v);
                    return (PyAdamasList*)(uintptr_t)v;
                case PYADAMAS_RC_DICT:
                    return pa_dict_keys((PyAdamasDict*)(uintptr_t)v);
                case PYADAMAS_RC_STR:
                    return pa_str_to_list((PyAdamasStr*)(uintptr_t)v);
                default:
                    pa_raise_c("TypeError", "object is not iterable");
                    return NULL;
            }
        }
        case PA_TAG_NONE:
            pa_raise_c("TypeError", "'NoneType' object is not iterable");
            return NULL;
        case PA_TAG_UNK:
            return pa_any_iter_list(v);
        default:
            /* INT / BOOL / FRAW / FBOX / CLASS / BIG：不可迭代 */
            if (tag == PA_TAG_BOOL)
                pa_raise_c("TypeError", "'bool' object is not iterable");
            else if (tag == PA_TAG_INT)
                pa_raise_c("TypeError", "'int' object is not iterable");
            else if (tag == PA_TAG_FRAW || tag == PA_TAG_FBOX)
                pa_raise_c("TypeError", "'float' object is not iterable");
            else
                pa_raise_c("TypeError", "object is not iterable");
            return NULL;
    }
}

/* P2：unknown dict 的 (key, value) 对物化（pprint 递归取键值用）。
   未知值不支持方法调用（.items() 在 unknown 上被 codegen 拒绝），所以
   由运行时按类型码取表。只有 dict 可以；set 用迭代（键即元素）。 */
PyAdamasList* pa_any_dict_items(int64_t v, int64_t tag) {
    PyAdamasDict* d = NULL;
    if (tag == PA_TAG_HEAP) {
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
        if (h->kind == PYADAMAS_RC_DICT) d = (PyAdamasDict*)(uintptr_t)v;
    } else if (tag == PA_TAG_UNK) {
        if (pa_unknown_is_heap(v)) {
            PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
            if (h->kind == PYADAMAS_RC_DICT) d = (PyAdamasDict*)(uintptr_t)v;
        }
    } else if (tag >= PA_TAG_FBOX && tag <= PA_TAG_STR) {
        /* FBOX/STR 不可能是 dict；其它精确堆子类也不是 dict。 */
    }
    if (!d) {
        pa_raise_c("TypeError", "object is not a dict");
        return NULL;
    }
    return pa_dict_items(d);
}
void pa_attr_set(void* obj, PyAdamasStr* name, int64_t value) {
    void** s = attr_slot(obj);
    if (!name || !s) panic_c("setattr() on an object without dynamic attributes");
    PyAdamasDict* d = (PyAdamasDict*)*s;
    if (!d) {
        d = pa_dict_new(1, 1, 2, -1, -1);
        *s = d;
    }
    pa_own_inc(1, (int64_t)(uintptr_t)name);
    pa_own_inc(2, value);
    pa_dict_set(d, (int64_t)(uintptr_t)name, value);
}

int64_t pa_is_complex(void* obj) {
    if (!obj) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ ||
        h->size < (int64_t)sizeof(PyAdamasComplex))
        return 0;
    return ((PyAdamasComplex*)obj)->magic == PYADAMAS_COMPLEX_MAGIC ? 1 : 0;
}

