// ir/module_map.h —— 外部模块映射（--module-map）
//
// 由 `pyadamas deps --json` 的输出派生：逻辑模块名 → 源文件路径。
// 编译器按它解析 import（显式映射优先，未命中回落现有路径搜索），
// 让构建系统/包管理器可以完全接管「哪个名字对应哪个文件」。
#pragma once

#include <map>
#include <optional>
#include <string>

namespace pyadamas {

struct ModuleMap {
    // 逻辑模块名（`a` / `a.b`）→ 源文件绝对路径（已按 map 文件所在目录解析）。
    std::map<std::string, std::string> paths;

    bool contains(const std::string& name) const { return paths.find(name) != paths.end(); }
};

// 解析 deps --json 的产物（同构格式）。相对路径按 map 文件所在目录解析。
// 任何条目指向不存在的文件都算错误（不静默跳过）：失败返回 nullopt 并填 err。
std::optional<ModuleMap> load_module_map(const std::string& file, std::string& err);

}  // namespace pyadamas
