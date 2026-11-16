// rt_hash.c —— sha256 的 C 实现，供 stdlib/hashlib.py 通过 intrinsic 调用。
//
// hashlib 的纯 PyAdamas 版本对每个字节做 Python 级运算，实测比 CPython 的
// C 实现慢 5 倍左右；这里把压缩函数下沉到运行时，接口保持最小：
//   __pa_sha256(data: bytes) -> bytes（32 字节摘要）
#include "rt_internal.h"

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t x = w[i - 15];
        uint32_t y = w[i - 2];
        uint32_t s0 = rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
        uint32_t s1 = rotr32(y, 17) ^ rotr32(y, 19) ^ (y >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

PyAdamasStr* pa_sha256_digest(PyAdamasStr* data) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const uint8_t* p = (const uint8_t*)(data && data->data ? data->data : "");
    int64_t n = data ? data->len : 0;
    uint64_t total_bits = (uint64_t)n * 8;

    int64_t off = 0;
    while (n - off >= 64) {
        sha256_block(h, p + off);
        off += 64;
    }

    uint8_t tail[128];
    int64_t rem = n - off;
    if (rem > 0) memcpy(tail, p + off, (size_t)rem);
    tail[rem] = 0x80;
    int64_t pad = (rem + 1 + 8 <= 64) ? 64 : 128;
    memset(tail + rem + 1, 0, (size_t)(pad - rem - 1));
    for (int i = 0; i < 8; ++i)
        tail[pad - 1 - i] = (uint8_t)(total_bits >> (8 * i));
    sha256_block(h, tail);
    if (pad == 128) sha256_block(h, tail + 64);

    PyAdamasStr* out = str_alloc_raw(32);
    char* d = (char*)out->data;
    for (int i = 0; i < 8; ++i) {
        d[i * 4] = (char)(h[i] >> 24);
        d[i * 4 + 1] = (char)(h[i] >> 16);
        d[i * 4 + 2] = (char)(h[i] >> 8);
        d[i * 4 + 3] = (char)h[i];
    }
    return str_seal_n(out, 32);  /* digest 是 bytes：pub == 字节数 */
}

/* 直接返回小写十六进制串，省掉 Python 侧的逐字节转换。 */
PyAdamasStr* pa_sha256_hex(PyAdamasStr* data) {
    static const char kHex[] = "0123456789abcdef";
    PyAdamasStr* dig = pa_sha256_digest(data);
    /* digest 和 hex 串是两次分配，dig 只在本帧 C 局部变量里。 */
    pa_gc_pin(dig);
    PyAdamasStr* out = str_alloc_raw(64);
    char* d = (char*)out->data;
    for (int i = 0; i < 32; ++i) {
        unsigned v = (unsigned char)dig->data[i];
        d[i * 2] = kHex[v >> 4];
        d[i * 2 + 1] = kHex[v & 15];
    }
    str_seal_n(out, 64);  /* hex 串是 ASCII */
    pa_gc_unpin(dig);
    pa_rc_dec(dig);
    return out;
}
