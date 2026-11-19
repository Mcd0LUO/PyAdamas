/* stdlib/curl_glue.c —— urllib.request 的 libcurl easy API 封装（stdlib2）。
 *
 * 一次调用完成一个 HTTP 请求：init/setopt/perform/getinfo/cleanup 全在
 * C 侧，响应体/响应头用 realloc 增长缓冲收集（write/header 回调），成功
 * 后把缓冲所有权交给 Python 侧（pa_curl_copy 拷进 bytes，pa_curl_free
 * 释放）。Python 只传字符串/缓冲区/出参数组，不碰 CURL 句柄 ABI。
 *
 * out 数组口径：
 *   out[0]=CURLcode  out[1]=HTTP status  out[2]=body len  out[3]=body ptr
 *   out[4]=hdr len   out[5]=hdr ptr      out[6]=err len    out[7]=eff url len
 *   out[8]=连接是否已建立（1=是；超时发生在传输阶段，CPython 抛 TimeoutError）
 * 失败时 body/hdr ptr 为 0（NULL），errbuf 填错误消息。
 */
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* p;
    size_t len;
    size_t cap;
} PaCurlBuf;

static size_t curl_wr_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    PaCurlBuf* b = (PaCurlBuf*)ud;
    size_t n = sz * nm;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc < b->len + n) nc *= 2;
        char* np = (char*)realloc(b->p, nc);
        if (!np) return 0; /* OOM：中止传输（CURLE_WRITE_ERROR） */
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, ptr, n);
    b->len += n;
    return n;
}

int64_t pa_curl_request(const char* url, const char* method,
                        const char* body, int64_t body_len,
                        const char* headers, int64_t timeout_ms, int64_t follow,
                        int64_t* out, char* errbuf, int64_t errcap,
                        char* effbuf, int64_t effcap) {
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0; out[4] = 0; out[5] = 0;
    out[6] = 0; out[7] = 0; out[8] = 0;
    if (errcap > 0) errbuf[0] = 0;
    if (effcap > 0) effbuf[0] = 0;
    CURL* h = curl_easy_init();
    if (!h) {
        out[0] = CURLE_FAILED_INIT;
        return CURLE_FAILED_INIT;
    }
    PaCurlBuf bb = {NULL, 0, 0};
    PaCurlBuf hb = {NULL, 0, 0};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_wr_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &bb);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, curl_wr_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &hb);
    if (errcap >= CURL_ERROR_SIZE)
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (follow) {
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    }
    if (timeout_ms > 0)
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    if (method && method[0]) {
        if (strcmp(method, "HEAD") == 0) {
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        } else {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
        }
    }
    if (body_len >= 0) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }
    struct curl_slist* hl = NULL;
    if (headers && headers[0]) {
        const char* s = headers;
        while (*s) {
            const char* e = strchr(s, '\n');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n > 0) {
                char* line = (char*)malloc(n + 1);
                if (line) {
                    memcpy(line, s, n);
                    line[n] = 0;
                    hl = curl_slist_append(hl, line);
                    free(line);
                }
            }
            if (!e) break;
            s = e + 1;
        }
        if (hl) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    }
    CURLcode rc = curl_easy_perform(h);
    out[0] = (int64_t)rc;
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        out[1] = (int64_t)status;
        char* eff = NULL;
        curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff && effcap > 0) {
            strncpy(effbuf, eff, (size_t)effcap - 1);
            effbuf[effcap - 1] = 0;
        }
        out[2] = (int64_t)bb.len;
        out[3] = (int64_t)(intptr_t)bb.p;
        out[4] = (int64_t)hb.len;
        out[5] = (int64_t)(intptr_t)hb.p;
        out[7] = (int64_t)strlen(effbuf);
        /* 成功：bb.p/hb.p 所有权交调用方（pa_curl_free）。 */
    } else {
        free(bb.p);
        free(hb.p);
        if (errcap > 0 && errbuf[0] == 0) {
            const char* m = curl_easy_strerror(rc);
            strncpy(errbuf, m, (size_t)errcap - 1);
            errbuf[errcap - 1] = 0;
        }
        if (errcap > 0) out[6] = (int64_t)strlen(errbuf);
        double ct = 0.0;
        curl_easy_getinfo(h, CURLINFO_CONNECT_TIME, &ct);
        out[8] = ct > 0.0 ? 1 : 0;
    }
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(h);
    return (int64_t)rc;
}

void pa_curl_copy(char* dst, int64_t src, int64_t n) {
    if (n > 0) memcpy(dst, (void*)(intptr_t)src, (size_t)n);
}

void pa_curl_free(int64_t p) {
    free((void*)(intptr_t)p);
}
