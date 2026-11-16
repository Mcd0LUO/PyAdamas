// rt_datetime.c —— datetime.date / datetime.datetime 与 pathlib.Path
#include "rt_internal.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define PYADAMAS_DT_MAGIC 0x44544d4500000001LL    /* "DTME" */
#define PYADAMAS_PATH_MAGIC 0x5041544800000001LL  /* "PATH" */

struct PyAdamasDateTime {
    int64_t magic;
    int64_t year, month, day;
    int64_t hour, minute, second;
    int64_t is_date; /* 1 = date（没有时分秒），0 = datetime */
};

struct PyAdamasPath {
    int64_t magic;
    PyAdamasStr* path; /* owned */
};

static void dt_destroy(void* p) {
    (void)p; /* 没有指针字段 */
}

static PyAdamasDateTime* dt_make(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                             int64_t s, int64_t is_date) {
    if (mo < 1 || mo > 12) pa_raise_c("ValueError", "month must be in 1..12");
    static const int64_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int64_t dim = mo == 2 && leap ? 29 : kDays[mo - 1];
    if (d < 1 || d > dim) pa_raise_c("ValueError", "day is out of range for month");
    if (h < 0 || h > 23) pa_raise_c("ValueError", "hour must be in 0..23");
    if (mi < 0 || mi > 59) pa_raise_c("ValueError", "minute must be in 0..59");
    if (s < 0 || s > 59) pa_raise_c("ValueError", "second must be in 0..59");
    PyAdamasDateTime* dt =
        (PyAdamasDateTime*)rc_alloc((int64_t)sizeof(PyAdamasDateTime), dt_destroy, NULL, PYADAMAS_RC_OBJ);
    dt->magic = PYADAMAS_DT_MAGIC;
    dt->year = y;
    dt->month = mo;
    dt->day = d;
    dt->hour = h;
    dt->minute = mi;
    dt->second = s;
    dt->is_date = is_date;
    return dt;
}

PyAdamasDateTime* pa_date_new(int64_t y, int64_t mo, int64_t d) {
    return dt_make(y, mo, d, 0, 0, 0, 1);
}

PyAdamasDateTime* pa_datetime_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                                int64_t s) {
    return dt_make(y, mo, d, h, mi, s, 0);
}

PyAdamasDateTime* pa_datetime_now(int64_t is_date) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (is_date) return pa_date_new(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return pa_datetime_new(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                             tmv.tm_min, tmv.tm_sec);
}

/* idx：0 year、1 month、2 day、3 hour、4 minute、5 second、6 weekday（周一=0）。 */
int64_t pa_datetime_field(PyAdamasDateTime* dt, int64_t idx) {
    switch (idx) {
        case 0: return dt->year;
        case 1: return dt->month;
        case 2: return dt->day;
        case 3: return dt->hour;
        case 4: return dt->minute;
        case 5: return dt->second;
        case 6: {
            struct tm tmv;
            memset(&tmv, 0, sizeof tmv);
            tmv.tm_year = (int)dt->year - 1900;
            tmv.tm_mon = (int)dt->month - 1;
            tmv.tm_mday = (int)dt->day;
            time_t t = timegm(&tmv);
            struct tm out;
            gmtime_r(&t, &out);
            return out.tm_wday == 0 ? 6 : out.tm_wday - 1;
        }
        default: return 0;
    }
}

PyAdamasStr* pa_datetime_isoformat(PyAdamasDateTime* dt) {
    char buf[64];
    int n;
    if (dt->is_date)
        n = snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld", (long long)dt->year,
                     (long long)dt->month, (long long)dt->day);
    else
        n = snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld",
                     (long long)dt->year, (long long)dt->month, (long long)dt->day,
                     (long long)dt->hour, (long long)dt->minute, (long long)dt->second);
    return str_new(buf, n);
}

