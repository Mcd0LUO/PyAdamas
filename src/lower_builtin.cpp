// lower_builtin.cpp —— 内置函数调用降级的总入口
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_call(const std::string& name, Call& c) {
    // ── 定宽整数显式转换（P1）──────────────────────────────────────
    // cast_i8(x) 等：降级成 convert（按目标宽度截断 + 符号/零扩展）。
    // 结果类型由类型检查给出（tc_.type_of(&c)），这里只负责发指令。
    if (name.starts_with("cast_")) {
        TypePtr to = cast_target_of(name);
        TypePtr want = tc_.type_of(&c);
        if (!to) {
            err(c.line, c.col, std::format("unknown cast function '{}'", name));
            return const_none();
        }
        if (c.args.size() != 1) return const_none();
        TypePtr from = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        VReg r = convert_to(v, from, want ? want : to);
        return r;
    }

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
    if (VReg r = lower_builtin_vthread(name, c); r != kNoReg) return r;

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

}  // namespace pyadamas
