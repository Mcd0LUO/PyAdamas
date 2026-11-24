// driver/progcache.h —— 整程序产物缓存（0.478，P4 增量重编第一刀）
//
// 目标：同源码 + 同开关 + 同工具链的重复 build/run 不再跑 as/ld。
// 键覆盖**全部链接输入**：每个 .o 的内容摘要（含运行时对象）、target ABI、
// toolchain_id、gc/asan 开关与 cc 额外 flags、cimport 链接参数。任一项变化
// 键就变，所以「漏项 = 静默错误」的防线是：键只由真实链接输入推导，不从
// 命令行/选项名单猜。
//
// 失败语义：缓存是纯优化，任何 IO 失败都退化成「照常链接」，不报错。
#pragma once

#include <string>
#include <vector>

namespace pyadamas {

// 关掉：PYADAMAS_NO_PROGCACHE=1（诊断 / A-B 对比用）。--rebuild 由调用方绕过。
bool progcache_enabled();

// 缓存指纹用的 64 位文件摘要（按 8 字节字混合，不是密码学哈希）。
// 放在这里是因为 build.cpp 超过规模上限，而 toolchain_id 也要用它。
bool hash_file(const std::string& path, unsigned long long& out);

// 键 = 对象内容（按链接顺序）+ extra（target/toolchain/开关/链接参数）。
// 返回 64 位十六进制；读取任一对象失败时返回空串（= 不可用，照常链接）。
std::string progcache_key(const std::vector<std::string>& objs, const std::string& extra);

// 命中则把缓存产物写到 out（已存在且字节相同则跳过拷贝）。true = 命中。
bool progcache_fetch(const std::string& key, const std::string& out);

// 链接成功后把产物存进缓存（原子写，失败静默）。
void progcache_store(const std::string& key, const std::string& out);

}  // namespace pyadamas
