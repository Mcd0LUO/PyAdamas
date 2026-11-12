// rt_time.c —— time 模块
#include "rt_internal.h"

#include <errno.h>
#include <time.h>

static double ts_to_double(const struct timespec* ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

double pa_time_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
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
