// runtime/rt_buffer.c —— RC 管理的连续内存缓冲区（0.536，buf 阶段 1）
//
// 用途：让 pya 侧拿到一块**自动回收**的连续内存，直接作为 ptr[T] 传给 C
// 函数（dgemm/memcpy/memset），不再依赖 calloc 自建 + 手工释放（那条路
// 漏掉就泄漏）。
//
// 布局：RC 头之后是 PyAdamasBuffer{magic, data, len, elem_kind}；data 由
// malloc 分配，dtor 里 free。没有堆出边（元素只有标量），GC 按叶子处理。
#include "rt_internal.h"

#include <stdlib.h>
#include <string.h>

#define PYADAMAS_BUFFER_MAGIC 0x4255464600000001LL

typedef struct {
    int64_t magic;
    void* data;
    int64_t len;       // 元素个数
    int64_t elem_kind; // 0 = int64/float64（8 字节），1 = int32，2 = float32
} PyAdamasBuffer;

static int64_t elem_size(int64_t kind) {
    switch (kind) {
        case 1: return 4;
        case 2: return 4;
        default: return 8;
    }
}

static void buffer_destroy(void* p) {
    PyAdamasBuffer* b = (PyAdamasBuffer*)p;
    if (b->data) free(b->data);
    b->data = NULL;
    b->len = 0;
}

void* pa_buffer_new(int64_t n, int64_t elem_kind) {
    if (n < 0) panic_c("negative buffer size");
    int64_t es = elem_size(elem_kind);
    PyAdamasBuffer* b = (PyAdamasBuffer*)rc_alloc(
        (int64_t)sizeof(PyAdamasBuffer), buffer_destroy, NULL, PYADAMAS_RC_BUFFER);
    b->magic = PYADAMAS_BUFFER_MAGIC;
    b->len = n;
    b->elem_kind = elem_kind;
    // calloc：与「零初始化」的 C 习惯一致，也避免把未初始化内存交给 dgemm。
    b->data = calloc((size_t)(n > 0 ? n : 1), (size_t)es);
    if (!b->data) panic_c("buffer allocation failed");
    return b;
}

void* pa_buffer_data(void* p) {
    PyAdamasBuffer* b = (PyAdamasBuffer*)p;
    if (!b || b->magic != PYADAMAS_BUFFER_MAGIC) return NULL;
    return b->data;
}

int64_t pa_buffer_len(void* p) {
    PyAdamasBuffer* b = (PyAdamasBuffer*)p;
    if (!b || b->magic != PYADAMAS_BUFFER_MAGIC) return 0;
    return b->len;
}

int64_t pa_buffer_elem_kind(void* p) {
    PyAdamasBuffer* b = (PyAdamasBuffer*)p;
    return (b && b->magic == PYADAMAS_BUFFER_MAGIC) ? b->elem_kind : 0;
}
