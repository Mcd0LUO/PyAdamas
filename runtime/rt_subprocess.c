// rt_subprocess.c —— subprocess 模块：fork/exec、管道捕获与超时
#include "rt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <unistd.h>

#if defined(_WIN32)

/* ── Windows（win64）实现 ────────────────────────────────────────
   CreateProcess + 匿名管道替代 fork/exec/pipe。父进程侧的管道端用
   _open_osfhandle 转成 CRT fd，直接复用 rt_file.c 既有的文件对象路径；
   POSIX 那边靠 dup2/close 做的重定向，这里用 STARTUPINFO 的
   hStdInput/hStdOutput/hStdError 表达。POSIX 分支保持原样。 */

#define PYADAMAS_POPEN_MAGIC 0x504f50454e000001LL /* "POPEN" */

/* stdin/stdout/stderr 的取值与 CPython 一致：0 = 继承父进程，-1 = PIPE，
   -2 = STDOUT（只对 stderr 有意义），-3 = DEVNULL。 */
#define PYADAMAS_SP_PIPE (-1)
#define PYADAMAS_SP_STDOUT (-2)
#define PYADAMAS_SP_DEVNULL (-3)

struct PyAdamasProc {
    int64_t returncode;
    PyAdamasStr* out; /* owned */
    PyAdamasStr* err; /* owned */
};

struct PyAdamasPopen {
    int64_t magic;
    int64_t pid;
    int64_t status;
    int64_t has_rc;
    int64_t rc;
    int64_t in_fd, out_fd, err_fd; /* 转交给文件对象后置 -1 */
    PyAdamasFile* in_obj;
    PyAdamasFile* out_obj;
    PyAdamasFile* err_obj;
    int64_t hproc; /* 进程句柄（HANDLE），回收后置 0 */
};

static void proc_destroy(void* p) {
    PyAdamasProc* q = (PyAdamasProc*)p;
    if (q->out) pa_rc_dec(q->out);
    if (q->err) pa_rc_dec(q->err);
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

static int win_valid(HANDLE h) { return h != NULL && h != INVALID_HANDLE_VALUE; }

static void win_close(HANDLE* h) {
    if (win_valid(*h)) CloseHandle(*h);
    *h = NULL;
}

/* ── 命令行拼装（CPython subprocess.list2cmdline 的规则）───────── */

typedef struct {
    char* p;
    size_t len;
    size_t cap;
} WinCmdLine;

static void wcl_reserve(WinCmdLine* c, size_t extra) {
    if (c->len + extra + 1 > c->cap) {
        size_t ncap = c->cap ? c->cap : 256;
        while (c->len + extra + 1 > ncap) ncap *= 2;
        char* np = (char*)realloc(c->p, ncap);
        if (!np) panic_c("out of memory");
        c->p = np;
        c->cap = ncap;
    }
}

static void wcl_putn(WinCmdLine* c, const char* s, size_t n) {
    wcl_reserve(c, n);
    memcpy(c->p + c->len, s, n);
    c->len += n;
}

static void wcl_puts(WinCmdLine* c, const char* s) { wcl_putn(c, s, strlen(s)); }

static void wcl_putc(WinCmdLine* c, char ch) {
    wcl_reserve(c, 1);
    c->p[c->len++] = ch;
}

/* 一个实参的引号/反斜杠逃逸规则：空格或制表符触发整体加引号；引号前的
   反斜杠要加倍，引号本身用 \" 表示，结尾的反斜杠在引号内也要加倍。 */
static void wcl_arg(WinCmdLine* c, const char* a) {
    int quote = (a[0] == '\0');
    const char* p;
    for (p = a; *p; ++p) {
        if (*p == ' ' || *p == '\t') {
            quote = 1;
            break;
        }
    }
    if (!quote) {
        wcl_puts(c, a);
        return;
    }
    wcl_putc(c, '"');
    size_t bs = 0;
    for (p = a; *p; ++p) {
        if (*p == '\\') {
            ++bs;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; ++i) wcl_putc(c, '\\');
            wcl_putc(c, '"');
            bs = 0;
            continue;
        }
        for (size_t i = 0; i < bs; ++i) wcl_putc(c, '\\');
        wcl_putc(c, *p);
        bs = 0;
    }
    for (size_t i = 0; i < bs * 2; ++i) wcl_putc(c, '\\');
    wcl_putc(c, '"');
}

static char* win_command_line(PyAdamasList* argv, int64_t n, int64_t shell) {
    WinCmdLine c = {NULL, 0, 0};
    if (shell) {
        /* CPython：COMSPEC /c "..."。单字符串形式（PyAdamas 把它包成单元素
           列表）原样交给 cmd，列表形式才逐个 quote。 */
        const char* comspec = getenv("COMSPEC");
        if (!comspec || !*comspec) comspec = "cmd.exe";
        wcl_puts(&c, comspec);
        wcl_puts(&c, " /c \"");
        if (n == 1) {
            wcl_puts(&c, ((PyAdamasStr*)argv->data[0])->data);
        } else {
            for (int64_t i = 0; i < n; ++i) {
                if (i) wcl_putc(&c, ' ');
                wcl_arg(&c, ((PyAdamasStr*)argv->data[i])->data);
            }
        }
        wcl_putc(&c, '"');
    } else {
        for (int64_t i = 0; i < n; ++i) {
            if (i) wcl_putc(&c, ' ');
            wcl_arg(&c, ((PyAdamasStr*)argv->data[i])->data);
        }
    }
    wcl_reserve(&c, 0);
    c.p[c.len] = '\0';
    return c.p;
}

