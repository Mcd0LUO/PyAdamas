// cdecl.h —— C 头文件子集 → .cpya 声明文件
#pragma once

#include <string>
#include <vector>

namespace pyadamas {

// 把 header_path 里的 C 声明子集转换成 .cpya 文本。
// module 非空时作为 cmodule 名；links 原样写成带引号的 link 参数。
// include_dirs 是传给系统预处理器的 -I 搜索路径（为空只影响 #include 展开）。
// header 解析失败时返回空串并设置 err。
std::string cdecl_generate(const std::string& header_path, const std::string& module,
                           const std::vector<std::string>& links,
                           const std::vector<std::string>& include_dirs, std::string& err);

}  // namespace pyadamas
