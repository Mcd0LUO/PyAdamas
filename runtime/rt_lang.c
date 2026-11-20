/* rt_lang.c —— P4 语言缺口（PYADAMAS_LANG_GAPS，0.343 预留）的运行期支撑
 *
 * 设计：docs/numeric-types-design.md §3/§4 P4。本文件只做**加法**：
 * 所有入口只被门控开时发射的代码调用，门控关的程序一个字节都不碰。
 *
 * 两块设施：
 *
 * 1. 内建异常类当值（解锁 contextlib.suppress）。内建异常以前只有名字
 *    （pa_exc_register 登记父子名），值位置退化成 0。这里给每个异常名
 *    一个运行期类对象单例（pa_class_new，cid=-1 / 无 vtable / 名字=异常名），
 *    过 unknown 带 CLASS tag=11（P2 预留），同一性/比较/repr 都成立。
 *    单例由注册表持有一份永久引用，并登记成 GC 根（C 全局不在帧槽/临时栈
 *    里，major GC 看不见——与 rt_exc.c 的 exc_gc_roots_ensure 同一个坑）。
 *
 * 2. 方法名→槽的运行期登记表（解锁 closing / file 协议泛化）。pa 的
 *    vtable 槽号是编译期常量，运行期没有名字→槽映射；unknown 值上的
 *    方法调用被 codegen 拒绝。门控开时类定义点把「限定名.方法名→槽」
 *    登记进来，pa_method_call0 按实例 MRO 逐类查表做零参动态调用。
 *    只支持零参方法（close 等）：带参动态调用的 ABI 装填是后续工作。
 *
 * 内存所有权：注册表节点与名字串是 malloc 的，atexit 统一还（进程退出
 * 路径 GcShutdown 已把堆对象全清，这里**不能**再 dec 类对象——会 UAF）。
 * 类对象本体由 GC/RC 正常管理：注册表那份引用保证它活到进程退出。
 */
#include "rt_internal.h"

/* ── 内建异常类单例 ─────────────────────────────────────────── */

typedef struct ExcClassReg {
    char* name;
    PyAdamasClass* cls;   /* 永久引用（rc>=1）；地址登记成 GC 根 */
    struct ExcClassReg* next;
} ExcClassReg;

static ExcClassReg* exc_class_regs;

/* 与 rt_class.c 的 as_class 同口径（那边是 static，这里复制一份）：
   先查 RC 注册表确认是堆对象，再验类魔数。任意 unknown 字都安全。 */
static PyAdamasClass* lang_as_class(void* p) {
    if (!p) return NULL;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (!rc_index_has(h)) return NULL;
    PyAdamasClass* c = (PyAdamasClass*)p;
    return c->magic == PYADAMAS_CLASS_MAGIC ? c : NULL;
}

void* pa_exc_class_obj(PyAdamasStr* name) {
    if (!name || name->len < 0 || name->len >= 128)
        pa_raise_c("ValueError", "exception class name too long");
    char buf[128];
    memcpy(buf, name->data, (size_t)name->len);
    buf[name->len] = '\0';
    for (ExcClassReg* r = exc_class_regs; r; r = r->next)
        if (strcmp(r->name, buf) == 0) {
            pa_rc_inc(r->cls);
            return r->cls;
        }
    ExcClassReg* r = (ExcClassReg*)malloc(sizeof(ExcClassReg));
    if (!r) panic_c("out of memory");
    r->name = (char*)malloc((size_t)name->len + 1);
    if (!r->name) panic_c("out of memory");
    memcpy(r->name, buf, (size_t)name->len + 1);
    /* 构造期间可能触发 GC（str_new / rc_alloc）：先挂根再建对象。
       根槽就是 r->cls 的地址，建好后赋值即被扫到。 */
    r->cls = NULL;
    r->next = exc_class_regs;
    exc_class_regs = r;
    pa_gc_add_roots((void**)&r->cls, 1);
    PyAdamasStr* nm = str_new(buf, name->len);
    r->cls = (PyAdamasClass*)pa_class_new(-1, 0, NULL, nm);
    pa_rc_inc(r->cls);   /* 调用方拿到 +1；注册表留创建时那份 */
    return r->cls;
}

/* 类对象的名字：内建异常类是裸名（ValueError），用户类是限定名
   （mod.MyErr）。异常父子登记表（pa_exc_register）用裸类名，统一取
   最后一段再走 exc_is_a——与 except 的既有匹配口径一致。 */
