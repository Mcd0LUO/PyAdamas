/* rt_personality.c —— 零开销异常：Itanium C++ ABI 的 personality routine
 *
 * 设计文档：docs/zero-cost-exceptions.md。这里实现 Z1 需要的通用骨架：
 *   - 从 .eh_frame / .gcc_except_table 里取 LSDA，**真正通用解码**
 *     uleb128 / sleb128 / encoded pointer，不硬编码任何偏移；
 *   - phase 1（_UA_SEARCH_PHASE）查 call-site 表：PC 落在保护区、该条目
 *     带 landing pad 且 action != 0（不是纯清理）→ _URC_HANDLER_FOUND；
 *   - phase 2（_UA_CLEANUP_PHASE）：
 *       带 _UA_HANDLER_FRAME  → SetGR(GR0=exc) + SetIP(lp) → INSTALL_CONTEXT
 *       纯清理条目（action == 0）→ 同样安装上下文，landing pad 里自己
 *                                 _Unwind_Resume 继续展开
 *       其它帧（外层的 catch 帧）→ CONTINUE_UNWIND
 *   - **刻意不做类型匹配**（文档 101-104 行）：action != 0 一律当作 catch-all，
 *     landing pad 统一跳到编译器生成的 except 分发块，由它按类型匹配。
 *     所以 TType 表可以整个省略（encoding 0xff）；本文件只是把表头解出来
 *     备用，不做 filter 查询。
 *
 * 自包含：只依赖 <unwind.h>/<stdint.h>/<stddef.h>，不 include rt_internal.h。
 * 需要能被 gcc -O2 -Wall -Wextra -Werror -I runtime -c 直接编过（将来进 rt.o）。
 *
 * 关于 TType 偏移的实测结论（GCC 14 x86-64，z1 夹具核对）：
 *   LSDA 头里的 TType offset 是 uleb128，语义是「从该 uleb 字段之后的位置
 *   到 TType 表**末尾**的偏移」；GCC 的 get_ttype_entry 用
 *   TType - filter_index * size_of_encoded_value 反向下标取条目。
 *   本文件把 TType 表末尾指针存在 info.ttype_end，暂不使用。
 */

#include <unwind.h>
#include <stdint.h>
#include <stddef.h>
#ifdef PA_PERSONALITY_TRACE
#include <stdio.h>
#endif

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
    PA_LSDA_ERR_TRUNCATED = 2,  /* LEB128 过长、call-site 表长度越界 */
    PA_LSDA_ERR_OVERFLOW = 3,   /* 表项数量/长度超出合理上限 */
    PA_LSDA_ERR_NO_LSDA = 4     /* .cfi_lsda 缺失（本函数不该被调用） */
};

/* 最近一次解码错误：驱动 / 测试可读；正常运行只在真的坏表时才写。 */
int pa_personality_last_error = PA_LSDA_OK;

const char *pa_personality_error_str(int code)
{
    switch (code) {
    case PA_LSDA_OK: return "ok";
    case PA_LSDA_ERR_ENCODING: return "nonconforming-or-unsupported DW_EH_PE encoding";
    case PA_LSDA_ERR_TRUNCATED: return "truncated or overlong LEB128 / bad call-site length";
    case PA_LSDA_ERR_OVERFLOW: return "call-site table too large (plausibility limit)";
    case PA_LSDA_ERR_NO_LSDA: return "no LSDA in this frame";
    default: return "unknown";
    }
}

#ifdef PA_PERSONALITY_TRACE
/* 测试用观测点：Z1 驱动据此断言两阶段都被走到、各帧行为符合预期。
   只在 -DPA_PERSONALITY_TRACE 时存在，生产构建没有任何额外开销。 */
volatile int pa_dbg_calls;
volatile int pa_dbg_search;
volatile int pa_dbg_cleanup;
volatile int pa_dbg_handler_frames;
volatile int pa_dbg_installs;
volatile int pa_dbg_continue;
volatile int pa_dbg_errors;
volatile int pa_dbg_force_unwind;   /* _UA_FORCE_UNWIND 分支被走到的次数 */
volatile int pa_dbg_setip_calls;    /* _Unwind_SetIP 被调用的总次数 */
volatile int pa_dbg_verbose;   /* 1 = 每帧打一行日志（测试用 Z1_TRACE=1 打开） */
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

/* 读一个 encoded pointer。base 是 funcrel 用到的函数起点。 */
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
        /* 值放在按指针大小对齐的位置（此时格式位被忽略，按 absptr 读）。 */
        p = (const unsigned char *)(((uintptr_t)p + sizeof(void *) - 1)
                                    & ~(uintptr_t)(sizeof(void *) - 1));
        raw = *(const uint64_t *)(const void *)p;
        p += 8;
    } else {
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

        if (app == PA_EH_PE_pcrel)
            raw += (uint64_t)(uintptr_t)field;
        else if (app == PA_EH_PE_funcrel)
            raw += base;
        else if (app != 0x00) {
            *err = PA_LSDA_ERR_ENCODING;
            return 0;
        }
    }

    if (enc & PA_EH_PE_indirect) {
        uint64_t addr = raw;
        raw = *(const uint64_t *)(uintptr_t)addr;
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
    uint64_t ttype_end;             /* TType 表末尾地址（filter i 从后往前取），未用 */
    unsigned char call_site_encoding;
    const unsigned char *call_site_table;
    uint64_t call_site_len;
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

    /* 2) TType encoding + TType 表偏移（0xff = 没有类型表） */
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

    /* 3) call-site 表 encoding + 字节长度 */
    info->call_site_encoding = *p++;
    if (!pa_encoding_ok(info->call_site_encoding))
        return PA_LSDA_ERR_ENCODING;
    info->call_site_len = pa_uleb128(&p, &err);
    if (err)
        return err;
    info->call_site_table = p;
    return PA_LSDA_OK;
}

