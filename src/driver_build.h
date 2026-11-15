// driver_build.h —— 多模块构建驱动
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "ir.h"
#include "module.h"
#include "typecheck.h"

namespace pyadamas {

struct ModuleUnit {
    std::string key;           // 内部映射键；入口是 "@entry"
    std::string name;          // 符号前缀；入口是 "__entry__"
    std::string runtime_name;  // __name__
    std::string path;
    std::string source;
    std::unique_ptr<pyadamas::Module> ast;
    pyadamas::ModuleInfoPtr info;
    std::shared_ptr<pyadamas::TypeChecker> tc;
    std::optional<pyadamas::IRModule> ir;
    std::vector<std::string> deps;
    std::string cache_dir;
    std::string asm_path;
    std::string obj_path;
    std::string iface_path;
    std::string manifest_path;
    std::string asm_text;
    bool is_entry = false;
    bool is_package = false;
    std::string parent_name;
    bool from_cache = false;
    // 模块的 IR 是否用到 unknown 保守 retain（含 asyncio/生成器运行时）。
    // 写进缓存清单：命中缓存时也要参与整程序判断。
    bool needs_unknown_index = true;
    int state = 0;  // 0 未访问 / 1 正在访问 / 2 完成
};

struct Program {
    std::map<std::string, std::unique_ptr<ModuleUnit>> units;
    std::vector<ModuleUnit*> order;
    ModuleUnit* entry = nullptr;
    std::vector<std::string> include_dirs;
    // cimport .cpya 里 link 声明的链接参数（已归一化并去重）。
    std::vector<std::string> link_args;
    bool rebuild = false;
    bool verbose = false;
    int opt = 2;
    bool fast_math = false;
    // 任一模块需要 unknown 索引就保持开启；全都不需要时入口关掉维护。
    bool needs_unknown_index = false;
};

// 内建模块（math/os/json/...）的签名表，实现在 builtin_sigs.cpp。
ModuleInfoPtr builtin_module(const std::string& name);

// check 模式用的 import 图解析：加载依赖模块的接口（不生成代码），
// 返回一个 module_lookup 闭包给 TypeChecker。无法解析的依赖返回 nullptr
// （调用方按 unknown 处理）。diagnostics 里收集真正解析失败的模块。
using CheckLookup = std::function<ModuleInfoPtr(const std::string&)>;
CheckLookup load_check_lookup(const std::string& entry_path,
                               const std::vector<std::string>& include_dirs,
                               std::vector<std::string>& missing);
bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p);
bool prepare_program(Program& p, bool emit_objects, bool entry_ir);
bool link_program(Program& p, const std::string& output);

}  // namespace pyadamas
