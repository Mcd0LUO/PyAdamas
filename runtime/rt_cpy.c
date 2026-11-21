// rt_cpy.c —— CPython 桥：python3 worker 子进程 + 带标签 JSON 帧 IPC
//
// 设计文档：docs/cpybridge-design.md。要点：
//   - from python import <module> 的调用降级成 pa_cpy_call(module, attr,
//     args, descs)。首次调用懒拉起 python3 -u -c <worker>，全进程复用。
//   - 帧 = 一行 JSON：请求 {"id","m","f","a":[带标签值]}，应答
//     {"id","ok":true,"v":带标签值} 或 {"id","ok":false,"e":[MRO名],"msg"}。
//   - 值标签：null=None / true,false=bool / {"i"}=int / {"f"}=float /
//     {"s"}=str / {"y"}=bytes(base64) / {"l"}=list / {"t"}=tuple /
//     {"d"}=dict([[k,v]...])。int/float、tuple/list、str/bytes 全部显式区分。
//   - 参数 marshalling 按编译期描述符（rt_json 同款自定界语法，扩展
//     y=bytes、?=unknown 动态探针、(d<k><v>) 双描述符 dict）。
//   - 结果是 unknown 机器字：float 装 CPYFLOAT 盒，容器是真 list/dict。
//   - CPython 异常按 MRO 映射到 pyadamas 内置异常名，pa_raise_c 抛出，
//     try/except 可捕获；未知类型归 RuntimeError（消息带原名）。
//
// 先验原型边界（不做）：kwargs、handle 代理、超时、worker 池、生成器。
#include "rt_internal.h"

#if defined(_WIN32)

/* Windows 移植不在先验范围：fork/exec 路径与 rt_subprocess.c 的 Win 分支
   同构，后续按 CreateProcess + 匿名管道补齐。 */
int64_t pa_cpy_call(PyAdamasStr* module, PyAdamasStr* attr, PyAdamasList* args,
                    PyAdamasStr* descs) {
    (void)module; (void)attr; (void)args; (void)descs;
    pa_raise_c("OSError", "cpybridge: Windows support is not implemented yet");
    return 0;
}
void pa_print_cpy_value(int64_t v) { pa_print_i64(v); }

#else

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── worker 脚本（嵌入运行时的 Python 字符串）───────────────────── */

static const char CPY_WORKER[] =
    "import sys, json, base64, importlib\n"
    "mods = {}\n"
    "def dec(v):\n"
    "    if v is None or v is True or v is False:\n"
    "        return v\n"
    "    if isinstance(v, dict):\n"
    "        k = next(iter(v))\n"
    "        x = v[k]\n"
    "        if k == 'i': return x\n"
    "        if k == 'f': return float(x)\n"
    "        if k == 's': return x\n"
    "        if k == 'y': return base64.b64decode(x)\n"
    "        if k == 'l': return [dec(e) for e in x]\n"
    "        if k == 't': return tuple(dec(e) for e in x)\n"
    "        if k == 'd': return {dec(a): dec(b) for a, b in x}\n"
    "    raise TypeError('bad cpybridge frame')\n"
    "def enc(v):\n"
    "    if v is None: return None\n"
    "    if isinstance(v, bool): return v\n"
    "    if isinstance(v, int):\n"
    "        if not (-9223372036854775808 <= v < 9223372036854775808):\n"
    "            raise OverflowError('int out of pyadamas int64 range')\n"
    "        return {'i': v}\n"
    "    if isinstance(v, float): return {'f': v}\n"
    "    if isinstance(v, str): return {'s': v}\n"
    "    if isinstance(v, (bytes, bytearray)):\n"
    "        return {'y': base64.b64encode(bytes(v)).decode('ascii')}\n"
    "    if isinstance(v, list): return {'l': [enc(e) for e in v]}\n"
    "    if isinstance(v, tuple): return {'t': [enc(e) for e in v]}\n"
    "    if isinstance(v, dict):\n"
    "        return {'d': [[enc(k), enc(x)] for k, x in v.items()]}\n"
    "    raise TypeError('cannot marshal ' + type(v).__name__ + ' to pyadamas')\n"
    "for line in sys.stdin:\n"
    "    line = line.strip()\n"
    "    if not line: continue\n"
    "    req = json.loads(line)\n"
    "    if req.get('cmd') == 'exit': break\n"
    "    rid = req.get('id', 0)\n"
    "    try:\n"
    "        m = req['m']\n"
    "        if m not in mods: mods[m] = importlib.import_module(m)\n"
    "        fn = getattr(mods[m], req['f'])\n"
    "        r = fn(*[dec(a) for a in req['a']])\n"
    "        resp = {'id': rid, 'ok': True, 'v': enc(r)}\n"
    "    except Exception as e:\n"
    "        resp = {'id': rid, 'ok': False,\n"
    "                'e': [c.__name__ for c in type(e).__mro__],\n"
    "                'msg': str(e)}\n"
    "    sys.stdout.write(json.dumps(resp) + '\\n')\n"
    "    sys.stdout.flush()\n";

/* ── 输出缓冲 / JSON 字符串 / base64 ───────────────────────────── */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} CpyBuf;

