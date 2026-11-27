#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "driver/build.h"

namespace pyadamas {

// 缓存身份与工具链指纹：build.cpp 与 build_cache.cpp 共享的内部接口。
bool debug_alloc_runtime_enabled();
std::string cc_extra_flags();
std::string cache_temp_path(const std::string& path, const char* ext = "");
bool write_atomic(const std::string& path, const std::string& data);
int avx2_emit_tag();
uint64_t file_digest(const char* path);
std::string toolchain_id();
std::string cache_root();
void set_cache_paths(ModuleUnit& u);
std::optional<uint64_t> parse_u64(const std::string& s);

// 有界缓存淘汰（0.684）：写入一个缓存条目后调用。
// root = 缓存根目录（modcache 根或 progcache 目录），entry = 刚写入的条目
// 路径（目录或 .bin 文件），existed = 写入前该条目是否已存在。
// 超限（条目数/总字节）时按 mtime 从旧到新删除到重新进入上限；绝不删
// entry 本身；任何 IO 失败静默忽略（缓存只是优化，不能影响编译）。
// 上限默认 512 条 / 512 MiB，PYADAMAS_CACHE_MAX_ENTRIES /
// PYADAMAS_CACHE_MAX_MB 可覆盖，0 = 该项不限。
void cache_evict_after_store(const std::string& root, const std::string& entry,
                             bool existed);

}  // namespace pyadamas
