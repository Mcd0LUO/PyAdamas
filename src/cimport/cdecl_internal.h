// cimport/cdecl_internal.h —— cdecl 驱动与解析器之间共享的内部接口
#pragma once

#include <string>
#include <vector>

namespace pyadamas {

struct MacroDef {
    std::string name;
    std::string value;
    int line = 0;
};

struct Tok {
    enum K { Id, Num, Str, Punc } k = Punc;
    std::string s;
    int line = 0;
};

std::string cdecl_trim(const std::string& s);
std::string cdecl_preprocess(const std::string& src, std::vector<MacroDef>& defs);
std::vector<Tok> cdecl_lex(const std::string& code);
// 解析 token 流并生成 .cpya 文本；defs 是预处理器收集的常量。
std::string cdecl_process(const std::vector<Tok>& toks, const std::vector<MacroDef>& defs,
                          const std::string& header_path, const std::string& module,
                          const std::vector<std::string>& links, std::string& err);

}  // namespace pyadamas
