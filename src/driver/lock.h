// driver/lock.h —— pyadamas.lock 的解析、生成与校验（P0）
//
// 锁是「一次构建的唯一输入」：解析结果 = 包集合（坐标 + 相对路径 + 源码树哈希）
// + 模块映射（逻辑名 → 包内文件）+ 依赖边。格式刻意做成**行式、排序、可 diff**，
// 不含时间戳/主机名/绝对路径（§5.5.2 的确定性要求）。源码树的身份用 SHA-256，
// 因为它是完整性/信任根，不是缓存失效用的本地摘要。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "driver/manifest.h"
#include "ir/module_map.h"

namespace pyadamas {

struct LockedPackage {
    std::string name;
    std::string version;
    std::string path;  // 相对锁文件所在目录（项目根），'/' 分隔，可能带 '..'
    std::string tree;  // 64 位小写十六进制 SHA-256
};

struct LockedModule {
    std::string logical;  // 源码 import 写的名字
    std::string package;  // 提供者包名
    std::string module;   // 包内模块名（点分）
    std::string path;     // 相对所属包根的文件路径，'/' 分隔
};

struct LockedDep {
    std::string from;  // 包名；项目根包也在内
    std::string to;
    std::string path;  // 声明方清单里写的相对路径（规范化，'/' 分隔）
};

struct Lock {
    int format = 1;
    // 仅记录信息，不参与解析/不参与 --locked 比较（换工具链/目标不该让锁失效）。
    std::string compiler_version;
    std::string target;
    int module_format = 0;
    int module_abi = 0;
    std::string root_name;
    std::string root_version;
    std::map<std::string, LockedPackage> packages;  // 按包名排序
    std::map<std::string, LockedModule> modules;     // 按逻辑名排序
    std::vector<LockedDep> deps;                     // 按 (from,to) 排序

    std::string render() const;
};

// 源码树哈希：对排序后的「相对路径 + 文件长度 + 内容」算 SHA-256。
// 只覆盖内容（不含 mtime/权限），跳过 .git（VCS 元数据不是源码）。
// 符号链接/特殊文件明确报错：P0 不猜（P1 store 会解引用并记录目标）。
std::string tree_hash(const std::string& dir, std::string& err);

// 读清单 → 解析 path 依赖闭包 → 生成锁（不写文件）。失败时已打印 file:line:col。
std::optional<Lock> resolve_lock(const Manifest& root);

// 严格解析锁文件。格式错误/绝对路径/悬空引用都报错并返回 nullopt。
std::optional<Lock> read_lock(const std::string& file);
bool write_lock_file(const std::string& file, const std::string& text);

// 语义差异（忽略 toolchain 记录字段）。空串 = 一致；否则是可直接打印的多行 diff。
std::string diff_locks(const Lock& want, const Lock& got);

// 从锁派生与 `deps --json` 同 schema 的模块映射 JSON（路径为绝对路径，供现有
// load_module_map 加载；相对路径解析规则因此与 map 文件位置无关）。
std::string lock_module_map_json(const Lock& lock, const std::string& lock_dir);
std::optional<ModuleMap> module_map_from_lock(const Lock& lock, const std::string& lock_dir);

}  // namespace pyadamas