/* ── 句柄与进程创建 ───────────────────────────────────────────── */

static int win_pipe(HANDLE* rd, HANDLE* wr) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = (DWORD)sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    *rd = NULL;
    *wr = NULL;
    if (!CreatePipe(rd, wr, &sa, 0)) return -1;
    return 0;
}

static HANDLE win_nul_handle(void) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = (DWORD)sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE h = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    return win_valid(h) ? h : NULL;
}

/* 继承用的标准句柄副本：创建进程时子进程要拿到可继承的句柄，直接改
   父进程标准句柄的继承位会影响整个进程，所以先复制一份。复制失败就退回
   原句柄（通常仍然可以被继承）。 */
static HANDLE win_inherit_dup(HANDLE h, int* owned) {
    HANDLE d = NULL;
    *owned = 0;
    if (!win_valid(h)) return NULL;
    if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE,
                        DUPLICATE_SAME_ACCESS)) {
        *owned = 1;
        return d;
    }
    return h;
}

static int win_spawn(char* cmdline, const char* cwd, HANDLE hin, HANDLE hout, HANDLE herr,
                     int use_std, PROCESS_INFORMATION* pi, DWORD* werr) {
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = (DWORD)sizeof si;
    if (use_std) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hin;
        si.hStdOutput = hout;
        si.hStdError = herr;
    }
    memset(pi, 0, sizeof *pi);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                        (cwd && cwd[0]) ? cwd : NULL, &si, pi)) {
        *werr = GetLastError();
        return -1;
    }
    return 0;
}

/* 管道句柄转 CRT fd：所有权转移给 fd，之后 fdopen/fclose 就能统一走
   rt_file.c 的 PyAdamasFile。 */
static int64_t win_take_fd(HANDLE h, int write_end) {
    int flags = (write_end ? _O_WRONLY : _O_RDONLY) | _O_BINARY;
    int fd = _open_osfhandle((intptr_t)h, flags);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }
    return (int64_t)fd;
}

/* 读一侧直到当前没有数据。*ph 为 NULL 表示已经 EOF。 */
static void win_pipe_pump(HANDLE* ph, char** buf, size_t* len, size_t* cap) {
    HANDLE h = *ph;
    if (!win_valid(h)) {
        *ph = NULL;
        return;
    }
    /* 一次最多搬 1 MiB 就回到调用方，让带超时的循环能及时检查 deadline；
       否则子进程持续输出时这里会把超时饿死。 */
    size_t budget = (size_t)1 << 20;
    while (budget > 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            /* 写端已关：ReadFile 会立刻返回 0/ERROR_BROKEN_PIPE。 */
            char tmp[8192];
            DWORD got = 0;
            if (ReadFile(h, tmp, (DWORD)sizeof tmp, &got, NULL) && got > 0)
                buf_append(buf, len, cap, tmp, (size_t)got);
            else
                win_close(ph);
            return;
        }
        if (avail == 0) return;
        DWORD want = avail > 8192u ? 8192u : avail;
        if ((size_t)want > budget) want = (DWORD)budget;
        char tmp[8192];
        DWORD got = 0;
        if (!ReadFile(h, tmp, want, &got, NULL) || got == 0) {
            win_close(ph);
            return;
        }
        buf_append(buf, len, cap, tmp, (size_t)got);
        budget -= (size_t)got;
    }
}

static int64_t win_exit_code(DWORD code) {
    /* Windows 的退出码是 DWORD；CPython 原样返回无符号值。 */
    return (int64_t)(uint32_t)code;
}

/* ── subprocess.run ───────────────────────────────────────────── */

