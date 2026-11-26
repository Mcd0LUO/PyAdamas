#include "driver/build.h"
#include "driver/progcache.h"
#include "driver/util.h"
#include "backend/target_abi.h"
#include <algorithm>
#include <atomic>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string_view>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>
#include "common/ast.h"
#include "backend/codegen.h"
#include "syntax/desugar.h"
#include "driver/link.h"
#include "driver/modules.h"
#include "ir/ir.h"
#include "syntax/lexer.h"
#include "lower/lower.h"
#include "ir/module.h"
#include "syntax/parser.h"
#include "typecheck/typecheck.h"

namespace pyadamas {
bool debug_alloc_runtime_enabled() {
    static const bool on = [] {
        const char* c = std::getenv("PYADAMAS_CANARY");
        if (c && c[0] != '\0' && std::string_view(c) != "0") return true;
        const char* g = std::getenv("PYADAMAS_GUARD");
        if (g && g[0] != '\0' && std::string_view(g) != "0") return true;
        return false;
    }();
    return on;
}
std::string cc_extra_flags() {
    const char* f = std::getenv("PYADAMAS_CC_FLAGS");
    if (!f || !*f) return std::string();
    return std::string(" ") + f;
}
std::string cache_temp_path(const std::string& path, const char* ext = "") {
    static std::atomic<uint64_t> seq{0};
    return std::format("{}.tmp.{}.{}{}", path, static_cast<long long>(::getpid()),
                       seq.fetch_add(1, std::memory_order_relaxed), ext);
}
bool write_atomic(const std::string& path, const std::string& data) {
    std::string tmp = cache_temp_path(path);
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}
int avx2_emit_tag() {
    const char* v = std::getenv("PYADAMAS_AVX2");
    return (v != nullptr && v[0] == '0' && v[1] == '\0') ? 0 : 1;
}
uint64_t file_digest(const char* path) {
    unsigned long long h = 0;
    if (!pyadamas::hash_file(path, h)) return 0;
    return h;
}
std::string toolchain_id() {
    if (const char* over = std::getenv("PYADAMAS_TOOLCHAIN_ID"); over && over[0] != '\0')
        return std::format("env:{}", over);
    static const std::string id = [] {
        // 运行时换了分配器调试版（PYADAMAS_CANARY/GUARD）时，同一份源码
        // 会链接出不同行为的目标程序；模块对象本身与运行时变体无关，但
        // 把调试版摘要也并进工具链指纹更保守（换回默认时命中缓存也安全）。
        unsigned long long dbg = 0;
        if (debug_alloc_runtime_enabled())
            dbg = static_cast<unsigned long long>(file_digest(PYADAMAS_RT_DEBUG_OBJ));
        return std::format("exe:{:016x}|rt:{:016x}|dbg:{:016x}|fmt:{}|abi:{}",
                           static_cast<unsigned long long>(file_digest("/proc/self/exe")),
                           static_cast<unsigned long long>(file_digest(PYADAMAS_RT_OBJ)),
                           dbg, pyadamas::kModuleFormatVersion,
                           pyadamas::kModuleAbiVersion);
    }();
    return id;
}
std::string cache_root() {
    if (const char* d = std::getenv("PYADAMAS_CACHE_DIR"); d && d[0] != '\0')
        return d;
    return PYADAMAS_MODCACHE_DIR;
}
void set_cache_paths(ModuleUnit& u) {
    // 键 = 逻辑名 + **源码内容**哈希。
    //
    // 旧键是「名字 + 绝对路径哈希」（u.path）：项目一搬家/换机器就全 miss，
    // 缓存无法共享；而路径本身不携带任何编译输入信息，换个目录不改变产物。
    // 内容哈希让同一份源码在不同目录命中同一份产物；内容一变键就变，旧目录
    // 留给后续 GC/手动清理（正确性由 manifest 的 source_hash/deps 校验兜底）。
    //
    // 注意：缓存目录里不含路径，所以两个同内容同名的模块共用一个目录；
    // 原子写 + manifest 逐字段校验保证不会读到别人的产物。
    // 键里带版本段：版本进 mangling，同源码不同版本编出的符号不同，
    // 不能共用产物（P0 版本恒为 "0"，但键的形状现在定死）。
    std::string key = sanitize_symbol(u.name) + "-v" +
                      sanitize_symbol(pyadamas::module_version()) + "-" +
                      pyadamas::hex64(pyadamas::fnv1a64(u.source));
    u.cache_dir = cache_root() + "/" + key;
    u.asm_path = u.cache_dir + "/mod.s";
    u.obj_path = u.cache_dir + "/mod.o";
    u.iface_path = u.cache_dir + "/mod.pya";
    u.manifest_path = u.cache_dir + "/manifest";
}
std::optional<uint64_t> parse_u64(const std::string& s) {
    try {
        size_t pos = 0;
        uint64_t v = std::stoull(s, &pos, 10);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}
}  // namespace pyadamas