static const char* lang_plain_name(const char* q) {
    const char* dot = strrchr(q, '.');
    return dot ? dot + 1 : q;
}

/* unknown 字是不是运行期类对象（探针 + 魔数，任意机器字安全）。
   str/print/repr 的探针路径用它把类对象印成 <class '...'>，而不是指针数。
   门控关时类对象值不存在，这些分支永远不会命中。 */
int64_t pa_is_class_obj(void* v) {
    return lang_as_class(v) ? 1 : 0;
}

/* suppress.__exit__：with 协议给的 et 是异常类型名（str），存起来的
   异常类是类对象值。按名字走 exc_is_a 父子链（含自定义异常注册表）。 */
int64_t pa_exc_class_is_a(PyAdamasStr* name, void* cls) {
    if (!name) return 0;
    if (!pa_is_str(name))
        pa_raise_c("TypeError", "issubclass() arg 1 must be a class");
    PyAdamasClass* c = lang_as_class(cls);
    if (!c || !c->name)
        pa_raise_c("TypeError", "issubclass() arg 2 must be a class or tuple of classes");
    char buf[128];
    if (name->len < 0 || name->len >= (int64_t)sizeof(buf)) return 0;
    memcpy(buf, name->data, (size_t)name->len);
    buf[name->len] = '\0';
    return exc_is_a(buf, lang_plain_name(c->name->data)) ? 1 : 0;
}

/* issubclass(异常类对象, 异常类对象)：运行期按名字判父子。 */
int64_t pa_exc_subclass(void* a, void* b) {
    PyAdamasClass* ca = lang_as_class(a);
    PyAdamasClass* cb = lang_as_class(b);
    if (!ca || !ca->name)
        pa_raise_c("TypeError", "issubclass() arg 1 must be a class");
    if (!cb || !cb->name)
        pa_raise_c("TypeError", "issubclass() arg 2 must be a class or tuple of classes");
    return exc_is_a(lang_plain_name(ca->name->data), lang_plain_name(cb->name->data)) ? 1 : 0;
}

/* 异常类对象值当构造器：E = ValueError; E("x")。名字从类对象读，
   不用静态内层名（形参回填可能留着首个调用点的类型）。用户类对象存
   限定名（mod.MyErr），异常匹配登记的是裸名——取末段构造，否则
   except MyErr 匹配不上（exc_is_a 按裸名查登记表）。 */
static PyAdamasStr* lang_exc_type_name(PyAdamasClass* c) {
    const char* plain = lang_plain_name(c->name->data);
    return str_new(plain, (int64_t)strlen(plain));
}

PyAdamasExcValue* pa_exc_class_value_new(void* cls, PyAdamasStr* msg) {
    PyAdamasClass* c = lang_as_class(cls);
    if (!c || !c->name)
        pa_raise_c("TypeError", "exceptions must derive from BaseException");
    PyAdamasStr* tn = lang_exc_type_name(c);
    pa_rc_temp(tn);   /* 构造期间可能触发 GC：临时栈保命 */
    PyAdamasExcValue* v = pa_exc_value_new(tn, msg);
    pa_rc_temp_take(tn);
    pa_rc_dec(tn);
    return v;
}

PyAdamasExcValue* pa_exc_class_value_new_args(void* cls, PyAdamasList* args) {
    PyAdamasClass* c = lang_as_class(cls);
    if (!c || !c->name)
        pa_raise_c("TypeError", "exceptions must derive from BaseException");
    PyAdamasStr* tn = lang_exc_type_name(c);
    pa_rc_temp(tn);
    PyAdamasExcValue* v = pa_exc_value_new_args(tn, args);
    pa_rc_temp_take(tn);
    pa_rc_dec(tn);
    return v;
}

/* ── 方法名→槽登记表（零参动态调用） ────────────────────────── */

typedef struct MethodReg {
    char* qual;     /* 限定名 mod.Class（与 vtable MRO 元数据 m[4] 同口径） */
    char* meth;
    int64_t slot;
    struct MethodReg* next;
} MethodReg;

static MethodReg* method_regs;

static char* lang_strdup(PyAdamasStr* s) {
    char* p = (char*)malloc((size_t)s->len + 1);
    if (!p) panic_c("out of memory");
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = '\0';
    return p;
}