PyAdamasStr* pa_datetime_strftime(PyAdamasDateTime* dt, PyAdamasStr* fmt) {
    char* f = (char*)malloc((size_t)fmt->len + 1);
    if (!f) panic_c("out of memory");
    memcpy(f, fmt->data, (size_t)fmt->len);
    f[fmt->len] = '\0';
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = (int)dt->year - 1900;
    tmv.tm_mon = (int)dt->month - 1;
    tmv.tm_mday = (int)dt->day;
    tmv.tm_hour = (int)dt->hour;
    tmv.tm_min = (int)dt->minute;
    tmv.tm_sec = (int)dt->second;
    char buf[512];
    size_t n = strftime(buf, sizeof buf, f, &tmv);
    free(f);
    return str_new(buf, (int64_t)n);
}

/* ── datetime.timedelta ────────────────────────────────────── */

#define PYADAMAS_TD_MAGIC 0x54444c5400000001LL /* "TDLT" */

struct PyAdamasTimeDelta {
    int64_t magic;
    /* CPython 的规范化形式：0 <= seconds < 86400，0 <= microseconds < 1000000，
       days 可正可负。 */
    int64_t days, seconds, microseconds;
};

static void td_destroy(void* p) {
    (void)p; /* 没有指针字段 */
}

static void td_normalize(int64_t* d, int64_t* s, int64_t* us) {
    int64_t sec = *s + *us / 1000000;
    int64_t micro = *us % 1000000;
    if (micro < 0) {
        micro += 1000000;
        sec -= 1;
    }
    int64_t day = *d + sec / 86400;
    int64_t rem = sec % 86400;
    if (rem < 0) {
        rem += 86400;
        day -= 1;
    }
    *d = day;
    *s = rem;
    *us = micro;
}

PyAdamasTimeDelta* pa_timedelta_new(int64_t days, int64_t seconds, int64_t microseconds) {
    PyAdamasTimeDelta* td = (PyAdamasTimeDelta*)rc_alloc(
        (int64_t)sizeof(PyAdamasTimeDelta), td_destroy, NULL, PYADAMAS_RC_OBJ);
    td->magic = PYADAMAS_TD_MAGIC;
    td->days = days;
    td->seconds = seconds;
    td->microseconds = microseconds;
    td_normalize(&td->days, &td->seconds, &td->microseconds);
    return td;
}

/* idx：0 days、1 seconds、2 microseconds。 */
int64_t pa_timedelta_field(PyAdamasTimeDelta* td, int64_t idx) {
    switch (idx) {
        case 0: return td->days;
        case 1: return td->seconds;
        case 2: return td->microseconds;
        default: return 0;
    }
}

double pa_timedelta_total_seconds(PyAdamasTimeDelta* td) {
    return (double)td->days * 86400.0 + (double)td->seconds + (double)td->microseconds / 1e6;
}

int64_t pa_timedelta_cmp(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b) {
    if (a->days != b->days) return a->days < b->days ? -1 : 1;
    if (a->seconds != b->seconds) return a->seconds < b->seconds ? -1 : 1;
    if (a->microseconds != b->microseconds) return a->microseconds < b->microseconds ? -1 : 1;
    return 0;
}

PyAdamasTimeDelta* pa_timedelta_add(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b) {
    return pa_timedelta_new(a->days + b->days, a->seconds + b->seconds,
                            a->microseconds + b->microseconds);
}

PyAdamasTimeDelta* pa_timedelta_sub(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b) {
    return pa_timedelta_new(a->days - b->days, a->seconds - b->seconds,
                            a->microseconds - b->microseconds);
}

PyAdamasTimeDelta* pa_timedelta_mul(PyAdamasTimeDelta* td, int64_t k) {
    return pa_timedelta_new(td->days * k, td->seconds * k, td->microseconds * k);
}

PyAdamasTimeDelta* pa_timedelta_neg(PyAdamasTimeDelta* td) {
    return pa_timedelta_new(-td->days, -td->seconds, -td->microseconds);
}

/* timedelta 的总微秒数。用 __int128 承接 days * 86400e6，避免中间溢出。 */
static __int128 td_total_us(const PyAdamasTimeDelta* td) {
    return ((__int128)td->days) * 86400000000LL + ((__int128)td->seconds) * 1000000LL +
           td->microseconds;
}

