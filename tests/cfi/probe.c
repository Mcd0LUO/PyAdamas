// CFI 探针：用 libgcc 的 _Unwind_Backtrace 从当前帧往上走一遍。
// 它只认 .eh_frame 里的展开规则，既不解析 prologue 也不跟 rbp 链，
// 所以生成代码缺 CFI 时走到第一个生成帧就断掉，深度只剩 2。
#include "probe.h"

#include <unwind.h>

typedef struct {
    long n;
} Probe;

static _Unwind_Reason_Code probe_cb(struct _Unwind_Context *ctx, void *arg) {
    (void)ctx;
    Probe *p = (Probe *)arg;
    p->n++;
    return _URC_NO_REASON;
}

long probe_depth(void) {
    Probe p = {0};
    _Unwind_Backtrace(probe_cb, &p);
    return p.n;
}
