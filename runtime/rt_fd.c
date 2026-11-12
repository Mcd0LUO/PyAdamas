// rt_fd.c —— fd 等待与 poll
#include "rt_internal.h"

/* ── fd 等待与 poll 轮询 ─────────────────────────────────────── */

void* pa_asyncio_wait_fd(int64_t fd, int64_t write) {
    loop_ensure();
    PyAdamasAwait* w = await_new(6);
    w->fd = fd;
    w->write = write;
    return w;
}

/* 对 loop_fds 里注册的 fd 做一次 poll。返回是否完成了等待对象。 */
int poll_fds(int timeout_ms) {
    if (!loop_fds->len) return 0;
    int64_t n = loop_fds->len;
    struct pollfd* pfds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)n);
    int64_t* map = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!pfds || !map) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) {
        PyAdamasAwait* w = (PyAdamasAwait*)loop_fds->data[i];
        pfds[i].fd = (int)w->fd;
        pfds[i].events = (short)(w->write ? POLLOUT : POLLIN);
        pfds[i].revents = 0;
        map[i] = (int64_t)w;
    }
    int rc = poll(pfds, (nfds_t)n, timeout_ms);
    int fired = 0;
    if (rc > 0) {
        for (int64_t i = 0; i < n; ++i) {
            if (!pfds[i].revents) continue;
            int64_t owned = 0;
            for (int64_t j = 0; j < loop_fds->len; ++j)
                if (loop_fds->data[j] == map[i]) {
                    owned = list_take_at(loop_fds, j);
                    break;
                }
            if (!owned) continue;
            PyAdamasAwait* w = (PyAdamasAwait*)owned;
            w->done = 1;
            PyAdamasException* e = NULL;
            if (pfds[i].revents & (POLLERR | POLLNVAL))
                e = exc_new("OSError", "fd poll error");
            void* a = (void*)w->waiter;
            w->waiter = 0;
            if (a) deliver_owned(a, w, 0, e);
            else {
                w->result = 0;
                w->exc = (int64_t)e;
            }
            pa_rc_dec(w);  // 轮询表的引用
            fired++;
        }
    }
    free(pfds);
    free(map);
    return fired;
}