static PyAdamasTimeDelta* td_from_us(__int128 us) {
    const __int128 day = 86400000000LL;
    __int128 d = us / day;
    __int128 rem = us % day;
    if (rem < 0) {
        rem += day;
        --d;
    }
    return pa_timedelta_new((int64_t)d, (int64_t)(rem / 1000000), (int64_t)(rem % 1000000));
}

/* timedelta // int：向下取整（负数也要向 -inf 方向）。 */
PyAdamasTimeDelta* pa_timedelta_floordiv_int(PyAdamasTimeDelta* td, int64_t k) {
    if (k == 0) pa_raise_c("ZeroDivisionError", "integer division or modulo by zero");
    __int128 us = td_total_us(td);
    __int128 q = us / k;
    if (us % k != 0 && ((us < 0) != (k < 0))) --q;
    return td_from_us(q);
}

/* timedelta / int：四舍五入到微秒，恰好一半时取偶（CPython 语义）。 */
PyAdamasTimeDelta* pa_timedelta_truediv_int(PyAdamasTimeDelta* td, int64_t k) {
    if (k == 0) pa_raise_c("ZeroDivisionError", "division by zero");
    __int128 us = td_total_us(td);
    int neg = (us < 0) != (k < 0);
    __int128 a = us < 0 ? -us : us;
    __int128 b = k < 0 ? -k : k;
    __int128 q = a / b;
    __int128 r = a % b;
    if (r * 2 > b || (r * 2 == b && (q & 1))) ++q;
    return td_from_us(neg ? -q : q);
}

int64_t pa_timedelta_floordiv_td(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b) {
    __int128 ub = td_total_us(b);
    if (ub == 0) pa_raise_c("ZeroDivisionError", "integer division or modulo by zero");
    __int128 ua = td_total_us(a);
    __int128 q = ua / ub;
    if (ua % ub != 0 && ((ua < 0) != (ub < 0))) --q;
    return (int64_t)q;
}

double pa_timedelta_truediv_td(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b) {
    __int128 ub = td_total_us(b);
    if (ub == 0) pa_raise_c("ZeroDivisionError", "division by zero");
    return (double)td_total_us(a) / (double)ub;
}

/* datetime.date / datetime.datetime 的 replace：fields 是 6 个值，
   -1 表示保留原值（年份/月/日/时/分/秒都不会是 -1）。 */
PyAdamasDateTime* pa_datetime_replace(PyAdamasDateTime* dt, PyAdamasList* fields) {
    int64_t y = dt->year, mo = dt->month, d = dt->day;
    int64_t h = dt->hour, mi = dt->minute, s = dt->second;
    if (fields && fields->len >= 6) {
        const int64_t* f = fields->data;
        if (f[0] >= 0) y = f[0];
        if (f[1] >= 0) mo = f[1];
        if (f[2] >= 0) d = f[2];
        if (f[3] >= 0) h = f[3];
        if (f[4] >= 0) mi = f[4];
        if (f[5] >= 0) s = f[5];
    }
    if (dt->is_date) return pa_date_new(y, mo, d);
    return pa_datetime_new(y, mo, d, h, mi, s);
}

/* 与 CPython 的 str(timedelta) 对齐：days 段只在非 0 时出现，
   微秒非 0 时补 ".%06lld"。 */
PyAdamasStr* pa_timedelta_str(PyAdamasTimeDelta* td) {
    char buf[128];
    int64_t h = td->seconds / 3600;
    int64_t m = (td->seconds % 3600) / 60;
    int64_t s = td->seconds % 60;
    int n;
    if (td->days != 0) {
        n = snprintf(buf, sizeof buf, "%lld day%s, %lld:%02lld:%02lld", (long long)td->days,
                     (td->days == 1 || td->days == -1) ? "" : "s", (long long)h,
                     (long long)m, (long long)s);
    } else {
        n = snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m,
                     (long long)s);
    }
    if (td->microseconds != 0 && n > 0 && n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ".%06lld",
                      (long long)td->microseconds);
    return str_new(buf, (int64_t)n);
}

