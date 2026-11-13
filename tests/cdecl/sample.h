#ifndef SAMPLE_H
#define SAMPLE_H

#include <stddef.h>

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

enum Color { RED, GREEN = 5, BLUE };

extern int global_counter;
extern double global_ratio;

int add(int a, int b);
unsigned mix(unsigned a, unsigned b);
long sum_ptrs(const long *xs, int n);
double scale(double x, double k);
const char *name_of(int id);
void fill_buffer(int *buf, int n, int v);
Point make_point(int x, int y);
int apply2(int a, int b, int (*fn)(int, int));
int log_fmt(const char *fmt, ...);
int *alloc_ints(int n);
void free(void *p);

static inline int twice(int x) { return x * 2; }

#endif
