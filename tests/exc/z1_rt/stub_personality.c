/* stub_personality.c —— 第一步用的“假” personality routine。
 *
 * 刻意不解析 LSDA：landing pad 地址直接硬编码（z1_step1_lp），
 * 只用 _Unwind_GetIP 是否落在 z1_step1_fn 的地址区间判断当前帧是不是自己的帧。
 * 它保留在仓库里作对照：第二步换成真的 pa_personality 后，行为应当一致。
 */
#include <unwind.h>
#include <stdint.h>
#include <stdio.h>

extern char z1_step1_fn[];
extern char z1_step1_end[];
extern char z1_step1_lp[];

/* 观测点：driver 断言这些计数，用来证明两阶段都被走到。 */
volatile int z1_stub_calls;           /* personality 被调用总次数 */
volatile int z1_stub_search_calls;    /* _UA_SEARCH_PHASE 次数 */
volatile int z1_stub_cleanup_calls;   /* _UA_CLEANUP_PHASE 次数 */
volatile int z1_stub_handler_frame;   /* 带 _UA_HANDLER_FRAME 的次数 */
volatile int z1_stub_found;           /* 返回 HANDLER_FOUND 的次数 */
volatile int z1_stub_installs;        /* 返回 INSTALL_CONTEXT 的次数 */

#ifndef Z1_TRACE_DEFAULT
#define Z1_TRACE_DEFAULT 0
#endif
volatile int z1_stub_trace = Z1_TRACE_DEFAULT;  /* 1 = 往 stderr 打每次调用细节 */

/* 必须非 static + used：.cfi_personality 只保留符号引用，-O2 下
   没被 C 代码引用的 static 函数会被当死代码删掉。 */
__attribute__((used))
_Unwind_Reason_Code
z1_stub_personality(int version, _Unwind_Action actions,
                    _Unwind_Exception_Class ex_class,
                    struct _Unwind_Exception *exc,
                    struct _Unwind_Context *ctx)
{
    (void)version;
    (void)ex_class;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);

    ++z1_stub_calls;

    /* 只有 IP 落在自己的函数体里才认领。展开器给的是“调用点之后的
       返回地址”，对第一步来说它一定在 [z1_step1_fn, z1_step1_end) 内。 */
    if (ip < (uintptr_t)z1_step1_fn || ip >= (uintptr_t)z1_step1_end) {
        if (z1_stub_trace)
            fprintf(stderr, "[stub] ip=%#lx 不在本帧 [%p,%p)，CONTINUE\n",
                    (unsigned long)ip, (void *)z1_step1_fn, (void *)z1_step1_end);
        return _URC_CONTINUE_UNWIND;
    }

    if (actions & _UA_SEARCH_PHASE) {
        ++z1_stub_search_calls;
        ++z1_stub_found;
        if (z1_stub_trace)
            fprintf(stderr, "[stub] SEARCH ip=%#lx -> HANDLER_FOUND\n", (unsigned long)ip);
        return _URC_HANDLER_FOUND;
    }

    ++z1_stub_cleanup_calls;
    if (actions & _UA_HANDLER_FRAME)
        ++z1_stub_handler_frame;
    if (z1_stub_trace)
        fprintf(stderr, "[stub] CLEANUP ip=%#lx actions=%#x handler_frame=%d -> INSTALL %p\n",
                (unsigned long)ip, (unsigned)actions,
                (actions & _UA_HANDLER_FRAME) ? 1 : 0, (void *)z1_step1_lp);

    /* 把 exception 指针塞进 GR0（x86-64 = %rax），再把 IP 设成硬编码的
       landing pad。返回 INSTALL_CONTEXT 后展开器就会跳过去。 */
    _Unwind_SetGR(ctx, 0, (uintptr_t)exc);
    _Unwind_SetIP(ctx, (uintptr_t)z1_step1_lp);
    ++z1_stub_installs;
    return _URC_INSTALL_CONTEXT;
}
