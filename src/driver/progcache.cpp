// driver/progcache.cpp —— 见 progcache.h
#include "driver/progcache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

#include "driver/build.h"
#include "driver/build_cache.h"  // cache_evict_after_store（有界淘汰）
#include "typecheck/types.h"  // pyadamas::hex64 / fnv1a64

namespace pyadamas {
namespace {

namespace fs = std::filesystem;

bool debug_on() {
    static const bool on = [] {
        const char* d = std::getenv("PYADAMAS_PROGCACHE_DEBUG");
        return d && d[0] != '\0' && std::strcmp(d, "0") != 0;
    }();
    return on;
}

std::string cache_dir() { return prog_cache_root() + "/progcache"; }

// 流式 FNV-1a 64：对象文件可能几 MB，不整读进内存。
// 与 build.cpp 的 file_digest 同一套字级混合（见那里的注释：这是本地失效
// 指纹，不是信任根；逐字节 FNV 在大对象上太慢）。
bool detail_hash_file(const std::string& path, unsigned long long& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned long long h = 1469598103934665603ULL;
    auto mix_word = [&h](unsigned long long w) {
        h ^= w;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 29;
    };
    char buf[1 << 16];
    unsigned long long total = 0;
    while (in) {
        in.read(buf, static_cast<std::streamsize>(sizeof buf));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        total += static_cast<unsigned long long>(n);
        const std::streamsize words = n / 8;
        const char* p = buf;
        for (std::streamsize i = 0; i < words; ++i, p += 8) {
            unsigned long long w = 0;
            std::memcpy(&w, p, 8);
            mix_word(w);
        }
        for (const char* q = p; q < buf + n; ++q) {
            h ^= static_cast<unsigned char>(*q);
            h *= 1099511628211ULL;
        }
    }
    if (in.bad()) return false;
    mix_word(total);
    out = h;
    return true;
}

bool same_file(const std::string& a, const std::string& b) {
    std::error_code ec;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[1 << 16], bb[1 << 16];
    while (fa) {
        fa.read(ba, sizeof ba);
        const auto n = fa.gcount();
        fb.read(bb, sizeof bb);
        if (fb.gcount() != n || std::memcmp(ba, bb, static_cast<size_t>(n)) != 0) return false;
    }
    return true;
}

bool copy_file(const std::string& from, const std::string& to) {
    const std::string tmp = to + ".progcache.tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                             fs::perms::group_read | fs::perms::group_exec |
                             fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

bool hash_file(const std::string& path, unsigned long long& out) {
    return detail_hash_file(path, out);
}

bool progcache_enabled() {
    static const bool on = [] {
        const char* d = std::getenv("PYADAMAS_NO_PROGCACHE");
        return !(d && d[0] != '\0' && std::strcmp(d, "0") != 0);
    }();
    return on;
}

std::string progcache_key(const std::vector<std::string>& objs, const std::string& extra) {
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](std::string_view s) {
        for (char ch : s) {
            h ^= static_cast<unsigned char>(ch);
            h *= 1099511628211ULL;
        }
        h ^= 0xff;
        h *= 1099511628211ULL;
    };
    mix("progcache1");
    mix(extra);
    for (const auto& o : objs) {
        unsigned long long d = 0;
        if (!detail_hash_file(o, d)) return {};
        mix(std::to_string(d));
    }
    return hex64(h);
}

bool progcache_fetch(const std::string& key, const std::string& out) {
    if (key.empty()) return false;
    const std::string src = cache_dir() + "/" + key + ".bin";
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return false;
    if (same_file(src, out)) {
        if (debug_on()) std::fprintf(stderr, "[progcache] hit %s\n", key.c_str());
        return true;
    }
    if (!copy_file(src, out)) return false;
    if (debug_on()) std::fprintf(stderr, "[progcache] hit %s (written)\n", key.c_str());
    return true;
}

void progcache_store(const std::string& key, const std::string& out) {
    if (key.empty()) return;
    std::error_code ec;
    fs::create_directories(cache_dir(), ec);
    const std::string dst = cache_dir() + "/" + key + ".bin";
    const bool existed = fs::is_regular_file(dst, ec);
    if (copy_file(out, dst)) {
        // 有界淘汰（0.684）：程序缓存同样只有「加」没有「减」。
        cache_evict_after_store(cache_dir(), dst, existed);
        if (debug_on()) std::fprintf(stderr, "[progcache] store %s\n", key.c_str());
    }
}

}  // namespace pyadamas
