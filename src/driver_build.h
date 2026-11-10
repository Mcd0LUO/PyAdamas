// driver_build.h —— 多模块构建驱动
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ir.h"
#include "module.h"
#include "typecheck.h"

namespace py23 {

struct ModuleUnit {
    std::string key;           // 内部映射键；入口是 "@entry"
    std::string name;          // 符号前缀；入口是 "__entry__"
    std::string runtime_name;  // __name__
    std::string path;
    std::string source;
    std::unique_ptr<py23::Module> ast;
    py23::ModuleInfoPtr info;
    std::shared_ptr<py23::TypeChecker> tc;
    std::optional<py23::IRModule> ir;
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
    int state = 0;  // 0 未访问 / 1 正在访问 / 2 完成
};

struct Program {
    std::map<std::string, std::unique_ptr<ModuleUnit>> units;
    std::vector<ModuleUnit*> order;
    ModuleUnit* entry = nullptr;
    std::vector<std::string> include_dirs;
    bool rebuild = false;
    bool verbose = false;
    int opt = 2;
    bool fast_math = false;
};

bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p);
bool prepare_program(Program& p, bool emit_objects, bool entry_ir);
bool link_program(Program& p, const std::string& output);

}  // namespace py23
