// driver_link.h —— 直接调用 ld 链接的配置探测结果（见 driver_link.cpp）
#pragma once

#include <string>
#include <vector>

namespace pyadamas {

// 配置期探测成功且未被环境变量/额外 flag 否决时为 true。
bool direct_ld_available();
// 链接附加参数（cimport 的 link 声明）是否全是 ld 认识的产物：.c 源文件
// 只有 cc 驱动会现编，遇到就得分回退到 cc。
bool direct_ld_args_ok(const std::vector<std::string>& link_args);
// 汇编器命令行前缀："<as> --64 -c" 或回退的 "cc -c"。
std::string as_prefix();
// "ld <标志> -o 之前的部分"（含 crt 起始文件与 -L 目录）。
std::string direct_ld_prefix();
// "库与 crt 收尾"（-lgcc -lc -lgcc … crtendS.o crtn.o）。
std::string direct_ld_tail();

}  // namespace pyadamas
