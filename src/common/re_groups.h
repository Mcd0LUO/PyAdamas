// common/re_groups.h —— 从**字面量模式**数捕获组个数（re.findall 的返回类型用）。
//
// CPython 的 re.findall 在 0/1 个捕获组时返回 list[str]，2 个及以上返回
// list[tuple[str, ...]]（每个元素是各组匹配串）。PyAdamas 的模式通常是
// 字符串字面量，编译期数一遍组数就能给出准确类型；非字面量（变量/
// f-string）返回 -1，调用方保持旧的 list[str] 口径。
//
// 只做语法层面的扫描：转义、字符类、非捕获组 / 断言组 / 命名组。
#pragma once

#include <string>

#include "common/ast.h"

namespace pyadamas {

inline int re_pattern_groups(const std::string& s) {
    int groups = 0;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '[') {
            ++i;
            if (i < n && s[i] == '^') ++i;
            if (i < n && s[i] == ']') ++i;
            while (i < n && s[i] != ']') {
                if (s[i] == '\\') ++i;
                ++i;
            }
            ++i;
            continue;
        }
        if (c == '(') {
            if (i + 1 < n && s[i + 1] == '?') {
                // (?P<name>...) 是捕获组；(?P=name) 是反向引用；
                // (?: (?= (?! (?<= (?<! (?> (?i: (?i-m: (?# 都不是。
                if (i + 2 < n && s[i + 2] == 'P' && i + 3 < n && s[i + 3] == '<')
                    ++groups;
            } else {
                ++groups;
            }
        }
        ++i;
    }
    return groups;
}

inline int re_literal_group_count(const Expr* e) {
    if (!e || e->kind != ExprKind::Constant) return -1;
    const auto& c = static_cast<const Constant&>(*e);
    if (c.ck != Constant::CK::Str) return -1;
    return re_pattern_groups(c.s);
}

}  // namespace pyadamas
