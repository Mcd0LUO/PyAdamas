#ifndef SAMPLE_H
#define SAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "sample_extra.h"

#define MAGIC 42
#define RATIO 1.5
#define GREETING "hello"
#define FLAG true

typedef unsigned int uint;

typedef struct Point {
    int x;
    int y;
} Point;

typedef struct Node {
    long value;
    struct Node *next;
} Node;

/* 纯 float32 字段的结构体：单个 SSE chunk，可按值传递/返回 */
typedef struct FPoint {
    float x;
    float y;
} FPoint;

enum Color { RED, GREEN = 5, BLUE };

extern int global_counter;
extern double global_ratio;
extern float global_ratiof;

int add(int a, int b);
unsigned mix(unsigned a, unsigned b);
long sum_ptrs(const long *xs, int n);
double scale(double x, double k);
float scalef(float x, float k);
const char *name_of(int id);
void fill_buffer(int *buf, int n, int v);
Point make_point(int x, int y);
FPoint fpoint_make(float x, float y);
FPoint fpoint_add(FPoint a, FPoint b);
FPoint *fpoint_ref(void);
int apply2(int a, int b, int (*fn)(int, int));
int log_fmt(const char *fmt, ...);
int *alloc_ints(int n);
void free(void *p);

static inline int twice(int x) { return x * 2; }

#endif

/* union + 定长数组字段（.cpya 新语法：@union / T[N] / @packed） */
typedef union { int32_t i; double d; char bytes[8]; } sample_value;
typedef struct { uint32_t events; sample_value data; } sample_packed;
typedef struct { char name[16]; int32_t grid[2][3]; sample_value u; } sample_arrays;
sample_arrays *sample_make(void);
size_t sample_size(void);