PyAdamasProc* pa_subprocess_run(PyAdamasList* argv, int64_t capture, int64_t text,
                               double timeout, int64_t check, int64_t shell,
                               PyAdamasStr* cwd) {
    (void)text; /* 没有 bytes 类型：text=False 也按字符串返回 */
    int64_t n = argv ? argv->len : 0;
    if (n == 0) pa_raise_c("ValueError", "subprocess.run() needs a non-empty argument list");
    char* cmdline = win_command_line(argv, n, shell);

    HANDLE op[2] = {NULL, NULL}, ep[2] = {NULL, NULL};
    if (capture) {
        if (win_pipe(&op[0], &op[1]) != 0 || win_pipe(&ep[0], &ep[1]) != 0) {
            free(cmdline);
            win_close(&op[0]);
            win_close(&op[1]);
            win_close(&ep[0]);
            win_close(&ep[1]);
            pa_raise_c("OSError", "subprocess: pipe failed");
        }
        /* 父进程持有的读端不能被子进程继承，否则永远读不到 EOF。 */
        SetHandleInformation(op[0], HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(ep[0], HANDLE_FLAG_INHERIT, 0);
    }

    /* 三个标准流都显式传句柄：继承来的句柄未必可继承（例如被宿主重定向
       过的管道），复制一份可继承的副本，语义与 POSIX 的“继承 stdio”一致。 */
    int in_owned = 0, out_owned = 0, err_owned = 0;
    HANDLE h_in = win_inherit_dup(GetStdHandle(STD_INPUT_HANDLE), &in_owned);
    HANDLE h_out = capture ? op[1]
                           : win_inherit_dup(GetStdHandle(STD_OUTPUT_HANDLE), &out_owned);
    HANDLE h_err = capture ? ep[1]
                           : win_inherit_dup(GetStdHandle(STD_ERROR_HANDLE), &err_owned);

    PROCESS_INFORMATION pi;
    DWORD werr = 0;
    if (win_spawn(cmdline, cwd ? cwd->data : NULL, h_in, h_out, h_err, 1, &pi, &werr) != 0) {
        if (in_owned) CloseHandle(h_in);
        if (out_owned) CloseHandle(h_out);
        if (err_owned) CloseHandle(h_err);
        win_close(&op[0]);
        win_close(&op[1]);
        win_close(&ep[0]);
        win_close(&ep[1]);
        free(cmdline);
        char msg[160];
        snprintf(msg, sizeof msg, "subprocess: CreateProcess failed (error %lu)",
                 (unsigned long)werr);
        pa_raise_c("OSError", msg);
    }
    free(cmdline);
    if (in_owned) CloseHandle(h_in);
    if (out_owned) CloseHandle(h_out);
    if (err_owned) CloseHandle(h_err);
    if (capture) {
        win_close(&op[1]);
        win_close(&ep[1]);
    }
    CloseHandle(pi.hThread);
    HANDLE hproc = pi.hProcess;

    char* obuf = NULL;
    char* ebuf = NULL;
    size_t olen = 0, ocap = 0, elen = 0, ecap = 0;
    double deadline = timeout >= 0.0 ? now_sec() + timeout : -1.0;
    DWORD code = 0;
    int exited = 0;

    if (!capture) {
        for (;;) {
            if (deadline >= 0.0 && now_sec() >= deadline) {
                TerminateProcess(hproc, 1);
                WaitForSingleObject(hproc, INFINITE);
                CloseHandle(hproc);
                char msg[256];
                snprintf(msg, sizeof msg, "Command timed out after %g seconds", timeout);
                pa_raise_c("TimeoutExpired", msg);
            }
            DWORD w = WaitForSingleObject(hproc, 10);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_FAILED) {
                CloseHandle(hproc);
                pa_raise_c("OSError", "subprocess: wait failed");
            }
        }
        if (!GetExitCodeProcess(hproc, &code)) code = (DWORD)-1;
        CloseHandle(hproc);
    } else {
        HANDLE out_h = op[0];
        HANDLE err_h = ep[0];
        op[0] = ep[0] = NULL;
        for (;;) {
            if (deadline >= 0.0 && now_sec() >= deadline) {
                TerminateProcess(hproc, 1);
                WaitForSingleObject(hproc, INFINITE);
                CloseHandle(hproc);
                win_close(&out_h);
                win_close(&err_h);
                free(obuf);
                free(ebuf);
                char msg[256];
                snprintf(msg, sizeof msg, "Command timed out after %g seconds", timeout);
                pa_raise_c("TimeoutExpired", msg);
            }
            if (out_h) win_pipe_pump(&out_h, &obuf, &olen, &ocap);
            if (err_h) win_pipe_pump(&err_h, &ebuf, &elen, &ecap);
            if (!exited) {
                DWORD w = WaitForSingleObject(hproc, 0);
                if (w == WAIT_OBJECT_0) {
                    if (!GetExitCodeProcess(hproc, &code)) code = (DWORD)-1;
                    exited = 1;
                } else if (w == WAIT_FAILED) {
                    win_close(&out_h);
                    win_close(&err_h);
                    CloseHandle(hproc);
                    free(obuf);
                    free(ebuf);
                    pa_raise_c("OSError", "subprocess: wait failed");
                }
            }
            if (exited && !out_h && !err_h) break;
            Sleep(1);
        }
        CloseHandle(hproc);
    }

    int64_t rc = win_exit_code(code);

    if (check && rc != 0) {
        free(obuf);
        free(ebuf);
        char msg[256];
        snprintf(msg, sizeof msg, "Command returned non-zero exit status %lld", (long long)rc);
        pa_raise_c("CalledProcessError", msg);
    }

    PyAdamasProc* proc =
        (PyAdamasProc*)rc_alloc((int64_t)sizeof(PyAdamasProc), proc_destroy, NULL, PYADAMAS_RC_OBJ);
    proc->returncode = rc;
    proc->out = str_new(obuf ? obuf : "", (int64_t)olen);
    proc->err = str_new(ebuf ? ebuf : "", (int64_t)elen);
    free(obuf);
    free(ebuf);
    return proc;
}