/* 公历日期 <-> 1970-01-01 起的天数（Howard Hinnant 的算法）。 */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t* y, int64_t* m, int64_t* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = mm;
    *d = dd;
}

/* datetime/date → 自 1970-01-01 00:00:00 起的微秒数。 */
static int64_t dt_epoch_us(PyAdamasDateTime* dt) {
    int64_t days = days_from_civil(dt->year, dt->month, dt->day);
    int64_t secs = dt->hour * 3600 + dt->minute * 60 + dt->second;
    return days * 86400000000LL + secs * 1000000LL;
}

PyAdamasDateTime* pa_datetime_add_delta(PyAdamasDateTime* dt, PyAdamasTimeDelta* td,
                                        int64_t sub) {
    int64_t sign = sub ? -1 : 1;
    if (dt->is_date) {
        // date + timedelta 只用 days 部分（CPython 语义）。
        int64_t days = days_from_civil(dt->year, dt->month, dt->day) + sign * td->days;
        int64_t y, m, d;
        civil_from_days(days, &y, &m, &d);
        return pa_date_new(y, m, d);
    }
    int64_t us = dt_epoch_us(dt) + sign * (td->days * 86400000000LL +
                                            td->seconds * 1000000LL + td->microseconds);
    int64_t days = us / 86400000000LL;
    int64_t rem = us % 86400000000LL;
    if (rem < 0) {
        rem += 86400000000LL;
        --days;
    }
    int64_t y, m, d;
    civil_from_days(days, &y, &m, &d);
    int64_t secs = rem / 1000000LL;
    return pa_datetime_new(y, m, d, secs / 3600, (secs % 3600) / 60, secs % 60);
}

PyAdamasTimeDelta* pa_datetime_diff(PyAdamasDateTime* a, PyAdamasDateTime* b) {
    int64_t us = dt_epoch_us(a) - dt_epoch_us(b);
    return pa_timedelta_new(us / 86400000000LL, (us % 86400000000LL) / 1000000LL,
                            us % 1000000LL);
}

