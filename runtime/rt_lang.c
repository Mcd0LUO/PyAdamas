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

/* ── P4 块3（0.346）：运行期门控 ─────────────────────────────────
   生成代码门控开时入口模块体首发 LangGapsEnable → pa_lang_gaps_enable。
   帧捕获（rt_debug.c）/富异常上下文接管（rt_exc.c）第一行判它；关时全是
   空操作。门控是程序级 env，所有模块同门控，进程级一个变量足够。 */
int pa_lang_gaps_rt;
void pa_lang_gaps_enable(void) { pa_lang_gaps_rt = 1; }

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
    if (!pa_unknown_is_heap((int64_t)(uintptr_t)p)) return NULL;
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

/* 沿实例 MRO 找方法实现（与 pa_super_lookup 同一套元数据布局：
   meta=vt-7，m[2]=vtable / m[3]=own_slots / m[4]=限定名）。命中且槽在
   该类 own_slots 里（尊重覆盖）才返回该类 vtable 的实现。找不到报
   AttributeError。返回的函数指针按调用方口径 cast（零参/带参）。 */
static void* lang_method_fn(void* obj, const char* meth) {
    if (pa_unknown_is_heap((int64_t)(uintptr_t)obj)) {
        PyAdamasRc* h = (PyAdamasRc*)obj - 1;
        if (h->kind == PYADAMAS_RC_INSTANCE) {
            const int64_t* vt = *(const int64_t**)obj;
            if (vt) {
                const int64_t* meta = vt - 7;
                const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
                int64_t n = mro ? mro[0] : 0;
                for (int64_t i = 1; i <= n; ++i) {
                    const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
                    if (!m) continue;
                    const char* cn = (const char*)(uintptr_t)m[4];
                    if (!cn) continue;
                    const MethodReg* r = lang_method_lookup(cn, meth);
                    if (!r) continue;
                    const int64_t* own = (const int64_t*)(uintptr_t)m[3];
                    int64_t k = own ? own[0] : 0;
                    int found = 0;
                    for (int64_t j = 1; j <= k; ++j)
                        if (own[j] == r->slot) { found = 1; break; }
                    if (!found) continue;
                    const int64_t* ovt = (const int64_t*)(uintptr_t)m[2];
                    if (!ovt) continue;
                    void* fn = (void*)(uintptr_t)ovt[r->slot];
                    if (fn) return fn;
                }
            }
        }
    }
    char buf[192];
    snprintf(buf, sizeof buf, "object has no attribute '%s'", meth);
    pa_raise_c("AttributeError", buf);
    return NULL;
}

/* 零参动态方法调用（closing 的 .close()）。分派口径：
   - file 对象（pa_is_file）：close 直调 pa_file_close；其它 AttributeError。
   - 类实例：lang_method_fn 沿 MRO 查表。
   返回值原样交出；只用于返回 None/标量的方法。返回堆值的方法动态调
   会丢引用——closing 只发 close，文档化边界。 */
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
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(obj, meth);
    return fn(obj);
}

/* ── file 协议泛化（P4 块2）：实例侧分派 ─────────────────────────
   rt_file.c 的 pa_file_* 入口在 !pa_is_file(v) 时委托到这里（门控关时
   file 槽只可能是真 file，委托分支永不命中）。实例方法 ABI 与生成代码
   一致：self 在第一个整型参寄存器，其余按签名；堆返回值带 +1 引用
   （与 pa_file_read 同口径，调用方 push_temp 接管）。
   只支持 int/str/指针参（file 协议面没有浮点参）；带默认参的方法由
   调用方显式传规范默认值（read(-1)/seek(off,0)/truncate(-1)）。 */

