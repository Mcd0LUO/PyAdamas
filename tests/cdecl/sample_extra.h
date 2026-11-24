/* cdecl 端到端测试的“真实头文件形态”样例：
   #include 另一份头、导出宏、对象式宏当修饰符、__attribute__、
   同名 tag/alias 的不透明句柄、指针 typedef、函数指针 typedef。 */
#ifndef SAMPLE_EXTRA_H
#define SAMPLE_EXTRA_H

#include <stdint.h>

#define SAMPLE_API extern
#define SAMPLE_SCALE(x) ((x) * 2)

typedef struct opaque_handle opaque_handle;

typedef struct named_pair {
    int32_t a;
    int32_t b;
} named_pair;

typedef named_pair *named_pair_ptr;

typedef int (*sample_cb)(int, const char *);

SAMPLE_API __attribute__((visibility("default"))) int extra_twice(int x);
SAMPLE_API int extra_call(sample_cb cb, int x);
SAMPLE_API opaque_handle *extra_open(void);
SAMPLE_API named_pair extra_make_pair(int a, int b);

#endif