int64_t pa_datetime_cmp(PyAdamasDateTime* a, PyAdamasDateTime* b) {
    int64_t va = dt_epoch_us(a);
    int64_t vb = dt_epoch_us(b);
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

/* ── pathlib ───────────────────────────────────────────────── */

static void path_destroy(void* p) {
    PyAdamasPath* pp = (PyAdamasPath*)p;
    if (pp->path) pa_rc_dec(pp->path);
}

/* 接管一个**刚产生**的字符串（str_alloc_raw / str_seal 的结果，rc=1，
 * 由调用方作为产生者持有），不额外 inc。
 *
 * 为什么需要它：pa_path_new 会 pa_rc_inc(s)，那是给「传进来的已有引用」
 * 用的。如果传的是刚 str_alloc_raw 出来的串，那份产生者引用就没人放了 ——
 * ASan 实测 147_pathlib_extra / 141_stdlib_pathlib_extra 各泄漏一条。
 *
 * 用法约定：str_alloc_raw / str_seal 之后直接 return pa_path_adopt(...)。 */
static PyAdamasPath* pa_path_adopt(PyAdamasStr* s) {
    pa_gc_pin(s);
    PyAdamasPath* p = (PyAdamasPath*)rc_alloc((int64_t)sizeof(PyAdamasPath), path_destroy, NULL, PYADAMAS_RC_OBJ);
    p->magic = PYADAMAS_PATH_MAGIC;
    p->path = s;
    pa_gc_unpin(s);
    return p;
}

PyAdamasPath* pa_path_new(PyAdamasStr* s) {
    /* 调用方常把 str_alloc / str_new 的结果直接传进来，那个串只存在于 C
       局部变量或表达式临时值里。下面的 rc_alloc 若触发主收集，参数串不在
       任何根上，会被清扫，随后 p->path 就是悬空指针。入口先 pin 参数：
       它临时补一个引用（不改变所有权），分配完成并 rc_inc 后再撤销。 */
    pa_gc_pin(s);
    PyAdamasPath* p = (PyAdamasPath*)rc_alloc((int64_t)sizeof(PyAdamasPath), path_destroy, NULL, PYADAMAS_RC_OBJ);
    p->magic = PYADAMAS_PATH_MAGIC;
    p->path = s;
    if (s) pa_rc_inc(s);
    pa_gc_unpin(s);
    return p;
}

/* 返回转移引用（调用方拿到一份），和运行时其它返回 str* 的函数一致。 */
PyAdamasStr* pa_path_str(PyAdamasPath* p) {
    if (p->path) pa_rc_inc(p->path);
    return p->path;
}

/* p / "x"：只在两边都不是分隔符时补一个 '/'。 */
PyAdamasPath* pa_path_join(PyAdamasPath* p, PyAdamasStr* s) {
    PyAdamasStr* a = p->path;
    int64_t an = a ? a->len : 0;
    int64_t bn = s ? s->len : 0;
    int sep = an > 0 && bn > 0 && a->data[an - 1] != '/' && s->data[0] != '/';
    PyAdamasStr* out = str_alloc_raw(an + bn + (sep ? 1 : 0));
    char* d = (char*)out->data;
    if (an) memcpy(d, a->data, (size_t)an);
    int64_t k = an;
    if (sep) d[k++] = '/';
    if (bn) memcpy(d + k, s->data, (size_t)bn);
    return pa_path_adopt(str_seal_n(out, (a ? a->pub : 0) + (s ? s->pub : 0) + (sep ? 1 : 0)));
}

/* 最后一段的起点（跳过结尾的 '/'）。 */
static int64_t path_last_seg(const char* s, int64_t n) {
    int64_t end = n;
    while (end > 0 && s[end - 1] == '/') --end;
    int64_t i = end;
    while (i > 0 && s[i - 1] != '/') --i;
    return i;
}

PyAdamasStr* pa_path_name(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t i = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > i && s->data[end - 1] == '/') --end;
    return str_new(s->data + i, end - i);
}

PyAdamasStr* pa_path_suffix(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t start = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > start && s->data[end - 1] == '/') --end;
    for (int64_t i = end - 1; i > start; --i)
        if (s->data[i] == '.') return str_new(s->data + i, end - i);
    return str_new("", 0);
}

PyAdamasPath* pa_path_with_suffix(PyAdamasPath* p, PyAdamasStr* suffix) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    int64_t start = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > start && s->data[end - 1] == '/') --end;
    int64_t cut = end;
    for (int64_t i = end - 1; i > start; --i)
        if (s->data[i] == '.') {
            cut = i;
            break;
        }
    int64_t sn = suffix ? suffix->len : 0;
    PyAdamasStr* out = str_alloc_raw(cut + sn);
    if (cut) memcpy((char*)out->data, s->data, (size_t)cut);
    if (sn) memcpy((char*)out->data + cut, suffix->data, (size_t)sn);
    return pa_path_adopt(str_seal(out));
}

PyAdamasList* pa_path_parts(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    PyAdamasList* out = pa_list_new(0, 1);
    /* 循环里每个 str_new 都是主收集触发点；out 只在本帧 C 局部变量里。 */
    pa_gc_pin(out);
    int64_t i = 0;
    if (n > 0 && s->data[0] == '/') {
        pa_list_append(out, (int64_t)str_new("/", 1));
        i = 1;
    }
    while (i < n) {
        while (i < n && s->data[i] == '/') ++i;
        int64_t start = i;
        while (i < n && s->data[i] != '/') ++i;
        int64_t len = i - start;
        if (len == 0) continue;
        if (len == 1 && s->data[start] == '.') continue;
        pa_list_append(out, (int64_t)str_new(s->data + start, len));
    }
    pa_gc_unpin(out);
    return out;
}

