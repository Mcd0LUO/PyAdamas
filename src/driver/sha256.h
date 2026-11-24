// driver/sha256.h —— 最小 SHA-256（包管理锁的源码树哈希）
//
// 为什么自己实现：锁里的树哈希是「内容完整性」的信任根，必须用密码学哈希
// 而不是 FNV（§5.8：FNV 只够本地缓存失效）。但项目有「不引入第三方依赖」
// 的硬约束，OpenSSL/libcrypto 又会新增链接依赖，所以这里放一份独立、
// 无依赖的实现。
//
// 只做流式摘要（文件可能很大，不能整读进内存）。不做 HMAC/签名。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pyadamas {

class Sha256 {
  public:
    Sha256();

    void update(const void* data, std::size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    // 结束摘要并返回 64 位小写十六进制。调用后对象不可复用（需要重来请新建）。
    std::string final_hex();

  private:
    void compress(const unsigned char* block);

    std::uint32_t state_[8];
    unsigned char buf_[64];
    std::size_t buffered_ = 0;
    std::uint64_t total_ = 0;  // 已喂入的字节数（不含 padding）
};

std::string sha256_hex(std::string_view data);

}  // namespace pyadamas
