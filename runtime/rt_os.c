// rt_os.c —— os 模块：目录与文件系统操作
#include "rt_internal.h"

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