PyAdamasStr* pa_fileinst_read(void* v, int64_t n) {
    PyAdamasStr* (*fn)(void*, int64_t) =
        (PyAdamasStr* (*)(void*, int64_t))lang_method_fn(v, "read");
    return fn(v, n);
}
PyAdamasStr* pa_fileinst_readline(void* v, int64_t n) {
    PyAdamasStr* (*fn)(void*, int64_t) =
        (PyAdamasStr* (*)(void*, int64_t))lang_method_fn(v, "readline");
    return fn(v, n);
}
PyAdamasList* pa_fileinst_readlines(void* v, int64_t hint) {
    PyAdamasList* (*fn)(void*, int64_t) =
        (PyAdamasList* (*)(void*, int64_t))lang_method_fn(v, "readlines");
    return fn(v, hint);
}
int64_t pa_fileinst_write(void* v, PyAdamasStr* s) {
    int64_t (*fn)(void*, PyAdamasStr*) =
        (int64_t (*)(void*, PyAdamasStr*))lang_method_fn(v, "write");
    return fn(v, s);
}
void pa_fileinst_writelines(void* v, PyAdamasList* l) {
    void (*fn)(void*, PyAdamasList*) =
        (void (*)(void*, PyAdamasList*))lang_method_fn(v, "writelines");
    fn(v, l);
}
int64_t pa_fileinst_seek(void* v, int64_t off, int64_t whence) {
    int64_t (*fn)(void*, int64_t, int64_t) =
        (int64_t (*)(void*, int64_t, int64_t))lang_method_fn(v, "seek");
    return fn(v, off, whence);
}
int64_t pa_fileinst_tell(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "tell");
    return fn(v);
}
int64_t pa_fileinst_truncate(void* v, int64_t size) {
    int64_t (*fn)(void*, int64_t) =
        (int64_t (*)(void*, int64_t))lang_method_fn(v, "truncate");
    return fn(v, size);
}
void pa_fileinst_close(void* v) {
    void (*fn)(void*) = (void (*)(void*))lang_method_fn(v, "close");
    fn(v);
}
void pa_fileinst_flush(void* v) {
    void (*fn)(void*) = (void (*)(void*))lang_method_fn(v, "flush");
    fn(v);
}
int64_t pa_fileinst_closed(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "closed");
    return fn(v);
}
int64_t pa_fileinst_fileno(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "fileno");
    return fn(v);
}
int64_t pa_fileinst_isatty(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "isatty");
    return fn(v);
}
int64_t pa_fileinst_readable(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "readable");
    return fn(v);
}
int64_t pa_fileinst_writable(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "writable");
    return fn(v);
}
int64_t pa_fileinst_seekable(void* v) {
    int64_t (*fn)(void*) = (int64_t (*)(void*))lang_method_fn(v, "seekable");
    return fn(v);
}
/* name/mode/encoding/errors：StringIO/BytesIO 在 CPython 里也没有这些
   属性；实例侧统一 AttributeError（不读实例内存冒充 PyAdamasFile）。 */
PyAdamasStr* pa_fileinst_name(void* v) {
    (void)v;
    pa_raise_c("AttributeError", "file-like object has no attribute 'name'");
    return NULL;
}
PyAdamasStr* pa_fileinst_mode(void* v) {
    (void)v;
    pa_raise_c("AttributeError", "file-like object has no attribute 'mode'");
    return NULL;
}
PyAdamasStr* pa_fileinst_encoding(void* v) {
    (void)v;
    pa_raise_c("AttributeError", "file-like object has no attribute 'encoding'");
    return NULL;
}
PyAdamasStr* pa_fileinst_errors(void* v) {
    (void)v;
    pa_raise_c("AttributeError", "file-like object has no attribute 'errors'");
    return NULL;
}

/* ── P4 块3：traceback 对象 + sys.exc_info ─────────────────────
 *
 * tb 是 RC OBJ + 魔数，包一条 pa_trace_clone 来的物理帧链（malloc，
 * 字符串是调试表 rodata 的借用指针，不占引用）。帧链不是 RC 对象，
 * 没有 GC 出边，release 回调为 NULL；destroy 里 pa_trace_free 整链还掉。
 * 门控关时 phys 恒 NULL，tb 帧链为空——exc_info 仍可用（类值/异常值
 * 不依赖捕获），只是没有帧（与 suppress 的门控语义同口径）。 */

#define PYADAMAS_TB_MAGIC 0x54424652414d4501LL  /* "TBFRAME" + 版本 1 */
typedef struct PyAdamasTb {
    int64_t magic;
    PyAdamasTraceFrame* frames;  /* owned malloc 链，外→内 */
} PyAdamasTb;

static void tb_destroy(void* p) {
    PyAdamasTb* tb = (PyAdamasTb*)p;
    pa_trace_free(tb->frames);
}

