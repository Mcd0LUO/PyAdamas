// rt_datetime.c —— datetime.date / datetime.datetime 与 pathlib.Path
#include "rt_internal.h"

#include <dirent.h>
#include <errno.h>
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

static void path_list_push(Py23List* out, const char* full) {
    size_t n = strlen(full);
    Py23Str* s = str_alloc((int64_t)n);
    memcpy((char*)s->data, full, n);
    /* py23_path_new 返回的引用直接交给列表持有（list_destroy 负责 dec）。 */
    py23_list_append(out, (int64_t)py23_path_new(s));
}

static int path_is_dot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

Py23List* py23_path_iterdir(Py23Path* p) {
    char* c = path_cstr(p);
    DIR* d = opendir(c);
    if (!d) {
        free(c);
        py23_raise_c("FileNotFoundError", "No such file or directory");
    }
    Py23List* out = py23_list_new(0, 1);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (path_is_dot(e->d_name)) continue;
        char* full = path_join_cstr(c, e->d_name);
        path_list_push(out, full);
        free(full);
    }
    closedir(d);
    free(c);
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
                      Py23List* out) {
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

Py23List* py23_path_glob(Py23Path* p, Py23Str* pattern) {
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
                py23_raise_c("ValueError", "pathlib: pattern has too many segments");
            }
            segs[nseg].p = pattern->data + st;
            segs[nseg].n = i - st;
            ++nseg;
        }
    }

    Py23List* out = py23_list_new(0, 1);
    if (nseg > 0) glob_walk(base, segs, nseg, 0, out);
    free(base);
    return out;
}

int64_t py23_path_mkdir(Py23Path* p, int64_t mode, int64_t parents, int64_t exist_ok) {
    char* c = path_cstr(p);
    struct stat st;
    int existed = stat(c, &st) == 0;
    if (existed) {
        if (!S_ISDIR(st.st_mode) || !exist_ok) {
            free(c);
            py23_raise_c("FileExistsError", "File exists");
        }
        free(c);
        return 0;
    }

    if (!parents) {
        if (mkdir(c, (mode_t)mode) != 0) {
            int e = errno;
            free(c);
            py23_raise_c(e == ENOENT ? "FileNotFoundError" : "OSError", "cannot create directory");
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
            py23_raise_c("OSError", "cannot create directory");
        }
        c[i] = save;
    }
    free(c);
    return 0;
}

