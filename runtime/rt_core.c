// rt_core.c —— 引用计数、临时值栈、闭包、字符串分配、循环收集器
#include "rt_internal.h"

/* ── 引用计数 ─────────────────────────────────────────────────── */

/* 隐藏头放在负载前面，负载指针（字符串的 len、对象的 vtable）保持原样。
   字面量在 .data.rel.ro 里静态构造，rc 是 INT64_MIN，inc/dec 直接跳过。

   头同时是循环收集器的注册表节点：
   - dtor：正常引用计数归零时调用（负责 dec 子对象、释放列表 data）；
   - release：GC 判断对象不可达时调用，只把「指向可达对象的引用」减掉，
     不释放任何东西（子对象由清扫阶段统一处理）；
   - size/kind：GC 保守扫描负载时要知道范围。 */

Py23Rc* heap_head;
int64_t alloc_since_gc;
int gc_ready;
int gc_sweeping;

/* 下一次触发全堆扫描的分配量。固定 2MB 会让大堆反复全扫——1M 个
   存活对象约 88MB，按 2MB 触发要扫四十多遍，总成本接近 O(n²)。
   每次扫描完按「本次存活字节数」重置，存活堆翻倍才扫一次，总扫描
   量是 O(live)；垃圾多、存活少时回落到 2MB 下限，扫描本身也便宜。 */
static int64_t gc_next_threshold = PY23_GC_THRESHOLD;

void panic_c(const char* msg);

/* 存活堆对象索引：unknown 值的静态类型看不见，不能直接 rc_inc（可能
   是整数），先按 payload 查注册表；查到才说明它真是堆对象。索引只用于
   这条保守 retain 路径，开放寻址 + 墓碑。 */
Py23Rc** rc_index;
size_t rc_index_cap;
size_t rc_index_fill;  // 非空槽（存活 + 墓碑）
size_t rc_index_live;  // 存活对象数
Py23Rc* const RC_TOMB = (Py23Rc*)(intptr_t)-1;

size_t rc_index_slot(Py23Rc* h) {
    uint64_t x = (uint64_t)(uintptr_t)h >> 4;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return (size_t)x & (rc_index_cap - 1);
}

void rc_index_rehash(size_t ncap) {
    Py23Rc** old = rc_index;
    size_t ocap = rc_index_cap;
    rc_index = (Py23Rc**)calloc(ncap, sizeof(Py23Rc*));
    if (!rc_index) panic_c("out of memory");
    rc_index_cap = ncap;
    rc_index_fill = 0;
    for (size_t i = 0; i < ocap; ++i) {
        Py23Rc* h = old[i];
        if (!h || h == RC_TOMB) continue;
        size_t s = rc_index_slot(h);
        while (rc_index[s]) s = (s + 1) & (ncap - 1);
        rc_index[s] = h;
        rc_index_fill++;
    }
    free(old);
}

void rc_index_put(Py23Rc* h) {
    if (rc_index_cap == 0) {
        rc_index_rehash(64);
    } else if ((rc_index_fill + 1) * 10 >= rc_index_cap * 7) {
        // 存活对象少、墓碑多时原地压缩，否则翻倍。
        size_t ncap = (rc_index_live + 1) * 10 >= rc_index_cap * 4
                          ? rc_index_cap * 2
                          : rc_index_cap;
        rc_index_rehash(ncap);
    }
    size_t s = rc_index_slot(h);
    size_t tomb = (size_t)-1;
    while (rc_index[s]) {
        if (rc_index[s] == RC_TOMB && tomb == (size_t)-1) tomb = s;
        s = (s + 1) & (rc_index_cap - 1);
    }
    if (tomb != (size_t)-1) {
        rc_index[tomb] = h;  // 复用墓碑，不增加 fill
    } else {
        rc_index[s] = h;
        rc_index_fill++;
    }
    rc_index_live++;
}

