// rt_os.c —— os 模块：目录与文件系统操作
#include "rt_internal.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void os_raise(const char* what, int err) {
    const char* type = "OSError";
    switch (err) {
        case ENOENT: type = "FileNotFoundError"; break;
        case EEXIST: type = "FileExistsError"; break;
        case EACCES:
        case EPERM: type = "PermissionError"; break;
        case ENOTDIR: type = "NotADirectoryError"; break;
        case EISDIR: type = "IsADirectoryError"; break;
        default: break;
    }
    char buf[512];
    snprintf(buf, sizeof buf, "%s: %s", what, strerror(err));
    pa_raise_c(type, buf);
}

void pa_os_mkdir(PyAdamasStr* path) {
    if (mkdir(path->data, 0777) != 0) os_raise("mkdir", errno);
}

void pa_os_rmdir(PyAdamasStr* path) {
    if (rmdir(path->data) != 0) os_raise("rmdir", errno);
}

void pa_os_remove(PyAdamasStr* path) {
    if (unlink(path->data) != 0) os_raise("remove", errno);
}

void pa_os_rename(PyAdamasStr* from, PyAdamasStr* to) {
    if (rename(from->data, to->data) != 0) os_raise("rename", errno);
}

PyAdamasStr* pa_os_getcwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) os_raise("getcwd", errno);
    return str_new(buf, (int64_t)strlen(buf));
}

int64_t pa_os_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
}

/* os.path.join 的 posix 语义：后一段以 '/' 开头就整体替换，
   否则按需补一个分隔符；空段照样补分隔符（join("a", "") == "a/"）。 */
PyAdamasStr* pa_os_path_join(PyAdamasList* parts) {
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) panic_c("out of memory");
    out[0] = '\0';
    for (int64_t i = 0; i < parts->len; ++i) {
        PyAdamasStr* p = (PyAdamasStr*)parts->data[i];
        int64_t n = p->len;
        size_t need = len + (size_t)n + 2;
        if (need > cap) {
            while (need > cap) cap *= 2;
            char* nb = (char*)realloc(out, cap);
            if (!nb) panic_c("out of memory");
            out = nb;
        }
        if (n > 0 && p->data[0] == '/') {
            memcpy(out, p->data, (size_t)n);
            len = (size_t)n;
        } else if (len == 0 || out[len - 1] == '/') {
            memcpy(out + len, p->data, (size_t)n);
            len += (size_t)n;
        } else {
            out[len++] = '/';
            memcpy(out + len, p->data, (size_t)n);
            len += (size_t)n;
        }
        out[len] = '\0';
    }
    PyAdamasStr* res = str_new(out, (int64_t)len);
    free(out);
    return res;
}

int64_t pa_os_path_exists(PyAdamasStr* path) {
    return access(path->data, F_OK) == 0 ? 1 : 0;
}

int64_t pa_os_path_isdir(PyAdamasStr* path) {
    struct stat st;
    return stat(path->data, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
}

int64_t pa_os_path_isfile(PyAdamasStr* path) {
    struct stat st;
    return stat(path->data, &st) == 0 && S_ISREG(st.st_mode) ? 1 : 0;
}

/* ── os 扩展：目录枚举、环境变量 ─────────────────────────────── */

PyAdamasList* pa_os_listdir(PyAdamasStr* path) {
    DIR* d = opendir(path->data);
    if (!d) os_raise("listdir", errno);
    PyAdamasList* out = pa_list_new(0, 1);
    /* 目录项多时循环里的 str_new 必然会撞上主收集；out 只在 C 局部变量里。 */
    pa_gc_pin(out);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        PyAdamasStr* s = str_new(e->d_name, (int64_t)strlen(e->d_name));
        pa_list_append(out, (int64_t)s);
    }
    closedir(d);
    pa_gc_unpin(out);
    return out;
}

PyAdamasStr* pa_os_getenv(PyAdamasStr* name, PyAdamasStr* def) {
    const char* v = getenv(name->data);
    if (!v) return str_new(def->data, def->len);
    return str_new(v, (int64_t)strlen(v));
}

/* ── os.path 文本处理 ────────────────────────────────────────── */

static int64_t last_slash(const char* p, int64_t n) {
    for (int64_t i = n - 1; i >= 0; --i)
        if (p[i] == '/') return i;
    return -1;
}

PyAdamasStr* pa_os_path_basename(PyAdamasStr* s) {
    int64_t i = last_slash(s->data, s->len);
    return str_new(s->data + i + 1, s->len - i - 1);
}

PyAdamasStr* pa_os_path_dirname(PyAdamasStr* s) {
    int64_t i = last_slash(s->data, s->len);
    if (i < 0) return str_new("", 0);
    int64_t end = i + 1;
    while (end > 1 && s->data[end - 1] == '/') --end;
    return str_new(s->data, end);
}

