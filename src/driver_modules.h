// driver_modules.h —— 模块表查询/import 解析的内部接口
//
// 从 driver_build.cpp 拆出（纯代码移动）：模块表与解析在 driver_modules.cpp，
// check 模式 import 图在 driver_check.cpp，构建/缓存/链接留在 driver_build.cpp。
// 这些函数原来在 driver_build.cpp 的匿名名字空间里，拆分后跨 TU 共用，
// 改为外部链接（签名与函数体不变）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "driver_build.h"
#include "module.h"

namespace pyadamas {

struct ResolvedModule {
    std::string path;
    bool is_package = false;
};

enum class ModuleKind : std::uint8_t { Native, Python, NativePython };

struct ModuleEntry {
    std::string_view name;
    ModuleKind kind;
    std::string_view source;
    std::string_view deps;
};

struct ImportReq {
    std::string name;
    int line = 1;
    int col = 1;
    std::vector<std::string> from_names;  // ImportFrom 的名字，可能是子模块
};

struct ImportIssue {
    int line = 1;
    int col = 1;
    std::string msg;
};

std::string sanitize_symbol(const std::string& s);
bool valid_module_name(const std::string& s);
std::string canonical_path(const std::string& p);
std::optional<ResolvedModule> resolve_module_path(const std::string& importer,
                                                  const std::string& full_name,
                                                  const std::vector<std::string>& include_dirs);
const ModuleEntry* find_module_entry(std::string_view name);
bool is_native_module(const ModuleEntry* e);
std::string module_source_path(const ModuleEntry& e);
bool path_under_stdlib(const std::string& path);
std::optional<ResolvedModule> resolve_import_source(const std::string& importer,
                                                    const std::string& name,
                                                    const std::vector<std::string>& include_dirs);
bool validate_module_table();
void scan_imports_body(std::vector<pyadamas::StmtPtr>& body, std::vector<ImportReq>& reqs,
                       std::vector<ImportIssue>& issues, const std::string& pkg);
std::string unit_package(const ModuleUnit& u);
pyadamas::ModuleInfoPtr merge_extension_module(const pyadamas::ModuleInfoPtr& base,
                                               const pyadamas::ModuleInfoPtr& ext);

}  // namespace pyadamas
