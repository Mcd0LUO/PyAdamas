#include "sample.h"

#include <stdlib.h>
#include <string.h>

int global_counter = 7;
double global_ratio = 2.5;
float global_ratiof = 1.25f;

int add(int a, int b) { return a + b; }

unsigned mix(unsigned a, unsigned b) { return (a << 1) ^ b; }

long sum_ptrs(const long *xs, int n) {
    long s = 0;
    for (int i = 0; i < n; ++i) s += xs[i];
    return s;
}

double scale(double x, double k) { return x * k; }

float scalef(float x, float k) { return x * k; }

const char *name_of(int id) { return id == 0 ? "zero" : "other"; }

void fill_buffer(int *buf, int n, int v) {
    for (int i = 0; i < n; ++i) buf[i] = v + i;
}

Point make_point(int x, int y) {
    Point p = {x, y};
    return p;
}

FPoint fpoint_make(float x, float y) {
    FPoint p = {x, y};
    return p;
}

FPoint fpoint_add(FPoint a, FPoint b) {
    FPoint p = {a.x + b.x, a.y + b.y};
    return p;
}

static FPoint fpoint_g = {7.25f, 0.5f};

FPoint *fpoint_ref(void) { return &fpoint_g; }

int apply2(int a, int b, int (*fn)(int, int)) { return fn(a, b); }

int log_fmt(const char *fmt, ...) { return (int)strlen(fmt); }

int *alloc_ints(int n) { return (int *)calloc((size_t)n, sizeof(int)); }