PyAdamasList* pa_os_path_splitext(PyAdamasStr* s) {
    int64_t sep = last_slash(s->data, s->len);
    int64_t dot = -1;
    for (int64_t i = s->len - 1; i > sep; --i)
        if (s->data[i] == '.') {
            dot = i;
            break;
        }
    int64_t cut = s->len;
    if (dot > sep) {
        // basename 里点号前面必须还有非点字符，".bashrc" 不算扩展名
        int64_t k = sep + 1;
        while (k < dot && s->data[k] == '.') ++k;
        if (k < dot) cut = dot;
    }
    PyAdamasList* out = pa_list_new(2, 1);
    /* 三次分配：out、root、ext 依次是「只在本帧 C 局部变量里」的对象，
       后一次分配触发主收集时前一个必须已经在根上。 */
    pa_gc_pin(out);
    PyAdamasStr* root = str_new(s->data, cut);
    pa_gc_pin(root);
    PyAdamasStr* ext = str_new(s->data + cut, s->len - cut);
    pa_gc_pin(ext);
    pa_list_append(out, (int64_t)root);
    pa_list_append(out, (int64_t)ext);
    pa_gc_unpin(ext);
    pa_gc_unpin(root);
    pa_gc_unpin(out);
    return out;
}

typedef struct {
    const char* p;
    int64_t n;
} PathComp;

PyAdamasStr* pa_os_path_normpath(PyAdamasStr* s) {
    const char* p = s->data;
    int64_t n = s->len;
    int64_t initial_slashes = 0;
    if (n > 0 && p[0] == '/') {
        initial_slashes = 1;
        if (n > 1 && p[1] == '/' && (n < 3 || p[2] != '/')) initial_slashes = 2;
    }
    int64_t cap = 16, cnt = 0;
    PathComp* comps = (PathComp*)malloc(sizeof(PathComp) * (size_t)cap);
    if (!comps) panic_c("out of memory");
    int64_t i = 0;
    while (i < n) {
        while (i < n && p[i] == '/') ++i;
        int64_t start = i;
        while (i < n && p[i] != '/') ++i;
        int64_t len = i - start;
        if (len == 0 || (len == 1 && p[start] == '.')) continue;
        int is_dotdot = len == 2 && p[start] == '.' && p[start + 1] == '.';
        if (is_dotdot) {
            int top_is_dotdot = cnt > 0 && comps[cnt - 1].n == 2 &&
                                comps[cnt - 1].p[0] == '.' && comps[cnt - 1].p[1] == '.';
            if (cnt > 0 && !top_is_dotdot) {
                --cnt;
                continue;
            }
            if (!initial_slashes || top_is_dotdot) {
                if (cnt == cap) {
                    cap *= 2;
                    comps = (PathComp*)realloc(comps, sizeof(PathComp) * (size_t)cap);
                    if (!comps) panic_c("out of memory");
                }
                comps[cnt].p = p + start;
                comps[cnt].n = len;
                ++cnt;
            }
            continue;
        }
        if (cnt == cap) {
            cap *= 2;
            comps = (PathComp*)realloc(comps, sizeof(PathComp) * (size_t)cap);
            if (!comps) panic_c("out of memory");
        }
        comps[cnt].p = p + start;
        comps[cnt].n = len;
        ++cnt;
    }
    int64_t total = initial_slashes;
    for (int64_t k = 0; k < cnt; ++k) total += comps[k].n + (k ? 1 : 0);
    if (total == 0) {
        free(comps);
        if (initial_slashes) return str_new("/", initial_slashes);
        return str_new(".", 1);
    }
    PyAdamasStr* out = str_alloc(total);
    char* q = (char*)out->data;
    int64_t pos = 0;
    for (int64_t k = 0; k < initial_slashes; ++k) q[pos++] = '/';
    for (int64_t k = 0; k < cnt; ++k) {
        if (k) q[pos++] = '/';
        memcpy(q + pos, comps[k].p, (size_t)comps[k].n);
        pos += comps[k].n;
    }
    free(comps);
    return out;
}

PyAdamasStr* pa_os_path_abspath(PyAdamasStr* s) {
    if (s->len > 0 && s->data[0] == '/') return pa_os_path_normpath(s);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) os_raise("getcwd", errno);
    size_t cl = strlen(cwd);
    char* joined = (char*)malloc(cl + 1 + (size_t)s->len + 1);
    if (!joined) panic_c("out of memory");
    memcpy(joined, cwd, cl);
    joined[cl] = '/';
    memcpy(joined + cl + 1, s->data, (size_t)s->len);
    joined[cl + 1 + (size_t)s->len] = 0;
    PyAdamasStr* tmp = str_new(joined, (int64_t)(cl + 1 + (size_t)s->len));
    free(joined);
    /* normpath 内部还要分配；tmp 只在本帧 C 局部变量里。 */
    pa_gc_pin(tmp);
    PyAdamasStr* out = pa_os_path_normpath(tmp);
    pa_gc_unpin(tmp);
    pa_rc_dec(tmp);
    return out;
}

