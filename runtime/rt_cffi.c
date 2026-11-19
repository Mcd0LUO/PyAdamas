/* rt_cffi.c —— 免写直调与动态库句柄的 dlopen/dlsym 后端
 *
 * 两条入口共享同一套符号解析：
 *   - from c import X      → pa_c_sym_default（RTLD_DEFAULT，进程全局符号表）
 *   - dylib.load(p).sym(...) → pa_dylib_load + pa_c_sym_handle（指定句柄）
 *
 * 契约（ctypes 式）：没有声明文件，调用方对 ABI 负责。这里只负责把符号
 * 地址找出来；找不到时抛 OSError，消息带符号名/库路径，绝不静默返回 0
 * 让调用点跳进空指针。
 */
#include "rt_internal.h"

#ifndef _WIN32
#include <dlfcn.h>

int64_t pa_dylib_load(PyAdamasStr* path) {
    void* h = dlopen(path->data, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        char buf[1024];
        snprintf(buf, sizeof buf, "dylib.load: cannot open '%s': %s",
                 path->data, e ? e : "unknown error");
        pa_raise_c("OSError", buf);
    }
    return (int64_t)(uintptr_t)h;
}

int64_t pa_dylib_close(int64_t handle) {
    return (int64_t)dlclose((void*)(uintptr_t)handle);
}

int64_t pa_c_sym_default(const char* name) {
    dlerror();  /* 清掉旧错误，dlsym 的 NULL 要用 dlerror 区分 */
    void* p = dlsym(RTLD_DEFAULT, name);
    if (dlerror()) {
        char buf[512];
        snprintf(buf, sizeof buf, "undefined C symbol '%s' (dlsym RTLD_DEFAULT)", name);
        pa_raise_c("OSError", buf);
    }
    return (int64_t)(uintptr_t)p;
}

int64_t pa_c_sym_handle(int64_t handle, const char* name) {
    dlerror();
    void* p = dlsym((void*)(uintptr_t)handle, name);
    if (dlerror()) {
        char buf[512];
        snprintf(buf, sizeof buf, "undefined C symbol '%s' in dylib handle", name);
        pa_raise_c("OSError", buf);
    }
    return (int64_t)(uintptr_t)p;
}

#else /* _WIN32：dlfcn 不存在，先给明确诊断（Win64 移植另行设计） */

int64_t pa_dylib_load(PyAdamasStr* path) {
    (void)path;
    pa_raise_c("NotImplementedError", "dylib.load requires POSIX dlopen");
}
int64_t pa_dylib_close(int64_t handle) {
    (void)handle;
    pa_raise_c("NotImplementedError", "dylib.close requires POSIX dlclose");
}
int64_t pa_c_sym_default(const char* name) {
    (void)name;
    pa_raise_c("NotImplementedError", "from c import requires POSIX dlsym");
}
int64_t pa_c_sym_handle(int64_t handle, const char* name) {
    (void)handle; (void)name;
    pa_raise_c("NotImplementedError", "dylib symbol lookup requires POSIX dlsym");
}
#endif