PyAdamasStr* pa_path_stem(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t start = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > start && s->data[end - 1] == '/') --end;
    for (int64_t i = end - 1; i > start; --i)
        if (s->data[i] == '.') return str_new(s->data + start, i - start);
    return str_new(s->data + start, end - start);
}

PyAdamasPath* pa_path_parent(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    int64_t end = n;
    while (end > 0 && s->data[end - 1] == '/') --end;
    int64_t i = end;
    while (i > 0 && s->data[i - 1] != '/') --i;
    int64_t cut = i;
    while (cut > 1 && s->data[cut - 1] == '/') --cut;
    PyAdamasStr* out = str_alloc_raw(cut);
    if (cut) memcpy((char*)out->data, s->data, (size_t)cut);
    return pa_path_adopt(str_seal(out));
}

static char* path_cstr(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    char* b = (char*)malloc((size_t)n + 1);
    if (!b) panic_c("out of memory");
    if (n) memcpy(b, s->data, (size_t)n);
    b[n] = '\0';
    return b;
}

int64_t pa_path_exists(PyAdamasPath* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0;
    free(c);
    return ok ? 1 : 0;
}

int64_t pa_path_is_file(PyAdamasPath* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0 && S_ISREG(st.st_mode);
    free(c);
    return ok ? 1 : 0;
}

int64_t pa_path_is_dir(PyAdamasPath* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0 && S_ISDIR(st.st_mode);
    free(c);
    return ok ? 1 : 0;
}

/* 读整个文件：沿用 file 模块的解码路径（UTF-8 原样返回）。 */
PyAdamasStr* pa_path_read_text(PyAdamasPath* p) {
    char* c = path_cstr(p);
    FILE* f = fopen(c, "rb");
    if (!f) {
        free(c);
        pa_raise_c("FileNotFoundError", "No such file or directory");
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    fseek(f, 0, SEEK_SET);
    PyAdamasStr* out = str_alloc_raw(sz);
    size_t got = sz ? fread((char*)out->data, 1, (size_t)sz, f) : 0;
    out->len = (int64_t)got;
    /* Path.read_bytes 的静态类型是 bytes：pub = 字节数（内容可能不是
       合法 UTF-8，绝不能按 str 数码点）。 */
    out->pub = (int64_t)got;
    fclose(f);
    free(c);
    return out;
}

int64_t pa_path_write_text(PyAdamasPath* p, PyAdamasStr* text) {
    char* c = path_cstr(p);
    FILE* f = fopen(c, "wb");
    if (!f) {
        free(c);
        pa_raise_c("OSError", "cannot open file for writing");
    }
    if (text && text->len) fwrite(text->data, 1, (size_t)text->len, f);
    fclose(f);
    free(c);
    return 0;
}

/* read_bytes / write_bytes 与文本版本共用同一套字节读写；类型上由
   调用点区分 bytes。 */
PyAdamasStr* pa_path_read_bytes(PyAdamasPath* p) {
    return pa_path_read_text(p);
}

int64_t pa_path_write_bytes(PyAdamasPath* p, PyAdamasStr* data) {
    return pa_path_write_text(p, data);
}

int64_t pa_path_is_absolute(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    return s && s->len > 0 && s->data[0] == '/' ? 1 : 0;
}

/* relative_to：base 必须是 path 的前缀（按路径分量），否则 ValueError。 */
PyAdamasPath* pa_path_relative_to(PyAdamasPath* p, PyAdamasStr* base) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    int64_t bn = base ? base->len : 0;
    const char* sd = s ? s->data : "";
    const char* bd = base ? base->data : "";
    while (bn > 0 && bd[bn - 1] == '/') --bn;
    int ok = n >= bn && (bn == 0 || memcmp(sd, bd, (size_t)bn) == 0);
    if (ok && n > bn && sd[bn] != '/') ok = 0;
    if (!ok) pa_raise_c("ValueError", "is not in the subpath of");
    int64_t off = bn < n ? bn + 1 : n;
    PyAdamasStr* out = str_alloc_raw(n - off);
    if (n > off) memcpy((char*)out->data, sd + off, (size_t)(n - off));
    return pa_path_adopt(str_seal(out));
}