int64_t pa_subprocess_returncode(PyAdamasProc* p) { return p->returncode; }

PyAdamasStr* pa_subprocess_stdout(PyAdamasProc* p) {
    if (!p || !p->out) return str_new("", 0);
    return str_new(p->out->data, p->out->len);
}

PyAdamasStr* pa_subprocess_stderr(PyAdamasProc* p) {
    if (!p || !p->err) return str_new("", 0);
    return str_new(p->err->data, p->err->len);
}

/* ── Popen ───────────────────────────────────────────────────── */

static void popen_destroy(void* p) {
    PyAdamasPopen* q = (PyAdamasPopen*)p;
    if (q->in_fd >= 0) _close((int)q->in_fd);
    if (q->out_fd >= 0) _close((int)q->out_fd);
    if (q->err_fd >= 0) _close((int)q->err_fd);
    if (q->in_obj) pa_rc_dec(q->in_obj);
    if (q->out_obj) pa_rc_dec(q->out_obj);
    if (q->err_obj) pa_rc_dec(q->err_obj);
    /* 句柄被回收时子进程没人等了：杀掉并回收，避免僵尸进程/句柄泄漏。 */
    if (q->hproc) {
        HANDLE h = (HANDLE)(intptr_t)q->hproc;
        if (!q->has_rc && q->pid > 0) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, INFINITE);
            q->has_rc = 1;
        }
        CloseHandle(h);
        q->hproc = 0;
    }
}

/* 非阻塞（block=0）或阻塞地回收子进程。返回 1 表示已经有退出码。 */
static int64_t popen_reap(PyAdamasPopen* p, int64_t block) {
    if (p->has_rc) return 1;
    if (!p->hproc) {
        p->has_rc = 1;
        p->rc = -1;
        return 1;
    }
    HANDLE h = (HANDLE)(intptr_t)p->hproc;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (!GetExitCodeProcess(h, &code)) code = (DWORD)-1;
        p->rc = win_exit_code(code);
        p->has_rc = 1;
        CloseHandle(h);
        p->hproc = 0;
        return 1;
    }
    if (w == WAIT_FAILED) {
        p->rc = -1;
        p->has_rc = 1;
        CloseHandle(h);
        p->hproc = 0;
        return 1;
    }
    return 0;
}

