// driver_link.cpp —— 直接调用 ld 链接（省掉 gcc 驱动的固定开销）
//
// 背景（0.311 取证）：一次 pyadamas build 的墙钟里，cc 汇编 ~17%、链接 ~70%。
// 链接的 36 ms 里 ld.bfd 本身约 25 ms，其余 ~11 ms 是 gcc 驱动（specs 解析 +
// 库搜索 + 再 exec collect2 + 再 exec ld）。这三层进程链对「每次编译都链一次
// 短命程序」的用法是纯开销：本文件把 gcc 的链接命令行在 **配置期** 复刻成
// 一份固定的 ld 前缀/后缀，运行期直接用 ld 链接，省掉驱动与 collect2。
//
// 约束：
//   * 生成的二进制必须与 cc 链接的逐字节相同（--build-id 由输入内容决定，
//     实测 same)；配置期探测失败（找不到 crt*.o / ld / 动态装载器）时
//     前缀为空，调用方自动回退到 cc，行为与之前一致；
//   * 带 sanitizer 等额外 flag（PYADAMAS_CC_FLAGS 非空）或设置了
//     PYADAMAS_NO_DIRECT_LD 时一律回退 cc —— 那些场景需要驱动挑 .so；
//   * 只在 ELF/GNU 目标下可用（Win64 交叉汇编不走链接）。
#include <cstdlib>
#include <string>

#include "driver_link.h"

#ifndef PYADAMAS_AS_EXECUTABLE
#define PYADAMAS_AS_EXECUTABLE ""
#endif
#ifndef PYADAMAS_LD_EXECUTABLE
#define PYADAMAS_LD_EXECUTABLE ""
#endif
#ifndef PYADAMAS_LD_FLAGS
#define PYADAMAS_LD_FLAGS ""
#endif
#ifndef PYADAMAS_LD_TAIL
#define PYADAMAS_LD_TAIL ""
#endif

namespace pyadamas {

bool direct_ld_available() {
    static const bool ok = [] {
        if (std::getenv("PYADAMAS_NO_DIRECT_LD")) return false;
        // 需要 gcc 驱动挑库时（sanitizer 等）不能绕过它。
        const char* extra = std::getenv("PYADAMAS_CC_FLAGS");
        if (extra && *extra) return false;
        if (PYADAMAS_LD_EXECUTABLE[0] == 0) return false;
        if (PYADAMAS_LD_FLAGS[0] == 0) return false;
        return true;
    }();
    return ok;
}

std::string direct_ld_prefix() { return PYADAMAS_LD_EXECUTABLE " " PYADAMAS_LD_FLAGS; }

// 汇编器前缀：配置期拿到 as 路径时直接 exec as（省一次 gcc 驱动 + 进程链），
// 否则用 "cc -c"（行为与之前一致）。
bool direct_ld_args_ok(const std::vector<std::string>& link_args) {
    // cimport/cdecl 的 link 声明可以直接给 .c 源文件（由 cc 驱动在链接时
    // 现编）；这些只有驱动认识，ld 会把 .c 当链接脚本。遇到这种就回退 cc。
    for (const std::string& a : link_args) {
        if (a.starts_with("-l") || a.starts_with("-L") || a.starts_with("-Wl,")) continue;
        if (a.ends_with(".o") || a.ends_with(".a") || a.ends_with(".so")) continue;
        return false;
    }
    return true;
}

std::string as_prefix() {
    if (PYADAMAS_AS_EXECUTABLE[0] == 0) return "cc -c";
    return std::string(PYADAMAS_AS_EXECUTABLE) + " --64 -c";
}

std::string direct_ld_tail() { return PYADAMAS_LD_TAIL; }

}  // namespace pyadamas