void rc_index_del(Py23Rc* h) {
    if (rc_index_cap == 0) return;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) {
            rc_index[s] = RC_TOMB;
            rc_index_live--;
            return;
        }
        s = (s + 1) & (rc_index_cap - 1);
    }
}

int rc_index_has(Py23Rc* h) {
    if (rc_index_cap == 0) return 0;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) return 1;
        s = (s + 1) & (rc_index_cap - 1);
    }
    return 0;
}

/* 代码生成把静态字符串字面量指针列表放在 .Lstrtab，模块入口在跑用户
   代码前调用这里登记。字面量 rc=INT64_MIN 永生，不进 GC 堆链，但
   unknown 值的运行期类型检查要靠 RC 索引确认指针确实指向 Py23Str。 */
void py23_register_literals(void** objs, int64_t n) {
    for (int64_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        Py23Rc* h = (Py23Rc*)(uintptr_t)objs[i] - 1;
        if (!rc_index_has(h)) rc_index_put(h);
    }
}

/* 对 unknown 值做保守 retain：不是堆指针就什么也不做。查不到的对象
   包括静态字面量（永生，不需要 inc）和整数。 */
int64_t py23_rc_inc_unknown(int64_t v) {
    if (!v) return 0;
    Py23Rc* h = (Py23Rc*)(uintptr_t)((uint64_t)v - (uint64_t)PY23_RC_HDR);
    if (!rc_index_has(h)) return 0;
    if (h->rc == PY23_RC_IMMORTAL) return 1;
    if (gc_sweeping) {
        if (h->marked) h->rc++;
        return 1;
    }
    if (h->rc <= 0) return 1;
    h->rc++;
    return 1;
}

void py23_rc_dec(void* p);

/* unknown 值的保守释放：先按注册表确认是堆指针，再走正常 dec。
   用于「结果类型静态不可知」的跨 await/gather 值。 */
void py23_rc_dec_unknown(int64_t v) {
    if (!v) return;
    Py23Rc* h = (Py23Rc*)(uintptr_t)((uint64_t)v - (uint64_t)PY23_RC_HDR);
    if (!rc_index_has(h)) return;
    if (h->rc == PY23_RC_IMMORTAL) return;
    py23_rc_dec((void*)(uintptr_t)v);
}

void py23_gc_collect(void);
void py23_gc_shutdown(void);
void py23_gc_unref_child(void* p);
void py23_gc_add_roots(void** roots, int64_t n_roots);
void dict_gc_release(void* p);

Py23List* py23_list_new(int64_t cap, int64_t elem_kind);
void py23_list_append(Py23List* l, int64_t v);

void panic_c(const char* msg) {
    fprintf(stderr, "py23: runtime error: %s\n", msg);
    exit(1);
}

void rc_link(Py23Rc* h) {
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    rc_index_put(h);
}

void rc_unlink(Py23Rc* h) {
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
    rc_index_del(h);
}

void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    if (size < 0) panic_c("negative allocation size");
    if (gc_ready && !gc_sweeping && heap_head &&
        alloc_since_gc >= gc_next_threshold)
        py23_gc_collect();
    Py23Rc* h = (Py23Rc*)malloc(sizeof(Py23Rc) + (size_t)size);
    if (!h) panic_c("out of memory");
    h->rc = 1;
    h->dtor = dtor;
    h->release = release;
    h->size = size;
    h->kind = kind;
    h->marked = 0;
    rc_link(h);
    alloc_since_gc += size + PY23_RC_HDR;
    void* p = (char*)h + sizeof(Py23Rc);
    memset(p, 0, (size_t)size);
    return p;
}

void py23_rc_inc(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (h->rc <= 0) panic_c("refcount increment on a freed object");
    h->rc++;
}

void py23_rc_dec(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (gc_sweeping) {
        // 清扫阶段只调整可达对象的计数，绝不在 GC 内部触发释放。
        if (h->marked && h->rc > 0) h->rc--;
        return;
    }
    if (h->rc <= 0) panic_c("refcount decrement on a freed object");
    if (--h->rc == 0) {
        rc_unlink(h);
        if (h->dtor) h->dtor(p);
        free(h);
    }
}