static PyAdamasTb* tb_new(PyAdamasTraceFrame* frames) {
    PyAdamasTb* tb = (PyAdamasTb*)rc_alloc((int64_t)sizeof(PyAdamasTb),
                                           tb_destroy, NULL, PYADAMAS_RC_OBJ);
    tb->magic = PYADAMAS_TB_MAGIC;
    tb->frames = frames;
    return tb;
}

static PyAdamasTb* tb_check(void* p) {
    if (!p || !pa_unknown_is_heap((int64_t)(uintptr_t)p))
        pa_raise_c("TypeError", "expected a traceback object");
    PyAdamasTb* tb = (PyAdamasTb*)p;
    if (tb->magic != PYADAMAS_TB_MAGIC)
        pa_raise_c("TypeError", "expected a traceback object");
    return tb;
}

/* sys.exc_info()：except 体里返回 (类值, 异常值, tb)；outside except →
   (None, None, None)。类值复用块1 单例（pa_exc_class_obj，+1）；异常值
   pa_exc_value_from_exc（+1，args/cause 同 except as e 口径）；tb 包
   current_exc->phys 的克隆（+1）。三个引用都转交给三元组（kind 1）。 */
PyAdamasList* pa_exc_info(void) {
    if (!current_exc)
        return pa_tuple_new_filled(3, 1, 0, 0, 0, 0);
    /* 构造期可能触发 GC（str_new / rc_alloc）：三元组先挂临时栈保命，
       元素引用在 append 前都是 C 局部，临时栈接管后逐个转移所有权。 */
    PyAdamasStr* tn = str_new(current_exc->type, (int64_t)strlen(current_exc->type));
    pa_rc_temp(tn);
    void* cls = pa_exc_class_obj(tn);
    pa_rc_temp(cls);
    /* 单例自己持名字；tn 只是查表用。先取回所有权再 dec——temp 挂着时
       dec 到 0 会当场销毁，temp_take 再读就是 UAF。 */
    pa_rc_temp_take(tn);
    pa_rc_dec(tn);
    PyAdamasExcValue* val = pa_exc_value_from_exc(current_exc);
    pa_rc_temp(val);
    PyAdamasTb* tb = tb_new(pa_trace_clone(current_exc->phys));
    pa_rc_temp(tb);
    PyAdamasList* t = pa_tuple_new_filled(3, 1, (int64_t)(uintptr_t)cls,
                                         (int64_t)(uintptr_t)val,
                                         (int64_t)(uintptr_t)tb, 0);
    /* filled 已接管三个引用；临时栈那份摘掉（temp_take 按值摘除，不 dec）。 */
    pa_rc_temp_take(cls);
    pa_rc_temp_take(val);
    pa_rc_temp_take(tb);
    return t;
}

/* __pa_tb_frames(tb) → list[(file, line, func)]，外→内物化。
   None 按空表处理（stdlib/traceback.py 的 extract_tb(None) 口径）。
   内层元组 (str, int, str) 混合种类 → kind 2；外层全堆 → kind 1。 */
PyAdamasList* pa_tb_frames(void* p) {
    if (!p) return pa_list_new(0, 1);
    PyAdamasTb* tb = tb_check(p);
    int64_t n = 0;
    for (const PyAdamasTraceFrame* f = tb->frames; f; f = f->next) ++n;
    PyAdamasList* out = pa_list_new(n, 1);
    pa_rc_temp(out);
    for (const PyAdamasTraceFrame* f = tb->frames; f; f = f->next) {
        PyAdamasStr* fs = str_new(f->file, (int64_t)strlen(f->file));
        pa_rc_temp(fs);
        PyAdamasStr* fn = str_new(f->func, (int64_t)strlen(f->func));
        pa_rc_temp(fn);
        PyAdamasList* tup = pa_list_new(3, 2);
        pa_rc_temp(tup);
        pa_list_append(tup, (int64_t)(uintptr_t)fs, 1);
        pa_list_append(tup, (int64_t)f->line, 0);
        pa_list_append(tup, (int64_t)(uintptr_t)fn, 1);
        pa_list_append(out, (int64_t)(uintptr_t)tup, 1);
        pa_rc_temp_take(fs);
        pa_rc_temp_take(fn);
        pa_rc_temp_take(tup);
    }
    pa_rc_temp_take(out);
    return out;
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
