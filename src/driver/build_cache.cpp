#include "driver/build.h"
#include "driver/build_cache.h"
#include "driver/progcache.h"
#include "driver/util.h"
#include "common/target_abi.h"
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <sys/file.h>
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

// 声明在 GC 辅助块之后定义（set_cache_paths 更早使用）。
void cache_hold_module_lock(const std::string& root);

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
std::string cache_temp_path(const std::string& path, const char* ext) {
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
    // 取缓存根共享锁：本进程存续期间，别的进程的淘汰不会删我们正在用的条目。
    cache_hold_module_lock(cache_root());
}
// ── 有界缓存淘汰（0.684）────────────────────────────────────────────────
// modcache / progcache 此前只有「加」没有「减」：源码哈希一变键就变，旧条目
// 永远留着（build/dev/modcache 实测 28 MB / 223 条且只增不减）。这里做
// best-effort 上界回收：写入条目后更新记账文件；超出条目数/总字节上限就
// 扫描一遍，按 mtime 从旧到新删到重新进入上限。删除失败（权限/竞态）静默
// 忽略 —— 缓存是纯优化，任何 IO 问题都不能影响编译。
void gc_at_exit_impl();  // atexit 的 GC 入口（定义在本文件后部）
namespace {
namespace fs = std::filesystem;
constexpr uint64_t k_default_max_entries = 512;
constexpr uint64_t k_default_max_mb = 512;
const char k_gc_state_name[] = ".cache_gc";
struct GcLimits {
    uint64_t max_entries = k_default_max_entries;
    uint64_t max_bytes = k_default_max_mb << 20;
    bool enabled() const { return max_entries != 0 || max_bytes != 0; }
};
uint64_t env_limit(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    auto o = parse_u64(v);
    return o ? *o : fallback;
}
const GcLimits& gc_limits() {
    // 进程内读一次 env；0 = 该项不限（两项都 0 则完全不记账、零开销）。
    static const GcLimits l = [] {
        GcLimits g;
        g.max_entries = env_limit("PYADAMAS_CACHE_MAX_ENTRIES", k_default_max_entries);
        const uint64_t mb = env_limit("PYADAMAS_CACHE_MAX_MB", k_default_max_mb);
        g.max_bytes = mb > (UINT64_MAX >> 20) ? UINT64_MAX : mb << 20;
        return g;
    }();
    return l;
}
std::string gc_state_path(const std::string& root) { return root + "/" + k_gc_state_name; }
bool read_gc_state(const std::string& root, uint64_t& entries, uint64_t& bytes) {
    std::ifstream in(gc_state_path(root), std::ios::binary);
    if (!in) return false;
    uint64_t e = 0, b = 0;
    if (!(in >> e >> b)) return false;
    entries = e;
    bytes = b;
    return true;
}
bool write_gc_state(const std::string& root, uint64_t entries, uint64_t bytes) {
    std::error_code ec;
    fs::create_directories(root, ec);
    return write_atomic(gc_state_path(root), std::format("{} {}\n", entries, bytes));
}
// 单个条目的字节数：文件=size；目录=递归求和（容忍扫描期间的变化）。
uint64_t path_bytes(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        const auto sz = fs::file_size(p, ec);
        return ec ? 0 : static_cast<uint64_t>(sz);
    }
    if (!fs::is_directory(p, ec)) return 0;
    uint64_t total = 0;
    auto end = fs::recursive_directory_iterator();
    for (auto it = fs::recursive_directory_iterator(
             p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const auto sz = it->file_size(e2);
        if (!e2) total += static_cast<uint64_t>(sz);
    }
    return total;
}
struct GcEntry {
    fs::path path;
    fs::file_time_type mtime;
    uint64_t bytes = 0;
};
// ── 跨进程并发安全（0.684 修）──────────────────────────────────
// 淘汰（扫描+删除）与「编译进程使用条目」必须互斥：多进程套件里，进程 B
// 的 GC 可能删掉进程 A 正在读/准备链接的 mod.o，导致 A 的编译失败。
// 只按 mtime 加宽限期不够：**缓存命中**的条目可能很旧却正被 A 使用，宽限期
// 保护不了。做法是 flock：
//   * 每个编译进程在 set_cache_paths（第一次解析条目路径）时对
//     <root>/.cache_lock 取共享锁 LOCK_SH，直到进程退出；
//   * 淘汰推迟到进程退出（atexit）：先放共享锁，再非阻塞独占锁
//     LOCK_EX|LOCK_NB；拿不到（还有编译在跑）就跳过，下次再收；
//   * 独占锁在手时扫描+删除，删除前再 stat 一次确认 mtime 没变。
// 因此任何时刻都不会删别的进程正在用的条目。最坏情况：长期持续有编译在跑
// 时暂时不淘汰（缓存可短暂超过上限），静默期后的下一次退出会收回到低水位。
struct LockState {
    int module_fd = -1;
    std::set<std::string> pending_roots;
    bool atexit_registered = false;
};
LockState& lock_state() {
    static LockState s;
    return s;
}
void register_atexit() {
    LockState& s = lock_state();
    if (s.atexit_registered) return;
    s.atexit_registered = true;
    std::atexit(gc_at_exit_impl);
}
int open_lock_file(const std::string& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    return ::open((root + "/.cache_lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
}
// 扫描 root 下的缓存条目：跳过点文件（含记账文件）、progcache 子树和临时文件。
void scan_entries(const std::string& root, std::vector<GcEntry>& out, uint64_t& total_bytes) {
    out.clear();
    total_bytes = 0;
    std::error_code ec;
    auto end = fs::directory_iterator();
    for (auto it = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;           // 记账文件/锁文件
        if (name == "progcache") continue;                       // 根目录下的程序缓存子树
        if (name.find(".tmp.") != std::string::npos) continue;  // 半成品
        std::error_code e2;
        const auto mt = fs::last_write_time(it->path(), e2);
        if (e2) continue;
        // 安全：并发编译可能正在写同一条目。条目以 manifest 最后落盘为
        // 「写完」标志 —— 没有 manifest 的目录不删（只有很老的半成品才
        // 当作失效条目回收，避免崩溃留下的目录永久占位）。
        std::error_code me;
        if (fs::is_directory(it->path(), me) &&
            !fs::exists(it->path() / "manifest", me)) {
            const auto age = fs::file_time_type::clock::now() - mt;
            if (age < std::chrono::hours(1)) continue;
        }
        const uint64_t bytes = path_bytes(it->path());
        out.push_back({it->path(), mt, bytes});
        total_bytes += bytes;
    }
}
// 独占锁下的真实扫描+淘汰（低水位 = 上限的 7/8，摊销下一次扫描）。
// 只按 mtime 从旧到新删；删除前再 stat 一次确认 mtime 没变（防御性）。
void gc_root_now(const std::string& root, const GcLimits& l) {
    const uint64_t low_entries =
        l.max_entries == 0 ? UINT64_MAX : l.max_entries - l.max_entries / 8;
    const uint64_t low_bytes =
        l.max_bytes == 0 ? UINT64_MAX : l.max_bytes - l.max_bytes / 8;
    std::vector<GcEntry> list;
    uint64_t bytes = 0;
    scan_entries(root, list, bytes);
    uint64_t entries = list.size();
    if ((l.max_entries == 0 || entries <= l.max_entries) &&
        (l.max_bytes == 0 || bytes <= l.max_bytes)) {
        // 记账可能被并发进程写偏大：顺手校正，避免以后每次退出都白扫一遍。
        write_gc_state(root, entries, bytes);
        return;
    }
    std::sort(list.begin(), list.end(),
              [](const GcEntry& a, const GcEntry& b) { return a.mtime < b.mtime; });
    for (const GcEntry& e : list) {
        if (entries <= low_entries && bytes <= low_bytes) break;
        std::error_code ec2;
        const auto mt2 = fs::last_write_time(e.path, ec2);
        if (ec2 || mt2 != e.mtime) continue;  // 扫描后被动过：这轮不删
        std::error_code ec;
        fs::remove_all(e.path, ec);
        if (ec) continue;  // 删除失败：静默忽略，下次再试
        entries -= 1;
        bytes -= std::min(bytes, e.bytes);
    }
    write_gc_state(root, entries, bytes);
}
}  // namespace

void cache_hold_module_lock(const std::string& root) {
    LockState& s = lock_state();
    if (s.module_fd >= 0) return;
    const int fd = open_lock_file(root);
    if (fd < 0) return;  // 打不开锁文件：退化为无锁（仍 best-effort）
    if (::flock(fd, LOCK_SH) != 0) {
        ::close(fd);
        return;
    }
    s.module_fd = fd;
}

void gc_at_exit_impl() {
    LockState& s = lock_state();
    // 本进程不再使用条目：先放共享锁，再尝试非阻塞独占锁（有编译在跑就跳过）。
    if (s.module_fd >= 0) ::flock(s.module_fd, LOCK_UN);
    for (const std::string& root : s.pending_roots) {
        const int fd = open_lock_file(root);
        if (fd < 0) continue;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {  // 还有进程持共享锁
            ::close(fd);
            continue;
        }
        gc_root_now(root, gc_limits());
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
    if (s.module_fd >= 0) {
        ::close(s.module_fd);
        s.module_fd = -1;
    }
}

void cache_evict_after_store(const std::string& root, const std::string& entry, bool existed) {
    const GcLimits& l = gc_limits();
    if (!l.enabled()) return;
    uint64_t entries = 0;
    uint64_t bytes = 0;
    if (!read_gc_state(root, entries, bytes)) {
        // 首次写入/记账文件损坏：扫一遍拿真实基线（只初始化，不删）。
        std::vector<GcEntry> list;
        scan_entries(root, list, bytes);
        entries = list.size();
    }
    if (!existed) {
        entries += 1;
        bytes += path_bytes(entry);
    }
    write_gc_state(root, entries, bytes);
    if ((l.max_entries != 0 && entries > l.max_entries) ||
        (l.max_bytes != 0 && bytes > l.max_bytes)) {
        // 真正淘汰推迟到进程退出（atexit），独占锁下做：并发编译期间绝不删
        // 别人正在用的条目。
        lock_state().pending_roots.insert(root);
        register_atexit();
    }
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
