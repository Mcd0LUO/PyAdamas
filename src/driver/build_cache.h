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

}  // namespace pyadamas
