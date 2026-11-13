// cdecl.cpp —— C 头文件子集 → .cpya（驱动与词法）
#include "cdecl.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cdecl_internal.h"

namespace pyadamas {
namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
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
                           const std::vector<std::string>& links, std::string& err) {
    std::ifstream in(header_path);
    if (!in) {
        err = std::format("cannot open header '{}'", header_path);
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();

    std::vector<MacroDef> defs;
    std::string code = cdecl_preprocess(src, defs);
    std::vector<Tok> toks = cdecl_lex(code);
    return cdecl_process(toks, defs, header_path, module, links, err);
}

}  // namespace pyadamas
