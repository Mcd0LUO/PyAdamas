/* rt_personality.c —— 零开销异常：Itanium C++ ABI 的 personality routine
 *
 * 设计文档：docs/zero-cost-exceptions.md。
 *
 * 语义（Z1.5 起按 ABI 口径，与 libstdc++ __gxx_personality_v0 对齐）：
 *   - phase 1（_UA_SEARCH_PHASE）：查 call-site 表找到覆盖 PC 的条目；
 *     action 下标 == 0            → 只有 cleanup，继续往外找（CONTINUE_UNWIND）；
 *     action 下标 != 0            → 解析 action 记录链（两个 SLEB128：filter, disp）：
 *          filter == 0            → cleanup（CONTINUE_UNWIND）
 *          filter >  0            → handler（读 TType 表判 catch-all / typed catch）
 *          filter <  0            → exception spec（本实现不维护异常规格，选择
 *                                   CONTINUE_UNWIND + 诊断计数，见 README「已知局限」）
 *   - phase 2（_UA_CLEANUP_PHASE）：
 *          _UA_HANDLER_FRAME + filter>0 → SetGR/SetIP → _URC_INSTALL_CONTEXT
 *          没有 HANDLER_FRAME + 纯 cleanup → SetGR/SetIP → _URC_INSTALL_CONTEXT
 *          （清理 landing pad 自己负责 _Unwind_Resume 继续展开）
 *          其它帧（外层 handler 帧等）→ CONTINUE_UNWIND
 *
 * 类型匹配：**仍然不做**（Z3 再做）。具体表现：filter > 0 时虽然会把 TType 表项解出来，
 * 但非 null 的 typed catch 也一律按「匹配」处理（catch-all 语义）。所以我们自己的
 * 异常（不跨 C++ 类型系统）行为是对的；与 C++ 类型系统互通要等 Z3/Z4 补类型匹配。
 *
 * LSDA 解码是通用的（uleb128/sleb128/encoded pointer，不硬编码偏移），
 * 并遵循 libgcc/unwind-pe.h 的关键细节：**读出的原始值为 0 时不施加 pcrel/funcrel
 * 相对基址、也不做 indirect 解引用**（这正是 GCC catch-all 的「4 字节 0 + 无重定位」
 * 能表示 null type_info* 的原因）。
 *
 * 自包含：只依赖 <unwind.h>/<stdint.h>/<stddef.h>，不 include rt_internal.h。
 * 需要能被 gcc -O2 -Wall -Wextra -Werror -I runtime -c 直接编过（将来进 rt.o）。
 */

#include <unwind.h>
#include <stdint.h>
#include <stddef.h>
#ifdef PA_PERSONALITY_TRACE
#include <stdio.h>
#endif

/* 方案 G 的帧清理钩子（docs/exc-frame-cleanup.md §5.2）：phase 2 走到
   _UA_HANDLER_FRAME 那一帧时、安装 landing pad 之前，批量释放比 handler 帧更
   深的帧记录。本文件刻意自包含（tests/exc/z1_rt 直接编译链接它，不链接
   rt.o），所以实现不在本编译单元：runtime/rt_eh.c 用构造函数把包装装进来。
   没装（Z1 夹具）时为 NULL，personality 只做 ABI 安装、不做清理。
   签名带 ex_class：由实现方决定只对「我们自己的异常」释放帧（foreign 异常
   不碰我们的帧链）。 */
void (*pa_personality_frame_hook)(uint64_t ex_class, uintptr_t target_cfa);

/* ── DWARF 异常表编码常量（GCC 的 unwind.h 里没有，自己定义保持自包含） ── */
enum {
    PA_EH_PE_absptr = 0x00,
    PA_EH_PE_uleb128 = 0x01,
    PA_EH_PE_udata2 = 0x02,
    PA_EH_PE_udata4 = 0x03,
    PA_EH_PE_udata8 = 0x04,
    PA_EH_PE_sleb128 = 0x09,
    PA_EH_PE_sdata2 = 0x0a,
    PA_EH_PE_sdata4 = 0x0b,
    PA_EH_PE_sdata8 = 0x0c,