PyAdamasPopen* pa_popen_new(PyAdamasList* argv, int64_t shell, PyAdamasStr* cwd, int64_t stdin_mode,
                          int64_t stdout_mode, int64_t stderr_mode) {
    int64_t n = argv ? argv->len : 0;
    if (n == 0) pa_raise_c("ValueError", "Popen() needs a non-empty argument list");
    char* cmdline = win_command_line(argv, n, shell);

    HANDLE ip[2] = {NULL, NULL}, op[2] = {NULL, NULL}, ep[2] = {NULL, NULL};
    HANDLE nul = NULL;
    HANDLE child_in = NULL, child_out = NULL, child_err = NULL;
    int err_is_out = 0;
    int64_t in_fd = -1, out_fd = -1, err_fd = -1;
    const char* failmsg = "Popen: pipe failed";
    DWORD werr = 0;

    if (stdin_mode == PYADAMAS_SP_PIPE) {
        if (win_pipe(&ip[0], &ip[1]) != 0) goto fail;
        SetHandleInformation(ip[1], HANDLE_FLAG_INHERIT, 0);
        child_in = ip[0];
        {
            HANDLE h = ip[1];
            ip[1] = NULL;
            in_fd = win_take_fd(h, 1);
            if (in_fd < 0) {
                failmsg = "Popen: cannot wrap stdin pipe";
                goto fail;
            }
        }
    } else if (stdin_mode == PYADAMAS_SP_DEVNULL) {
        nul = win_nul_handle();
        if (!nul) goto fail;
        child_in = nul;
    }

    if (stdout_mode == PYADAMAS_SP_PIPE) {
        if (win_pipe(&op[0], &op[1]) != 0) goto fail;
        SetHandleInformation(op[0], HANDLE_FLAG_INHERIT, 0);
        child_out = op[1];
        {
            HANDLE h = op[0];
            op[0] = NULL;
            out_fd = win_take_fd(h, 0);
            if (out_fd < 0) {
                failmsg = "Popen: cannot wrap stdout pipe";
                goto fail;
            }
        }
    } else if (stdout_mode == PYADAMAS_SP_DEVNULL) {
        if (!nul) nul = win_nul_handle();
        if (!nul) goto fail;
        child_out = nul;
    }

    if (stderr_mode == PYADAMAS_SP_STDOUT) {
        err_is_out = 1;
    } else if (stderr_mode == PYADAMAS_SP_PIPE) {
        if (win_pipe(&ep[0], &ep[1]) != 0) goto fail;
        SetHandleInformation(ep[0], HANDLE_FLAG_INHERIT, 0);
        child_err = ep[1];
        {
            HANDLE h = ep[0];
            ep[0] = NULL;
            err_fd = win_take_fd(h, 0);
            if (err_fd < 0) {
                failmsg = "Popen: cannot wrap stderr pipe";
                goto fail;
            }
        }
    } else if (stderr_mode == PYADAMAS_SP_DEVNULL) {
        if (!nul) nul = win_nul_handle();
        if (!nul) goto fail;
        child_err = nul;
    }

    int own_in = 0, own_out = 0, own_err = 0;
    HANDLE h_in, h_out, h_err;
    if (child_in)
        h_in = child_in;
    else
        h_in = win_inherit_dup(GetStdHandle(STD_INPUT_HANDLE), &own_in);
    if (child_out)
        h_out = child_out;
    else
        h_out = win_inherit_dup(GetStdHandle(STD_OUTPUT_HANDLE), &own_out);
    if (err_is_out) {
        h_err = h_out;
        own_err = 0;
    } else if (child_err) {
        h_err = child_err;
    } else {
        h_err = win_inherit_dup(GetStdHandle(STD_ERROR_HANDLE), &own_err);
    }

    PROCESS_INFORMATION pi;
    if (win_spawn(cmdline, cwd ? cwd->data : NULL, h_in, h_out, h_err, 1, &pi, &werr) != 0) {
        if (own_in) CloseHandle(h_in);
        if (own_out) CloseHandle(h_out);
        if (own_err) CloseHandle(h_err);
        failmsg = "Popen: CreateProcess failed";
        goto fail;
    }
    free(cmdline);
    cmdline = NULL;
    if (own_in) CloseHandle(h_in);
    if (own_out) CloseHandle(h_out);
    if (own_err) CloseHandle(h_err);
    win_close(&ip[0]);
    win_close(&op[1]);
    win_close(&ep[1]);
    win_close(&nul);
    CloseHandle(pi.hThread);

    PyAdamasPopen* p =
        (PyAdamasPopen*)rc_alloc((int64_t)sizeof(PyAdamasPopen), popen_destroy, NULL, PYADAMAS_RC_OBJ);
    p->magic = PYADAMAS_POPEN_MAGIC;
    p->pid = (int64_t)pi.dwProcessId;
    p->status = 0;
    p->has_rc = 0;
    p->rc = 0;
    p->in_fd = in_fd;
    p->out_fd = out_fd;
    p->err_fd = err_fd;
    p->in_obj = NULL;
    p->out_obj = NULL;
    p->err_obj = NULL;
    p->hproc = (int64_t)(intptr_t)pi.hProcess;
    return p;

fail:
    win_close(&ip[0]);
    win_close(&ip[1]);
    win_close(&op[0]);
    win_close(&op[1]);
    win_close(&ep[0]);
    win_close(&ep[1]);
    win_close(&nul);
    if (in_fd >= 0) _close((int)in_fd);
    if (out_fd >= 0) _close((int)out_fd);
    if (err_fd >= 0) _close((int)err_fd);
    free(cmdline);
    {
        char msg[200];
        if (strcmp(failmsg, "Popen: CreateProcess failed") == 0)
            snprintf(msg, sizeof msg, "%s (error %lu)", failmsg, (unsigned long)werr);
        else
            snprintf(msg, sizeof msg, "%s", failmsg);
        pa_raise_c("OSError", msg);
    }
}

int64_t pa_popen_pid(PyAdamasPopen* p) { return p->pid; }
int64_t pa_popen_returncode(PyAdamasPopen* p) { return p->rc; }

static PyAdamasFile* popen_wrap(int64_t* fd, const char* mode) {
    /* Windows 的 CRT 文本模式会翻译 CRLF 并把 0x1A 当 EOF；管道必须走
       二进制模式，语义才和 POSIX 的字节流一致。 */
    const char* wmode = mode[0] == 'r' ? "rb" : "wb";
    PyAdamasStr* m = str_new(wmode, (int64_t)strlen(wmode));
    PyAdamasFile* f = pa_file_open(NULL, *fd, m, 0, 0, 1);
    pa_rc_dec(m);
    *fd = -1; /* 所有权转给文件对象 */
    return f;
}

PyAdamasFile* pa_popen_stdout(PyAdamasPopen* p) {
    if (!p->out_obj) {
        if (p->out_fd < 0)
            pa_raise_c("AttributeError", "stdout was not opened with PIPE");
        p->out_obj = popen_wrap(&p->out_fd, "r");
    }
    pa_rc_inc(p->out_obj);
    return p->out_obj;
}

PyAdamasFile* pa_popen_stderr(PyAdamasPopen* p) {
    if (!p->err_obj) {
        if (p->err_fd < 0)
            pa_raise_c("AttributeError", "stderr was not opened with PIPE");
        p->err_obj = popen_wrap(&p->err_fd, "r");
    }
    pa_rc_inc(p->err_obj);
    return p->err_obj;
}

PyAdamasFile* pa_popen_stdin(PyAdamasPopen* p) {
    if (!p->in_obj) {
        if (p->in_fd < 0) pa_raise_c("AttributeError", "stdin was not opened with PIPE");
        p->in_obj = popen_wrap(&p->in_fd, "w");
    }
    pa_rc_inc(p->in_obj);
    return p->in_obj;
}

int64_t pa_popen_wait(PyAdamasPopen* p, double timeout) {
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
            pa_raise_c("TimeoutExpired", msg);
        }
        Sleep(1);
    }
}

