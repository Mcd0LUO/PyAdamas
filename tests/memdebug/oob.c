/* oob.c —— 分配器调试开关的「越界写制造器」。
 *
 * 为什么需要 C：Python 层没有合法方式写越界（列表/字符串下标都有检查），
 * cimport 的 ptr/str 参数是唯一能把裸指针交给 C 的入口。这里故意写坏
 * PyAdamasStr 的字符区之后的内存：
 *   - canary 模式：写进对象尾部的 32 字节保护带 → 释放时 panic；
 *   - guard 模式：写穿到紧随其后的 PROT_NONE 页 → 立刻 SIGSEGV。
 * 只有 demo 用，不进常规套件。
 */
#include <stddef.h>

/* 从 p 起写 n 个 0x5A 字节（p 是 str 的字符区指针）。 */
void pa_memdbg_stomp(char* p, long off, long n) {
    long i;
    for (i = 0; i < n; ++i) p[off + i] = 0x5A;
}