    PA_EH_PE_pcrel = 0x10,
    PA_EH_PE_textrel = 0x20,
    PA_EH_PE_datarel = 0x30,
    PA_EH_PE_funcrel = 0x40,
    PA_EH_PE_aligned = 0x50,
    PA_EH_PE_indirect = 0x80,
    PA_EH_PE_omit = 0xff
};

/* LSDA 解码错误码：0 = 没错。测试用例故意改坏字节时按这些码断言。 */
enum {
    PA_LSDA_OK = 0,
    PA_LSDA_ERR_ENCODING = 1,   /* 非法 / 不支持的 DW_EH_PE 编码字节 */
    PA_LSDA_ERR_TRUNCATED = 2,  /* LEB128 过长、call-site 表长度越界（表越界） */
    PA_LSDA_ERR_OVERFLOW = 3,   /* 表项数量/长度超出合理上限 */
    PA_LSDA_ERR_NO_LSDA = 4,    /* .cfi_lsda 缺失（本函数不该被调用） */
    PA_LSDA_ERR_ACTION = 5,     /* action 记录链非法（指针不前进 / 记录过多） */
    PA_LSDA_ERR_TTYPE = 6       /* filter>0 但没有可用 TType 表（ABI 下无法解析 catch） */
};

/* 最近一次解码错误：驱动 / 测试可读；正常运行只在真的坏表时才写。 */
int pa_personality_last_error = PA_LSDA_OK;

const char *pa_personality_error_str(int code)
{
    switch (code) {
    case PA_LSDA_OK: return "ok";
    case PA_LSDA_ERR_ENCODING: return "nonconforming-or-unsupported DW_EH_PE encoding";
    case PA_LSDA_ERR_TRUNCATED: return "truncated/overlong LEB128 or call-site table out of bounds";
    case PA_LSDA_ERR_OVERFLOW: return "table too large (plausibility limit)";
    case PA_LSDA_ERR_NO_LSDA: return "no LSDA in this frame";
    case PA_LSDA_ERR_ACTION: return "malformed action record chain";
    case PA_LSDA_ERR_TTYPE: return "filter>0 but no usable TType table";
    default: return "unknown";
    }
}

#ifdef PA_PERSONALITY_TRACE
/* 测试用观测点：Z1.5 驱动据此断言 ABI 语义（filter 判定、catch-all、spec 处理）。
   只在 -DPA_PERSONALITY_TRACE 时存在，生产构建没有这些额外开销。 */
volatile int pa_dbg_calls;
volatile int pa_dbg_search;
volatile int pa_dbg_cleanup;
volatile int pa_dbg_handler_frames;
volatile int pa_dbg_installs;
volatile int pa_dbg_continue;
volatile int pa_dbg_errors;
volatile int pa_dbg_force_unwind;      /* _UA_FORCE_UNWIND 分支被走到的次数 */
volatile int pa_dbg_setip_calls;       /* _Unwind_SetIP 被调用的总次数 */
volatile int pa_dbg_cleanup_entries;   /* 判定为 cleanup 的 action（不认领 handler） */
volatile int pa_dbg_catch_all;         /* filter>0 且 TType 项为 null（catch-all） */
volatile int pa_dbg_typed_catch;       /* filter>0 且 TType 项非 null（typed catch，Z3 才做匹配） */
volatile int pa_dbg_spec;              /* filter<0：exception spec，本实现选择 CONTINUE */
volatile int pa_dbg_verbose;           /* 1 = 每帧打一行日志（测试用 Z1_TRACE=1 打开） */
#endif

/* ── LEB128 解码 ─────────────────────────────────────────────────── */

static uint64_t pa_uleb128(const unsigned char **pp, int *err)
{
    uint64_t value = 0;
    unsigned shift = 0;
    const unsigned char *p = *pp;

    for (int i = 0; i < 10; ++i) {
        unsigned char b = *p++;
        value |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *pp = p;
            return value;
        }
        shift += 7;
    }
    *err = PA_LSDA_ERR_TRUNCATED;
    *pp = p;
    return 0;
}

