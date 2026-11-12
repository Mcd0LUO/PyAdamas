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

/* 类模式的运行期类型检查。只认用户类实例；原始整数、str、list 等
   都不是类实例。查注册表保证对任意 unknown 机器字都安全。
   vtable 前 5 个字：MRO 元数据指针表、类编号、vtable、自身槽表、类名。 */
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
    const int64_t* meta = vt - 5;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    if (!mro) return 0;
    int64_t n = mro[0];
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (m && m[1] == class_id) return 1;
    }
    return 0;
}

/* 实例的运行期类编号；不是类实例返回 -1。读类属性时用它按实际类型
   选择存储槽——静态类型可能是基类，值却是子类。 */
int64_t pa_obj_class_id(void* obj) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return -1;
    return (vt - 5)[1];   /* 元数据第 1 个字是类编号 */
}

/* 跨模块类模式：类编号只在模块内唯一，用「模块名.类名」比较。 */
int64_t pa_is_named_class(void* obj, PyAdamasStr* qualname) {
    if (!qualname) return 0;
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return 0;
    const int64_t* meta = vt - 5;
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

/* super()：从实例的动态 MRO 里找 after 类之后的第一个「自己声明了
   该槽」的类，返回它的实现。没有就按 AttributeError 抛。 */
void* pa_super_lookup(void* obj, int64_t after_cid, int64_t slot, PyAdamasStr* name) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) {
        pa_raise_c("AttributeError", "'super' object has no attribute");
        return NULL;
    }
    const int64_t* meta = vt - 5;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    int64_t n = mro ? mro[0] : 0;
    int found = 0;
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (!m) continue;
        if (!found) {
            if (m[1] == after_cid) found = 1;
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

int64_t pa_is_complex(void* obj) {
    if (!obj) return 0;
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ ||
        h->size < (int64_t)sizeof(PyAdamasComplex))
        return 0;
    return ((PyAdamasComplex*)obj)->magic == PYADAMAS_COMPLEX_MAGIC ? 1 : 0;
}

