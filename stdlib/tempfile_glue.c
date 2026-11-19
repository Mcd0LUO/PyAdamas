/* stdlib/tempfile_glue.c —— tempfile 的独占创建原语（stdlib2）。
 *
 * mkstemp 语义：O_CREAT|O_EXCL|O_RDWR、0600，成功返回 fd（调用方拥有），
 * 失败返回归一化负码（与 shutil_glue.c 同口径）。Python 侧循环重试
 * EEXIST 并组装随机名（同 CPython _get_candidate_names 口径）。
 */
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

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

int64_t pa_tempfile_open_excl(const char* path) {
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return norm_err(errno);
    return (int64_t)fd;
}

const char* pa_tempfile_strerror(int64_t code) {
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