static int64_t pa_sleb128(const unsigned char **pp, int *err)
{
    uint64_t value = 0;
    unsigned shift = 0;
    const unsigned char *p = *pp;

    for (int i = 0; i < 10; ++i) {
        unsigned char b = *p++;
        value |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40))
                value |= ~(uint64_t)0 << shift;   /* 符号扩展 */
            *pp = p;
            return (int64_t)value;
        }
    }
    *err = PA_LSDA_ERR_TRUNCATED;
    *pp = p;
    return 0;
}

/* 编码字节是否合法（低 4 位 = 数据类型，0x70 位 = 应用方式）。 */
static int pa_encoding_ok(unsigned char enc)
{
    switch (enc & 0x0f) {
    case PA_EH_PE_absptr:
    case PA_EH_PE_uleb128:
    case PA_EH_PE_udata2:
    case PA_EH_PE_udata4:
    case PA_EH_PE_udata8:
    case PA_EH_PE_sleb128:
    case PA_EH_PE_sdata2:
    case PA_EH_PE_sdata4:
    case PA_EH_PE_sdata8:
        break;
    default:
        return 0;
    }
    switch (enc & 0x70) {
    case 0x00:              /* absptr，值就是绝对地址 */
    case PA_EH_PE_pcrel:    /* 相对字段自身 */
    case PA_EH_PE_funcrel:  /* 相对函数起点 */
    case PA_EH_PE_aligned:  /* 先按指针大小对齐再读 absptr */
        break;
    default:                /* textrel/datarel：运行时要段基址，当前产物用不到 */
        return 0;
    }
    return 1;
}

/* 固定宽度的编码值大小；uleb/sleb/omit 返回 0（可变长/无表）。 */
static unsigned pa_size_of_encoded_value(unsigned char enc)
{
    switch (enc & 0x0f) {
    case PA_EH_PE_absptr: return (unsigned)sizeof(void *);
    case PA_EH_PE_udata2:
    case PA_EH_PE_sdata2: return 2;
    case PA_EH_PE_udata4:
    case PA_EH_PE_sdata4: return 4;
    case PA_EH_PE_udata8:
    case PA_EH_PE_sdata8: return 8;
    default: return 0;
    }
}

/* 读一个 encoded pointer。base 是 funcrel 用到的函数起点。
   与 libgcc/unwind-pe.h:read_encoded_value_with_base 一致：
   **原始值为 0 时不做相对基址加法、也不做 indirect 解引用**，
   否则 GCC catch-all 的「4 字节 0 + 无重定位」TType 项会被解成非法地址。 */
static uint64_t pa_read_encoded(const unsigned char **pp, unsigned char enc,
                                uintptr_t base, int *err)
{
    const unsigned char *p = *pp;
    const unsigned char *field = p;
    unsigned char app = enc & 0x70;
    uint64_t raw = 0;

    if (!pa_encoding_ok(enc)) {
        *err = PA_LSDA_ERR_ENCODING;
        return 0;
    }

    if (app == PA_EH_PE_aligned) {
        /* 值放在按指针大小对齐的位置（此时格式位被忽略，按 absptr 读）
           且不参与 pcrel/indirect 处理（与 libgcc 一致）。 */
        p = (const unsigned char *)(((uintptr_t)p + sizeof(void *) - 1)
                                    & ~(uintptr_t)(sizeof(void *) - 1));
        raw = *(const uint64_t *)(const void *)p;
        p += 8;
        *pp = p;
        return raw;
    }

    switch (enc & 0x0f) {
    case PA_EH_PE_absptr:
    case PA_EH_PE_udata8:
    case PA_EH_PE_sdata8:
        raw = *(const uint64_t *)(const void *)p;   /* x86-64 上 absptr = 8 字节 */
        p += 8;
        break;
    case PA_EH_PE_uleb128:
        raw = pa_uleb128(&p, err);
        break;
    case PA_EH_PE_udata2:
        raw = *(const uint16_t *)(const void *)p;
        p += 2;
        break;
    case PA_EH_PE_udata4:
        raw = *(const uint32_t *)(const void *)p;
        p += 4;
        break;
    case PA_EH_PE_sleb128:
        raw = (uint64_t)pa_sleb128(&p, err);
        break;
    case PA_EH_PE_sdata2:
        raw = (uint64_t)(int64_t)*(const int16_t *)(const void *)p;
        p += 2;
        break;
    case PA_EH_PE_sdata4:
        raw = (uint64_t)(int64_t)*(const int32_t *)(const void *)p;
        p += 4;
        break;
    default:
        *err = PA_LSDA_ERR_ENCODING;
        return 0;
    }
    if (*err)
        return 0;

    if (raw != 0) {
        if (app == PA_EH_PE_pcrel)
            raw += (uint64_t)(uintptr_t)field;
        else if (app == PA_EH_PE_funcrel)
            raw += base;
        else if (app != 0x00) {
            *err = PA_LSDA_ERR_ENCODING;
            return 0;
        }
        if (enc & PA_EH_PE_indirect)
            raw = *(const uint64_t *)(uintptr_t)raw;
    }

    *pp = p;
    return raw;
}

