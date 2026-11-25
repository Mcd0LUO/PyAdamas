// 0.555：程序级按需开关（type_tags / lang_gaps）的探测。从 build.cpp 拆出来，
#include "driver/build.h"
#include "driver/util.h"

namespace pyadamas {

// 0.528：按需开启运行期类型标签（见 Program::type_tags 注释）。
//
// 关着时 isinstance(x, int/bool/bytes) 对动态值是静默错值；全量默认开又会给
// 每个 unknown 值多一个 tag 字（实测 12 基准 matmul +660%、spectral_norm +148%）。
// 折中：程序里出现 isinstance 才开。判据故意用**源码包含**而不是 AST 扫描——
// 编译起点还没有类型信息，而这里只决定表示层，误报的代价只是多一个字 tag。
bool program_wants_type_tags(const Program& p) {
    if (p.type_tags) return true;
    if (p.type_tags_off) return false;
    for (auto* u : p.order) {
        if (!u) continue;
        if (u->source.find("isinstance") != std::string::npos) return true;
    }
    return false;
}
// 0.555：程序里用 @dataclass 时按需打开 lang_gaps（与 type_tags 同一套「按需自动开」口径）：
// lang_gaps 会改语义（dataclass 编译期展开、file 协议泛化、异常类当值），默认全开会拖累别的程序的
// 行为；只在源码出现 dataclass 时开，显式 PYADAMAS_LANG_GAPS=0 可关。
bool program_wants_lang_gaps(const Program& p) {
    if (p.lang_gaps) return true;
    if (p.lang_gaps_off) return false;
    // 只对**项目代码**自动开：标准库/CPython Lib 的文件提到 dataclass 很常见，
    // 给它们开 lang_gaps 会改变诊断口径（实测 626 基线 default 500 -> 495）。
    const std::string sd = stdlib_dir();
    for (auto* u : p.order) {
        if (!u) continue;
        const std::string& path = u->path;
        if (path.find("/python3.") != std::string::npos) continue;
        if (!sd.empty() && path.rfind(sd, 0) == 0) continue;
        if (u->source.find("dataclass") != std::string::npos) return true;
    }
    return false;
}

}  // namespace pyadamas
