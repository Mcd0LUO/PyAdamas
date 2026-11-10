// rt_subprocess.c —— subprocess 模块：fork/exec、管道捕获与超时
#include "rt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

struct Py23Proc {
    int64_t returncode;
    Py23Str* out; /* owned */
    Py23Str* err; /* owned */
};

static void proc_destroy(void* p) {
    Py23Proc* q = (Py23Proc*)p;
    if (q->out) py23_rc_dec(q->out);
    if (q->err) py23_rc_dec(q->err);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void buf_append(char** buf, size_t* len, size_t* cap, const char* p, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (*len + n + 1 > ncap) ncap *= 2;
        char* nb = (char*)realloc(*buf, ncap);
        if (!nb) panic_c("out of memory");
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, p, n);
    *len += n;
    (*buf)[*len] = '\0';
}

Py23Proc* py23_subprocess_run(Py23List* argv, int64_t capture, int64_t text,
                              double timeout, int64_t check) {
    (void)text;  /* 没有 bytes 类型：text=False 也按字符串返回 */
    int64_t n = argv ? argv->len : 0;
    if (n == 0) py23_raise_c("ValueError", "subprocess.run() needs a non-empty argument list");
    char** cargv = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!cargv) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) cargv[i] = (char*)((Py23Str*)argv->data[i])->data;
    cargv[n] = NULL;

    int op[2] = {-1, -1}, ep[2] = {-1, -1};
    if (capture && (pipe(op) != 0 || pipe(ep) != 0)) {
        free(cargv);
        py23_raise_c("OSError", "subprocess: pipe failed");
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(cargv);
        py23_raise_c("OSError", "subprocess: fork failed");
    }
    if (pid == 0) {
        if (capture) {
            dup2(op[1], 1);
            dup2(ep[1], 2);
            close(op[0]);
            close(op[1]);
            close(ep[0]);
            close(ep[1]);
        }
        execvp(cargv[0], cargv);
        _exit(127);
    }
    free(cargv);
    int ofd = -1, efd = -1;
    if (capture) {
        close(op[1]);
        close(ep[1]);
        ofd = op[0];
        efd = ep[0];
    }

    char* obuf = NULL;
    char* ebuf = NULL;
    size_t olen = 0, ocap = 0, elen = 0, ecap = 0;
    double deadline = timeout >= 0.0 ? now_sec() + timeout : -1.0;
    int status = 0, reaped = 0;

    for (;;) {
        if (deadline >= 0.0 && now_sec() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            if (ofd >= 0) close(ofd);
            if (efd >= 0) close(efd);
            free(obuf);
            free(ebuf);
            char msg[256];
            snprintf(msg, sizeof msg, "Command timed out after %g seconds", timeout);
            py23_raise_c("TimeoutExpired", msg);
        }
        int pipes_open = ofd >= 0 || efd >= 0;
        if (reaped && !pipes_open) break;
        if (pipes_open) {
            struct pollfd pfds[2];
            int nf = 0;
            if (ofd >= 0) {
                pfds[nf].fd = ofd;
                pfds[nf].events = POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            if (efd >= 0) {
                pfds[nf].fd = efd;
                pfds[nf].events = POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            int wait_ms = -1;
            if (deadline >= 0.0) {
                double rem = deadline - now_sec();
                if (rem < 0.0) rem = 0.0;
                wait_ms = (int)(rem * 1000.0) + 1;
            }
            int pr = poll(pfds, (nfds_t)nf, wait_ms);
            if (pr > 0) {
                for (int i = 0; i < nf; ++i) {
                    if (!pfds[i].revents) continue;
                    int fd = pfds[i].fd;
                    char tmp[8192];
                    ssize_t got = read(fd, tmp, sizeof tmp);
                    if (got > 0) {
                        if (fd == ofd)
                            buf_append(&obuf, &olen, &ocap, tmp, (size_t)got);
                        else
                            buf_append(&ebuf, &elen, &ecap, tmp, (size_t)got);
                    } else if (got == 0 ||
                               (got < 0 && errno != EINTR && errno != EAGAIN)) {
                        close(fd);
                        if (fd == ofd)
                            ofd = -1;
                        else
                            efd = -1;
                    }
                }
            } else if (pr < 0 && errno != EINTR) {
                if (ofd >= 0) {
                    close(ofd);
                    ofd = -1;
                }
                if (efd >= 0) {
                    close(efd);
                    efd = -1;
                }
            }
        } else {
            int wait_ms = 10;
            if (deadline >= 0.0) {
                double rem = deadline - now_sec();
                if (rem < 0.0) rem = 0.0;
                int rms = (int)(rem * 1000.0) + 1;
                if (rms < wait_ms) wait_ms = rms;
            }
            poll(NULL, 0, wait_ms);
        }
        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid || (w < 0 && errno == ECHILD)) reaped = 1;
        }
    }

    int rc;
    if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        rc = -WTERMSIG(status);
    else
        rc = -1;

    if (check && rc != 0) {
        free(obuf);
        free(ebuf);
        char msg[256];
        snprintf(msg, sizeof msg, "Command returned non-zero exit status %lld", (long long)rc);
        py23_raise_c("CalledProcessError", msg);
    }

    Py23Proc* proc =
        (Py23Proc*)rc_alloc((int64_t)sizeof(Py23Proc), proc_destroy, NULL, PY23_RC_OBJ);
    proc->returncode = rc;
    proc->out = str_new(obuf ? obuf : "", (int64_t)olen);
    proc->err = str_new(ebuf ? ebuf : "", (int64_t)elen);
    free(obuf);
    free(ebuf);
    return proc;
}

int64_t py23_subprocess_returncode(Py23Proc* p) { return p->returncode; }

Py23Str* py23_subprocess_stdout(Py23Proc* p) {
    if (!p || !p->out) return str_new("", 0);
    return str_new(p->out->data, p->out->len);
}

Py23Str* py23_subprocess_stderr(Py23Proc* p) {
    if (!p || !p->err) return str_new("", 0);
    return str_new(p->err->data, p->err->len);
}
