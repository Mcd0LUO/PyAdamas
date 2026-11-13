// lower_builtin.cpp —— 内置函数调用降级的总入口
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_call(const std::string& name, Call& c) {
    // print 的关键字是 sep / end，得单独处理，不能走下面这条通用拦截
    if (name == "print") return lower_print(c);

    if (!c.keywords.empty() && name != "dict" && name != "open" && name != "sorted") {
        err(c.line, c.col, std::format("{}() does not take keyword arguments here", name));
    }

    // 按类别拆到 lower_builtin_*.cpp；各自返回 kNoReg 表示不归它管。
    if (VReg r = lower_builtin_reflect(name, c); r != kNoReg) return r;
    if (VReg r = lower_builtin_seq(name, c); r != kNoReg) return r;
    if (VReg r = lower_builtin_container(name, c); r != kNoReg) return r;
    if (VReg r = lower_builtin_text(name, c); r != kNoReg) return r;
    if (VReg r = lower_builtin_misc(name, c); r != kNoReg) return r;

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

}  // namespace pyadamas
