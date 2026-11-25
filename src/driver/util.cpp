// driver/util.cpp —— 文件读取、错误片段与临时文件
#include "driver/util.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace pyadamas {

namespace {

void print_snippet(const std::string& source, int line, int col) {
    if (line <= 0) return;
    size_t cur = 1;
    size_t begin = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (cur == static_cast<size_t>(line)) break;
        if (source[i] == '\n') {
            ++cur;
            begin = i + 1;
        }
    }
    if (cur != static_cast<size_t>(line)) return;
    size_t end = source.find('\n', begin);
    if (end == std::string::npos) end = source.size();
    if (end <= begin) return;

    std::string_view text(source.data() + begin, end - begin);
    constexpr size_t kMaxCols = 120;
    size_t caret = (col > 0) ? static_cast<size_t>(col - 1) : 0;
    size_t start = 0;
    if (text.size() > kMaxCols) {
        if (caret > kMaxCols / 3) start = caret - kMaxCols / 3;
        if (start + kMaxCols > text.size()) start = text.size() - kMaxCols;
        if (caret < start) caret = start;
    }
    size_t len = std::min(kMaxCols, text.size() - start);

    // 展开 tab，否则插入符对不齐
    std::string shown;
    size_t caret_pad = 0;
    for (size_t i = start; i < start + len; ++i) {
        if (i == caret) caret_pad = shown.size();
        char c = text[i];
        if (c == '\t') {
            size_t n = 4 - shown.size() % 4;
            shown.append(n, ' ');
        } else {
            shown.push_back(c);
        }
    }
    if (caret >= start + len) caret_pad = shown.size();

    const char* ellipsis = start ? "..." : "";
    std::fprintf(stderr, "  %5d | %s%.*s\n", line, ellipsis, static_cast<int>(shown.size()),
                 shown.data());
    std::string pad(std::strlen(ellipsis) + caret_pad, ' ');
    std::fprintf(stderr, "        | %s^\n", pad.c_str());
}

}  // namespace

std::expected<std::string, std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot open '{}'", path));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::unexpected(std::format("failed while reading '{}'", path));
    return ss.str();
}

void report(const std::string& path, const std::string& source, int line, int col,
            std::string_view msg) {
    std::fprintf(stderr, "%s:%d:%d: error: %.*s\n", path.c_str(), line, col,
                 static_cast<int>(msg.size()), msg.data());
    print_snippet(source, line, col);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::expected<std::string, std::string> make_temp(const char* suffix) {
    std::string tmpl = std::format("/tmp/pyadamas-XXXXXX{}", suffix);
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), static_cast<int>(std::strlen(suffix)));
    if (fd < 0) return std::unexpected("cannot create temporary file");
    ::close(fd);
    return std::string(buf.data());
}


// 运行期定位 stdlib 目录（发布版二进制要能在别的机器上跑）：
//   1. 环境变量 PYADAMAS_STDLIB（显式覆盖）
//   2. 编译期路径 PYADAMAS_STDLIB_DIR（开发机构建时就是仓库里的 stdlib/）
//   3. 可执行文件旁边：<exe>/stdlib、<exe>/../stdlib、<exe>/../share/pyadamas/stdlib
// 都找不到时返回编译期值（后续读模块会明确报错，不会静默用空表）。
const std::string& stdlib_dir() {
    static const std::string dir = [] {
        std::error_code ec;
        if (const char* env = std::getenv("PYADAMAS_STDLIB"); env && *env) {
            if (std::filesystem::is_directory(env, ec)) return std::string(env);
        }
        const std::string compiled = PYADAMAS_STDLIB_DIR;
        if (std::filesystem::is_directory(compiled, ec)) return compiled;
#if defined(__linux__)
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            std::filesystem::path exe_dir = std::filesystem::path(buf).parent_path();
            for (auto cand : {exe_dir / "stdlib", exe_dir / ".." / "stdlib",
                              exe_dir / ".." / "share" / "pyadamas" / "stdlib"}) {
                if (std::filesystem::is_directory(cand, ec))
                    return std::filesystem::weakly_canonical(cand, ec).string();
            }
        }
#endif
        return compiled;
    }();
    return dir;
}

}  // namespace pyadamas