/* ── LSDA 头解析 ─────────────────────────────────────────────────── */

typedef struct {
    const unsigned char *lsda;
    uintptr_t region_start;         /* _Unwind_GetRegionStart：函数起点 */
    uint64_t lp_start;              /* LPStart：landing pad 偏移的基准 */
    unsigned char ttype_encoding;
    uint64_t ttype_end;             /* TType 表末尾地址（第 i 项在 ttype_end - i*size），
                                       0 表示没有类型表 */
    unsigned char call_site_encoding;
    const unsigned char *call_site_table;
    const unsigned char *action_table;  /* call-site 表之后紧跟 action 表 */
} pa_lsda_info;

static int pa_parse_lsda(struct _Unwind_Context *ctx, pa_lsda_info *info)
{
    const unsigned char *p;
    unsigned char lp_enc;
    int err = PA_LSDA_OK;

    p = (const unsigned char *)_Unwind_GetLanguageSpecificData(ctx);
    info->lsda = p;
    info->region_start = (uintptr_t)_Unwind_GetRegionStart(ctx);
    info->lp_start = info->region_start;
    info->ttype_encoding = PA_EH_PE_omit;
    info->ttype_end = 0;
    if (!p)
        return PA_LSDA_ERR_NO_LSDA;

    /* 1) LPStart encoding（0xff = 省略，用函数起点） */
    lp_enc = *p++;
    if (lp_enc != PA_EH_PE_omit) {
        if (!pa_encoding_ok(lp_enc))
            return PA_LSDA_ERR_ENCODING;
        info->lp_start = pa_read_encoded(&p, lp_enc, info->region_start, &err);
        if (err)
            return err;
    }

    /* 2) TType encoding + TType 表偏移（0xff = 没有类型表，偏移字段整段消失） */
    info->ttype_encoding = *p++;
    if (info->ttype_encoding != PA_EH_PE_omit) {
        uint64_t off;
        if (!pa_encoding_ok(info->ttype_encoding))
            return PA_LSDA_ERR_ENCODING;
        off = pa_uleb128(&p, &err);
        if (err)
            return err;
        info->ttype_end = (uint64_t)(uintptr_t)p + off;
    }

    /* 3) call-site 表 encoding + 字节长度；action 表紧跟在 call-site 表之后 */
    info->call_site_encoding = *p++;
    if (!pa_encoding_ok(info->call_site_encoding))
        return PA_LSDA_ERR_ENCODING;
    {
        uint64_t cs_len = pa_uleb128(&p, &err);
        if (err)
            return err;
        info->call_site_table = p;
        info->action_table = p + cs_len;
    }
    return PA_LSDA_OK;
}

/* 取 TType 表第 filter 项（1 起、反向索引：ttype_end - filter*size）。
   返回 0 表示 catch-all（null type_info*），非 0 是 typed catch 的 type_info 指针。 */
