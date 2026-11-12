// rt_datetime.c —— datetime.date / datetime.datetime 与 pathlib.Path
#include "rt_internal.h"

#include <sys/stat.h>
#include <unistd.h>

#define PY23_DT_MAGIC 0x44544d4500000001LL    /* "DTME" */
#define PY23_PATH_MAGIC 0x5041544800000001LL  /* "PATH" */

struct Py23DateTime {
    int64_t magic;
    int64_t year, month, day;
    int64_t hour, minute, second;
    int64_t is_date; /* 1 = date（没有时分秒），0 = datetime */
};

struct Py23Path {
    int64_t magic;
    Py23Str* path; /* owned */
};

static void dt_destroy(void* p) {
    (void)p; /* 没有指针字段 */
}

static Py23DateTime* dt_make(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                             int64_t s, int64_t is_date) {
    if (mo < 1 || mo > 12) py23_raise_c("ValueError", "month must be in 1..12");
    if (d < 1 || d > 31) py23_raise_c("ValueError", "day is out of range for month");
    if (h < 0 || h > 23) py23_raise_c("ValueError", "hour must be in 0..23");
    if (mi < 0 || mi > 59) py23_raise_c("ValueError", "minute must be in 0..59");
    if (s < 0 || s > 59) py23_raise_c("ValueError", "second must be in 0..59");
    Py23DateTime* dt =
        (Py23DateTime*)rc_alloc((int64_t)sizeof(Py23DateTime), dt_destroy, NULL, PY23_RC_OBJ);
    dt->magic = PY23_DT_MAGIC;
    dt->year = y;
    dt->month = mo;
    dt->day = d;
    dt->hour = h;
    dt->minute = mi;
    dt->second = s;
    dt->is_date = is_date;
    return dt;
}

Py23DateTime* py23_date_new(int64_t y, int64_t mo, int64_t d) {
    return dt_make(y, mo, d, 0, 0, 0, 1);
}

Py23DateTime* py23_datetime_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                                int64_t s) {
    return dt_make(y, mo, d, h, mi, s, 0);
}

Py23DateTime* py23_datetime_now(int64_t is_date) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (is_date) return py23_date_new(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return py23_datetime_new(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                             tmv.tm_min, tmv.tm_sec);
}