static void cb_raw(CpyBuf* b, const char* p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char* nb = (char*)realloc(b->buf, cap);
        if (!nb) panic_c("out of memory");
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    b->buf[b->len] = '\0';
}
static void cb_ch(CpyBuf* b, char c) { cb_raw(b, &c, 1); }
static void cb_cstr(CpyBuf* b, const char* s) { cb_raw(b, s, strlen(s)); }
static void cb_i64(CpyBuf* b, int64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    cb_raw(b, tmp, (size_t)n);
}
static void cb_u4(CpyBuf* b, uint32_t cp) {
    char tmp[8];
    /* cp 由调用方保证 <= 0xFFFF；用返回值确认没截断（与 rt_json 同口径，
       比让 -Wformat-truncation 去猜可靠）。 */
    int wn = snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)cp);
    if (wn < 0 || wn >= (int)sizeof tmp) panic_c("cpybridge: unicode escape overflow");
    cb_raw(b, tmp, 6);
}
static void cb_utf8_cp(CpyBuf* b, uint32_t cp) {
    if (cp >= 0x10000) {
        uint32_t v = cp - 0x10000;
        cb_u4(b, 0xD800 + (v >> 10));
        cb_u4(b, 0xDC00 + (v & 0x3FF));
    } else {
        cb_u4(b, cp);
    }
}

/* JSON 字符串（ensure_ascii 口径：非 ASCII 走 \\uXXXX，与 rt_json 一致）。 */
static void cb_jstr(CpyBuf* b, const char* data, int64_t n) {
    cb_ch(b, '"');
    const unsigned char* p = (const unsigned char*)data;
    int64_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c == '"') { cb_cstr(b, "\\\""); i++; }
        else if (c == '\\') { cb_cstr(b, "\\\\"); i++; }
        else if (c == '\n') { cb_cstr(b, "\\n"); i++; }
        else if (c == '\r') { cb_cstr(b, "\\r"); i++; }
        else if (c == '\t') { cb_cstr(b, "\\t"); i++; }
        else if (c == '\b') { cb_cstr(b, "\\b"); i++; }
        else if (c == '\f') { cb_cstr(b, "\\f"); i++; }
        else if (c < 0x20) { cb_u4(b, c); i++; }
        else if (c < 0x80) { cb_ch(b, (char)c); i++; }
        else {
            uint32_t cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else { cb_u4(b, c); i++; continue; }
            if (i + extra >= n) { cb_u4(b, c); i++; continue; }
            int ok = 1;
            for (int k = 1; k <= extra; ++k) {
                unsigned char cc = p[i + k];
                if ((cc & 0xC0) != 0x80) { ok = 0; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) { cb_u4(b, c); i++; continue; }
            cb_utf8_cp(b, cp);
            i += extra + 1;
        }
    }
    cb_ch(b, '"');
}

static const char B64_TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void cb_b64(CpyBuf* b, const unsigned char* p, size_t n) {
    size_t i = 0;
    while (i + 2 < n) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        char out[4] = {B64_TAB[v >> 18], B64_TAB[(v >> 12) & 63],
                       B64_TAB[(v >> 6) & 63], B64_TAB[v & 63]};
        cb_raw(b, out, 4);
        i += 3;
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t)p[i] << 16;
        char out[4] = {B64_TAB[v >> 18], B64_TAB[(v >> 12) & 63], '=', '='};
        cb_raw(b, out, 4);
    } else if (i + 2 == n) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        char out[4] = {B64_TAB[v >> 18], B64_TAB[(v >> 12) & 63],
                       B64_TAB[(v >> 6) & 63], '='};
        cb_raw(b, out, 4);
    }
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64 解码到 malloc 缓冲；非法输入抛 ValueError。 */
static unsigned char* b64_decode(const char* s, size_t n, size_t* out_n) {
    unsigned char* out = (unsigned char*)malloc(n / 4 * 3 + 4);
    if (!out) panic_c("out of memory");
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '=' || s[i] == '\n' || s[i] == '\r') continue;
        int v = b64_val(s[i]);
        if (v < 0) { free(out); pa_raise_c("ValueError", "cpybridge: bad base64"); }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    *out_n = o;
    return out;
}

/* ── 参数 marshalling ──────────────────────────────────────────── */

/* 自定界描述符长度（与 rt_json.c 的 desc_len 同语法）。 */
static size_t cpy_desc_len(const char* d) {
    if (*d != '(') return 1;
    int depth = 0;
    size_t i = 0;
    do {
        if (d[i] == '(') depth++;
        else if (d[i] == ')') depth--;
        i++;
    } while (depth > 0 && d[i] != '\0');
    return i;
}

static void cpy_marshal(CpyBuf* b, int64_t v, const char* d, size_t n);

/* unknown 实参的动态探针：堆按 kind 分派，标量按 int。
   固有歧义（设计文档有载）：int/bool/None 在 unknown 里都是裸字
   （0 按 None 发）；str/bytes 运行期同构（按 str 发）；tuple/list 同构
   （按 list 发）。静态类型已知的实参走描述符路径，没有这些歧义。 */
