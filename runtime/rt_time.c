// rt_time.c —— time 模块
#include "rt_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double ts_to_double(const struct timespec* ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

double pa_time_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts_to_double(&ts);
}

static int64_t ts_to_ns(const struct timespec* ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

int64_t pa_time_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts_to_ns(&ts);
}

int64_t pa_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_to_ns(&ts);
}

/* perf_counter 与 monotonic 同源（CPython 在 Linux 上也用 CLOCK_MONOTONIC）。 */
int64_t pa_time_perf_counter_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_to_ns(&ts);
}

double pa_time_process_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts_to_double(&ts);
}

double pa_time_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_to_double(&ts);
}

double pa_time_perf_counter(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_to_double(&ts);
}

void pa_time_sleep(double seconds) {
    if (!(seconds >= 0.0)) pa_raise_c("ValueError", "sleep length must be non-negative");
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) req.tv_nsec = 0;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}


/* ── struct_time：本项目用「9 元 int 元组」表示（CPython 也有下标访问）──
   字段顺序与 struct_time 一致：year, mon, mday, hour, min, sec, wday, yday,
   isdst；与 CPython 的差异是 t.tm_year 这类字段名不可用（用 t[0]），
   见 docs/stdlib-gaps.md。 */
static PyAdamasList* tm_to_tuple(const struct tm* t) {
    PyAdamasList* l = pa_list_new(9, 0);
    pa_list_append(l, (int64_t)t->tm_year + 1900, 0);
    pa_list_append(l, (int64_t)t->tm_mon + 1, 0);
    pa_list_append(l, (int64_t)t->tm_mday, 0);
    pa_list_append(l, (int64_t)t->tm_hour, 0);
    pa_list_append(l, (int64_t)t->tm_min, 0);
    pa_list_append(l, (int64_t)t->tm_sec, 0);
    /* CPython struct_time：tm_wday 周一=0（glibc 周日=0），tm_yday 1-based。 */
    pa_list_append(l, (int64_t)((t->tm_wday + 6) % 7), 0);
    pa_list_append(l, (int64_t)t->tm_yday + 1, 0);
    pa_list_append(l, (int64_t)t->tm_isdst, 0);
    return l;
}

static void tuple_to_tm(PyAdamasList* l, struct tm* t) {
    memset(t, 0, sizeof *t);
    int64_t v[9] = {1970, 1, 1, 0, 0, 0, 0, 0, 0};
    if (l) {
        for (int64_t i = 0; i < 9 && i < l->len; ++i) v[i] = l->data[i];
    }
    t->tm_year = (int)v[0] - 1900;
    t->tm_mon = (int)v[1] - 1;
    t->tm_mday = (int)v[2];
    t->tm_hour = (int)v[3];
    t->tm_min = (int)v[4];
    t->tm_sec = (int)v[5];
    t->tm_wday = (int)((v[6] + 1) % 7);  /* CPython(周一=0) -> glibc(周日=0) */
    t->tm_yday = (int)v[7] - 1;
    t->tm_isdst = (int)v[8];
}

PyAdamasList* pa_time_localtime(double secs) {
    struct tm tmv;
    time_t tt;
    if (secs <= -1e299) tt = time(NULL);
    else tt = (time_t)secs;
    localtime_r(&tt, &tmv);
    return tm_to_tuple(&tmv);
}

PyAdamasList* pa_time_gmtime(double secs) {
    struct tm tmv;
    time_t tt;
    if (secs <= -1e299) tt = time(NULL);
    else tt = (time_t)secs;
    gmtime_r(&tt, &tmv);
    return tm_to_tuple(&tmv);
}

PyAdamasStr* pa_time_strftime(PyAdamasStr* fmt, PyAdamasList* tm) {
    struct tm tmv;
    if (tm) {
        tuple_to_tm(tm, &tmv);
    } else {
        time_t now = time(NULL);
        localtime_r(&now, &tmv);
    }
    char buf[1024];
    size_t n = strftime(buf, sizeof buf, fmt->data, &tmv);
    return str_new(buf, (int64_t)n);
}

double pa_time_mktime(PyAdamasList* tm) {
    struct tm tmv;
    tuple_to_tm(tm, &tmv);
    if (!tm || tm->len <= 8) tmv.tm_isdst = -1;
    time_t r = mktime(&tmv);
    if (r == (time_t)-1) pa_raise_c("OverflowError", "mktime result out of range");
    return (double)r;
}
