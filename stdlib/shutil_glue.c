/* stdlib/shutil_glue.c —— shutil 的 stat/utime/symlink C 胶水（stdlib2）。
 *
 * os 原生面没有 stat/utime/symlink/readlink；shutil 的 copymode/copystat/
 * copy2/move/rmtree/copytree 需要它们。按一批 gzip_glue.c 的口径：C 侧
 * 只做系统调用，错误归一化成负码返回，Python 侧映射异常类型。
 *
 * 归一化错误码（跨平台稳定，不暴露裸 errno）：
 *   -1 ENOENT  -2 EEXIST  -3 EACCES/EPERM  -4 ENOTDIR  -5 EISDIR
 *   -6 EXDEV   -7 ENOTEMPTY  -8 EINVAL  -9 其它
 */
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

static int64_t norm_err(int e) {
    switch (e) {
        case ENOENT: return -1;
        case EEXIST: return -2;
        case EACCES:
        case EPERM: return -3;
        case ENOTDIR: return -4;
        case EISDIR: return -5;
        case EXDEV: return -6;
        case ENOTEMPTY: return -7;
        case EINVAL: return -8;
        default: return -9;
    }
}

/* stat/lstat 信息写进 out[7]：
 *   [0] mode  [1] mtime_sec  [2] mtime_nsec  [3] atime_sec  [4] atime_nsec
 *   [5] size  [6] kind 位掩码（1=regular 2=dir 4=symlink）
 * follow=0 用 lstat。成功返回 0，失败返回归一化负码。 */
int64_t pa_shutil_stat(const char* path, int64_t follow, int64_t* out) {
    struct stat st;
    int rc = follow ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) return norm_err(errno);
    out[0] = (int64_t)st.st_mode;
    out[1] = (int64_t)st.st_mtim.tv_sec;
    out[2] = (int64_t)st.st_mtim.tv_nsec;
    out[3] = (int64_t)st.st_atim.tv_sec;
    out[4] = (int64_t)st.st_atim.tv_nsec;
    out[5] = (int64_t)st.st_size;
    int64_t kind = 0;
    if (S_ISREG(st.st_mode)) kind |= 1;
    if (S_ISDIR(st.st_mode)) kind |= 2;
    if (S_ISLNK(st.st_mode)) kind |= 4;
    out[6] = kind;
    return 0;
}

/* utimensat：atime/mtime 按 sec+nsec 写回。follow=0 不跟随符号链接。 */
int64_t pa_shutil_utime(const char* path, int64_t at_sec, int64_t at_nsec,
                        int64_t mt_sec, int64_t mt_nsec, int64_t follow) {
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)at_sec;
    ts[0].tv_nsec = (long)at_nsec;
    ts[1].tv_sec = (time_t)mt_sec;
    ts[1].tv_nsec = (long)mt_nsec;
    if (utimensat(AT_FDCWD, path, ts, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
        return norm_err(errno);
    return 0;
}

/* readlink：成功返回长度（buf 补 NUL）；缓冲区不够返回 -1；失败返回负码。 */
int64_t pa_shutil_readlink(const char* path, char* buf, int64_t cap) {
    ssize_t n = readlink(path, buf, (size_t)cap);
    if (n < 0) return norm_err(errno);
    if (n >= cap) return -1;
    buf[n] = 0;
    return (int64_t)n;
}

int64_t pa_shutil_symlink(const char* target, const char* linkpath) {
    if (symlink(target, linkpath) != 0) return norm_err(errno);
    return 0;
}

/* 错误码 → 消息文本（strerror 的静态缓冲，调用方立即拷走）。 */
const char* pa_shutil_strerror(int64_t code) {
    int e = EINVAL;
    switch (code) {
        case -1: e = ENOENT; break;
        case -2: e = EEXIST; break;
        case -3: e = EACCES; break;
        case -4: e = ENOTDIR; break;
        case -5: e = EISDIR; break;
        case -6: e = EXDEV; break;
        case -7: e = ENOTEMPTY; break;
        case -8: e = EINVAL; break;
        default: break;
    }
    return strerror(e);
}