/* idx：0 year、1 month、2 day、3 hour、4 minute、5 second、6 weekday（周一=0）。 */
int64_t py23_datetime_field(Py23DateTime* dt, int64_t idx) {
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

Py23Str* py23_datetime_isoformat(Py23DateTime* dt) {
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

Py23Str* py23_datetime_strftime(Py23DateTime* dt, Py23Str* fmt) {
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

/* ── pathlib ───────────────────────────────────────────────── */

static void path_destroy(void* p) {
    Py23Path* pp = (Py23Path*)p;
    if (pp->path) py23_rc_dec(pp->path);
}

Py23Path* py23_path_new(Py23Str* s) {
    Py23Path* p = (Py23Path*)rc_alloc((int64_t)sizeof(Py23Path), path_destroy, NULL, PY23_RC_OBJ);
    p->magic = PY23_PATH_MAGIC;
    p->path = s;
    if (s) py23_rc_inc(s);
    return p;
}

/* 返回转移引用（调用方拿到一份），和运行时其它返回 str* 的函数一致。 */
Py23Str* py23_path_str(Py23Path* p) {
    if (p->path) py23_rc_inc(p->path);
    return p->path;
}

/* p / "x"：只在两边都不是分隔符时补一个 '/'。 */
Py23Path* py23_path_join(Py23Path* p, Py23Str* s) {
    Py23Str* a = p->path;
    int64_t an = a ? a->len : 0;
    int64_t bn = s ? s->len : 0;
    int sep = an > 0 && bn > 0 && a->data[an - 1] != '/' && s->data[0] != '/';
    Py23Str* out = str_alloc(an + bn + (sep ? 1 : 0));
    char* d = (char*)out->data;
    if (an) memcpy(d, a->data, (size_t)an);
    int64_t k = an;
    if (sep) d[k++] = '/';
    if (bn) memcpy(d + k, s->data, (size_t)bn);
    return py23_path_new(out);
}

/* 最后一段的起点（跳过结尾的 '/'）。 */
static int64_t path_last_seg(const char* s, int64_t n) {
    int64_t end = n;
    while (end > 0 && s[end - 1] == '/') --end;
    int64_t i = end;
    while (i > 0 && s[i - 1] != '/') --i;
    return i;
}

Py23Str* py23_path_name(Py23Path* p) {
    Py23Str* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t i = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > i && s->data[end - 1] == '/') --end;
    return str_new(s->data + i, end - i);
}

Py23Str* py23_path_suffix(Py23Path* p) {
    Py23Str* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t start = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > start && s->data[end - 1] == '/') --end;
    for (int64_t i = end - 1; i > start; --i)
        if (s->data[i] == '.') return str_new(s->data + i, end - i);
    return str_new("", 0);
}

Py23Str* py23_path_stem(Py23Path* p) {
    Py23Str* s = p->path;
    int64_t n = s ? s->len : 0;
    if (n == 0) return str_new("", 0);
    int64_t start = path_last_seg(s->data, n);
    int64_t end = n;
    while (end > start && s->data[end - 1] == '/') --end;
    for (int64_t i = end - 1; i > start; --i)
        if (s->data[i] == '.') return str_new(s->data + start, i - start);
    return str_new(s->data + start, end - start);
}

Py23Path* py23_path_parent(Py23Path* p) {
    Py23Str* s = p->path;
    int64_t n = s ? s->len : 0;
    int64_t end = n;
    while (end > 0 && s->data[end - 1] == '/') --end;
    int64_t i = end;
    while (i > 0 && s->data[i - 1] != '/') --i;
    int64_t cut = i;
    while (cut > 1 && s->data[cut - 1] == '/') --cut;
    Py23Str* out = str_alloc(cut);
    if (cut) memcpy((char*)out->data, s->data, (size_t)cut);
    return py23_path_new(out);
}

static char* path_cstr(Py23Path* p) {
    Py23Str* s = p->path;
    int64_t n = s ? s->len : 0;
    char* b = (char*)malloc((size_t)n + 1);
    if (!b) panic_c("out of memory");
    if (n) memcpy(b, s->data, (size_t)n);
    b[n] = '\0';
    return b;
}

int64_t py23_path_exists(Py23Path* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0;
    free(c);
    return ok ? 1 : 0;
}

int64_t py23_path_is_file(Py23Path* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0 && S_ISREG(st.st_mode);
    free(c);
    return ok ? 1 : 0;
}

int64_t py23_path_is_dir(Py23Path* p) {
    char* c = path_cstr(p);
    struct stat st;
    int ok = stat(c, &st) == 0 && S_ISDIR(st.st_mode);
    free(c);
    return ok ? 1 : 0;
}

/* 读整个文件：沿用 file 模块的解码路径（UTF-8 原样返回）。 */
Py23Str* py23_path_read_text(Py23Path* p) {
    char* c = path_cstr(p);
    FILE* f = fopen(c, "rb");
    if (!f) {
        free(c);
        py23_raise_c("FileNotFoundError", "No such file or directory");
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    fseek(f, 0, SEEK_SET);
    Py23Str* out = str_alloc(sz);
    size_t got = sz ? fread((char*)out->data, 1, (size_t)sz, f) : 0;
    out->len = (int64_t)got;
    fclose(f);
    free(c);
    return out;
}

int64_t py23_path_write_text(Py23Path* p, Py23Str* text) {
    char* c = path_cstr(p);
    FILE* f = fopen(c, "wb");
    if (!f) {
        free(c);
        py23_raise_c("OSError", "cannot open file for writing");
    }
    if (text && text->len) fwrite(text->data, 1, (size_t)text->len, f);
    fclose(f);
    free(c);
    return 0;
}
