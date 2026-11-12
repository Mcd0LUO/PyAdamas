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

/* 子进程侧：装好标准输入输出、关掉继承来的多余 fd、按需 chdir，然后 exec。
   close_list 是父进程持有的管道端，子进程必须关掉，否则父进程读不到 EOF。 */
static void child_setup(int c_in, int c_out, int c_err, const int* close_list, int nclose,
                        const char* cwd, char** argv, int64_t shell) {
    if (c_in >= 0 && c_in != 0) dup2(c_in, 0);
    if (c_out >= 0 && c_out != 1) dup2(c_out, 1);
    if (c_err >= 0 && c_err != 2) dup2(c_err, 2);
    for (int i = 0; i < nclose; ++i)
        if (close_list[i] >= 0) close(close_list[i]);
    if (cwd && cwd[0] && chdir(cwd) != 0) _exit(127);
    if (shell) {
        int64_t n = 0;
        while (argv[n]) ++n;
        /* CPython 的 shell=True：sh -c argv[0] argv[1] ...，后面的实参是 $0、$1 */
        char** sa = (char**)malloc(sizeof(char*) * (size_t)(n + 3));
        if (!sa) _exit(127);
        sa[0] = (char*)"sh";
        sa[1] = (char*)"-c";
        for (int64_t i = 0; i < n; ++i) sa[i + 2] = argv[i];
        sa[n + 2] = NULL;
        execv("/bin/sh", sa);
        _exit(127);
    }
    execvp(argv[0], argv);
    _exit(127);
}