void pa_method_register(PyAdamasStr* qual, PyAdamasStr* meth, int64_t slot) {
    if (!qual || !meth || slot < 0) return;
    MethodReg* r = (MethodReg*)malloc(sizeof(MethodReg));
    if (!r) panic_c("out of memory");
    r->qual = lang_strdup(qual);
    r->meth = lang_strdup(meth);
    r->slot = slot;
    r->next = method_regs;   /* 新登记在前：重定义类时新槽胜出 */
    method_regs = r;
}

static const MethodReg* lang_method_lookup(const char* qual, const char* meth) {
    for (const MethodReg* r = method_regs; r; r = r->next)
        if (strcmp(r->qual, qual) == 0 && strcmp(r->meth, meth) == 0)
            return r;
    return NULL;
}

/* 零参动态方法调用。分派口径：
   - file 对象（RC OBJ + sizeof(PyAdamasFile)，pa_is_file）：close 直调
     pa_file_close；其它名字报 AttributeError。
   - 类实例（RC INSTANCE）：沿 MRO 逐类查登记表，命中且槽在该类
     own_slots 里（尊重覆盖）才调该类 vtable 的实现——与 pa_super_lookup
     同一套元数据布局（meta=vt-6，m[2]=vtable / m[3]=own_slots / m[4]=名）。
   - 其它：AttributeError。
   返回值原样交出；只用于返回 None/标量的方法（close）。返回堆值的
   方法在这里会丢引用——调用方（closing）只发 close，文档化边界。 */
int64_t pa_method_call0(void* obj, PyAdamasStr* name) {
    char meth[128];
    if (!name || name->len < 0 || name->len >= (int64_t)sizeof(meth))
        pa_raise_c("AttributeError", "method name too long");
    memcpy(meth, name->data, (size_t)name->len);
    meth[name->len] = '\0';
    if (!obj) {
        char buf[192];
        snprintf(buf, sizeof buf, "'NoneType' object has no attribute '%s'", meth);
        pa_raise_c("AttributeError", buf);
    }
    if (pa_is_file(obj)) {
        if (strcmp(meth, "close") == 0) {
            pa_file_close((PyAdamasFile*)obj);
            return 0;
        }
        char buf[192];
        snprintf(buf, sizeof buf, "'file' object has no attribute '%s'", meth);
        pa_raise_c("AttributeError", buf);
    }
    PyAdamasRc* h = (PyAdamasRc*)obj - 1;
    if (rc_index_has(h) && h->kind == PYADAMAS_RC_INSTANCE) {
        const int64_t* vt = *(const int64_t**)obj;
        if (vt) {
            const int64_t* meta = vt - 6;
            const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
            int64_t n = mro ? mro[0] : 0;
            for (int64_t i = 1; i <= n; ++i) {
                const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
                if (!m) continue;
                const char* cn = (const char*)(uintptr_t)m[4];
                if (!cn) continue;
                const MethodReg* r = lang_method_lookup(cn, meth);
                if (!r) continue;
                /* 槽必须在该类自己声明的槽表里：继承来的实现由声明类
                   自己的登记负责，避免拿别类的槽号读本类 vtable。 */
                const int64_t* own = (const int64_t*)(uintptr_t)m[3];
                int64_t k = own ? own[0] : 0;
                int found = 0;
                for (int64_t j = 1; j <= k; ++j)
                    if (own[j] == r->slot) { found = 1; break; }
                if (!found) continue;
                const int64_t* ovt = (const int64_t*)(uintptr_t)m[2];
                if (!ovt) continue;
                int64_t (*fn)(void*) = (int64_t (*)(void*))(uintptr_t)ovt[r->slot];
                if (!fn) continue;
                return fn(obj);
            }
        }
    }
    char buf[192];
    snprintf(buf, sizeof buf, "object has no attribute '%s'", meth);
    pa_raise_c("AttributeError", buf);
    return 0;
}

/* ── 退出清理 ─────────────────────────────────────────────────
   只还 malloc 的登记表节点与名字串。类对象本体是 RC 堆对象：正常退出
   路径 GcShutdown 已按「整堆皆垃圾」清掉，这里再 dec 就是 UAF。 */
static void lang_shutdown(void) {
    while (exc_class_regs) {
        ExcClassReg* r = exc_class_regs;
        exc_class_regs = r->next;
        free(r->name);
        free(r);
    }
    while (method_regs) {
        MethodReg* r = method_regs;
        method_regs = r->next;
        free(r->qual);
        free(r->meth);
        free(r);
    }
}

static void __attribute__((constructor)) lang_install_shutdown(void) {
    atexit(lang_shutdown);
}
