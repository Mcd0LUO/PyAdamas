// ir/deps_json.h —— `pyadamas deps --json` 的模块图序列化
//
// 这是给构建系统 / 包管理器 / IDE 用的**稳定数据平面接口**：
//   * 确定性：同样的输入产出逐字节相同的 JSON（模块按名字排序，无时间戳）；
//   * 无绝对路径：项目内模块给相对项目根的路径，stdlib 模块给 `stdlib/<rel>`；
//   * 版本/格式信息在顶层，供消费方判断兼容性。
#pragma once

#include <string>

#include "driver/build.h"

namespace pyadamas {

// p 必须已经过 prepare_program(p, false, false)（接口已建好）。
// root_abs = 项目根绝对路径（入口文件所在目录）；cwd = 当前工作目录（用于把
// 根写成相对路径，JSON 里不出现绝对路径）。
std::string module_graph_json(const Program& p, const std::string& root_abs,
                              const std::string& cwd);

}  // namespace pyadamas
