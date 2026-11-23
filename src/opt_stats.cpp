// opt_stats.cpp —— pass 有效性统计的实现（见 opt_stats.h）
#include "opt_stats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace pyadamas {
namespace {

struct PassStat {
    int64_t funcs = 0;
    int64_t changed = 0;
};

// 故意 new 而不放静态对象：g_stats_flusher 的析构在 main 之后运行，
// 函数内静态 map 会先被销毁 → flush 读已析构的 map（实测输出乱码/垃圾计数）。
std::map<std::string, PassStat>& stats_table() {
    static auto* t = new std::map<std::string, PassStat>();
    return *t;
}

inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t fp_type(uint64_t h, TypePtr t) {
    if (!t) return mix(h, 0x5f3a9c1dULL);
    h = mix(h, static_cast<uint64_t>(t->kind));
    return h;
}

}  // namespace

bool opt_stats_on() {
    static const bool on = [] {
        const char* v = std::getenv("PYADAMAS_OPT_STATS");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    return on;
}

uint64_t opt_fp(const IRFunction& fn) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = mix(h, static_cast<uint64_t>(fn.blocks.size()));
    for (const auto& b : fn.blocks) {
        h = mix(h, static_cast<uint64_t>(b.id));
        h = mix(h, static_cast<uint64_t>(b.insts.size()));
        h = mix(h, b.terminated() ? 1u : 0u);
        for (const auto& in : b.insts) {
            h = mix(h, static_cast<uint64_t>(in.op));
            h = mix(h, static_cast<uint64_t>(in.bop));
            h = mix(h, static_cast<uint64_t>(in.uop));
            h = mix(h, static_cast<uint64_t>(in.cop));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.dst)));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.a)));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.b)));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.c)));
            h = mix(h, static_cast<uint64_t>(in.imm));
            uint64_t fb = 0;
            static_assert(sizeof(fb) == sizeof(in.fimm), "fimm 大小假设");
            std::memcpy(&fb, &in.fimm, sizeof fb);
            h = mix(h, fb);
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.target)));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.target2)));
            h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(in.idx.kind)));
            h = mix(h, static_cast<uint64_t>(in.idx.value));
            h = mix(h, static_cast<uint64_t>(in.fwidth));
            h = mix(h, in.funsigned ? 1u : 0u);
            h = mix(h, static_cast<uint64_t>(in.args.size()));
            for (VReg r : in.args) h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(r)));
            h = mix(h, static_cast<uint64_t>(in.cargs.size()));
            for (uint8_t k : in.cargs) h = mix(h, k);
            h = fp_type(h, in.ty);
        }
    }
    return h;
}

void opt_stats_observe(const char* pass, bool changed) {
    PassStat& s = stats_table()[pass ? pass : "?"];
    ++s.funcs;
    if (changed) ++s.changed;
}

void opt_stats_flush() {
    if (!opt_stats_on()) return;
    auto& t = stats_table();
    if (t.empty()) return;
    std::string zero;
    for (const auto& [name, s] : t) {
        std::fprintf(stderr, "pass=%s changed=%lld funcs=%lld\n", name.c_str(),
                     static_cast<long long>(s.changed), static_cast<long long>(s.funcs));
        if (s.changed == 0) {
            if (!zero.empty()) zero += ",";
            zero += name;
        }
    }
    std::fprintf(stderr, "zero_hit=%s\n", zero.c_str());
    std::fflush(stderr);
}

namespace {
struct StatsFlusher {
    ~StatsFlusher() { opt_stats_flush(); }
};
StatsFlusher g_stats_flusher;
}  // namespace

}  // namespace pyadamas
