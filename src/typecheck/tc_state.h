#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/ast.h"
#include "typecheck/types.h"

namespace pyadamas {

struct Diag {
    int line = 1;
    int col = 1;
    std::string msg;
};

// ════════════════════════════════════════════════════════════════════
//  检查器
// ════════════════════════════════════════════════════════════════════

struct Symbol {
    TypePtr ty;
    int line = 1;
    bool annotated = false;  // 由标注确定，不允许再改类型
    bool is_global = false;
    // 名字由 def 绑定。函数名在 Python 里是可随时改绑的普通变量
    // （types.py 的 `_c = _c()` 就是拿调用结果覆盖函数名），
    // 所以它不像标注那样锁死类型。
    bool from_def = false;
    // class 语句绑定的名字：函数内定义的类也靠它和「类实例变量」区分。
    bool is_class_name = false;
    // 槽已退化成动态表示（见 stmt_for）：同一个名字被不同类型元素的 for
    // 先后绑定过，槽不能再收窄回某个具体类型 —— 前面循环的存储点会按新
    // 类型逐元素转换，而读取点仍按原类型解释（实测把 int 位模式当 double
    // 读）。置位后 declare 不再用后续赋值给槽定型。
    bool dynamic = false;
};

// from 内置模块 import 的函数：保留原始签名，调用点据此走模块专用规则。
struct NativeAlias {
    std::string module;
    std::string member;
    TypePtr sig;
};

struct Scope {
    std::unordered_map<std::string, Symbol> vars;
    // import m 绑定的模块别名；只用于属性/from 解析，不是运行时值。
    std::unordered_map<std::string, ModuleInfoPtr> modules;
    Scope* parent = nullptr;
    // 函数作用域上累积的「标注过的名字 → (类型, 首次声明的行)」。块作用域
    // 是临时的，光靠 vars 查不到已经结束的块里声明过什么；而 Python 的名字
    // 是函数作用域，块里的 `c: str` 与函数级的 `c: big` 指的是同一个槽。
    std::unordered_map<std::string, std::pair<TypePtr, int>> ann_seen;
    bool is_module = false;
    // 这个作用域属于哪个函数；模块 / 推导式 / 类作用域为 nullptr。
    // 闭包捕获靠它判断「名字是不是从外层函数来的」。
    const FunctionDef* fn_owner = nullptr;
};

// functools.partial 的绑定结果。sig 是返回的函数签名；bound_params 按
// 实参求值顺序记录被绑定形参的下标（pos 在前，kwonly 从 pos.size() 起），
// remaining 是仍暴露给调用方的原始形参下标，顺序与 sig 的形参一致。
struct PartialPlan {
    FuncInfoPtr sig;
    std::vector<int> bound_params;
    std::vector<int> remaining;
};

}  // namespace pyadamas