static void cpy_marshal_dyn(CpyBuf* b, int64_t v) {
    if (!pa_unknown_is_heap(v)) {
        if (v == 0) { cb_cstr(b, "null"); return; }
        cb_cstr(b, "{\"i\":");
        cb_i64(b, v);
        cb_ch(b, '}');
        return;
    }
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (h->kind == PYADAMAS_RC_STR) {
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)v;
        cb_cstr(b, "{\"s\":");
        cb_jstr(b, s->data, s->len);
        cb_ch(b, '}');
        return;
    }
    if (h->kind == PYADAMAS_RC_CPYFLOAT) {
        double d;
        memcpy(&d, (void*)(uintptr_t)v, sizeof d);
        char tmp[64];
        format_double(tmp, sizeof tmp, d);
        cb_cstr(b, "{\"f\":");
        cb_cstr(b, tmp);
        cb_ch(b, '}');
        return;
    }
    if (h->kind == PYADAMAS_RC_LIST) {
        PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
        cb_cstr(b, "{\"l\":[");
        for (int64_t i = 0; i < l->len; ++i) {
            if (i) cb_ch(b, ',');
            cpy_marshal_dyn(b, l->data[i]);
        }
        cb_cstr(b, "]}");
        return;
    }
    if (h->kind == PYADAMAS_RC_DICT) {
        PyAdamasDict* m = (PyAdamasDict*)(uintptr_t)v;
        cb_cstr(b, "{\"d\":[");
        for (int64_t i = 0; i < m->len; ++i) {
            if (i) cb_ch(b, ',');
            cb_ch(b, '[');
            cpy_marshal_dyn(b, pa_dict_key_at(m, i));
            cb_ch(b, ',');
            cpy_marshal_dyn(b, pa_dict_val_at(m, i));
            cb_ch(b, ']');
        }
        cb_cstr(b, "]}");
        return;
    }
    pa_raise_c("TypeError",
               "cpybridge: cannot marshal this unknown value to CPython "
               "(handle proxies are future work)");
}

static void cpy_marshal(CpyBuf* b, int64_t v, const char* d, size_t n) {
    char t = d[0];
    if (t == '?') { cpy_marshal_dyn(b, v); return; }
    if (t == '(') {
        char k = d[1];
        if (k == 'l') {
            PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
            const char* cd = d + 2;
            size_t cn = n - 3;
            cb_cstr(b, "{\"l\":[");
            for (int64_t i = 0; i < l->len; ++i) {
                if (i) cb_ch(b, ',');
                cpy_marshal(b, l->data[i], cd, cn);
            }
            cb_cstr(b, "]}");
            return;
        }
        if (k == 't') {
            PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
            cb_cstr(b, "{\"t\":[");
            size_t pos = 2;
            int64_t idx = 0;
            while (pos + 1 < n) {
                size_t cl = cpy_desc_len(d + pos);
                if (idx) cb_ch(b, ',');
                if (idx >= l->len)
                    pa_raise_c("ValueError", "cpybridge: tuple descriptor mismatch");
                cpy_marshal(b, l->data[idx], d + pos, cl);
                pos += cl;
                idx++;
            }
            cb_cstr(b, "]}");
            return;
        }
        if (k == 'd') {
            PyAdamasDict* m = (PyAdamasDict*)(uintptr_t)v;
            size_t kl = cpy_desc_len(d + 2);
            const char* kd = d + 2;
            const char* vd = d + 2 + kl;
            size_t vn = n - 3 - kl;
            cb_cstr(b, "{\"d\":[");
            for (int64_t i = 0; i < m->len; ++i) {
                if (i) cb_ch(b, ',');
                cb_ch(b, '[');
                cpy_marshal(b, pa_dict_key_at(m, i), kd, kl);
                cb_ch(b, ',');
                cpy_marshal(b, pa_dict_val_at(m, i), vd, vn);
                cb_ch(b, ']');
            }
            cb_cstr(b, "]}");
            return;
        }
        pa_raise_c("ValueError", "cpybridge: bad type descriptor");
    }
    if (t == 'n') { cb_cstr(b, "null"); return; }
    if (t == 'b') { cb_cstr(b, v ? "true" : "false"); return; }
    if (t == 'i') {
        cb_cstr(b, "{\"i\":");
        cb_i64(b, v);
        cb_ch(b, '}');
        return;
    }
    if (t == 'f') {
        double dv;
        memcpy(&dv, &v, sizeof dv);
        char tmp[64];
        format_double(tmp, sizeof tmp, dv);
        cb_cstr(b, "{\"f\":");
        cb_cstr(b, tmp);
        cb_ch(b, '}');
        return;
    }
    if (t == 's') {
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)v;
        cb_cstr(b, "{\"s\":");
        cb_jstr(b, s->data, s->len);
        cb_ch(b, '}');
        return;
    }
    if (t == 'y') {
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)v;
        cb_cstr(b, "{\"y\":\"");
        cb_b64(b, (const unsigned char*)s->data, (size_t)s->len);
        cb_cstr(b, "\"}");
        return;
    }
    pa_raise_c("ValueError", "cpybridge: bad type descriptor");
}

/* ── worker 生命周期 ───────────────────────────────────────────── */

static pid_t w_pid = -1;
static int w_in = -1;   /* 写 → worker stdin */
static int w_out = -1;  /* 读 ← worker stdout */
static int w_err = -1;  /* 读 ← worker stderr（非阻塞，诊断用） */
static int64_t w_next_id = 1;
static int w_atexit_done = 0;

/* stdout 读缓冲（跨调用复用；line 指针在下次 read 前有效）。 */
static char* w_rbuf = NULL;
static size_t w_rlen = 0, w_rcap = 0, w_rpos = 0;

static void cpy_close_fds(void) {
    if (w_in >= 0) { close(w_in); w_in = -1; }
    if (w_out >= 0) { close(w_out); w_out = -1; }
    if (w_err >= 0) { close(w_err); w_err = -1; }
}

/* 退出 drain：发 exit 帧 + 关 stdin（EOF 兜底），限时等 worker 收尾，
   超时 SIGKILL。atexit 注册一次；worker 不碰 pyadamas 对象，与 RC/GC
   的 shutdown 顺序无耦合。 */
