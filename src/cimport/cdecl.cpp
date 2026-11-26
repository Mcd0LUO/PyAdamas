// cimport/cdecl.cpp —— C 头文件子集 → .cpya（驱动与词法）
#include "cimport/cdecl.h"

#include <sys/wait.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cimport/cdecl_internal.h"

namespace pyadamas {
namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// shell 单引号转义：头文件路径/宏里出现引号也不会破坏命令行。
std::string sh_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
}

bool run_capture(const std::string& cmd, std::string& out) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = pclose(p);
    return st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// 用系统 C 预处理器展开 #include 与函数式宏。真实头文件（zlib.h、sqlite3.h）
// 的导出宏/条件编译只有预处理器能正确展开；失败（没有 cc）时返回 false，
// 调用方回退到内置的极简预处理。
bool cpp_expand(const std::string& header_path, const std::vector<std::string>& inc,
                std::string& out) {
    const char* env = std::getenv("PYADAMAS_CDECL_CPP");
    std::string cc = (env && *env) ? env : "cc";
    std::string cmd = sh_quote(cc) + " -E -P -x c";
    for (const auto& d : inc) cmd += " -I" + sh_quote(d);
    cmd += " " + sh_quote(header_path) + " 2>/dev/null";
    return run_capture(cmd, out);
}

// -P 只去掉行标记，#pragma 仍在。它们没被剥离时，词法会把
// `#pragma ... typedef unsigned z_crc_t;` 当成一条语句，typedef 就丢了。
void strip_directive_lines(std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        size_t b = i;
        while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
        if (b < e && s[b] == '#') out.push_back('\n');
        else {
            out.append(s, i, e - i);
            out.push_back('\n');
        }
        i = e + 1;
    }
    s = std::move(out);
}

// 删掉 GCC 扩展：__attribute__((...))、__extension__、__asm__(...) 等。
// 真实头文件里到处都有；不删的话属性里的 '(' 会被当成函数参数表。
void strip_gcc_extensions(std::string& s) {
    auto skip_balanced = [&s](size_t p, char open, char close) {
        int d = 0;
        for (; p < s.size(); ++p) {
            if (s[p] == open) ++d;
            else if (s[p] == close && --d == 0) return p + 1;
        }
        return s.size();
    };
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 13, "__attribute__") == 0) {
            size_t p = i + 13;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            if (p < s.size() && s[p] == '(') i = skip_balanced(p, '(', ')');
            else i = p;
            out.push_back(' ');
            continue;
        }
        if (s.compare(i, 13, "__extension__") == 0) {
            i += 13;
            out.push_back(' ');
            continue;
        }
        // asm 标签：`extern int x __asm__("name");`。只匹配独立标识符。
        size_t asm_len = 0;
        if (s.compare(i, 7, "__asm__") == 0) asm_len = 7;
        else if (s.compare(i, 5, "__asm") == 0) asm_len = 5;
        else if (s.compare(i, 3, "asm") == 0) asm_len = 3;
        if (asm_len) {
            auto ident = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            };
            size_t after = i + asm_len;
            bool left_ok = i == 0 || !ident(s[i - 1]);
            bool right_ok = after >= s.size() || !ident(s[after]);
            size_t p = after;
            while (right_ok && p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            if (left_ok && right_ok && p < s.size() && s[p] == '(') {
                i = skip_balanced(p, '(', ')');
                out.push_back(' ');
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    s = std::move(out);
}

}  // namespace

std::string cdecl_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}


