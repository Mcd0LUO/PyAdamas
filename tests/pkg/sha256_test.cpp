// tests/pkg/sha256_test.cpp —— SHA-256 已知答案测试（FIPS 180-4 / NIST 向量）
//
// 为什么单独测哈希：锁的树哈希是「内容完整性」的信任根。如果 SHA-256 实现错了，
// 「两次 resolve 字节相同」「改源码哈希变了」这些行为测试仍然会全绿（一个常量
// 函数也能满足它们），所以必须有已知答案向量兜底。
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "driver/sha256.h"

namespace {

int failures = 0;

void expect(const std::string& what, const std::string& got, const std::string& want) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(),
                 want.c_str());
    ++failures;
}

void expect_sha(const std::string& what, std::string_view input, const std::string& want) {
    expect(what, pyadamas::sha256_hex(input), want);
}

}  // namespace

int main() {
    // NIST/FIPS 180-4 的经典向量（也覆盖 padding 的三种边界：空、单块、
    // 两块；最后一条一兆字节，覆盖多块 + 流式）。
    expect_sha("sha256(\"\")", "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_sha("sha256(\"abc\")", "abc",
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect_sha("sha256(448-bit msg)",
               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect_sha("sha256(896-bit msg)",
               "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
               "klmnopqrlmnopqrsmnopqrstnopqrstu",
               "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    {
        pyadamas::Sha256 h;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.update(chunk);
        expect("sha256(1e6 x 'a')", h.final_hex(),
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    // 增量喂入必须与一次性摘要一致，覆盖 64 字节块的边界。
    const std::size_t sizes[] = {0, 1, 55, 56, 57, 63, 64, 65, 127, 128, 129, 1000};
    for (const std::size_t n : sizes) {
        std::string data;
        data.reserve(n);
        for (std::size_t i = 0; i < n; ++i) data.push_back(static_cast<char>('a' + (i % 26)));
        pyadamas::Sha256 incremental;
        for (std::size_t i = 0; i < n; i += 7)
            incremental.update(data.data() + i, std::min<std::size_t>(7, n - i));
        expect("chunked len=" + std::to_string(n), incremental.final_hex(),
               pyadamas::sha256_hex(data));
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d sha256 vector(s) failed\n", failures);
        return 1;
    }
    std::printf("sha256 ok (NIST vectors + chunking)\n");
    return 0;
}