static uint64_t pa_get_ttype_entry(const pa_lsda_info *info, uint64_t filter, int *err)
{
    unsigned size = pa_size_of_encoded_value(info->ttype_encoding);
    uint64_t off;
    uint64_t addr;
    const unsigned char *p;

    if (size == 0 || info->ttype_end == 0) {
        *err = PA_LSDA_ERR_TTYPE;
        return 0;
    }
    off = filter * (uint64_t)size;
    if (off > info->ttype_end || (uint64_t)info->region_start > info->ttype_end - off) {
        *err = PA_LSDA_ERR_TTYPE;   /* 反向索引下溢 */
        return 0;
    }
    addr = info->ttype_end - off;
    p = (const unsigned char *)(uintptr_t)addr;
    return pa_read_encoded(&p, info->ttype_encoding, info->region_start, err);
}

/* ── action 记录分类 ─────────────────────────────────────────────── */

enum pa_action_kind {
    PA_ACTION_CLEANUP = 0,  /* filter==0（或 call-site action 下标为 0） */
    PA_ACTION_HANDLER = 1,  /* filter>0 */
    PA_ACTION_SPEC = 2      /* filter<0：exception spec，本实现不支持 */
};

#define PA_MAX_ACTION_DEPTH 64

/* 解析 call-site 的 action 下标对应的 action 记录链。
   ABI：action 下标 0 = 没有 action 记录（纯 cleanup）；否则从
   action_table + (action - 1) 起读 (SLEB filter, SLEB disp)，disp!=0 时继续下一条。 */
static int pa_classify_action(const pa_lsda_info *info, uint64_t cs_action,
                              enum pa_action_kind *kind, int64_t *filter_out, int *err)
{
    const unsigned char *p;

    if (cs_action == 0) {
        *kind = PA_ACTION_CLEANUP;      /* 只有 cleanup，不读 action 表 */
        *filter_out = 0;
        return PA_LSDA_OK;
    }
    p = info->action_table + (cs_action - 1);
    for (int depth = 0; depth < PA_MAX_ACTION_DEPTH; ++depth) {
        int64_t filter = pa_sleb128(&p, err);
        if (*err)
            return *err;
        (void)pa_sleb128(&p, err);   /* disp：typed catch 不匹配时才沿链继续；
                                        我们不做类型匹配，所以不追链 */
        if (*err)
            return *err;

        *filter_out = filter;
        if (filter == 0) {
            *kind = PA_ACTION_CLEANUP;  /* 零 filter = cleanup（ABI 语义） */
            return PA_LSDA_OK;
        }
        if (filter < 0) {
            *kind = PA_ACTION_SPEC;     /* 负 filter = exception spec */
            return PA_LSDA_OK;
        }

        /* filter > 0：ABI 上要查 TType 表并做类型匹配。我们还没有类型匹配
           （Z3 再做），这里读出类型项（顺便校验表），然后无论 null（catch-all）
           还是 typed 都按「handler」处理。 */
        {
            uint64_t ttype = pa_get_ttype_entry(info, (uint64_t)filter, err);
            if (*err)
                return *err;
#ifdef PA_PERSONALITY_TRACE
            if (ttype == 0)
                ++pa_dbg_catch_all;
            else
                ++pa_dbg_typed_catch;
#endif
            (void)ttype;
        }
        *kind = PA_ACTION_HANDLER;
        return PA_LSDA_OK;
    }
    *err = PA_LSDA_ERR_ACTION;
    return *err;
}

/* ── call-site 表查询 ─────────────────────────────────────────────── */

/* 在 call-site 表里找覆盖 pc 的条目。
   返回 1 = 命中（lp/action 已填），0 = 没命中或该条目 landing pad 为 0，
   -1 = 表解码失败（err 里给错误码）。 */