/* 复制一份 Path（返回新引用，调用方负责释放）。 */
static PyAdamasPath* path_clone(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    PyAdamasStr* cp = str_alloc_raw(s ? s->len : 0);
    if (s && s->len) memcpy((char*)cp->data, s->data, (size_t)s->len);
    return pa_path_adopt(str_seal(cp));
}

/* resolve / absolute：realpath 失败（路径不存在）时退回「cwd + 原路径」。 */
PyAdamasPath* pa_path_resolve(PyAdamasPath* p) {
    char* c = path_cstr(p);
    char* r = realpath(c, NULL);
    if (r) {
        PyAdamasStr* s = str_new(r, (int64_t)strlen(r));
        free(r);
        free(c);
        return pa_path_new(s);
    }
    free(c);
    return path_clone(p);
}

PyAdamasPath* pa_path_absolute(PyAdamasPath* p) {
    PyAdamasStr* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n > 0 && s->data[0] == '/') return path_clone(p);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
    PyAdamasStr* out = str_alloc_raw((int64_t)strlen(cwd) + 1 + n);
    char* d = (char*)out->data;
    size_t cl = strlen(cwd);
    memcpy(d, cwd, cl);
    d[cl] = '/';
    if (n) memcpy(d + cl + 1, s->data, (size_t)n);
    return pa_path_adopt(str_seal(out));
}

int64_t pa_path_touch(PyAdamasPath* p) {
    char* c = path_cstr(p);
    FILE* f = fopen(c, "ab");
    if (!f) {
        free(c);
        pa_raise_c("OSError", "cannot touch file");
    }
    fclose(f);
    free(c);
    return 0;
}

/* dir + "/" + name（dir 已有尾斜杠时不重复补）。 */
static char* path_join_cstr(const char* dir, const char* name) {
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    int sep = dn > 0 && dir[dn - 1] != '/';
    char* b = (char*)malloc(dn + nn + (size_t)sep + 1);
    if (!b) panic_c("out of memory");
    memcpy(b, dir, dn);
    size_t k = dn;
    if (sep) b[k++] = '/';
    memcpy(b + k, name, nn);
    b[k + nn] = '\0';
    return b;
}

static void path_list_push(PyAdamasList* out, const char* full) {
    size_t n = strlen(full);
    PyAdamasStr* s = str_alloc_raw((int64_t)n);
    memcpy((char*)s->data, full, n);
    str_seal(s);
    /* pa_path_new 返回的引用直接交给列表持有（list_destroy 负责 dec）。 */
    pa_list_append(out, (int64_t)pa_path_new(s));
}

static int path_is_dot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

PyAdamasList* pa_path_iterdir(PyAdamasPath* p) {
    char* c = path_cstr(p);
    DIR* d = opendir(c);
    if (!d) {
        free(c);
        pa_raise_c("FileNotFoundError", "No such file or directory");
    }
    PyAdamasList* out = pa_list_new(0, 1);
    /* path_list_push 里每次 str_alloc / pa_path_new 都会分配，out 必须先 pin。 */
    pa_gc_pin(out);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (path_is_dot(e->d_name)) continue;
        char* full = path_join_cstr(c, e->d_name);
        path_list_push(out, full);
        free(full);
    }
    closedir(d);
    free(c);
    pa_gc_unpin(out);
    return out;
}

