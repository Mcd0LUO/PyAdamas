// rt_file.c —— 文件对象与文本编码
#include "rt_internal.h"

#include <errno.h>
#if defined(_WIN32)
/* Windows 没有 iconv。非 UTF-8 编码在 Windows 上不支持，用恒等桩顶住：
   文件内容按原始字节处理，不做编码转换。 */
typedef void* iconv_t;
#define iconv_open(t, f) ((iconv_t)0)
#define iconv_close(c) (0)
#define iconv(c, i, il, o, ol) ((size_t)-1)
#else
#include <iconv.h>
#endif
#include <unistd.h>

/* ── 文件对象 ─────────────────────────────────────────────────── */

/* newline 参数（与 Python 对应）。 */
enum { PYADAMAS_NL_UNIVERSAL = 0, PYADAMAS_NL_NONE = 1, PYADAMAS_NL_LF = 2, PYADAMAS_NL_CR = 3,
       PYADAMAS_NL_CRLF = 4 };

struct PyAdamasFile {
    FILE* fp;
    int64_t fd;
    int64_t closed;
    int64_t closefd;
    int64_t binary;
    int64_t readable;
    int64_t writable;
    int64_t newline;
    char* vbuf;       /* buffering > 1 时自己分配的全缓冲 */
    PyAdamasStr* name;    /* 拥有 */
    PyAdamasStr* mode;    /* 拥有，规范化后的 Python 风格 mode */
    PyAdamasStr* encoding; /* 拥有，规范化后的编码名 */
    PyAdamasStr* errors;  /* 拥有，strict / ignore / replace */
    iconv_t dec_cd;   /* 源编码 → UTF-8；NULL 表示恒等 */
    iconv_t enc_cd;   /* UTF-8 → 目标编码 */
    /* 解码缓冲：已转成 UTF-8 且做完换行翻译的字节 */
    char* dbuf;
    int64_t dlen;
    int64_t dpos;
    int64_t dcap;
    /* 上一块末尾不完整的多字节序列，留到下一块 */
    char rawleft[16];
    int64_t rawleft_len;
    int64_t pending_cr;
    int64_t eof;
    int64_t text_off;  /* 文本模式的逻辑位置（解码后的 UTF-8 字节数） */
    int64_t raw_base;  /* 当前解码块对应的原始文件偏移 */
};

static void file_destroy(void* p) {
    PyAdamasFile* f = (PyAdamasFile*)p;
    if (f->fp) {
        fclose(f->fp);
        f->fp = NULL;
    }
    free(f->vbuf);
    free(f->dbuf);
    if (f->name) pa_rc_dec(f->name);
    if (f->mode) pa_rc_dec(f->mode);
    if (f->encoding) pa_rc_dec(f->encoding);
    if (f->errors) pa_rc_dec(f->errors);
    if (f->dec_cd) iconv_close(f->dec_cd);
    if (f->enc_cd) iconv_close(f->enc_cd);
}

static char* file_cstr(PyAdamasStr* s) {
    char* p = (char*)malloc((size_t)s->len + 1);
    if (!p) panic_c("out of memory");
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = 0;
    return p;
}

