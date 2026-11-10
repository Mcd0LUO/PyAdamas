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
    py23_raise_c(type, buf);
}

void py23_os_mkdir(Py23Str* path) {
    if (mkdir(path->data, 0777) != 0) os_raise("mkdir", errno);
}

void py23_os_rmdir(Py23Str* path) {
    if (rmdir(path->data) != 0) os_raise("rmdir", errno);
}

void py23_os_remove(Py23Str* path) {
    if (unlink(path->data) != 0) os_raise("remove", errno);
}

void py23_os_rename(Py23Str* from, Py23Str* to) {
    if (rename(from->data, to->data) != 0) os_raise("rename", errno);
}

Py23Str* py23_os_getcwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) os_raise("getcwd", errno);
    return str_new(buf, (int64_t)strlen(buf));
}

int64_t py23_os_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
}

/* os.path.join 的 posix 语义：后一段以 '/' 开头就整体替换，
   否则按需补一个分隔符；空段照样补分隔符（join("a", "") == "a/"）。 */
Py23Str* py23_os_path_join(Py23List* parts) {
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) panic_c("out of memory");
    out[0] = '\0';
    for (int64_t i = 0; i < parts->len; ++i) {
        Py23Str* p = (Py23Str*)parts->data[i];
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
    Py23Str* res = str_new(out, (int64_t)len);
    free(out);
    return res;
}

int64_t py23_os_path_exists(Py23Str* path) {
    return access(path->data, F_OK) == 0 ? 1 : 0;
}

int64_t py23_os_path_isdir(Py23Str* path) {
    struct stat st;
    return stat(path->data, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
}

int64_t py23_os_path_isfile(Py23Str* path) {
    struct stat st;
    return stat(path->data, &st) == 0 && S_ISREG(st.st_mode) ? 1 : 0;
}

/* ── os 扩展：目录枚举、环境变量 ─────────────────────────────── */

Py23List* py23_os_listdir(Py23Str* path) {
    DIR* d = opendir(path->data);
    if (!d) os_raise("listdir", errno);
    Py23List* out = py23_list_new(0, 1);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        Py23Str* s = str_new(e->d_name, (int64_t)strlen(e->d_name));
        py23_list_append(out, (int64_t)s);
    }
    closedir(d);
    return out;
}

Py23Str* py23_os_getenv(Py23Str* name, Py23Str* def) {
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

Py23Str* py23_os_path_basename(Py23Str* s) {
    int64_t i = last_slash(s->data, s->len);
    return str_new(s->data + i + 1, s->len - i - 1);
}

Py23Str* py23_os_path_dirname(Py23Str* s) {
    int64_t i = last_slash(s->data, s->len);
    if (i < 0) return str_new("", 0);
    int64_t end = i + 1;
    while (end > 1 && s->data[end - 1] == '/') --end;
    return str_new(s->data, end);
}

Py23List* py23_os_path_splitext(Py23Str* s) {
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
    Py23List* out = py23_list_new(2, 1);
    Py23Str* root = str_new(s->data, cut);
    Py23Str* ext = str_new(s->data + cut, s->len - cut);
    py23_list_append(out, (int64_t)root);
    py23_list_append(out, (int64_t)ext);
    return out;
}

typedef struct {
    const char* p;
    int64_t n;
} PathComp;

Py23Str* py23_os_path_normpath(Py23Str* s) {
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
    Py23Str* out = str_alloc(total);
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

Py23Str* py23_os_path_abspath(Py23Str* s) {
    if (s->len > 0 && s->data[0] == '/') return py23_os_path_normpath(s);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) os_raise("getcwd", errno);
    size_t cl = strlen(cwd);
    char* joined = (char*)malloc(cl + 1 + (size_t)s->len + 1);
    if (!joined) panic_c("out of memory");
    memcpy(joined, cwd, cl);
    joined[cl] = '/';
    memcpy(joined + cl + 1, s->data, (size_t)s->len);
    joined[cl + 1 + (size_t)s->len] = 0;
    Py23Str* tmp = str_new(joined, (int64_t)(cl + 1 + (size_t)s->len));
    free(joined);
    Py23Str* out = py23_os_path_normpath(tmp);
    py23_rc_dec(tmp);
    return out;
}

Py23Str* py23_os_path_expanduser(Py23Str* s) {
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
    Py23Str* out = str_new(joined, (int64_t)(hl + (size_t)(s->len - 1)));
    free(joined);
    return out;
}