/* 表达式临时值栈：每个新堆值在产生时带着 rc=1 入栈，语句/循环/函数
   边界按 mark 统一释放。这样不需要做活跃性分析，也不会漏掉某条路径。
   入栈不增加计数——栈接管的是「产生者」本来就持有的那一个引用。 */
void** temp_stack;
int64_t temp_len;
int64_t temp_cap;

int64_t py23_rc_mark(void) { return temp_len; }

void py23_rc_temp(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (temp_len == temp_cap) {
        int64_t ncap = temp_cap ? temp_cap * 2 : 64;
        void** ns = (void**)realloc(temp_stack, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        temp_stack = ns;
        temp_cap = ncap;
    }
    temp_stack[temp_len++] = p;
}

void py23_rc_cleanup(int64_t mark) {
    if (mark < 0 || mark > temp_len) panic_c("invalid temporary stack mark");
    while (temp_len > mark) py23_rc_dec(temp_stack[--temp_len]);
}

/* 程序退出前由入口函数调用：临时栈此时已经清空，把栈本身的存储还掉。 */
void py23_rc_shutdown(void) {
    free(temp_stack);
    temp_stack = NULL;
    temp_len = 0;
    temp_cap = 0;
}

/* ── 闭包 ─────────────────────────────────────────────────────── */

/* 闭包负载 = [code, n_caps, cell0, cell1…]。
   code 是静态代码地址（不是堆指针）；cell 是单元素 list，闭包持有引用。 */
void closure_destroy(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) py23_rc_dec((void*)c[2 + i]);
}

void closure_gc_release(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) py23_gc_unref_child((void*)c[2 + i]);
}

void* py23_closure_new(int64_t code, int64_t n_caps) {
    if (n_caps < 0) panic_c("negative closure capture count");
    void* p = rc_alloc(16 + 8 * n_caps, closure_destroy, closure_gc_release,
                       PY23_RC_CLOSURE);
    ((int64_t*)p)[0] = code;
    ((int64_t*)p)[1] = n_caps;
    return p;
}

Py23Str* str_alloc(int64_t len) {
    if (len < 0) panic_c("negative string length");
    Py23Str* s =
        (Py23Str*)rc_alloc((int64_t)sizeof(Py23Str) + len + 1, NULL, NULL, PY23_RC_STR);
    s->len = len;
    s->hash = 0;
    char* p = (char*)(s + 1);
    p[len] = '\0';
    s->data = p;
    return s;
}

Py23Str* str_new(const char* p, int64_t len) {
    Py23Str* s = str_alloc(len);
    memcpy((char*)s->data, p, (size_t)len);
    return s;
}

/* ── 循环收集器 ───────────────────────────────────────────────── */

void** gc_globals;
int64_t gc_n_globals;
int64_t gc_globals_cap;
uintptr_t gc_stack_top;
Py23Rc** gc_table;
size_t gc_table_cap;
Py23Rc** gc_work;
size_t gc_work_len;

#if defined(__SANITIZE_ADDRESS__)
#define PY23_NO_ASAN __attribute__((no_sanitize_address))
#else
#define PY23_NO_ASAN
#endif

size_t gc_hash(uintptr_t p) {
    return (size_t)((p >> 4) * 11400714819323198485ull);
}

void gc_table_insert(Py23Rc* h) {
    size_t mask = gc_table_cap - 1;
    size_t i = gc_hash((uintptr_t)h + (uintptr_t)PY23_RC_HDR) & mask;
    while (gc_table[i]) i = (i + 1) & mask;
    gc_table[i] = h;
}