static int pa_find_call_site(const pa_lsda_info *info, uintptr_t pc,
                             uint64_t *lp_out, uint64_t *action_out, int *err)
{
    const unsigned char *p = info->call_site_table;
    int entries = 0;

    while (p < info->action_table) {
        const unsigned char *entry = p;
        uint64_t start, len, lp;
        uint64_t action;

        start = pa_read_encoded(&p, info->call_site_encoding, info->region_start, err);
        if (*err)
            return -1;
        len = pa_read_encoded(&p, info->call_site_encoding, info->region_start, err);
        if (*err)
            return -1;
        lp = pa_read_encoded(&p, info->call_site_encoding, info->region_start, err);
        if (*err)
            return -1;
        action = pa_uleb128(&p, err);   /* action 下标固定是 uleb128 */
        if (*err)
            return -1;

        if ((uint64_t)(p - entry) == 0 || p > info->action_table) {
            *err = PA_LSDA_ERR_TRUNCATED;   /* 表长和条目对不上：结构越界 */
            return -1;
        }

        if (++entries > 4096) {
            *err = PA_LSDA_ERR_OVERFLOW;
            return -1;
        }

        /* call-site 表按 start 排序，可以提前结束。 */
        if (pc < info->lp_start + start)
            return 0;
        if (pc < info->lp_start + start + len) {
            if (lp == 0)
                return 0;       /* 落在保护区但该区间没有 landing pad */
            *lp_out = info->lp_start + lp;
            *action_out = action;
            return 1;
        }
    }
    return 0;
}

/* ── personality routine ─────────────────────────────────────────── */

/* 必须非 static + used：.cfi_personality 只保留符号引用，
   -O2 下没被 C 代码直接引用的函数会被当死代码删掉。 */
__attribute__((used))
_Unwind_Reason_Code
pa_personality(int version, _Unwind_Action actions,
               _Unwind_Exception_Class ex_class,
               struct _Unwind_Exception *exc,
               struct _Unwind_Context *ctx)
{
    pa_lsda_info info;
    enum pa_action_kind kind;
    uint64_t lp = 0, cs_action = 0;
    int64_t filter = 0;
    uintptr_t ip;
    uintptr_t pc;
    int before_insn = 0;
    int err;
    int found;

#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_calls;
#endif

    if (version != 1)
        return _URC_FATAL_PHASE1_ERROR;

    /* 强制展开（_UA_FORCE_UNWIND，来自 _Unwind_ForcedUnwind；注意
       _Unwind_Backtrace 本身不调用 personality）：只穿过，绝不安装 landing pad，
       否则运行时的 traceback / 线程取消会崩。 */
    if (actions & _UA_FORCE_UNWIND) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_force_unwind;
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* 没有 LSDA 的帧当作没有处理。 */
    if (!_Unwind_GetLanguageSpecificData(ctx)) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* PC 语义：展开器给的 IP 是抛出点 call 之后的返回地址；减 1 之后才是
       “调用指令所在地址”，这样即使 call 是保护区最后一条指令（返回地址
       正好等于区间末尾）也能命中 [start, start+len)。 */
    ip = (uintptr_t)_Unwind_GetIPInfo(ctx, &before_insn);
    pc = before_insn ? ip : ip - 1;

    err = pa_parse_lsda(ctx, &info);
    if (err != PA_LSDA_OK) {
        pa_personality_last_error = err;
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_errors;
        if (pa_dbg_verbose)
            fprintf(stderr, "[pa_personality] actions=%#x ip=%#lx LSDA 解码失败 err=%d\n",
                    (unsigned)actions, (unsigned long)ip, err);
#endif
        return _URC_FATAL_PHASE1_ERROR;
    }

    found = pa_find_call_site(&info, pc, &lp, &cs_action, &err);
    if (found < 0) {
        pa_personality_last_error = err;
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_errors;
        if (pa_dbg_verbose)
            fprintf(stderr, "[pa_personality] actions=%#x ip=%#lx call-site 表解码失败 err=%d\n",
                    (unsigned)actions, (unsigned long)ip, err);
#endif
        return _URC_FATAL_PHASE1_ERROR;
    }