static PyAdamasStr* file_norm_lower(PyAdamasStr* s) {
    PyAdamasStr* out = str_alloc_raw(s->len);
    char* p = (char*)out->data;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        p[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    return str_seal_n(out, s->pub);  /* ASCII 大小写不改变码点数 */
}

static int file_str_is(PyAdamasStr* s, const char* lit) {
    size_t n = strlen(lit);
    return s && s->len == (int64_t)n && memcmp(s->data, lit, n) == 0;
}

PyAdamasFile* pa_file_open(PyAdamasStr* path, int64_t fd, PyAdamasStr* mode, int64_t buffering,
                         int64_t newline, int64_t closefd) {
    char base = 0;
    int plus = 0, bin = 0, text = 0;
    const char* mp = mode ? mode->data : "r";
    int64_t mlen = mode ? mode->len : 1;
    for (int64_t i = 0; i < mlen; ++i) {
        char c = mp[i];
        if (c == 'r' || c == 'w' || c == 'a' || c == 'x') {
            if (base)
                pa_raise_c("ValueError", "invalid mode: exactly one of r/w/a/x expected");
            base = c;
        } else if (c == '+') {
            plus = 1;
        } else if (c == 'b') {
            bin = 1;
        } else if (c == 't') {
            text = 1;
        } else {
            pa_raise_c("ValueError", "invalid open() mode");
        }
    }
    if (!base) pa_raise_c("ValueError", "invalid open() mode");
    if (bin && text) pa_raise_c("ValueError", "can't have text and binary mode at once");
    if (buffering == 0 && !bin)
        pa_raise_c("ValueError", "can't have unbuffered text I/O");

    char cmode[8];
    int cn = 0;
    cmode[cn++] = base;
    if (plus) cmode[cn++] = '+';
    if (bin) cmode[cn++] = 'b';
    cmode[cn] = 0;

    FILE* fp = NULL;
    int64_t fdnum = fd;
    if (fd >= 0) {
        int fdc = (int)fd;
        if (!closefd) {
            fdc = dup(fdc);
            if (fdc < 0) pa_raise_c("OSError", "bad file descriptor");
        }
        fdnum = fdc;
        fp = fdopen(fdc, cmode);
        if (!fp) pa_raise_c("OSError", "cannot open file descriptor");
    } else {
        char* p = file_cstr(path);
        if (base == 'x') {
            char xmode[10];
            int xn = 0;
            xmode[xn++] = 'w';
            xmode[xn++] = 'x';
            if (plus) xmode[xn++] = '+';
            if (bin) xmode[xn++] = 'b';
            xmode[xn] = 0;
            fp = fopen(p, xmode);
        } else {
            fp = fopen(p, cmode);
        }
        free(p);
        if (!fp) pa_raise_c("OSError", "cannot open file");
        fdnum = fileno(fp);
    }

    PyAdamasFile* f =
        (PyAdamasFile*)rc_alloc((int64_t)sizeof(PyAdamasFile), file_destroy, NULL, PYADAMAS_RC_OBJ);
    memset(f, 0, sizeof *f);
    /* 下面三次 str_new（mode/encoding/errors）都会走 rc_alloc，f 此时只在
       本帧 C 局部变量里，先 pin 住，否则主收集落在这里会把它清扫掉。 */
    pa_gc_pin(f);
    f->fp = fp;
    f->fd = fdnum;
    f->closefd = closefd;
    f->binary = bin;
    f->readable = base == 'r' || plus;
    f->writable = base != 'r' || plus;
    f->newline = bin ? PYADAMAS_NL_NONE : (newline < 0 || newline > 4 ? 0 : newline);
    f->name = path;
    if (f->name) pa_rc_inc(f->name);
    {
        char nb[4];
        int nn = 0;
        nb[nn++] = base;
        if (plus) nb[nn++] = '+';
        if (bin) nb[nn++] = 'b';
        f->mode = str_new(nb, nn);
    }
    f->encoding = str_new("utf-8", 5);
    f->errors = str_new("strict", 6);
    pa_gc_unpin(f);
    if (buffering == 0) {
        setvbuf(fp, NULL, _IONBF, 0);
    } else if (buffering == 1) {
        setvbuf(fp, NULL, _IOLBF, 0);
    } else if (buffering > 1) {
        f->vbuf = (char*)malloc((size_t)buffering);
        if (!f->vbuf) panic_c("out of memory");
        setvbuf(fp, f->vbuf, _IOFBF, (size_t)buffering);
    }
    return f;
}

/* encoding/errors 必须在任何 I/O 之前配置；未知编码抛 LookupError。 */
void pa_file_config(PyAdamasFile* f, PyAdamasStr* enc, PyAdamasStr* errors) {
    if (!f) return;
    if (f->binary) {
        if (enc || errors)
            pa_raise_c("ValueError",
                         "binary mode doesn't take an encoding/errors argument");
        return;
    }
    PyAdamasStr* e = enc ? file_norm_lower(enc) : str_new("utf-8", 5);
    /* er 的分配可能触发主收集，而 e 此时只在 C 局部变量里。 */
    pa_gc_pin(e);
    PyAdamasStr* er = errors ? file_norm_lower(errors) : str_new("strict", 6);
    pa_gc_unpin(e);
    if (!file_str_is(er, "strict") && !file_str_is(er, "ignore") &&
        !file_str_is(er, "replace")) {
        pa_rc_dec(e);
        pa_rc_dec(er);
        pa_raise_c("LookupError", "unknown error handler name");
    }
    iconv_t d = (iconv_t)0;
    iconv_t en = (iconv_t)0;
    {
        /* Python 的编码名和 glibc iconv 不完全一致（如 "latin-1"）。
           依次尝试：原名、去分隔符大写名、常见别名映射。UTF-8 也走
           iconv：这样非法字节会按 errors 策略处理，和 CPython 一致。 */
        char norm[64];
        int nn = 0;
        for (int64_t i = 0; i < e->len && nn < 63; ++i) {
            char c = e->data[i];
            if (c == '-' || c == '_' || c == ' ') continue;
            norm[nn++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        norm[nn] = 0;
        const char* mapped = NULL;
        if (!strcmp(norm, "LATIN1") || !strcmp(norm, "L1") || !strcmp(norm, "ISO88591") ||
            !strcmp(norm, "88591"))
            mapped = "ISO-8859-1";
        else if (!strcmp(norm, "UTF16"))
            mapped = "UTF-16";
        else if (!strcmp(norm, "UTF16LE"))
            mapped = "UTF-16LE";
        else if (!strcmp(norm, "UTF16BE"))
            mapped = "UTF-16BE";
        else if (!strcmp(norm, "UTF32"))
            mapped = "UTF-32";
        else if (!strcmp(norm, "UTF32LE"))
            mapped = "UTF-32LE";
        else if (!strcmp(norm, "UTF32BE"))
            mapped = "UTF-32BE";
        else if (!strcmp(norm, "ASCII") || !strcmp(norm, "USASCII") || !strcmp(norm, "646"))
            mapped = "ASCII";
        const char* cands[3];
        int nc = 0;
        char* orig = file_cstr(e);
        cands[nc++] = orig;
        cands[nc++] = norm;
        if (mapped) cands[nc++] = mapped;
        for (int ci = 0; ci < nc && d == (iconv_t)0; ++ci) {
            iconv_t cd = iconv_open("UTF-8", cands[ci]);
            if (cd == (iconv_t)-1) continue;
            iconv_t ce = iconv_open(cands[ci], "UTF-8");
            if (ce == (iconv_t)-1) {
                iconv_close(cd);
                continue;
            }
            d = cd;
            en = ce;
        }
        free(orig);
        if (d == (iconv_t)0) {
            pa_rc_dec(e);
            pa_rc_dec(er);
            pa_raise_c("LookupError", "unknown encoding");
        }
    }
    if (f->dec_cd) iconv_close(f->dec_cd);
    if (f->enc_cd) iconv_close(f->enc_cd);
    f->dec_cd = d;
    f->enc_cd = en;
    if (f->encoding) pa_rc_dec(f->encoding);
    if (f->errors) pa_rc_dec(f->errors);
    f->encoding = e;
    f->errors = er;
}

static FILE* file_check(PyAdamasFile* f) {
    if (!f || !f->fp || f->closed)
        pa_raise_c("ValueError", "I/O operation on closed file");
    return f->fp;
}

/* ── 文本模式的解码缓冲 ───────────────────────────────────────── */

static void file_dbuf_append(PyAdamasFile* f, const char* p, int64_t n, int translate) {
    if (n <= 0) return;
    if (f->dlen + n > f->dcap) {
        int64_t ncap = f->dcap ? f->dcap : 8192;
        while (ncap < f->dlen + n) ncap *= 2;
        char* nb = (char*)realloc(f->dbuf, (size_t)ncap);
        if (!nb) panic_c("out of memory");
        f->dbuf = nb;
        f->dcap = ncap;
    }
    char* out = f->dbuf + f->dlen;
    if (!translate) {
        memcpy(out, p, (size_t)n);
        f->dlen += n;
        return;
    }
    /* 通用换行：\r\n 与单独 \r 折成 \n，跨块用 pending_cr 挂起 */
    for (int64_t i = 0; i < n; ++i) {
        char c = p[i];
        if (f->pending_cr) {
            f->pending_cr = 0;
            out[f->dlen++] = '\n';
            if (c == '\n') continue;
        }
        if (c == '\r') {
            f->pending_cr = 1;
            continue;
        }
        out[f->dlen++] = c;
    }
}

/* 把一块原始字节转成 UTF-8 并追加到解码缓冲；返回 1 表示读到了数据。 */
static int file_fill(PyAdamasFile* f) {
    if (f->eof) return 0;
    f->dpos = f->dlen = 0;
    char raw[8192];
    int64_t rn = 0;
    if (f->rawleft_len) {
        memcpy(raw, f->rawleft, (size_t)f->rawleft_len);
        rn = f->rawleft_len;
        f->rawleft_len = 0;
    }
    size_t want = sizeof raw - (size_t)rn;
    size_t got = fread(raw + rn, 1, want, f->fp);
    int64_t total = rn + (int64_t)got;
    f->raw_base = (int64_t)ftell(f->fp) - total;
    if (total == 0) {
        if (f->pending_cr) {
            f->pending_cr = 0;
            file_dbuf_append(f, "\n", 1, 0);
        }
        f->eof = 1;
        return f->dlen > 0;
    }
    int translate = !f->binary && f->newline == PYADAMAS_NL_UNIVERSAL;
    if (!f->dec_cd) {
        file_dbuf_append(f, raw, total, translate);
    } else {
        char outbuf[16384];
        char* in = raw;
        size_t inleft = (size_t)total;
        for (;;) {
            char* outp = outbuf;
            size_t outleft = sizeof outbuf;
            size_t r = iconv(f->dec_cd, &in, &inleft, &outp, &outleft);
            file_dbuf_append(f, outbuf, (int64_t)(sizeof outbuf - outleft), translate);
            if (r != (size_t)-1) break;
            if (errno == E2BIG) continue;
            if (errno == EINVAL) {
                /* 不完整的多字节序列：留到下一块 */
                if (inleft > 0 && inleft <= sizeof f->rawleft) {
                    memcpy(f->rawleft, in, inleft);
                    f->rawleft_len = (int64_t)inleft;
                }
                break;
            }
            if (errno == EILSEQ) {
                if (file_str_is(f->errors, "strict"))
                    pa_raise_c("UnicodeDecodeError", "invalid byte sequence");
                if (file_str_is(f->errors, "replace"))
                    file_dbuf_append(f, "\xEF\xBF\xBD", 3, translate);
                ++in;
                --inleft;
                continue;
            }
            break;
        }
    }
    if (got < want) f->eof = 1;
    return f->dlen > 0;
}

static int file_takec(PyAdamasFile* f) {
    if (f->dpos >= f->dlen && !file_fill(f)) return -1;
    ++f->text_off;
    return (unsigned char)f->dbuf[f->dpos++];
}

PyAdamasStr* pa_file_read(PyAdamasFile* f, int64_t n) {
    FILE* fp = file_check(f);
    if (f->binary) {
        char* buf = NULL;
        int64_t len = 0, cap = 0;
        int64_t remain = n < 0 ? INT64_MAX : n;
        char chunk[4096];
        while (remain > 0) {
            size_t want = remain < (int64_t)sizeof chunk ? (size_t)remain : sizeof chunk;
            size_t got = fread(chunk, 1, want, fp);
            if (got == 0) break;
            if (len + (int64_t)got > cap) {
                int64_t ncap = cap ? cap : 8192;
                while (ncap < len + (int64_t)got) ncap *= 2;
                char* nb = (char*)realloc(buf, (size_t)ncap);
                if (!nb) panic_c("out of memory");
                buf = nb;
                cap = ncap;
            }
            memcpy(buf + len, chunk, got);
            len += (int64_t)got;
            if (n >= 0) remain -= (int64_t)got;
            if (got < want) break;
        }
        PyAdamasStr* s = str_new_bytes(buf ? buf : "", len);
        free(buf);
        return s;
    }
    char* out = NULL;
    int64_t len = 0, cap = 0;
    while (n < 0 || len < n) {
        if (f->dpos >= f->dlen && !file_fill(f)) break;
        int64_t avail = f->dlen - f->dpos;
        int64_t take = (n < 0 || avail <= n - len) ? avail : n - len;
        if (len + take > cap) {
            int64_t ncap = cap ? cap : 8192;
            while (ncap < len + take) ncap *= 2;
            char* nb = (char*)realloc(out, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            out = nb;
            cap = ncap;
        }
        memcpy(out + len, f->dbuf + f->dpos, (size_t)take);
        f->dpos += take;
        f->text_off += take;
        len += take;
    }
    PyAdamasStr* s = str_new(out ? out : "", len);
    free(out);
    return s;
}

PyAdamasStr* pa_file_readline(PyAdamasFile* f, int64_t size) {
    FILE* fp = file_check(f);
    if (f->binary) {
        char* buf = NULL;
        int64_t len = 0, cap = 0;
        int c;
        while ((size < 0 || len < size) && (c = fgetc(fp)) != EOF) {
            if (len + 1 > cap) {
                int64_t ncap = cap ? cap * 2 : 128;
                char* nb = (char*)realloc(buf, (size_t)ncap);
                if (!nb) panic_c("out of memory");
                buf = nb;
                cap = ncap;
            }
            buf[len++] = (char)c;
            if (c == '\n') break;
        }
        PyAdamasStr* s = str_new_bytes(buf ? buf : "", len);
        free(buf);
        return s;
    }
    char* buf = NULL;
    int64_t len = 0, cap = 0;
    for (;;) {
        if (size >= 0 && len >= size) break;
        int c = file_takec(f);
        if (c < 0) break;
        if (len + 1 > cap) {
            int64_t ncap = cap ? cap * 2 : 128;
            char* nb = (char*)realloc(buf, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    PyAdamasStr* s = str_new(buf ? buf : "", len);
    free(buf);
    return s;
}

PyAdamasList* pa_file_readlines(PyAdamasFile* f, int64_t hint) {
    PyAdamasList* out = pa_list_new(4, 1);
    /* 每行一次 pa_file_readline（内部 str_new）；out 只在本帧 C 局部变量里。 */
    pa_gc_pin(out);
    int64_t total = 0;
    for (;;) {
        PyAdamasStr* line = pa_file_readline(f, -1);
        if (line->len == 0) break;
        pa_rc_inc(line);
        pa_list_append(out, (int64_t)line, 2);
        total += line->len;
        if (hint > 0 && total >= hint) break;
    }
    pa_gc_unpin(out);
    return out;
}

/* ── 写入 ─────────────────────────────────────────────────────── */

/* 写侧换行翻译：newline="\r" / "\r\n" 时把 \n 换掉。 */
static PyAdamasStr* file_translate_write(PyAdamasFile* f, PyAdamasStr* s) {
    if ((f->newline != PYADAMAS_NL_CR && f->newline != PYADAMAS_NL_CRLF)) return s;
    if (f->newline == PYADAMAS_NL_CR) {
        PyAdamasStr* out = str_new(s->data, s->len);
        for (int64_t i = 0; i < out->len; ++i)
            if (((char*)out->data)[i] == '\n') ((char*)out->data)[i] = '\r';
        return out;
    }
    int64_t extra = 0;
    for (int64_t i = 0; i < s->len; ++i)
        if (s->data[i] == '\n') ++extra;
    PyAdamasStr* out = str_alloc_raw(s->len + extra);
    char* p = (char*)out->data;
    int64_t j = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if (s->data[i] == '\n') {
            p[j++] = '\r';
            p[j++] = '\n';
        } else {
            p[j++] = s->data[i];
        }
    }
    return str_seal_n(out, s->pub + extra);  /* 每个 \n 换成 \r\n 只多一个 ASCII */
}

// 返回实际写出的字节数；出错按 errors 策略处理。
static void file_write_bytes(PyAdamasFile* f, const char* p, int64_t n) {
    if (n <= 0) return;
    if (!f->enc_cd) {
        fwrite(p, 1, (size_t)n, f->fp);
        return;
    }
    char outbuf[16384];
    char* in = (char*)p;
    size_t inleft = (size_t)n;
    for (;;) {
        char* outp = outbuf;
        size_t outleft = sizeof outbuf;
        size_t r = iconv(f->enc_cd, &in, &inleft, &outp, &outleft);
        size_t produced = sizeof outbuf - outleft;
        if (produced) fwrite(outbuf, 1, produced, f->fp);
        if (r != (size_t)-1) break;
        if (errno == E2BIG) continue;
        if (errno == EINVAL) { /* 输入不是完整的 UTF-8 序列 */
            if (file_str_is(f->errors, "strict"))
                pa_raise_c("UnicodeEncodeError", "invalid UTF-8 input");
            break;
        }
        if (errno == EILSEQ) {
            if (file_str_is(f->errors, "strict"))
                pa_raise_c("UnicodeEncodeError",
                             "character cannot be encoded in the target encoding");
            if (file_str_is(f->errors, "replace")) fwrite("?", 1, 1, f->fp);
            ++in;
            --inleft;
            continue;
        }
        break;
    }
}

/* 把底层句柄退回到逻辑位置并清空解码缓冲。文本模式预读会让 ftell
   领先于逻辑位置，写/截断之前必须先对齐。 */
static void file_sync_raw(PyAdamasFile* f) {
    if (f->binary) return;
    if (!(f->dpos < f->dlen || f->rawleft_len || f->pending_cr)) return;
    int64_t raw_target = f->raw_base + f->dpos + f->rawleft_len;
    if (fseek(f->fp, (long)raw_target, SEEK_SET) != 0)
        pa_raise_c("OSError", "seek failed");
    f->dlen = f->dpos = 0;
    f->rawleft_len = 0;
    f->pending_cr = 0;
    f->eof = 0;
    f->raw_base = raw_target;
    if (f->dec_cd) iconv(f->dec_cd, NULL, NULL, NULL, NULL);
    if (f->enc_cd) iconv(f->enc_cd, NULL, NULL, NULL, NULL);
}

int64_t pa_file_write(PyAdamasFile* f, PyAdamasStr* s) {
    FILE* fp = file_check(f);
    if (f->binary) {
        return (int64_t)fwrite(s->data, 1, (size_t)s->len, fp);
    }
    file_sync_raw(f);
    PyAdamasStr* t = file_translate_write(f, s);
    file_write_bytes(f, t->data, t->len);
    if (t != s) pa_rc_dec(t);
    f->text_off += s->len;
    return s->len;
}

void pa_file_writelines(PyAdamasFile* f, PyAdamasList* lines) {
    for (int64_t i = 0; i < lines->len; ++i)
        pa_file_write(f, (PyAdamasStr*)lines->data[i]);
}

/* 文本定位：位置按解码后的 UTF-8 字节计。从文件头重解码并丢弃前 target
   个字节；这是 O(n) 但和 tell 的返回值自洽，也与 CPython 的 cookie 同形。 */
static void file_text_reset(PyAdamasFile* f) {
    fflush(f->fp);
    if (fseek(f->fp, 0, SEEK_SET) != 0)
        pa_raise_c("OSError", "seek failed");
    f->dlen = f->dpos = 0;
    f->rawleft_len = 0;
    f->pending_cr = 0;
    f->eof = 0;
    f->text_off = 0;
    f->raw_base = 0;
    if (f->dec_cd) iconv(f->dec_cd, NULL, NULL, NULL, NULL);
    if (f->enc_cd) iconv(f->enc_cd, NULL, NULL, NULL, NULL);
}

static int64_t file_text_skip(PyAdamasFile* f, int64_t target) {
    while (f->text_off < target) {
        if (f->dpos >= f->dlen && !file_fill(f)) break;
        int64_t take = target - f->text_off;
        int64_t avail = f->dlen - f->dpos;
        if (take > avail) take = avail;
        f->dpos += take;
        f->text_off += take;
    }
    return f->text_off;
}

int64_t pa_file_seek(PyAdamasFile* f, int64_t off, int64_t whence) {
    FILE* fp = file_check(f);
    if (!f->binary) {
        int64_t target;
        if (whence == 2) {
            file_text_reset(f);
            int64_t total = file_text_skip(f, INT64_MAX);
            target = total + off;
            if (target < 0) pa_raise_c("OSError", "negative seek position");
            if (off != 0) {
                file_text_reset(f);
                file_text_skip(f, target);
            }
        } else {
            int64_t cur = whence == 1 ? f->text_off : 0;
            target = cur + off;
            if (target < 0) pa_raise_c("OSError", "negative seek position");
            file_text_reset(f);
            file_text_skip(f, target);
        }
        return f->text_off;
    }
    int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    if (fseek(fp, (long)off, w) != 0) pa_raise_c("OSError", "seek failed");
    f->dlen = f->dpos = 0;
    f->rawleft_len = 0;
    f->pending_cr = 0;
    f->eof = 0;
    f->text_off = 0;
    if (f->dec_cd) iconv(f->dec_cd, NULL, NULL, NULL, NULL);
    if (f->enc_cd) iconv(f->enc_cd, NULL, NULL, NULL, NULL);
    return (int64_t)ftell(fp);
}

int64_t pa_file_tell(PyAdamasFile* f) {
    FILE* fp = file_check(f);
    if (f->binary) return (int64_t)ftell(fp);
    return f->text_off;
}

int64_t pa_file_truncate(PyAdamasFile* f, int64_t size) {
    FILE* fp = file_check(f);
    /* CPython 无参 truncate 用底层缓冲位置（文本预读后是原始偏移）；
       显式 size 按字节截断，先把逻辑位置对齐到底层句柄。 */
    if (size < 0) {
        size = (int64_t)ftell(fp);
    } else {
        file_sync_raw(f);
    }
    fflush(fp);
    if (ftruncate(fileno(fp), (off_t)size) != 0)
        pa_raise_c("OSError", "truncate failed");
    return size;
}

int64_t pa_file_fileno(PyAdamasFile* f) { return (int64_t)fileno(file_check(f)); }
int64_t pa_file_isatty(PyAdamasFile* f) { return isatty(fileno(file_check(f))) ? 1 : 0; }
int64_t pa_file_readable(PyAdamasFile* f) { return f ? f->readable : 0; }
int64_t pa_file_writable(PyAdamasFile* f) { return f ? f->writable : 0; }
int64_t pa_file_seekable(PyAdamasFile* f) {
    if (!f || !f->fp || f->closed) return 0;
    return fseek(f->fp, 0, SEEK_CUR) == 0 ? 1 : 0;
}
int64_t pa_file_closed(PyAdamasFile* f) { return f ? f->closed : 1; }

PyAdamasStr* pa_file_name(PyAdamasFile* f) {
    if (!f) return str_new("", 0);
    if (f->name) return str_new(f->name->data, f->name->len);
    return pa_str_from_i64(f->fd);
}

PyAdamasStr* pa_file_mode(PyAdamasFile* f) {
    if (!f || !f->mode) return str_new("", 0);
    return str_new(f->mode->data, f->mode->len);
}

PyAdamasStr* pa_file_encoding(PyAdamasFile* f) {
    if (!f || !f->encoding) return str_new("", 0);
    return str_new(f->encoding->data, f->encoding->len);
}

PyAdamasStr* pa_file_errors(PyAdamasFile* f) {
    if (!f || !f->errors) return str_new("", 0);
    return str_new(f->errors->data, f->errors->len);
}

void pa_file_close(PyAdamasFile* f) {
    if (!f || !f->fp) return;
    fclose(f->fp);
    f->fp = NULL;
    f->closed = 1;
}

void pa_file_flush(PyAdamasFile* f) { fflush(file_check(f)); }

PyAdamasStr* pa_input(PyAdamasStr* prompt) {
    if (prompt && prompt->len > 0) {
        fwrite(prompt->data, 1, (size_t)prompt->len, stdout);
        fflush(stdout);
    }
    char* buf = NULL;
    int64_t len = 0, cap = 0;
    int c;
    int got_any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got_any = 1;
        if (c == '\n') break;
        if (len + 1 > cap) {
            int64_t ncap = cap ? cap * 2 : 128;
            char* nb = (char*)realloc(buf, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (!got_any) {
        free(buf);
        pa_raise_c("EOFError", "EOF when reading a line");
    }
    if (len > 0 && buf[len - 1] == '\r') --len;
    PyAdamasStr* s = str_new(buf ? buf : "", len);
    free(buf);
    return s;
}

/* sys.stdin / sys.stdout / sys.stderr：直接包住 C 标准流（不 dup），
   这样和 print 共用同一个 FILE*，缓冲顺序一致；对象永久存活，进程退出
   时不会被析构掉。close() 会像 CPython 一样关掉底层流。 */
static PyAdamasFile* sys_stream_files[3];

PyAdamasFile* pa_sys_stream(int64_t which) {
    if (which < 0 || which > 2) return NULL;
    PyAdamasFile* cached = sys_stream_files[which];
    if (cached) return cached;
    FILE* fp = which == 0 ? stdin : which == 1 ? stdout : stderr;
    PyAdamasFile* f =
        (PyAdamasFile*)rc_alloc((int64_t)sizeof(PyAdamasFile), file_destroy, NULL, PYADAMAS_RC_OBJ);
    /* 下面的 name/mode 两次 str_new 都可能触发主收集：f 只在 C 局部变量里，
       而且它的 rc 还没被改成 IMMORTAL。 */
    pa_gc_pin(f);
    f->fp = fp;
    f->fd = (int64_t)which;
    f->closed = 0;
    f->closefd = 1;
    f->binary = 0;
    f->readable = which == 0;
    f->writable = which != 0;
    f->newline = PYADAMAS_NL_UNIVERSAL;
    f->vbuf = NULL;
    f->name = str_new(which == 0 ? "<stdin>" : which == 1 ? "<stdout>" : "<stderr>",
                      which == 0 ? 7 : which == 1 ? 8 : 8);
    f->mode = str_new(which == 0 ? "r" : "w", 1);
    pa_gc_unpin(f);  // f 的 rc 改成 IMMORTAL 之前撤销 pin（此处 dec 是空操作）
    ((PyAdamasRc*)f - 1)->rc = PYADAMAS_RC_IMMORTAL;
    sys_stream_files[which] = f;
    return f;
}