Py23Rc* gc_table_find(uintptr_t payload) {
    if (!gc_table_cap) return NULL;
    size_t mask = gc_table_cap - 1;
    size_t i = gc_hash(payload) & mask;
    while (gc_table[i]) {
        if ((uintptr_t)gc_table[i] + (uintptr_t)PY23_RC_HDR == payload)
            return gc_table[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

void gc_mark(uintptr_t v) {
    if (!v) return;
    Py23Rc* h = gc_table_find(v);
    if (!h || h->marked) return;
    h->marked = 1;
    gc_work[gc_work_len++] = h;
}

PY23_NO_ASAN static void gc_scan_range(const void* begin, const void* end) {
    uintptr_t a = (uintptr_t)begin;
    uintptr_t b = (uintptr_t)end;
    if (a >= b) return;
    for (; a + sizeof(uintptr_t) <= b; a += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, (const void*)a, sizeof v);
        gc_mark(v);
    }
}

void gc_scan_object(Py23Rc* h) {
    void* p = (char*)h + PY23_RC_HDR;
    switch (h->kind) {
        case PY23_RC_STR:
            break;
        case PY23_RC_LIST: {
            Py23List* l = (Py23List*)p;
            if (l->data)
                for (int64_t i = 0; i < l->len; ++i) gc_mark((uintptr_t)l->data[i]);
            break;
        }
        case PY23_RC_DICT: {
            Py23Dict* d = (Py23Dict*)p;
            for (int64_t i = 0; i < d->len; ++i) {
                Py23DictEntry e = d->entries[d->order[i]];
                gc_mark((uintptr_t)e.key);
                gc_mark((uintptr_t)e.val);
            }
            break;
        }
        case PY23_RC_OBJ:
        case PY23_RC_INSTANCE:
        case PY23_RC_CLOSURE: {
            for (int64_t off = 0; off + (int64_t)sizeof(uintptr_t) <= h->size;
                 off += (int64_t)sizeof(uintptr_t)) {
                uintptr_t v;
                memcpy(&v, (const char*)p + off, sizeof v);
                gc_mark(v);
            }
            break;
        }
        default:
            break;
    }
}

// 取真实栈指针：ASan 会把带地址的局部变量搬到 fake stack，用 &local
// 当扫描起点会跨到未映射区域。生成的代码帧本身在真实栈上。
PY23_NO_ASAN static uintptr_t gc_current_sp(void) {
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    return sp;
}

// setjmp 会把 callee-saved 寄存器存进 jmp_buf；生成代码里常驻寄存器的
// 堆指针就靠这一段兜住。
PY23_NO_ASAN static void gc_scan_registers(void) {
    jmp_buf regs;
    if (setjmp(regs) == 0) gc_scan_range(&regs, (char*)&regs + sizeof regs);
}

void py23_gc_add_roots(void** roots, int64_t n_roots) {
    if (!roots || n_roots <= 0) return;
    if (gc_n_globals + n_roots > gc_globals_cap) {
        int64_t ncap = gc_globals_cap ? gc_globals_cap : 64;
        while (ncap < gc_n_globals + n_roots) ncap *= 2;
        void** ns = (void**)realloc(gc_globals, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        gc_globals = ns;
        gc_globals_cap = ncap;
    }
    // 根表本身是静态存储，这里只复制槽地址。
    for (int64_t i = 0; i < n_roots; ++i) gc_globals[gc_n_globals++] = roots[i];
}

void py23_gc_init(void** roots, int64_t n_roots, void* stack_top) {
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    py23_gc_add_roots(roots, n_roots);
    gc_stack_top = (uintptr_t)stack_top;
    // 嵌入/基准场景可以关掉循环收集，只靠引用计数（会漏检环）。
    if (getenv("PY23_GC_DISABLE")) return;
    gc_ready = 1;
}

void py23_gc_unref_child(void* p) {
    if (!p) return;
    Py23Rc* h = gc_table_find((uintptr_t)p);
    if (h && h->marked && h->rc > 0) h->rc--;
}

void gc_drain(void) {
    while (gc_work_len) gc_scan_object(gc_work[--gc_work_len]);
}

void py23_gc_collect(void) {
    if (!gc_ready || !heap_head || !gc_stack_top) {
        alloc_since_gc = 0;
        return;
    }
    size_t n = 0;
    for (Py23Rc* h = heap_head; h; h = h->next) ++n;
    if (n == 0) {
        alloc_since_gc = 0;
        gc_next_threshold = PY23_GC_THRESHOLD;
        return;
    }
    gc_table_cap = 1;
    while (gc_table_cap < n * 2) gc_table_cap <<= 1;
    gc_table = (Py23Rc**)calloc(gc_table_cap, sizeof(Py23Rc*));
    gc_work = (Py23Rc**)malloc(n * sizeof(Py23Rc*));
    if (!gc_table || !gc_work) panic_c("out of memory");
    for (Py23Rc* h = heap_head; h; h = h->next) {
        h->marked = 0;
        gc_table_insert(h);
    }
    gc_work_len = 0;

    for (int64_t i = 0; i < gc_n_globals; ++i) {
        void* v = *(void**)gc_globals[i];
        gc_mark((uintptr_t)v);
    }
    for (int64_t i = 0; i < temp_len; ++i) gc_mark((uintptr_t)temp_stack[i]);
    gc_scan_range((const void*)gc_current_sp(), (const void*)gc_stack_top);
    gc_scan_registers();
    gc_drain();

    // 第一遍：只处理不可达对象指向可达对象的引用。此时所有头都还活着，
    // 查表安全；不调用 dtor，避免释放顺序问题。
    gc_sweeping = 1;
    for (Py23Rc* h = heap_head; h; h = h->next) {
        if (!h->marked && h->release)
            h->release((char*)h + PY23_RC_HDR);
    }

    // 第二遍：释放不可达对象，清理可达对象的 mark 位。
    int64_t live = 0;
    Py23Rc* h = heap_head;
    while (h) {
        Py23Rc* next = h->next;
        if (h->marked) {
            live += h->size + PY23_RC_HDR;
            h->marked = 0;
        } else {
            rc_unlink(h);
            if (h->kind == PY23_RC_OBJ)
                free_attached_exception((char*)h + PY23_RC_HDR, h->size);
            if (h->kind == PY23_RC_LIST) {
                Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
                free(l->data);
            } else if (h->kind == PY23_RC_DICT) {
                Py23Dict* d = (Py23Dict*)((char*)h + PY23_RC_HDR);
                free(d->entries);
                free(d->order);
            }
            free(h);
        }
        h = next;
    }
    gc_sweeping = 0;

    free(gc_table);
    free(gc_work);
    gc_table = NULL;
    gc_work = NULL;
    gc_table_cap = 0;
    gc_work_len = 0;
    alloc_since_gc = 0;
    gc_next_threshold = live > (int64_t)PY23_GC_THRESHOLD ? live : PY23_GC_THRESHOLD;
}

// 程序退出：所有还活着的堆对象（包括环）一次性还给系统。
void py23_gc_shutdown(void) {
    gc_sweeping = 1;
    while (heap_head) {
        Py23Rc* h = heap_head;
        rc_unlink(h);
        if (h->kind == PY23_RC_OBJ)
            free_attached_exception((char*)h + PY23_RC_HDR, h->size);
        if (h->kind == PY23_RC_LIST) {
            Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
            free(l->data);
        } else if (h->kind == PY23_RC_DICT) {
            Py23Dict* d = (Py23Dict*)((char*)h + PY23_RC_HDR);
            free(d->entries);
            free(d->order);
        }
        free(h);
    }
    gc_sweeping = 0;
    alloc_since_gc = 0;
    free(gc_globals);
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
}

/* 分模块编译：init 是模块顶层生成的 void(void)，ready 是模块 .bss 里的
   一次性标志。标志在调用前设置，初始化失败不会重试（与 Python 移除
   失败模块的行为不同，但至少不会死循环）。 */
void py23_import_module(int64_t* ready, void (*init)(void)) {
    if (!ready || !init || *ready) return;
    *ready = 1;
    init();
}