    if (found == 0 || lp == 0) {
        /* 当前 PC 不在任何带 landing pad 的保护区里：穿过这一帧。 */
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* 到了这里说明 PC 在保护区里且有 landing pad，按 ABI 语义解析 action。 */
    err = pa_classify_action(&info, cs_action, &kind, &filter, &err);
    if (err != PA_LSDA_OK) {
        pa_personality_last_error = err;
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_errors;
        if (pa_dbg_verbose)
            fprintf(stderr, "[pa_personality] actions=%#x ip=%#lx action 表解码失败 err=%d\n",
                    (unsigned)actions, (unsigned long)ip, err);
#endif
        return _URC_FATAL_PHASE1_ERROR;
    }

    if (kind == PA_ACTION_CLEANUP) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_cleanup_entries;
#endif
    }

#ifdef PA_PERSONALITY_TRACE
    if (pa_dbg_verbose) {
        static const char *const kind_name[] = { "cleanup", "handler", "spec" };
        fprintf(stderr,
                "[pa_personality] actions=%#x ip=%#lx pc=%#lx cfa=%#lx lp=%#lx action=%lu kind=%s filter=%ld\n",
                (unsigned)actions, (unsigned long)ip, (unsigned long)pc,
                (unsigned long)_Unwind_GetCFA(ctx), (unsigned long)lp,
                (unsigned long)cs_action, kind_name[(int)kind], (long)filter);
    }
#endif

    if (actions & _UA_SEARCH_PHASE) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_search;
#endif
        if (kind == PA_ACTION_HANDLER)
            return _URC_HANDLER_FOUND;
        if (kind == PA_ACTION_SPEC) {
            /* 选择的语义：exception spec 不支持 → 当作与本帧无关，继续展开；
               不 terminate（理由见 README「filter<0 的处理」）。 */
#ifdef PA_PERSONALITY_TRACE
            ++pa_dbg_spec;
#endif
        }
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* ── phase 2 ────────────────────────────────────────────────── */
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_cleanup;
#endif

    if (actions & _UA_HANDLER_FRAME) {
        if (kind != PA_ACTION_HANDLER) {
            /* 理论上 phase 1 只会在 filter>0 的帧上记 handler；防御性穿过。 */
#ifdef PA_PERSONALITY_TRACE
            if (kind == PA_ACTION_SPEC)
                ++pa_dbg_spec;
            ++pa_dbg_continue;
#endif
            return _URC_CONTINUE_UNWIND;
        }
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_handler_frames;
#endif
    } else if (kind != PA_ACTION_CLEANUP) {
        /* 外层 handler 帧 / spec：本轮展开在更内层就已经找到 handler，跳过。 */
#ifdef PA_PERSONALITY_TRACE
        if (kind == PA_ACTION_SPEC)
            ++pa_dbg_spec;
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* 方案 G：安装 landing pad 之前，把「比本 handler 帧更深」的帧记录批量
       释放掉（语义等价于老世界 longjmp 前的 pa_frame_unwind(handler_top->frame)）。
       - phase 2 只是逻辑展开：栈指针还在抛出点，被跳过帧的物理栈还完整，
         此刻释放不会踩到退役栈；
       - LP 还没执行，任何 call 都还没从 LP 的 rsp 往下写，所以这里是整条
         展开链上最后一个能安全读死帧记录的时刻（放到 LP 里做会被 LP 的第一
         条 call 覆盖，见设计文档 §3.2 的实测）；
       - phase 1 / _UA_FORCE_UNWIND 分支在上面已经早退，不会走到这里。 */
    if ((actions & _UA_HANDLER_FRAME) && pa_personality_frame_hook)
        pa_personality_frame_hook(ex_class, (uintptr_t)_Unwind_GetCFA(ctx));

#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_installs;
    ++pa_dbg_setip_calls;
#endif

    /* 安装上下文：GR0 = exception 指针（x86-64 的 %rax）；GR1 是类型信息槽，
       简化路径用不到，清 0 免得 landing pad 误读。
       安装后 landing pad 入口的 rsp = 抛出点那条 call 之前的 rsp（不是 CFA），
       所以编译器生成的 LP 可以照抄正常 epilogue 来恢复栈。 */
    _Unwind_SetGR(ctx, 0, (uintptr_t)exc);
    _Unwind_SetGR(ctx, 1, 0);
    _Unwind_SetIP(ctx, (uintptr_t)lp);
    return _URC_INSTALL_CONTEXT;
}