std::string cdecl_preprocess(const std::string& src, std::vector<MacroDef>& defs) {
    std::string clean;
    bool in_block = false;
    for (size_t i = 0; i < src.size();) {
        if (in_block) {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                in_block = false;
                i += 2;
            } else {
                if (src[i] == '\n') clean.push_back('\n');
                ++i;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            in_block = true;
            i += 2;
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        clean.push_back(src[i++]);
    }

    std::string code;
    std::istringstream is(clean);
    std::string line;
    int ln = 0;
    while (std::getline(is, line)) {
        ++ln;
        std::string t = cdecl_trim(line);
        if (!t.empty() && t[0] == '#') {
            std::string rest = cdecl_trim(t.substr(1));
            if (rest.starts_with("define")) {
                std::string body = rest.substr(6);
                if (!body.empty() && std::isspace(static_cast<unsigned char>(body[0]))) {
                    body = cdecl_trim(body);
                    size_t p = 0;
                    while (p < body.size() &&
                           (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_'))
                        ++p;
                    std::string name = body.substr(0, p);
                    // 函数式宏（NAME( 之间没有空格）不展开。
                    if (!name.empty() && p < body.size() && body[p] == '(') continue;
                    if (!name.empty()) defs.push_back({name, cdecl_trim(body.substr(p)), ln});
                }
            }
            continue;
        }
        code += line;
        code += '\n';
    }
    return code;
}

std::vector<Tok> cdecl_lex(const std::string& code) {
    std::vector<Tok> out;
    int line = 1;
    for (size_t i = 0; i < code.size();) {
        char c = code[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (is_ident_start(c)) {
            size_t b = i;
            while (i < code.size() && is_ident_char(code[i])) ++i;
            out.push_back({Tok::Id, code.substr(b, i - b), line});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < code.size() &&
             std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
            size_t b = i;
            while (i < code.size()) {
                char d = code[i];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '.' || d == '_') {
                    ++i;
                    continue;
                }
                if ((d == '+' || d == '-') && i > b &&
                    (code[i - 1] == 'e' || code[i - 1] == 'E') &&
                    i + 1 < code.size() &&
                    std::isdigit(static_cast<unsigned char>(code[i + 1]))) {
                    ++i;
                    continue;
                }
                break;
            }
            out.push_back({Tok::Num, code.substr(b, i - b), line});
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            size_t b = i++;
            bool esc = false;
            std::string inner;
            while (i < code.size()) {
                char d = code[i++];
                if (esc) {
                    inner.push_back('\\');
                    inner.push_back(d);
                    esc = false;
                    continue;
                }
                if (d == '\\') {
                    esc = true;
                    continue;
                }
                if (d == quote) break;
                inner.push_back(d);
            }
            if (quote == '\'') {
                // 字符常量：只支持单字节（含常见转义）。
                int64_t v = 0;
                if (inner.size() == 1) v = static_cast<unsigned char>(inner[0]);
                else if (inner.size() == 2 && inner[0] == '\\') {
                    switch (inner[1]) {
                        case 'n': v = '\n'; break;
                        case 't': v = '\t'; break;
                        case 'r': v = '\r'; break;
                        case '0': v = 0; break;
                        case '\\': v = '\\'; break;
                        case '\'': v = '\''; break;
                        default: v = static_cast<unsigned char>(inner[1]); break;
                    }
                }
                out.push_back({Tok::Num, std::to_string(v), line});
            } else {
                out.push_back({Tok::Str, inner, line});
            }
            (void)b;
            continue;
        }
        // 多字符运算符只保留枚举/常量表达式需要的几种。
        if (i + 1 < code.size()) {
            std::string two = code.substr(i, 2);
            if (two == "<<" || two == ">>" || two == "<=" || two == ">=" || two == "==" ||
                two == "!=" || two == "&&" || two == "||" || two == "->") {
                out.push_back({Tok::Punc, two, line});
                i += 2;
                continue;
            }
        }
        if (i + 2 < code.size() && code.substr(i, 3) == "...") {
            out.push_back({Tok::Punc, "...", line});
            i += 3;
            continue;
        }
        out.push_back({Tok::Punc, std::string(1, c), line});
        ++i;
    }
    return out;
}

std::string cdecl_generate(const std::string& header_path, const std::string& module,
                           const std::vector<std::string>& links,
                           const std::vector<std::string>& include_dirs, std::string& err) {
    // 0.561：头文件路径**找不到就搜**。`sys/socket.h` 在 Debian/Ubuntu 上位于
    // /usr/include/x86_64-linux-gnu/ 下，让用户自己拼绝对路径不合理。
    std::string resolved = header_path;
    {
        std::error_code ec;
        auto is_file = [&](const std::string& p) {
            return std::filesystem::is_regular_file(p, ec);
        };
        if (!is_file(resolved) && !std::filesystem::path(resolved).is_absolute()) {
            std::vector<std::string> roots = include_dirs;
            roots.push_back("/usr/include");
            roots.push_back("/usr/local/include");
            // 多架构目录：/usr/include/<triple>
            for (auto& d : std::filesystem::directory_iterator("/usr/include", ec)) {
                std::string name = d.path().filename().string();
                if (name.find("-linux-gnu") != std::string::npos ||
                    name.find("-linux-musl") != std::string::npos)
                    roots.push_back(d.path().string());
            }
            for (auto& r : roots) {
                std::string cand = std::filesystem::path(r) / resolved;
                if (is_file(cand)) {
                    resolved = cand;
                    break;
                }
            }
        }
    }
    std::ifstream in(resolved);
    if (!in) {
        err = std::format("cannot open header '{}'", header_path);
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();

    // 内置预处理只负责收集对象式宏常量（#define X 42）；代码本体优先交给
    // 系统预处理器，才能展开 #include 和 ZEXTERN/SQLITE_API 这类导出宏。
    std::vector<MacroDef> defs;
    std::string internal = cdecl_preprocess(src, defs);
    std::string code;
    if (!cpp_expand(resolved, include_dirs, code) || code.empty()) {
        code = std::move(internal);
    } else {
        strip_directive_lines(code);
        strip_gcc_extensions(code);
    }
    std::vector<Tok> toks = cdecl_lex(code);
    return cdecl_process(toks, defs, resolved, module, links, err);
}

}  // namespace pyadamas