void pa_popen_kill(PyAdamasPopen* p) {
    if (!p->has_rc && p->hproc) TerminateProcess((HANDLE)(intptr_t)p->hproc, 1);
}

void pa_popen_terminate(PyAdamasPopen* p) {
    if (!p->has_rc && p->hproc) TerminateProcess((HANDLE)(intptr_t)p->hproc, 1);
}

void pa_popen_send_signal(PyAdamasPopen* p, int64_t sig) {
    /* Windows 没有 POSIX 信号。CTRL_C/CTRL_BREAK 需要进程组和共享控制台，
       这里按 CPython 的 SIGTERM 语义统一走 TerminateProcess。 */
    if (!p->has_rc && p->hproc && sig != 0)
        TerminateProcess((HANDLE)(intptr_t)p->hproc, 1);
}

/* 读一侧直到当前没有数据。*fd 为 -1 表示已关闭。 */
static void win_fd_pump(int64_t* fd, char** buf, size_t* len, size_t* cap) {
    int f = (int)*fd;
    intptr_t hv = _get_osfhandle(f);
    if (hv == -1) {
        _close(f);
        *fd = -1;
        return;
    }
    HANDLE h = (HANDLE)hv;
    size_t budget = (size_t)1 << 20;
    while (budget > 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            char tmp[8192];
            int got = _read(f, tmp, (unsigned)sizeof tmp);
            if (got > 0)
                buf_append(buf, len, cap, tmp, (size_t)got);
            else {
                _close(f);
                *fd = -1;
            }
            return;
        }
        if (avail == 0) return;
        char tmp[8192];
        DWORD want = avail > 8192u ? 8192u : avail;
        if ((size_t)want > budget) want = (DWORD)budget;
        int got = _read(f, tmp, (unsigned)want);
        if (got <= 0) {
            _close(f);
            *fd = -1;
            return;
        }
        buf_append(buf, len, cap, tmp, (size_t)got);
        budget -= (size_t)got;
    }
}

/* communicate(input)：写 input、关 stdin，然后同时收 stdout/stderr，最后回收。
   返回 [stdout, stderr]。 */
