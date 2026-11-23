// opt_stats.h —— 优化 pass 有效性统计（PYADAMAS_OPT_STATS=1 开启）
//
// 设计目标：默认零成本、不污染各 pass 实现。驱动（opt.cpp）在每个 pass 调用点
// 用 opt_stats_run 包一层；开启时对「函数 IR 指纹」在 pass 前后各取一次，
// 指纹变了就记该 pass 一次命中。退出时按 pass 打印：
//
//   pass=<name> changed=<n> funcs=<n>
//   zero_hit=<name1>,<name2>
//
// 用途：验证某个 pass 是否真的在真实输入上生效（0.420 发现 promote_globals
// 从未生效、0.419 发现 fuse_dict_bump 判据过窄，都是靠这种审计）。
#pragma once

#include "opt.h"

#include <cstdint>

namespace pyadamas {

// 开关（getenv 一次，后续只读静态 bool）。关闭时调用方只付一次分支。
bool opt_stats_on();

// 函数 IR 指纹：块序列 + 每条指令的操作码/操作数/立即数/跳转目标/实参/类型。
// 只在 opt_stats_on() 为真时调用。
uint64_t opt_fp(const IRFunction& fn);

// 记一次 pass 访问（funcs+1）与是否改变（changed+1）。
void opt_stats_observe(const char* pass, bool changed);

// 立即输出汇总（正常由退出时的静态对象自动调用；测试也可手动调）。
void opt_stats_flush();

// pass 调用点的包装：关闭时零开销（一次静态 bool 分支）。
template <typename F>
inline void opt_stats_run(const char* pass, IRFunction& fn, F&& body) {
    if (!opt_stats_on()) {
        body();
        return;
    }
    const uint64_t before = opt_fp(fn);
    body();
    opt_stats_observe(pass, opt_fp(fn) != before);
}

}  // namespace pyadamas
