// driver/util.h —— 命令行/构建共用的文件与诊断辅助
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace pyadamas {

std::expected<std::string, std::string> read_file(const std::string& path);
void report(const std::string& path, const std::string& source, int line, int col,
            std::string_view msg);
std::string shell_quote(const std::string& s);
std::expected<std::string, std::string> make_temp(const char* suffix);

}  // namespace pyadamas