PyAdamasList* pa_popen_communicate(PyAdamasPopen* p, PyAdamasStr* input) {
    if (input && p->in_obj) {
        pa_file_write(p->in_obj, input);
        pa_file_close(p->in_obj);
    } else if (p->in_fd >= 0) {
        if (input && input->len) {
            const char* q = input->data;
            size_t left = (size_t)input->len;
            while (left > 0) {
                unsigned chunk = left > (size_t)(1u << 20) ? (unsigned)(1u << 20) : (unsigned)left;
                int w = _write((int)p->in_fd, q, chunk);
                if (w > 0) {
                    q += w;
                    left -= (size_t)w;
                } else if (w < 0 && errno != EINTR) {
                    break;
                }
            }
        }
        _close((int)p->in_fd);
        p->in_fd = -1;
    }

    char* obuf = NULL;
    char* ebuf = NULL;
    size_t olen = 0, ocap = 0, elen = 0, ecap = 0;

    if (p->out_obj && p->err_obj) {
        /* 两边都已经被包装成文件对象：只能顺序读。 */
        PyAdamasStr* a = pa_file_read(p->out_obj, -1);
        PyAdamasStr* b = pa_file_read(p->err_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        pa_rc_dec(a);
        pa_rc_dec(b);
    } else if (p->out_obj) {
        PyAdamasStr* a = pa_file_read(p->out_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        pa_rc_dec(a);
        if (p->err_fd >= 0) win_fd_pump(&p->err_fd, &ebuf, &elen, &ecap);
    } else if (p->err_obj) {
        PyAdamasStr* b = pa_file_read(p->err_obj, -1);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        pa_rc_dec(b);
        if (p->out_fd >= 0) win_fd_pump(&p->out_fd, &obuf, &olen, &ocap);
    } else {
        while (p->out_fd >= 0 || p->err_fd >= 0) {
            if (p->out_fd >= 0) win_fd_pump(&p->out_fd, &obuf, &olen, &ocap);
            if (p->err_fd >= 0) win_fd_pump(&p->err_fd, &ebuf, &elen, &ecap);
            if (p->out_fd < 0 && p->err_fd < 0) break;
            Sleep(1);
        }
    }

    popen_reap(p, 1);

    PyAdamasList* out = pa_list_new(2, 1);
    PyAdamasStr* so = str_new(obuf ? obuf : "", (int64_t)olen);
    PyAdamasStr* se = str_new(ebuf ? ebuf : "", (int64_t)elen);
    pa_list_append(out, (int64_t)so);
    pa_list_append(out, (int64_t)se);
    free(obuf);
    free(ebuf);
    return out;
}

#else /* !defined(_WIN32) */

struct PyAdamasProc {
    int64_t returncode;
    PyAdamasStr* out; /* owned */
    PyAdamasStr* err; /* owned */
};

static void proc_destroy(void* p) {
    PyAdamasProc* q = (PyAdamasProc*)p;
    if (q->out) pa_rc_dec(q->out);
    if (q->err) pa_rc_dec(q->err);
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

PyAdamasProc* pa_subprocess_run(PyAdamasList* argv, int64_t capture, int64_t text,
                              double timeout, int64_t check, int64_t shell,
                              PyAdamasStr* cwd) {
    (void)text;  /* 没有 bytes 类型：text=False 也按字符串返回 */
    int64_t n = argv ? argv->len : 0;
    if (n == 0) pa_raise_c("ValueError", "subprocess.run() needs a non-empty argument list");
    char** cargv = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!cargv) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) cargv[i] = (char*)((PyAdamasStr*)argv->data[i])->data;
    cargv[n] = NULL;

    int op[2] = {-1, -1}, ep[2] = {-1, -1};
    if (capture && (pipe(op) != 0 || pipe(ep) != 0)) {
        free(cargv);
        pa_raise_c("OSError", "subprocess: pipe failed");
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(cargv);
        pa_raise_c("OSError", "subprocess: fork failed");
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
            pa_raise_c("TimeoutExpired", msg);
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
        pa_raise_c("CalledProcessError", msg);
    }

    PyAdamasProc* proc =
        (PyAdamasProc*)rc_alloc((int64_t)sizeof(PyAdamasProc), proc_destroy, NULL, PYADAMAS_RC_OBJ);
    proc->returncode = rc;
    proc->out = str_new(obuf ? obuf : "", (int64_t)olen);
    proc->err = str_new(ebuf ? ebuf : "", (int64_t)elen);
    free(obuf);
    free(ebuf);
    return proc;
}

int64_t pa_subprocess_returncode(PyAdamasProc* p) { return p->returncode; }

PyAdamasStr* pa_subprocess_stdout(PyAdamasProc* p) {
    if (!p || !p->out) return str_new("", 0);
    return str_new(p->out->data, p->out->len);
}

PyAdamasStr* pa_subprocess_stderr(PyAdamasProc* p) {
    if (!p || !p->err) return str_new("", 0);
    return str_new(p->err->data, p->err->len);
}

/* ── Popen ───────────────────────────────────────────────────── */

#define PYADAMAS_POPEN_MAGIC 0x504f50454e000001LL /* "POPEN" */

/* stdin/stdout/stderr 的取值：0 = 继承父进程，-1 = PIPE，-2 = STDOUT
   （只对 stderr 有意义），-3 = DEVNULL。和 CPython 的常量取值一致。 */
#define PYADAMAS_SP_PIPE (-1)
#define PYADAMAS_SP_STDOUT (-2)
#define PYADAMAS_SP_DEVNULL (-3)

struct PyAdamasPopen {
    int64_t magic;
    int64_t pid;
    int64_t status;
    int64_t has_rc;
    int64_t rc;
    int64_t in_fd, out_fd, err_fd; /* 转交给文件对象后置 -1 */
    PyAdamasFile* in_obj;
    PyAdamasFile* out_obj;
    PyAdamasFile* err_obj;
};

static void popen_destroy(void* p) {
    PyAdamasPopen* q = (PyAdamasPopen*)p;
    if (q->in_fd >= 0) close((int)q->in_fd);
    if (q->out_fd >= 0) close((int)q->out_fd);
    if (q->err_fd >= 0) close((int)q->err_fd);
    if (q->in_obj) pa_rc_dec(q->in_obj);
    if (q->out_obj) pa_rc_dec(q->out_obj);
    if (q->err_obj) pa_rc_dec(q->err_obj);
    /* 句柄被回收时子进程没人等了：杀掉并回收，避免僵尸。CPython 不这么做，
       但 pyadamas 的引用计数释放时机不可预期，留着更糟。 */
    if (!q->has_rc && q->pid > 0) {
        int st = 0;
        kill((pid_t)q->pid, SIGKILL);
        waitpid((pid_t)q->pid, &st, 0);
        q->status = st;
        q->has_rc = 1;
    }
}

/* 非阻塞（block=0）或阻塞地回收子进程。返回 1 表示已经有退出码。 */
static int64_t popen_reap(PyAdamasPopen* p, int64_t block) {
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

PyAdamasPopen* pa_popen_new(PyAdamasList* argv, int64_t shell, PyAdamasStr* cwd, int64_t stdin_mode,
                          int64_t stdout_mode, int64_t stderr_mode) {
    int64_t n = argv ? argv->len : 0;
    if (n == 0) pa_raise_c("ValueError", "Popen() needs a non-empty argument list");
    char** cargv = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!cargv) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) cargv[i] = (char*)((PyAdamasStr*)argv->data[i])->data;
    cargv[n] = NULL;

    int ip[2] = {-1, -1}, op[2] = {-1, -1}, ep[2] = {-1, -1};
    int devnull = -1;
    int child_in = -1, child_out = -1, child_err = -1;

    if (stdin_mode == PYADAMAS_SP_PIPE) {
        if (pipe(ip) != 0) {
            free(cargv);
            pa_raise_c("OSError", "Popen: pipe failed");
        }
        child_in = ip[0];
    } else if (stdin_mode == PYADAMAS_SP_DEVNULL) {
        devnull = open("/dev/null", O_RDWR);
        child_in = devnull;
    }
    if (stdout_mode == PYADAMAS_SP_PIPE) {
        if (pipe(op) != 0) {
            free(cargv);
            pa_raise_c("OSError", "Popen: pipe failed");
        }
        child_out = op[1];
    } else if (stdout_mode == PYADAMAS_SP_DEVNULL) {
        if (devnull < 0) devnull = open("/dev/null", O_RDWR);
        child_out = devnull;
    }
    if (stderr_mode == PYADAMAS_SP_STDOUT) {
        child_err = child_out; /* 并到 stdout */
    } else if (stderr_mode == PYADAMAS_SP_PIPE) {
        if (pipe(ep) != 0) {
            free(cargv);
            pa_raise_c("OSError", "Popen: pipe failed");
        }
        child_err = ep[1];
    } else if (stderr_mode == PYADAMAS_SP_DEVNULL) {
        if (devnull < 0) devnull = open("/dev/null", O_RDWR);
        child_err = devnull;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(cargv);
        pa_raise_c("OSError", "Popen: fork failed");
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

    PyAdamasPopen* p =
        (PyAdamasPopen*)rc_alloc((int64_t)sizeof(PyAdamasPopen), popen_destroy, NULL, PYADAMAS_RC_OBJ);
    p->magic = PYADAMAS_POPEN_MAGIC;
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

int64_t pa_popen_pid(PyAdamasPopen* p) { return p->pid; }
int64_t pa_popen_returncode(PyAdamasPopen* p) { return p->rc; }

static PyAdamasFile* popen_wrap(int64_t* fd, const char* mode) {
    PyAdamasStr* m = str_new(mode, (int64_t)strlen(mode));
    PyAdamasFile* f = pa_file_open(NULL, *fd, m, 0, 0, 1);
    pa_rc_dec(m);
    *fd = -1; /* 所有权转给文件对象 */
    return f;
}

PyAdamasFile* pa_popen_stdout(PyAdamasPopen* p) {
    if (!p->out_obj) {
        if (p->out_fd < 0)
            pa_raise_c("AttributeError", "stdout was not opened with PIPE");
        p->out_obj = popen_wrap(&p->out_fd, "r");
    }
    pa_rc_inc(p->out_obj);
    return p->out_obj;
}

PyAdamasFile* pa_popen_stderr(PyAdamasPopen* p) {
    if (!p->err_obj) {
        if (p->err_fd < 0)
            pa_raise_c("AttributeError", "stderr was not opened with PIPE");
        p->err_obj = popen_wrap(&p->err_fd, "r");
    }
    pa_rc_inc(p->err_obj);
    return p->err_obj;
}

PyAdamasFile* pa_popen_stdin(PyAdamasPopen* p) {
    if (!p->in_obj) {
        if (p->in_fd < 0) pa_raise_c("AttributeError", "stdin was not opened with PIPE");
        p->in_obj = popen_wrap(&p->in_fd, "w");
    }
    pa_rc_inc(p->in_obj);
    return p->in_obj;
}

int64_t pa_popen_wait(PyAdamasPopen* p, double timeout) {
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
            pa_raise_c("TimeoutExpired", msg);
        }
        poll(NULL, 0, 1);
    }
}