PyAdamasStr* pa_os_path_expanduser(PyAdamasStr* s) {
    if (s->len == 0 || s->data[0] != '~') return str_new(s->data, s->len);
    int64_t i = 1;
    while (i < s->len && s->data[i] != '/') ++i;
    if (i != 1) return str_new(s->data, s->len);  // ~user 暂不支持
    const char* home = getenv("HOME");
    if (!home) return str_new(s->data, s->len);
    size_t hl = strlen(home);
    char* joined = (char*)malloc(hl + (size_t)(s->len - 1) + 1);
    if (!joined) panic_c("out of memory");
    memcpy(joined, home, hl);
    memcpy(joined + hl, s->data + 1, (size_t)(s->len - 1));
    joined[hl + (size_t)(s->len - 1)] = 0;
    PyAdamasStr* out = str_new(joined, (int64_t)(hl + (size_t)(s->len - 1)));
    free(joined);
    return out;
}

/* ── 补充的常用项 ───────────────────────────────────────────── */

int64_t pa_os_getpid(void) { return (int64_t)getpid(); }

void pa_os_chdir(PyAdamasStr* path) {
    if (chdir(path->data) != 0) os_raise("chdir", errno);
}

void pa_os_chmod(PyAdamasStr* path, int64_t mode) {
    if (chmod(path->data, (mode_t)mode) != 0) os_raise("chmod", errno);
}

int64_t pa_os_access(PyAdamasStr* path, int64_t mode) {
    return access(path->data, (int)mode) == 0 ? 1 : 0;
}

/* makedirs：逐级创建。中间层已存在直接跳过；最后一级已存在时，只有
   exist_ok 为真才不报错（和 CPython 一致）。 */
void pa_os_makedirs(PyAdamasStr* path, int64_t exist_ok) {
    int64_t n = path->len;
    if (n == 0) pa_raise_c("FileNotFoundError", "No such file or directory");
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) panic_c("out of memory");
    memcpy(buf, path->data, (size_t)n);
    buf[n] = '\0';
    for (int64_t i = 1; i <= n; ++i) {
        if (i < n && buf[i] != '/') continue;
        char save = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0777) != 0) {
            int e = errno;
            if (e != EEXIST || (i == n && !exist_ok)) {
                buf[i] = save;
                free(buf);
                os_raise("makedirs", e);
            }
        }
        buf[i] = save;
    }
    free(buf);
}

PyAdamasStr* pa_os_path_realpath(PyAdamasStr* s) {
    char* r = realpath(s->data, NULL);
    if (!r) return str_new(s->data, s->len);
    PyAdamasStr* out = str_new(r, (int64_t)strlen(r));
    free(r);
    return out;
}

int64_t pa_os_path_getsize(PyAdamasStr* s) {
    struct stat st;
    if (stat(s->data, &st) != 0) os_raise("getsize", errno);
    return (int64_t)st.st_size;
}

double pa_os_path_getmtime(PyAdamasStr* s) {
    struct stat st;
    if (stat(s->data, &st) != 0) os_raise("getmtime", errno);
    return (double)st.st_mtime;
}

/* os.path.split：拆成 (head, tail)，规则与 posixpath 一致。 */
PyAdamasList* pa_os_path_split(PyAdamasStr* s) {
    int64_t n = s->len;
    int64_t i = n;
    while (i > 0 && s->data[i - 1] == '/') --i;
    while (i > 0 && s->data[i - 1] != '/') --i;
    int64_t head_len = i;
    while (head_len > 1 && s->data[head_len - 1] == '/') --head_len;
    PyAdamasStr* head = str_new(s->data, head_len);
    pa_gc_pin(head);
    PyAdamasStr* tail = str_new(s->data + i, n - i);
    pa_gc_pin(tail);
    PyAdamasList* out = pa_list_new(2, 1);
    pa_gc_pin(out);
    pa_rc_inc(head);
    pa_list_append(out, (int64_t)head);
    pa_rc_inc(tail);
    pa_list_append(out, (int64_t)tail);
    pa_gc_unpin(out);
    pa_gc_unpin(tail);
    pa_gc_unpin(head);
    return out;
}

int64_t pa_os_path_samefile(PyAdamasStr* a, PyAdamasStr* b) {
    struct stat sa, sb;
    if (stat(a->data, &sa) != 0 || stat(b->data, &sb) != 0) return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
