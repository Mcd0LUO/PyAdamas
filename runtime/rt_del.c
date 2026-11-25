// rt_del.c —— 用户类 __del__（0.539）
//
// RC 归零释放实例时、调 dtor 之前，经元数据 dunder 表第 4 项（下标 3）取 __del__
// 并调用一次。异常在 **C 侧**用 pa_handler_push + setjmp 兜住：打一行、丢弃、继续释放，
// 不终止程序（用 pa_exc_type_name/message 取内容，pa_exc_clear 清 pending）。
//
// 不保证：GC 环回收（gc_free_body）**不调用** __del__（与 CPython 的差异，文档写明）。
// 防重入：in_del_call 全局标志。`__del__` 里触发另一个对象的析构时，那一次的 __del__
// 会被跳过（代价，文档写明）。
#include "rt_internal.h"

#include <stdio.h>

static int in_del_call;

/* 程序里有没有带 __del__ 的类：codegen 在「有」时发射强符号 1 覆盖这个弱定义。
   绝大多数程序没有 __del__，释放路径因此只多一次全局读（实测 binary_trees
   不加这条会 +8.3%，因为每个实例释放都要做一次 rc_index_has 查表）。 */
__attribute__((weak)) int64_t pa_has_del_classes = 0;

static void write_str(FILE* f, PyAdamasStr* s) {
    if (!s || !s->data) return;
    fwrite(s->data, 1, (size_t)s->len, f);
}

void pa_instance_call_del(void* p) {
    if (!p || in_del_call) return;
    void (*fn)(void*) = (void (*)(void*))pa_instance_dunder_fn(p, 3);
    if (!fn) return;
    in_del_call = 1;
    void* h = pa_handler_push();
    if (setjmp(((PyAdamasHandler*)h)->env) == 0) {
        fn(p);
        pa_handler_leave(h);
    } else {
        fflush(stdout);  // 与 stdout 的先后顺序确定，便于对拍
        PyAdamasStr* t = pa_exc_type_name();
        PyAdamasStr* m = pa_exc_message();
        fputs("__del__ raised ", stderr);
        write_str(stderr, t);
        fputs(": ", stderr);
        write_str(stderr, m);
        fputc('\n', stderr);
        pa_rc_dec(t);
        pa_rc_dec(m);
        pa_exc_clear();
        pa_handler_leave(h);
    }
    in_del_call = 0;
}
