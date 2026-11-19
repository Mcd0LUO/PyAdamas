/* gzip_glue.c —— raw deflate/inflate 的 C 侧胶水（stdlib/gzip.py 用）。
 *
 * z_stream 完全留在 C 侧：Python 只传 bytes 缓冲区与 list[int] 出参，
 * 避免在 .cpya 里复刻 z_stream 的指针/回调字段 ABI。链接时由 cc 现编
 * （cimport 的 link "gzip_glue.c"），配合 link z。
 *
 * 两个入口都按 raw deflate（windowBits = -MAX_WBITS）工作：gzip 的
 * 头/尾由 Python 侧按 CPython gzip 模块的口径组装/校验，字节级可控。
 */
#include <zlib.h>
#include <stdint.h>
#include <string.h>

/* 输入分块上限：uInt 是 32 位，留一半余量。 */
#define PA_CHUNK 0x7FFFFFFF

/* raw deflate。成功返回写入字节数；dst 不够返回 -1（调用方扩容重试，
 * 从头重新压缩）；其它负值是 zlib 错误码。level 0..9。 */
int64_t pa_gzip_deflate_raw(const void* src, int64_t src_len,
                            void* dst, int64_t dst_cap,
                            int64_t level, int64_t* out_len) {
    z_stream s;
    memset(&s, 0, sizeof s);
    int ret = deflateInit2(&s, (int)level, Z_DEFLATED, -MAX_WBITS, 8,
                           Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return ret;
    const unsigned char* p = (const unsigned char*)src;
    int64_t remain = src_len;
    int64_t produced = 0;
    for (;;) {
        uInt chunk = remain > PA_CHUNK ? PA_CHUNK : (uInt)remain;
        s.next_in = (Bytef*)p;
        s.avail_in = chunk;
        s.next_out = (Bytef*)dst + produced;
        s.avail_out = (uInt)(dst_cap - produced);
        int flush = (remain - chunk == 0) ? Z_FINISH : Z_NO_FLUSH;
        ret = deflate(&s, flush);
        produced = (int64_t)s.total_out;
        if (ret == Z_STREAM_END) {
            *out_len = produced;
            deflateEnd(&s);
            return produced;
        }
        if (ret != Z_OK) {
            /* Z_BUF_ERROR 且输出已满：缓冲区不够，让调用方扩容。 */
            *out_len = produced;
            deflateEnd(&s);
            if (ret == Z_BUF_ERROR && s.avail_out == 0) return -1;
            return ret;
        }
        if (s.avail_out == 0 && remain - chunk > 0) {
            *out_len = produced;
            deflateEnd(&s);
            return -1;
        }
        p += chunk;
        remain -= chunk;
        if (remain == 0 && flush == Z_FINISH) {
            /* 输出恰好满且流已结束不了：扩容重试。 */
            *out_len = produced;
            deflateEnd(&s);
            return -1;
        }
    }
}

/* raw inflate。返回 Z_STREAM_END(1)＝完整解出；Z_OK(0) 或 Z_BUF_ERROR(-5)
 * 且 out_len == dst_cap＝输出缓冲不够（调用方扩容重试）；其它负值＝错误
 * （截断/损坏）。out_len 写出字节数，in_used 写出消耗的输入字节数
 * （多 member gzip 靠它定位下一个 member）。 */
int64_t pa_gzip_inflate_raw(const void* src, int64_t src_len,
                            void* dst, int64_t dst_cap,
                            int64_t* out_len, int64_t* in_used) {
    z_stream s;
    memset(&s, 0, sizeof s);
    int ret = inflateInit2(&s, -MAX_WBITS);
    if (ret != Z_OK) return ret;
    const unsigned char* p = (const unsigned char*)src;
    int64_t remain = src_len;
    for (;;) {
        uInt chunk = remain > PA_CHUNK ? PA_CHUNK : (uInt)remain;
        s.next_in = (Bytef*)p;
        s.avail_in = chunk;
        s.next_out = (Bytef*)dst + s.total_out;
        s.avail_out = (uInt)(dst_cap - (int64_t)s.total_out);
        int flush = (remain - chunk == 0) ? Z_FINISH : Z_NO_FLUSH;
        ret = inflate(&s, flush);
        *out_len = (int64_t)s.total_out;
        *in_used = (int64_t)s.total_in;
        if (ret == Z_STREAM_END || ret == Z_OK) {
            inflateEnd(&s);
            return ret;
        }
        if (ret == Z_BUF_ERROR) {
            inflateEnd(&s);
            /* 输出满 → 扩容信号；否则是真错误（输入截断）。 */
            return *out_len == dst_cap ? Z_OK : ret;
        }
        inflateEnd(&s);
        return ret;
    }
}

/* zlib 格式（windowBits=MAX_WBITS）inflate：zlib.decompress 用。
   返回口径同 pa_gzip_inflate_raw：1 完成 / 0 输出满 / 负值错误。
   截断流返回 Z_BUF_ERROR(-5)，与 CPython 的 -5 消息对齐。 */
int64_t pa_zlib_inflate(const void* src, int64_t src_len,
                        void* dst, int64_t dst_cap,
                        int64_t* out_len, int64_t* in_used) {
    z_stream s;
    memset(&s, 0, sizeof s);
    int ret = inflateInit2(&s, MAX_WBITS);
    if (ret != Z_OK) return ret;
    const unsigned char* p = (const unsigned char*)src;
    int64_t remain = src_len;
    for (;;) {
        uInt chunk = remain > PA_CHUNK ? PA_CHUNK : (uInt)remain;
        s.next_in = (Bytef*)p;
        s.avail_in = chunk;
        s.next_out = (Bytef*)dst + s.total_out;
        s.avail_out = (uInt)(dst_cap - (int64_t)s.total_out);
        int flush = (remain - chunk == 0) ? Z_FINISH : Z_NO_FLUSH;
        ret = inflate(&s, flush);
        *out_len = (int64_t)s.total_out;
        *in_used = (int64_t)s.total_in;
        if (ret == Z_STREAM_END || ret == Z_OK) {
            inflateEnd(&s);
            return ret;
        }
        if (ret == Z_BUF_ERROR) {
            inflateEnd(&s);
            return *out_len == dst_cap ? Z_OK : ret;
        }
        inflateEnd(&s);
        return ret;
    }
}
