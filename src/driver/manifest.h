// driver/manifest.h —— pyadamas.toml 的最小 TOML 子集（P0）
//
// 为什么手写解析器：项目有「不引入第三方依赖」的硬约束，而我们需要的是
// 一个**严格**的子集（只认 `[package]/[dependencies]/[modules]/[aliases]`），
// 严格到「不认识的键/语法一律报错并指到行列」——这比引入通用 TOML 库更安全，
// 因为清单是构建输入，静默忽略一个拼错的键会让锁与人的意图不一致。
//
// 支持的语法（单行子集，刻意不实现多行字符串/数组/日期）：
//   [table]                     表头（不接受 [a.b] 嵌套表）
//   key = "value"               基本字符串（\\ \" \n \t \r）
//   key = { a = "x", b = "y" }  单行内联表
//   # 注释                      行首或值后
// 键可以是 bare key 或带引号的字符串（模块名含 '.' 时必须用引号）。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pyadamas {

// 1-based 行列，与 report()/lexer 的口径一致。
struct SourceLoc {
    int line = 0;
    int col = 0;
};

struct ManifestDep {
    std::string path;  // 清单里写的路径（相对本清单所在目录；未规范化）
    SourceLoc loc;
};

struct ManifestModule {
    std::string package;  // [dependencies] 里的键（包名）
    std::string module;   // 该包内的模块名（点分）
    SourceLoc loc;
};

struct Manifest {
    std::string path;    // 清单文件的绝对路径
    std::string dir;     // 清单所在目录（绝对）
    std::string source;  // 原文（report() 打印片段用）
    std::string name;
    std::string version;
    SourceLoc package_loc;
    std::map<std::string, ManifestDep> deps;
    // 逻辑名 →（包, 模块）。Q1 的落地：源码只写逻辑名，版本/包名只出现在清单/锁。
    std::map<std::string, ManifestModule> modules;
};

// 读取并校验清单。任何错误都用 report() 打印（file:line:col + 片段）并返回 nullopt。
std::optional<Manifest> load_manifest(const std::string& file);

// 从 path（文件或目录）所在目录向上查找 pyadamas.toml。
// 找不到返回 nullopt（不是错误：这就是「无清单项目」的判定）。
std::optional<std::string> find_manifest(const std::string& path);

bool valid_package_name(const std::string& s);
bool valid_version(const std::string& s);
// 点分模块名，每段必须是合法标识符（与 driver/modules.cpp 的 import 语法一致）。
bool valid_dotted_module(const std::string& s);

}  // namespace pyadamas