Py23Proc* py23_subprocess_run(Py23List* argv, int64_t capture, int64_t text,
                              double timeout, int64_t check, int64_t shell,
                              Py23Str* cwd) {
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
        int cl[4] = {capture ? op[0] : -1, capture ? op[1] : -1, capture ? ep[0] : -1,
                     capture ? ep[1] : -1};
        child_setup(-1, capture ? op[1] : -1, capture ? ep[1] : -1, cl, 4,
                    cwd ? cwd->data : NULL, cargv, shell);
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

/* ── Popen ───────────────────────────────────────────────────── */

#define PY23_POPEN_MAGIC 0x504f50454e000001LL /* "POPEN" */

/* stdin/stdout/stderr 的取值：0 = 继承父进程，-1 = PIPE，-2 = STDOUT
   （只对 stderr 有意义），-3 = DEVNULL。和 CPython 的常量取值一致。 */
#define PY23_SP_PIPE (-1)
#define PY23_SP_STDOUT (-2)
#define PY23_SP_DEVNULL (-3)

struct Py23Popen {
    int64_t magic;
    int64_t pid;
    int64_t status;
    int64_t has_rc;
    int64_t rc;
    int64_t in_fd, out_fd, err_fd; /* 转交给文件对象后置 -1 */
    Py23File* in_obj;
    Py23File* out_obj;
    Py23File* err_obj;
};

static void popen_destroy(void* p) {
    Py23Popen* q = (Py23Popen*)p;
    if (q->in_fd >= 0) close((int)q->in_fd);
    if (q->out_fd >= 0) close((int)q->out_fd);
    if (q->err_fd >= 0) close((int)q->err_fd);
    if (q->in_obj) py23_rc_dec(q->in_obj);
    if (q->out_obj) py23_rc_dec(q->out_obj);
    if (q->err_obj) py23_rc_dec(q->err_obj);
    /* 句柄被回收时子进程没人等了：杀掉并回收，避免僵尸。CPython 不这么做，
       但 py23 的引用计数释放时机不可预期，留着更糟。 */
    if (!q->has_rc && q->pid > 0) {
        int st = 0;
        kill((pid_t)q->pid, SIGKILL);
        waitpid((pid_t)q->pid, &st, 0);
        q->status = st;
        q->has_rc = 1;
    }
}

/* 非阻塞（block=0）或阻塞地回收子进程。返回 1 表示已经有退出码。 */
static int64_t popen_reap(Py23Popen* p, int64_t block) {
    if (p->has_rc) return 1;
    int st = 0;
    pid_t w = waitpid((pid_t)p->pid, &st, block ? 0 : WNOHANG);
    if (w == (pid_t)p->pid || (w < 0 && errno == ECHILD)) {
        p->status = st;
        if (WIFEXITED(st))
            p->rc = WEXITSTATUS(st);
        else if (WIFSIGNALED(st))
            p->rc = -WTERMSIG(st);
        else
            p->rc = -1;
        p->has_rc = 1;
        return 1;
    }
    return 0;
}

Py23Popen* py23_popen_new(Py23List* argv, int64_t shell, Py23Str* cwd, int64_t stdin_mode,
                          int64_t stdout_mode, int64_t stderr_mode) {
    int64_t n = argv ? argv->len : 0;
    if (n == 0) py23_raise_c("ValueError", "Popen() needs a non-empty argument list");
    char** cargv = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!cargv) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) cargv[i] = (char*)((Py23Str*)argv->data[i])->data;
    cargv[n] = NULL;

    int ip[2] = {-1, -1}, op[2] = {-1, -1}, ep[2] = {-1, -1};
    int devnull = -1;
    int child_in = -1, child_out = -1, child_err = -1;

    if (stdin_mode == PY23_SP_PIPE) {
        if (pipe(ip) != 0) {
            free(cargv);
            py23_raise_c("OSError", "Popen: pipe failed");
        }
        child_in = ip[0];
    } else if (stdin_mode == PY23_SP_DEVNULL) {
        devnull = open("/dev/null", O_RDWR);
        child_in = devnull;
    }
    if (stdout_mode == PY23_SP_PIPE) {
        if (pipe(op) != 0) {
            free(cargv);
            py23_raise_c("OSError", "Popen: pipe failed");
        }
        child_out = op[1];
    } else if (stdout_mode == PY23_SP_DEVNULL) {
        if (devnull < 0) devnull = open("/dev/null", O_RDWR);
        child_out = devnull;
    }
    if (stderr_mode == PY23_SP_STDOUT) {
        child_err = child_out; /* 并到 stdout */
    } else if (stderr_mode == PY23_SP_PIPE) {
        if (pipe(ep) != 0) {
            free(cargv);
            py23_raise_c("OSError", "Popen: pipe failed");
        }
        child_err = ep[1];
    } else if (stderr_mode == PY23_SP_DEVNULL) {
        if (devnull < 0) devnull = open("/dev/null", O_RDWR);
        child_err = devnull;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(cargv);
        py23_raise_c("OSError", "Popen: fork failed");
    }
    if (pid == 0) {
        int cl[7] = {ip[0], ip[1], op[0], op[1], ep[0], ep[1], devnull};
        child_setup(child_in, child_out, child_err, cl, 7, cwd ? cwd->data : NULL, cargv,
                    shell);
    }
    free(cargv);

    if (ip[0] >= 0) close(ip[0]);
    if (op[1] >= 0) close(op[1]);
    if (ep[1] >= 0) close(ep[1]);
    if (devnull >= 0) close(devnull);

    Py23Popen* p =
        (Py23Popen*)rc_alloc((int64_t)sizeof(Py23Popen), popen_destroy, NULL, PY23_RC_OBJ);
    p->magic = PY23_POPEN_MAGIC;
    p->pid = (int64_t)pid;
    p->status = 0;
    p->has_rc = 0;
    p->rc = 0;
    p->in_fd = ip[1];
    p->out_fd = op[0];
    p->err_fd = ep[0];
    p->in_obj = NULL;
    p->out_obj = NULL;
    p->err_obj = NULL;
    return p;
}

int64_t py23_popen_pid(Py23Popen* p) { return p->pid; }
int64_t py23_popen_returncode(Py23Popen* p) { return p->rc; }

static Py23File* popen_wrap(int64_t* fd, const char* mode) {
    Py23Str* m = str_new(mode, (int64_t)strlen(mode));
    Py23File* f = py23_file_open(NULL, *fd, m, 0, 0, 1);
    py23_rc_dec(m);
    *fd = -1; /* 所有权转给文件对象 */
    return f;
}

Py23File* py23_popen_stdout(Py23Popen* p) {
    if (!p->out_obj) {
        if (p->out_fd < 0)
            py23_raise_c("AttributeError", "stdout was not opened with PIPE");
        p->out_obj = popen_wrap(&p->out_fd, "r");
    }
    py23_rc_inc(p->out_obj);
    return p->out_obj;
}

Py23File* py23_popen_stderr(Py23Popen* p) {
    if (!p->err_obj) {
        if (p->err_fd < 0)
            py23_raise_c("AttributeError", "stderr was not opened with PIPE");
        p->err_obj = popen_wrap(&p->err_fd, "r");
    }
    py23_rc_inc(p->err_obj);
    return p->err_obj;
}

Py23File* py23_popen_stdin(Py23Popen* p) {
    if (!p->in_obj) {
        if (p->in_fd < 0) py23_raise_c("AttributeError", "stdin was not opened with PIPE");
        p->in_obj = popen_wrap(&p->in_fd, "w");
    }
    py23_rc_inc(p->in_obj);
    return p->in_obj;
}