static void cpy_shutdown(void) {
    if (w_pid <= 0) { cpy_close_fds(); return; }
    if (w_in >= 0) {
        const char exit_frame[] = "{\"cmd\":\"exit\"}\n";
        ssize_t wr = write(w_in, exit_frame, sizeof exit_frame - 1);
        (void)wr;
        close(w_in);
        w_in = -1;
    }
    for (int i = 0; i < 50; ++i) {
        pid_t r = waitpid(w_pid, NULL, WNOHANG);
        if (r == w_pid || r < 0) { w_pid = -1; break; }
        usleep(10000);
    }
    if (w_pid > 0) {
        kill(w_pid, SIGKILL);
        waitpid(w_pid, NULL, 0);
        w_pid = -1;
    }
    cpy_close_fds();
}

static void cpy_spawn(void) {
    int ip[2] = {-1, -1}, op[2] = {-1, -1}, ep[2] = {-1, -1};
    if (pipe(ip) != 0) pa_raise_c("OSError", "cpybridge: pipe failed");
    if (pipe(op) != 0) {
        close(ip[0]); close(ip[1]);
        pa_raise_c("OSError", "cpybridge: pipe failed");
    }
    if (pipe(ep) != 0) {
        close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
        pa_raise_c("OSError", "cpybridge: pipe failed");
    }
    /* 写断 pipe 要拿 EPIPE 而不是 SIGPIPE 杀进程。 */
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
        close(ep[0]); close(ep[1]);
        pa_raise_c("OSError", "cpybridge: fork failed");
    }
    if (pid == 0) {
        dup2(ip[0], 0);
        dup2(op[1], 1);
        dup2(ep[1], 2);
        close(ip[0]); close(ip[1]);
        close(op[0]); close(op[1]);
        close(ep[0]); close(ep[1]);
        execlp("python3", "python3", "-u", "-c", CPY_WORKER, (char*)NULL);
        _exit(127);
    }
    close(ip[0]); close(op[1]); close(ep[1]);
    w_pid = pid;
    w_in = ip[1];
    w_out = op[0];
    w_err = ep[0];
    fcntl(w_err, F_SETFL, O_NONBLOCK);
    w_rlen = w_rpos = 0;
    if (!w_atexit_done) {
        w_atexit_done = 1;
        atexit(cpy_shutdown);
    }
}

/* worker 死亡诊断：收 stderr 尾、收尸、关 fd，抛 RuntimeError。
   先验阶段不自动重拉（状态一致性优先；重拉策略见设计文档）。 */
static void cpy_crash(const char* what) {
    char tail[2048];
    size_t tn = 0;
    if (w_err >= 0) {
        for (;;) {
            ssize_t r = read(w_err, tail + tn, sizeof tail - 1 - tn);
            if (r <= 0) break;
            tn += (size_t)r;
            if (tn >= sizeof tail - 1) break;
        }
    }
    tail[tn] = '\0';
    cpy_close_fds();
    if (w_pid > 0) {
        kill(w_pid, SIGKILL);
        waitpid(w_pid, NULL, 0);
        w_pid = -1;
    }
    char msg[2400];
    snprintf(msg, sizeof msg, "cpybridge: CPython worker %s%s%s",
             what, tn ? ": " : "", tn ? tail : "");
    pa_raise_c("RuntimeError", msg);
}

static void cpy_write_all(const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(w_in, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            cpy_crash("write failed (worker died?)");
            return;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
}

/* 读一行（\\n 结尾）。返回的指针指向静态读缓冲，下次调用前有效。 */
static char* cpy_read_line(void) {
    for (;;) {
        if (w_rlen > w_rpos) {
            char* nl = (char*)memchr(w_rbuf + w_rpos, '\n', w_rlen - w_rpos);
            if (nl) {
                *nl = '\0';
                char* line = w_rbuf + w_rpos;
                w_rpos = (size_t)(nl - w_rbuf) + 1;
                return line;
            }
        }
        if (w_rpos > 0) {
            memmove(w_rbuf, w_rbuf + w_rpos, w_rlen - w_rpos);
            w_rlen -= w_rpos;
            w_rpos = 0;
        }
        if (w_rlen + 4096 + 1 > w_rcap) {
            size_t ncap = w_rcap ? w_rcap * 2 : 8192;
            while (ncap < w_rlen + 4096 + 1) ncap *= 2;
            char* nb = (char*)realloc(w_rbuf, ncap);
            if (!nb) panic_c("out of memory");
            w_rbuf = nb;
            w_rcap = ncap;
        }
        ssize_t r = read(w_out, w_rbuf + w_rlen, 4096);
        if (r < 0) {
            if (errno == EINTR) continue;
            cpy_crash("read failed");
            return NULL;
        }
        if (r == 0) {
            cpy_crash("exited unexpectedly (EOF on response pipe)");
            return NULL;
        }
        w_rlen += (size_t)r;
    }
}

/* 每次调用前机会性 drain stderr（非阻塞），防 worker 警告写满 pipe。 */
static void cpy_drain_stderr(void) {
    char sink[4096];
    if (w_err < 0) return;
    for (;;) {
        ssize_t r = read(w_err, sink, sizeof sink);
        if (r <= 0) break;
    }
}

/* ── 应答 JSON 解析（动态 DOM）─────────────────────────────────── */

typedef struct CJ {
    int kind; /* 0 null 1 bool 2 int 3 float 4 str 5 arr 6 obj */
    int64_t i;
    double f;
    char* s;
    size_t sn;
    struct CJ** v; /* arr 元素 / obj 值 */
    char** k;      /* obj 键（与 v 平行） */
    size_t n;
} CJ;

static void cj_free(CJ* j) {
    if (!j) return;
    free(j->s);
    for (size_t i = 0; i < j->n; ++i) {
        if (j->k) free(j->k[i]);
        cj_free(j->v[i]);
    }
    free(j->v);
    free(j->k);
    free(j);
}

static CJ* cj_new(int kind) {
    CJ* j = (CJ*)calloc(1, sizeof(CJ));
    if (!j) panic_c("out of memory");
    j->kind = kind;
    return j;
}

static void cj_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static _Noreturn void cj_fail(const char* what) {
    char msg[160];
    snprintf(msg, sizeof msg, "cpybridge: bad response frame: %s", what);
    pa_raise_c("RuntimeError", msg);
}

static int cj_hex4(const char** p) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = (*p)[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else cj_fail("invalid \\u escape");
        v = v * 16 + d;
    }
    *p += 4;
    return v;
}