/* ── call-site 表查询 ─────────────────────────────────────────────── */

/* Z3 扩展点：这里的 action 只与 0 做比较，**刻意不读 action 表里的 filter**，
   避免假设 filter 一定是 0 或 1。将来做 phase 1 类型过滤时，在 action != 0
   的分支里从 action 表（call_site 表之后）读 sleb128 的 filter 即可，本函数
   已经保留了 info.call_site_table / call_site_len，方便算 action 表起点。

   在 call-site 表里找覆盖 pc 的条目。
   返回 1 = 命中（带 landing pad），0 = 没命中或该条目 landing pad 为 0，
   -1 = 表解码失败（err 里给错误码）。 */
static int pa_find_landing_pad(const pa_lsda_info *info, uintptr_t pc,
                               uint64_t *lp_out, uint64_t *action_out, int *err)
{
    const unsigned char *p = info->call_site_table;
    uint64_t remaining = info->call_site_len;
    int entries = 0;

    while (remaining > 0) {
        const unsigned char *entry = p;
        uint64_t start, len, lp, action;
        uint64_t consumed;

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

        consumed = (uint64_t)(p - entry);
        if (consumed == 0 || consumed > remaining)
            return -1;                  /* 表长度和条目对不上：坏表 */

        remaining -= consumed;
        if (++entries > 4096) {
            *err = PA_LSDA_ERR_OVERFLOW;
            return -1;
        }

        if (pc >= info->lp_start + start && pc < info->lp_start + start + len) {
            if (lp == 0)
                return 0;               /* 落在保护区但该区间没有 landing pad */
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
    uint64_t lp = 0, action = 0;
    uintptr_t ip;
    uintptr_t pc;
    int before_insn = 0;
    int err;
    int found;

    (void)ex_class;

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

    /* 没有 LSDA 的帧（例如只挂了 personality 没挂表）当作没有处理。 */
    if (!_Unwind_GetLanguageSpecificData(ctx)) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    /* PC 语义：展开器给的 IP 是抛出点 call 之后的返回地址；减 1 之后才是
       “调用指令所在地址”，这样即使 call 是保护区最后一条指令（返回地址
       正好等于区间末尾）也能命中 [start,start+len)。 */
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

    found = pa_find_landing_pad(&info, pc, &lp, &action, &err);
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

#ifdef PA_PERSONALITY_TRACE
    if (pa_dbg_verbose)
        fprintf(stderr,
                "[pa_personality] actions=%#x ip=%#lx pc=%#lx cfa=%#lx region=%#lx lp_start=%#lx found=%d lp=%#lx action=%lu\n",
                (unsigned)actions, (unsigned long)ip, (unsigned long)pc,
                (unsigned long)_Unwind_GetCFA(ctx),
                (unsigned long)info.region_start, (unsigned long)info.lp_start,
                found, (unsigned long)lp, (unsigned long)action);
#endif

    if (found == 0 || lp == 0) {
        /* 当前 PC 不在任何带 landing pad 的保护区里：穿过这一帧。 */
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

    if (actions & _UA_SEARCH_PHASE) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_search;
#endif
        if (action == 0) {
            /* 纯清理条目不是 handler：phase 1 继续往外找。 */
#ifdef PA_PERSONALITY_TRACE
            ++pa_dbg_continue;
#endif
            return _URC_CONTINUE_UNWIND;
        }
        return _URC_HANDLER_FOUND;
    }

    /* ── phase 2 ────────────────────────────────────────────────── */
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_cleanup;
#endif

    if (actions & _UA_HANDLER_FRAME) {
        if (action == 0) {
            /* 理论上 phase 1 不会把纯清理帧记成 handler；防御性穿过。 */
#ifdef PA_PERSONALITY_TRACE
            ++pa_dbg_continue;
#endif
            return _URC_CONTINUE_UNWIND;
        }
    } else if (action != 0) {
        /* 外层 catch 帧：本轮展开在更内层就已经找到 handler，这里只跳过。 */
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_continue;
#endif
        return _URC_CONTINUE_UNWIND;
    }

#ifdef PA_PERSONALITY_TRACE
    if (actions & _UA_HANDLER_FRAME)
        ++pa_dbg_handler_frames;
    ++pa_dbg_installs;
#endif

    /* 安装上下文：GR0 = exception 指针（x86-64 的 %rax）；GR1 是类型信息槽，
       简化路径用不到，清 0 免得 landing pad 误读。
       安装后 landing pad 入口的 rsp = 抛出点那条 call 之前的 rsp（不是 CFA），
       所以编译器生成的 LP 可以照抄正常 epilogue 来恢复栈。 */
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_setip_calls;
#endif
    _Unwind_SetGR(ctx, 0, (uintptr_t)exc);
    _Unwind_SetGR(ctx, 1, 0);
    _Unwind_SetIP(ctx, (uintptr_t)lp);
    return _URC_INSTALL_CONTEXT;
}