int64_t py23_popen_wait(Py23Popen* p, double timeout) {
    if (popen_reap(p, 0)) return p->rc;
    if (timeout < 0.0) {
        popen_reap(p, 1);
        return p->rc;
    }
    double deadline = now_sec() + timeout;
    for (;;) {
        if (popen_reap(p, 0)) return p->rc;
        if (now_sec() >= deadline) {
            char msg[256];
            snprintf(msg, sizeof msg, "Timeout %g expired", timeout);
            py23_raise_c("TimeoutExpired", msg);
        }
        poll(NULL, 0, 1);
    }
}

void py23_popen_kill(Py23Popen* p) {
    if (!p->has_rc) kill((pid_t)p->pid, SIGKILL);
}

void py23_popen_terminate(Py23Popen* p) {
    if (!p->has_rc) kill((pid_t)p->pid, SIGTERM);
}

void py23_popen_send_signal(Py23Popen* p, int64_t sig) {
    if (!p->has_rc) kill((pid_t)p->pid, (int)sig);
}

/* 读一侧直到 EOF，追加进缓冲区。 */
static void drain(int64_t* fd, char** buf, size_t* len, size_t* cap) {
    char tmp[8192];
    for (;;) {
        ssize_t got = read((int)*fd, tmp, sizeof tmp);
        if (got > 0) {
            buf_append(buf, len, cap, tmp, (size_t)got);
        } else if (got == 0) {
            close((int)*fd);
            *fd = -1;
            return;
        } else if (errno != EINTR) {
            close((int)*fd);
            *fd = -1;
            return;
        }
    }
}

/* communicate(input)：写 input、关 stdin，然后同时收 stdout/stderr，最后回收。
   返回 [stdout, stderr]。 */
Py23List* py23_popen_communicate(Py23Popen* p, Py23Str* input) {
    if (input && p->in_obj) {
        py23_file_write(p->in_obj, input);
        py23_file_close(p->in_obj);
    } else if (p->in_fd >= 0) {
        if (input && input->len) {
            const char* q = input->data;
            size_t left = (size_t)input->len;
            while (left > 0) {
                ssize_t w = write((int)p->in_fd, q, left);
                if (w > 0) {
                    q += w;
                    left -= (size_t)w;
                } else if (w < 0 && errno != EINTR) {
                    break;
                }
            }
        }
        close((int)p->in_fd);
        p->in_fd = -1;
    }

    char* obuf = NULL;
    char* ebuf = NULL;
    size_t olen = 0, ocap = 0, elen = 0, ecap = 0;

    if (p->out_obj && p->err_obj) {
        /* 两边都已经被包装成文件对象：只能顺序读。 */
        Py23Str* a = py23_file_read(p->out_obj, -1);
        Py23Str* b = py23_file_read(p->err_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        py23_rc_dec(a);
        py23_rc_dec(b);
    } else if (p->out_obj) {
        Py23Str* a = py23_file_read(p->out_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        py23_rc_dec(a);
        if (p->err_fd >= 0) drain(&p->err_fd, &ebuf, &elen, &ecap);
    } else if (p->err_obj) {
        Py23Str* b = py23_file_read(p->err_obj, -1);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        py23_rc_dec(b);
        if (p->out_fd >= 0) drain(&p->out_fd, &obuf, &olen, &ocap);
    } else {
        while (p->out_fd >= 0 || p->err_fd >= 0) {
            struct pollfd pfds[2];
            int nf = 0;
            if (p->out_fd >= 0) {
                pfds[nf].fd = (int)p->out_fd;
                pfds[nf].events = POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            if (p->err_fd >= 0) {
                pfds[nf].fd = (int)p->err_fd;
                pfds[nf].events = POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            int pr = poll(pfds, (nfds_t)nf, -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < nf; ++i) {
                if (!pfds[i].revents) continue;
                if ((int64_t)pfds[i].fd == p->out_fd)
                    drain(&p->out_fd, &obuf, &olen, &ocap);
                else
                    drain(&p->err_fd, &ebuf, &elen, &ecap);
            }
        }
    }

    popen_reap(p, 1);

    Py23List* out = py23_list_new(2, 1);
    Py23Str* so = str_new(obuf ? obuf : "", (int64_t)olen);
    Py23Str* se = str_new(ebuf ? ebuf : "", (int64_t)elen);
    py23_list_append(out, (int64_t)so);
    py23_list_append(out, (int64_t)se);
    free(obuf);
    free(ebuf);
    return out;
}