static void cj_put_utf8(CpyBuf* b, uint32_t cp) {
    if (cp < 0x80) cb_ch(b, (char)cp);
    else if (cp < 0x800) {
        cb_ch(b, (char)(0xC0 | (cp >> 6)));
        cb_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        cb_ch(b, (char)(0xE0 | (cp >> 12)));
        cb_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        cb_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        cb_ch(b, (char)(0xF0 | (cp >> 18)));
        cb_ch(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        cb_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        cb_ch(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* 解析 JSON 字符串（含外层引号）。 */
static char* cj_string(const char** p, size_t* out_n) {
    if (**p != '"') cj_fail("expected string");
    (*p)++;
    CpyBuf b = {NULL, 0, 0};
    while (**p) {
        unsigned char c = (unsigned char)*(*p)++;
        if (c == '"') {
            *out_n = b.len;
            if (!b.buf) { b.buf = (char*)malloc(1); b.buf[0] = '\0'; }
            return b.buf;
        }
        if (c == '\\') {
            char e = *(*p)++;
            switch (e) {
                case '"': cb_ch(&b, '"'); break;
                case '\\': cb_ch(&b, '\\'); break;
                case '/': cb_ch(&b, '/'); break;
                case 'b': cb_ch(&b, '\b'); break;
                case 'f': cb_ch(&b, '\f'); break;
                case 'n': cb_ch(&b, '\n'); break;
                case 'r': cb_ch(&b, '\r'); break;
                case 't': cb_ch(&b, '\t'); break;
                case 'u': {
                    uint32_t cp = (uint32_t)cj_hex4(p);
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if ((*p)[0] == '\\' && (*p)[1] == 'u') {
                            *p += 2;
                            uint32_t lo = (uint32_t)cj_hex4(p);
                            if (lo < 0xDC00 || lo > 0xDFFF) {
                                free(b.buf);
                                cj_fail("bad surrogate pair");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else { free(b.buf); cj_fail("unpaired surrogate"); }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        free(b.buf);
                        cj_fail("unpaired surrogate");
                    }
                    cj_put_utf8(&b, cp);
                    break;
                }
                default: free(b.buf); cj_fail("invalid escape");
            }
            continue;
        }
        cb_ch(&b, (char)c);
    }
    free(b.buf);
    cj_fail("unterminated string");
}

static CJ* cj_parse(const char** p, int depth) {
    if (depth > 256) cj_fail("frame nested too deep");
    cj_ws(p);
    char c = **p;
    if (c == '\0') cj_fail("unexpected end");
    if (c == 'n') {
        if (strncmp(*p, "null", 4) != 0) cj_fail("bad literal");
        *p += 4;
        return cj_new(0);
    }
    if (c == 't' || c == 'f') {
        int is_t = c == 't';
        if (strncmp(*p, is_t ? "true" : "false", is_t ? 4 : 5) != 0) cj_fail("bad literal");
        *p += is_t ? 4 : 5;
        CJ* j = cj_new(1);
        j->i = is_t;
        return j;
    }
    if (c == '"') {
        CJ* j = cj_new(4);
        j->s = cj_string(p, &j->sn);
        return j;
    }
    if (c == '[' || c == '{') {
        int is_obj = c == '{';
        (*p)++;
        CJ* j = cj_new(is_obj ? 6 : 5);
        size_t cap = 0;
        cj_ws(p);
        if (**p == (is_obj ? '}' : ']')) { (*p)++; return j; }
        for (;;) {
            if (j->n == cap) {
                cap = cap ? cap * 2 : 8;
                j->v = (CJ**)realloc(j->v, sizeof(CJ*) * cap);
                if (is_obj) j->k = (char**)realloc(j->k, sizeof(char*) * cap);
                if (!j->v || (is_obj && !j->k)) panic_c("out of memory");
            }
            if (is_obj) {
                cj_ws(p);
                size_t kn = 0;
                j->k[j->n] = cj_string(p, &kn);
                cj_ws(p);
                if (*(*p)++ != ':') cj_fail("expected ':'");
            }
            j->v[j->n] = cj_parse(p, depth + 1);
            j->n++;
            cj_ws(p);
            if (**p == ',') { (*p)++; continue; }
            break;
        }
        cj_ws(p);
        if (*(*p)++ != (is_obj ? '}' : ']')) cj_fail("expected closing bracket");
        return j;
    }
    /* 数字（含 Python json 的 NaN / Infinity / -Infinity 扩展）。 */
    if (strncmp(*p, "NaN", 3) == 0) {
        *p += 3;
        CJ* j = cj_new(3);
        j->f = (double)NAN;
        return j;
    }
    if (strncmp(*p, "-Infinity", 9) == 0) {
        *p += 9;
        CJ* j = cj_new(3);
        j->f = -(double)INFINITY;
        return j;
    }
    if (strncmp(*p, "Infinity", 8) == 0) {
        *p += 8;
        CJ* j = cj_new(3);
        j->f = (double)INFINITY;
        return j;
    }
    const char* start = *p;
    if (**p == '-' || **p == '+') (*p)++;
    int is_f = 0;
    while (**p) {
        char d = **p;
        if (d >= '0' && d <= '9') { (*p)++; continue; }
        if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
            is_f = 1;
            (*p)++;
            continue;
        }
        break;
    }
    size_t n = (size_t)(*p - start);
    if (n == 0 || n >= 64) cj_fail("invalid number");
    char tok[64];
    memcpy(tok, start, n);
    tok[n] = '\0';
    if (is_f) {
        CJ* j = cj_new(3);
        j->f = strtod(tok, NULL);
        return j;
    }
    errno = 0;
    char* endp = NULL;
    long long v = strtoll(tok, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0')
        pa_raise_c("OverflowError", "cpybridge: integer out of int64 range");
    CJ* j = cj_new(2);
    j->i = (int64_t)v;
    return j;
}

/* ── 应答解码：带标签值 → pyadamas unknown 机器字 ─────────────── */

static int64_t cpy_float_box(double d) {
    /* 必须走 tracked 分配：rc_alloc_untracked 不进 rc_index，而索引开启时
       unknown 探针只认索引（无回退），float 盒会变成「看不见的堆值」。
       CPYFLOAT 无出边，GC 按叶子处理，开销可接受。 */
    int64_t* p = (int64_t*)rc_alloc((int64_t)sizeof(int64_t), NULL, NULL,
                                    PYADAMAS_RC_CPYFLOAT);
    memcpy(p, &d, sizeof d);
    return (int64_t)(uintptr_t)p;
}

static int64_t cpy_decode(CJ* j, int depth);

static int64_t cpy_decode_list(CJ* x, int depth) {
    if (x->kind != 5) cj_fail("expected array payload");
    PyAdamasList* out = pa_list_new((int64_t)x->n, 2);
    pa_rc_temp(out);
    PA_PIN_SCOPE(out);
    for (size_t i = 0; i < x->n; ++i) {
        int64_t el = cpy_decode(x->v[i], depth + 1);
        pa_list_append(out, el, 2); /* 所有权转移，与 rt_json 同口径 */
    }
    PA_PIN_RELEASE(out);
    pa_rc_temp_take(out);
    return (int64_t)(uintptr_t)out;
}

static int64_t cpy_decode_dict(CJ* x, int depth) {
    if (x->kind != 5) cj_fail("expected pair array payload");
    size_t n = x->n;
    /* 键先解码进一个 kind 2 临时列表（RC 托管，异常路径自动清理）。
       全 str 键用 key_kind=1（内容哈希，pyadamas 侧才能按等值 str 查到）；
       否则 raw（int 键按字相等；float 盒/容器键按指针——先验限制）。 */
    PyAdamasList* klist = pa_list_new((int64_t)n, 2);
    pa_rc_temp(klist);
    PA_PIN_SCOPE(klist);
    int all_str = 1;
    for (size_t i = 0; i < n; ++i) {
        CJ* pair = x->v[i];
        if (pair->kind != 5 || pair->n != 2) cj_fail("expected [key, value] pair");
        int64_t k = cpy_decode(pair->v[0], depth + 1);
        pa_list_append(klist, k, 2);
        if (pa_unknown_is_heap(k)) {
            PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)k - (uint64_t)PYADAMAS_RC_HDR);
            if (h->kind != PYADAMAS_RC_STR) all_str = 0;
        } else {
            all_str = 0;
        }
    }
    PyAdamasDict* out = pa_dict_new(all_str ? 1 : 3, all_str ? 1 : 2, 2, -1, -1);
    pa_rc_temp(out);
    PA_PIN_SCOPE(out);
    for (size_t i = 0; i < n; ++i) {
        CJ* pair = x->v[i];
        int64_t k = klist->data[i]; /* 借用：klist 持有一份引用 */
        int64_t v = cpy_decode(pair->v[1], depth + 1);
        if (pa_unknown_is_heap(v)) pa_gc_pin((void*)(uintptr_t)v);
        /* 先给 dict 补一份入表引用，再 set（set 消费它）；v 的 pin 在
           set 之后还掉。顺序保证 v 解码抛异常时 k 不会被多 inc。 */
        pa_own_inc(all_str ? 1 : 2, k);
        pa_dict_set(out, k, v);
        if (pa_unknown_is_heap(v)) pa_gc_unpin((void*)(uintptr_t)v);
    }
    PA_PIN_RELEASE(out);
    pa_rc_temp_take(out);
    /* klist 的键引用已全部转给 dict（上面 inc 过）：销毁 klist 还掉它
       自己那份。异常路径上 klist/out 都在临时栈，cleanup 统一收尾。 */
    PA_PIN_RELEASE(klist);
    pa_rc_temp_take(klist);
    pa_rc_dec(klist);
    return (int64_t)(uintptr_t)out;
}

static int64_t cpy_decode(CJ* j, int depth) {
    if (depth > 256) cj_fail("value nested too deep");
    switch (j->kind) {
        case 0: return 0; /* None：unknown 里就是 0 */
        case 1: return j->i;
        case 2: return j->i;
        case 3: return cpy_float_box(j->f);
        case 4: return (int64_t)(uintptr_t)str_new(j->s, (int64_t)j->sn);
        case 6: {
            if (j->n != 1) cj_fail("tagged value must have one key");
            const char* tag = j->k[0];
            CJ* x = j->v[0];
            if (strcmp(tag, "i") == 0) {
                if (x->kind == 2) return x->i;
                if (x->kind == 3) {
                    if (x->f != (double)(int64_t)x->f)
                        cj_fail("int tag with fractional value");
                    return (int64_t)x->f;
                }
                cj_fail("bad int payload");
            }
            if (strcmp(tag, "f") == 0) {
                if (x->kind == 3) return cpy_float_box(x->f);
                if (x->kind == 2) return cpy_float_box((double)x->i);
                if (x->kind == 4) return cpy_float_box(strtod(x->s, NULL));
                cj_fail("bad float payload");
            }
            if (strcmp(tag, "s") == 0) {
                if (x->kind != 4) cj_fail("bad str payload");
                return (int64_t)(uintptr_t)str_new(x->s, (int64_t)x->sn);
            }
            if (strcmp(tag, "y") == 0) {
                if (x->kind != 4) cj_fail("bad bytes payload");
                size_t bn = 0;
                unsigned char* bp = b64_decode(x->s, x->sn, &bn);
                PyAdamasStr* s = str_new_bytes((const char*)bp, (int64_t)bn);
                free(bp);
                return (int64_t)(uintptr_t)s;
            }
            if (strcmp(tag, "l") == 0 || strcmp(tag, "t") == 0)
                return cpy_decode_list(x, depth); /* tuple 运行期与 list 同构 */
            if (strcmp(tag, "d") == 0) return cpy_decode_dict(x, depth);
            cj_fail("unknown value tag");
        }
        default: cj_fail("unexpected raw array");
    }
}

/* ── 异常映射 ──────────────────────────────────────────────────── */

/* pyadamas 内置异常名（与 typecheck is_exception_name 的常用子集对齐）：
   worker 发来异常 MRO 名表，取第一个命中的；全不命中归 RuntimeError，
   消息里带 CPython 原名。 */
static int cpy_exc_known(const char* n) {
    static const char* kNames[] = {
        "BaseException", "Exception", "ArithmeticError", "AssertionError",
        "AttributeError", "IndexError", "KeyError", "LookupError",
        "MemoryError", "NameError", "NotImplementedError", "OSError",
        "IOError", "OverflowError", "RecursionError", "RuntimeError",
        "StopIteration", "SyntaxError", "TypeError", "ValueError",
        "ZeroDivisionError", "FileNotFoundError", "PermissionError",
        "ImportError", "ModuleNotFoundError", "UnicodeDecodeError",
        "UnicodeEncodeError", "TimeoutError", "SystemExit", "KeyboardInterrupt",
        "ConnectionError", "BrokenPipeError", "UnicodeError", "ReferenceError",
        "BufferError", "EOFError", "FileExistsError", "IsADirectoryError",
        "NotADirectoryError", "SystemError", "FloatingPointError",
        "BlockingIOError", "ChildProcessError", "ConnectionAbortedError",
        "ConnectionRefusedError", "ConnectionResetError", "InterruptedError",
        "ProcessLookupError", "UnicodeTranslateError", "GeneratorExit",
        "StopAsyncIteration", "UnboundLocalError", "IndentationError",
        "TabError", "ExceptionGroup", "BaseExceptionGroup",
    };
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i)
        if (strcmp(n, kNames[i]) == 0) return 1;
    return 0;
}

static CJ* cj_obj_get(CJ* obj, const char* key) {
    if (!obj || obj->kind != 6) return NULL;
    for (size_t i = 0; i < obj->n; ++i)
        if (strcmp(obj->k[i], key) == 0) return obj->v[i];
    return NULL;
}

/* 把异常应答摘成 C 缓冲（类型名 + 消息），调用方先 cj_free 再抛，
   避免 longjmp 跳过 DOM 的 free。 */
static void cpy_exc_pick(CJ* resp, char* type, size_t tn, char* msg, size_t mn) {
    CJ* e = cj_obj_get(resp, "e");
    CJ* m = cj_obj_get(resp, "msg");
    const char* mstr = (m && m->kind == 4) ? m->s : "unknown CPython error";
    const char* picked = NULL;
    const char* first = NULL;
    if (e && e->kind == 5) {
        for (size_t i = 0; i < e->n; ++i) {
            if (e->v[i]->kind != 4) continue;
            if (!first) first = e->v[i]->s;
            if (cpy_exc_known(e->v[i]->s)) { picked = e->v[i]->s; break; }
        }
    }
    if (picked) {
        snprintf(type, tn, "%s", picked);
        snprintf(msg, mn, "%s", mstr);
    } else {
        snprintf(type, tn, "RuntimeError");
        snprintf(msg, mn, "CPython %s: %s", first ? first : "Exception", mstr);
    }
}

/* ── 入口 ──────────────────────────────────────────────────────── */

int64_t pa_cpy_call(PyAdamasStr* module, PyAdamasStr* attr, PyAdamasList* args,
                    PyAdamasStr* descs) {
    if (w_pid <= 0) cpy_spawn();
    cpy_drain_stderr();

    /* 组装请求帧。 */
    CpyBuf b = {NULL, 0, 0};
    int64_t id = w_next_id++;
    cb_cstr(&b, "{\"id\":");
    cb_i64(&b, id);
    cb_cstr(&b, ",\"m\":");
    cb_jstr(&b, module->data, module->len);
    cb_cstr(&b, ",\"f\":");
    cb_jstr(&b, attr->data, attr->len);
    cb_cstr(&b, ",\"a\":[");
    const char* dp = descs->data;
    const char* dend = descs->data + descs->len;
    int64_t nargs = args ? args->len : 0;
    for (int64_t i = 0; i < nargs; ++i) {
        if (i) cb_ch(&b, ',');
        if (dp >= dend) {
            free(b.buf);
            pa_raise_c("ValueError", "cpybridge: descriptor/argument count mismatch");
        }
        size_t dl = cpy_desc_len(dp);
        cpy_marshal(&b, args->data[i], dp, dl);
        dp += dl;
    }
    cb_cstr(&b, "]}\n");

    cpy_write_all(b.buf, b.len);
    free(b.buf);

    char* line = cpy_read_line();
    const char* p = line;
    CJ* resp = cj_parse(&p, 0);
    /* 整帧解析完再解码：DOM 是纯 malloc，先摘取再释放，异常路径不漏。 */
    CJ* jid = cj_obj_get(resp, "id");
    if (!jid || jid->kind != 2 || jid->i != id) {
        cj_free(resp);
        cpy_crash("response id mismatch (protocol desync)");
    }
    CJ* ok = cj_obj_get(resp, "ok");
    if (!ok || ok->kind != 1) {
        cj_free(resp);
        cpy_crash("response missing ok flag");
    }
    if (!ok->i) {
        char etype[128], emsg[2400];
        cpy_exc_pick(resp, etype, sizeof etype, emsg, sizeof emsg);
        cj_free(resp);
        pa_raise_c(etype, emsg);
    }
    CJ* v = cj_obj_get(resp, "v");
    if (!v) {
        cj_free(resp);
        cpy_crash("response missing value");
    }
    int64_t r = cpy_decode(v, 0);
    cj_free(resp);
    return r;
}

/* ── unknown 打印扩展（float 盒 / list / dict 的 repr）──────────── */

static void cpy_repr(CpyBuf* b, int64_t v) {
    if (!pa_unknown_is_heap(v)) {
        cb_i64(b, v);
        return;
    }
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (h->kind == PYADAMAS_RC_STR) {
        /* repr 口径：单引号 + 转义（容器内字符串与 CPython 一致）。 */
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)v;
        cb_ch(b, '\'');
        for (int64_t i = 0; i < s->len; ++i) {
            char c = s->data[i];
            if (c == '\'') cb_cstr(b, "\\'");
            else if (c == '\\') cb_cstr(b, "\\\\");
            else if (c == '\n') cb_cstr(b, "\\n");
            else if (c == '\r') cb_cstr(b, "\\r");
            else if (c == '\t') cb_cstr(b, "\\t");
            else cb_ch(b, c);
        }
        cb_ch(b, '\'');
        return;
    }
    if (h->kind == PYADAMAS_RC_BIGINT) {
        PyAdamasStr* s = pa_big_str((PyAdamasBig*)(uintptr_t)v);
        cb_cstr(b, s->data);
        pa_rc_dec(s);
        return;
    }
    if (h->kind == PYADAMAS_RC_CPYFLOAT) {
        double d;
        memcpy(&d, (void*)(uintptr_t)v, sizeof d);
        char tmp[64];
        format_double(tmp, sizeof tmp, d);
        cb_cstr(b, tmp);
        return;
    }
    if (h->kind == PYADAMAS_RC_LIST) {
        PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
        cb_ch(b, '[');
        for (int64_t i = 0; i < l->len; ++i) {
            if (i) cb_cstr(b, ", ");
            cpy_repr(b, l->data[i]);
        }
        cb_ch(b, ']');
        return;
    }
    if (h->kind == PYADAMAS_RC_DICT) {
        PyAdamasDict* m = (PyAdamasDict*)(uintptr_t)v;
        cb_ch(b, '{');
        for (int64_t i = 0; i < m->len; ++i) {
            if (i) cb_cstr(b, ", ");
            cpy_repr(b, pa_dict_key_at(m, i));
            cb_cstr(b, ": ");
            cpy_repr(b, pa_dict_val_at(m, i));
        }
        cb_ch(b, '}');
        return;
    }
    cb_i64(b, v);
}

/* pa_print_any 对 CPYFLOAT/LIST/DICT 的分派出口。顶层 float 不带引号，
   容器走 repr。只 malloc C 缓冲，不分配 pyadamas 对象，无 GC 风险。 */
void pa_print_cpy_value(int64_t v) {
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (h->kind == PYADAMAS_RC_CPYFLOAT) {
        double d;
        memcpy(&d, (void*)(uintptr_t)v, sizeof d);
        pa_print_f64(d);
        return;
    }
    CpyBuf b = {NULL, 0, 0};
    cpy_repr(&b, v);
    fwrite(b.buf, 1, b.len, stdout);
    free(b.buf);
}

#endif /* _WIN32 */