void pa_popen_kill(PyAdamasPopen* p) {
    if (!p->has_rc) kill((pid_t)p->pid, SIGKILL);
}

void pa_popen_terminate(PyAdamasPopen* p) {
    if (!p->has_rc) kill((pid_t)p->pid, SIGTERM);
}

void pa_popen_send_signal(PyAdamasPopen* p, int64_t sig) {
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
PyAdamasList* pa_popen_communicate(PyAdamasPopen* p, PyAdamasStr* input) {
    if (input && p->in_obj) {
        pa_file_write(p->in_obj, input);
        pa_file_close(p->in_obj);
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
        PyAdamasStr* a = pa_file_read(p->out_obj, -1);
        PyAdamasStr* b = pa_file_read(p->err_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        pa_rc_dec(a);
        pa_rc_dec(b);
    } else if (p->out_obj) {
        PyAdamasStr* a = pa_file_read(p->out_obj, -1);
        buf_append(&obuf, &olen, &ocap, a->data, (size_t)a->len);
        pa_rc_dec(a);
        if (p->err_fd >= 0) drain(&p->err_fd, &ebuf, &elen, &ecap);
    } else if (p->err_obj) {
        PyAdamasStr* b = pa_file_read(p->err_obj, -1);
        buf_append(&ebuf, &elen, &ecap, b->data, (size_t)b->len);
        pa_rc_dec(b);
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

    PyAdamasList* out = pa_list_new(2, 1);
    PyAdamasStr* so = str_new(obuf ? obuf : "", (int64_t)olen);
    PyAdamasStr* se = str_new(ebuf ? ebuf : "", (int64_t)elen);
    pa_list_append(out, (int64_t)so);
    pa_list_append(out, (int64_t)se);
    free(obuf);
    free(ebuf);
    return out;
}
#endif