/* 单段通配：* 和 ?，不做字符类。 */
static int path_pat_match(const char* p, int64_t pn, const char* s, int64_t sn) {
    int64_t pi = 0, si = 0, star = -1, mark = 0;
    while (si < sn) {
        if (pi < pn && (p[pi] == '?' || p[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < pn && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star >= 0) {
            pi = star + 1;
            si = ++mark;
        } else {
            return 0;
        }
    }
    while (pi < pn && p[pi] == '*') ++pi;
    return pi == pn;
}

typedef struct {
    const char* p;
    int64_t n;
} PatSeg;

static void glob_walk(const char* dir, const PatSeg* segs, int64_t nseg, int64_t idx,
                      PyAdamasList* out) {
    if (idx >= nseg) {
        path_list_push(out, dir);
        return;
    }
    const PatSeg* sg = &segs[idx];
    int last = idx == nseg - 1;

    /* ** 匹配零段或多段目录。 */
    if (sg->n == 2 && sg->p[0] == '*' && sg->p[1] == '*') {
        glob_walk(dir, segs, nseg, idx + 1, out);
        DIR* d = opendir(dir);
        if (!d) return;
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (path_is_dot(e->d_name)) continue;
            char* sub = path_join_cstr(dir, e->d_name);
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) glob_walk(sub, segs, nseg, idx, out);
            free(sub);
        }
        closedir(d);
        return;
    }

    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (path_is_dot(e->d_name)) continue;
        if (!path_pat_match(sg->p, sg->n, e->d_name, (int64_t)strlen(e->d_name))) continue;
        char* sub = path_join_cstr(dir, e->d_name);
        if (last) {
            path_list_push(out, sub);
        } else {
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) glob_walk(sub, segs, nseg, idx + 1, out);
        }
        free(sub);
    }
    closedir(d);
}

PyAdamasList* pa_path_glob(PyAdamasPath* p, PyAdamasStr* pattern) {
    /* 绝对模式从 "/" 起步，相对模式挂在 Path 自己下面。 */
    int absolute = pattern->len > 0 && pattern->data[0] == '/';
    char* base = absolute ? NULL : path_cstr(p);
    if (absolute) {
        base = (char*)malloc(2);
        if (!base) panic_c("out of memory");
        base[0] = '/';
        base[1] = '\0';
    }

    /* 切段：连续的 '/' 忽略，尾随 '/' 也忽略。 */
    PatSeg segs[32];
    int64_t nseg = 0;
    int64_t i = 0;
    int64_t n = pattern->len;
    while (i < n) {
        while (i < n && pattern->data[i] == '/') ++i;
        int64_t st = i;
        while (i < n && pattern->data[i] != '/') ++i;
        if (i > st) {
            if (nseg >= 32) {
                free(base);
                pa_raise_c("ValueError", "pathlib: pattern has too many segments");
            }
            segs[nseg].p = pattern->data + st;
            segs[nseg].n = i - st;
            ++nseg;
        }
    }

    PyAdamasList* out = pa_list_new(0, 1);
    /* glob_walk 递归里每个匹配都会分配字符串/Path 对象，out 只在本帧 C
       局部变量里，先 pin 再交给递归。 */
    pa_gc_pin(out);
    if (nseg > 0) glob_walk(base, segs, nseg, 0, out);
    pa_gc_unpin(out);
    free(base);
    return out;
}

int64_t pa_path_mkdir(PyAdamasPath* p, int64_t mode, int64_t parents, int64_t exist_ok) {
    char* c = path_cstr(p);
    struct stat st;
    int existed = stat(c, &st) == 0;
    if (existed) {
        if (!S_ISDIR(st.st_mode) || !exist_ok) {
            free(c);
            pa_raise_c("FileExistsError", "File exists");
        }
        free(c);
        return 0;
    }

    if (!parents) {
        if (mkdir(c, (mode_t)mode) != 0) {
            int e = errno;
            free(c);
            pa_raise_c(e == ENOENT ? "FileNotFoundError" : "OSError", "cannot create directory");
        }
        free(c);
        return 0;
    }

    int64_t n = (int64_t)strlen(c);
    for (int64_t i = 1; i <= n; ++i) {
        if (i != n && c[i] != '/') continue;
        char save = c[i];
        c[i] = '\0';
        if (c[0] && mkdir(c, (mode_t)mode) != 0 && errno != EEXIST) {
            c[i] = save;
            free(c);
            pa_raise_c("OSError", "cannot create directory");
        }
        c[i] = save;
    }
    free(c);
    return 0;
}

