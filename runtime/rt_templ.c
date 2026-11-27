// runtime/rt_templ.c —— PEP 750 t-string 的 Template / Interpolation 对象（0.567）
//
// t"..." 不求值成 str，而是保留结构：静态片段 + 每个插值（值、源码原文、
// 转换标志、格式说明）。库拿到它就能按结构处理（SQL 参数化、HTML 转义）。
//
// 简化：`strings` / `interpolations` / `values` 暴露为 **list**（CPython 是 tuple）。
// 我们的 tuple 是 codegen 内联构造的，运行时没有通用构造器；这里的用途（遍历、
// ''.join(...)）对 list 同样成立，差异写进 CHANGELOG。
#include "rt_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t strings;   // list[str]
    int64_t interps;   // list[Interpolation]
} PyAdamasTemplate;

typedef struct {
    int64_t value;     // 插值求值结果（可以是任意机器字）
    int64_t expr;      // 源码原文 str
    int64_t conv;      // 'r' / 's' / 'a' / ""
    int64_t fmt;       // 格式说明 str
} PyAdamasInterp;

void template_destroy(void* p) {
    PyAdamasTemplate* t = (PyAdamasTemplate*)p;
    if (t->strings) pa_rc_dec((void*)(uintptr_t)t->strings);
    if (t->interps) pa_rc_dec((void*)(uintptr_t)t->interps);
    t->strings = 0;
    t->interps = 0;
}

void interp_destroy(void* p) {
    PyAdamasInterp* i = (PyAdamasInterp*)p;
    if (i->expr) pa_rc_dec((void*)(uintptr_t)i->expr);
    if (i->conv) pa_rc_dec((void*)(uintptr_t)i->conv);
    if (i->fmt) pa_rc_dec((void*)(uintptr_t)i->fmt);
    i->expr = i->conv = i->fmt = 0;
}

int64_t pa_template_new(int64_t strings, int64_t interps) {
    PyAdamasTemplate* t = (PyAdamasTemplate*)rc_alloc(
        (int64_t)sizeof(PyAdamasTemplate), template_destroy, NULL, PYADAMAS_RC_OBJ_TEMPLATE);
    t->strings = strings;
    t->interps = interps;
    pa_rc_inc((void*)(uintptr_t)strings);
    pa_rc_inc((void*)(uintptr_t)interps);
    return (int64_t)(uintptr_t)t;
}

int64_t pa_interp_new(int64_t value, int64_t expr, int64_t conv, int64_t fmt) {
    PyAdamasInterp* i = (PyAdamasInterp*)rc_alloc(
        (int64_t)sizeof(PyAdamasInterp), interp_destroy, NULL, PYADAMAS_RC_OBJ_INTERP);
    i->value = value;
    i->expr = expr;
    i->conv = conv;
    i->fmt = fmt;
    pa_rc_inc((void*)(uintptr_t)expr);
    pa_rc_inc((void*)(uintptr_t)conv);
    pa_rc_inc((void*)(uintptr_t)fmt);
    return (int64_t)(uintptr_t)i;
}

static int64_t own_ref(int64_t v) {
    if (v) pa_rc_inc((void*)(uintptr_t)v);  // 借用转自有：调用方（temp 栈）负责释放
    return v;
}

int64_t pa_template_strings(int64_t t) {
    return own_ref(((PyAdamasTemplate*)(uintptr_t)t)->strings);
}

int64_t pa_template_interps(int64_t t) {
    return own_ref(((PyAdamasTemplate*)(uintptr_t)t)->interps);
}

// .values：把每个 Interpolation 的 value 收集成新 list（调用方拥有）。
int64_t pa_template_values(int64_t t) {
    PyAdamasTemplate* tp = (PyAdamasTemplate*)(uintptr_t)t;
    PyAdamasList* src = (PyAdamasList*)(uintptr_t)tp->interps;
    PyAdamasList* out = pa_list_new(src ? src->len : 0, 2);
    int64_t n = src ? src->len : 0;
    for (int64_t k = 0; k < n; ++k) {
        PyAdamasInterp* ip = (PyAdamasInterp*)(uintptr_t)src->data[k];
        pa_list_append(out, ip->value, 2);
    }
    return (int64_t)(uintptr_t)out;
}

int64_t pa_interp_value(int64_t i) { return ((PyAdamasInterp*)(uintptr_t)i)->value; }
int64_t pa_interp_expr(int64_t i) { return own_ref(((PyAdamasInterp*)(uintptr_t)i)->expr); }
int64_t pa_interp_conv(int64_t i) { return own_ref(((PyAdamasInterp*)(uintptr_t)i)->conv); }
int64_t pa_interp_fmt(int64_t i) { return own_ref(((PyAdamasInterp*)(uintptr_t)i)->fmt); }

// Template 的 repr：`Template(strings=N, interpolations=M)`（简化，够调试用）。
int64_t pa_template_repr(int64_t t) {
    PyAdamasTemplate* tp = (PyAdamasTemplate*)(uintptr_t)t;
    PyAdamasList* ss = (PyAdamasList*)(uintptr_t)tp->strings;
    PyAdamasList* is = (PyAdamasList*)(uintptr_t)tp->interps;
    char buf[96];
    snprintf(buf, sizeof buf, "Template(strings=%lld, interpolations=%lld)",
             (long long)(ss ? ss->len : 0), (long long)(is ? is->len : 0));
    return (int64_t)(uintptr_t)pa_str_from_cstr(buf);
}
