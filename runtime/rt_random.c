// rt_random.c —— random 模块：与 CPython 逐位一致的 MT19937
//
// 种子、random()、getrandbits()、_randbelow() 全部按 CPython
// _randommodule.c 的算法实现，同一个整数种子产生同样的序列，
// 差分用例在两端生成时可以直接复现。
#include "rt_internal.h"

#include <unistd.h>

#define MT_N 624
#define MT_M 397
#define MT_MATRIX_A 0x9908b0dfU
#define MT_UPPER_MASK 0x80000000U
#define MT_LOWER_MASK 0x7fffffffU

struct PyAdamasRandom {
    uint32_t mt[MT_N];
    int64_t index;
};

static void mt_init_genrand(PyAdamasRandom* r, uint32_t s) {
    r->mt[0] = s;
    for (uint32_t i = 1; i < MT_N; ++i)
        r->mt[i] = 1812433253U * (r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) + i;
    r->index = MT_N;
}

/* CPython 的 init_by_array：把种子拆成 32 位字后做双混洗。 */
static void mt_init_by_array(PyAdamasRandom* r, const uint32_t* key, int64_t key_len) {
    mt_init_genrand(r, 19650218U);
    int64_t i = 1, j = 0;
    int64_t k = MT_N > key_len ? MT_N : key_len;
    for (; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1664525U)) +
                   key[j] + (uint32_t)j;
        i++;
        j++;
        if (i >= MT_N) {
            r->mt[0] = r->mt[MT_N - 1];
            i = 1;
        }
        if (j >= key_len) j = 0;
    }
    for (k = MT_N - 1; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1566083941U)) -
                   (uint32_t)i;
        i++;
        if (i >= MT_N) {
            r->mt[0] = r->mt[MT_N - 1];
            i = 1;
        }
    }
    r->mt[0] = 0x80000000U;
}

/* CPython seed(int, version=2)：abs(int) 按 32 位小端拆字，0 用 [0]。 */
static void mt_seed_i64(PyAdamasRandom* r, int64_t seed) {
    uint64_t n = seed < 0 ? (uint64_t)(-(seed + 1)) + 1 : (uint64_t)seed;
    uint32_t key[2];
    int64_t kn = 0;
    if (n == 0) {
        key[0] = 0;
        kn = 1;
    } else {
        while (n && kn < 2) {
            key[kn++] = (uint32_t)(n & 0xffffffffU);
            n >>= 32;
        }
    }
    mt_init_by_array(r, key, kn);
}

static uint32_t mt_next32(PyAdamasRandom* r) {
    static const uint32_t mag01[2] = {0U, MT_MATRIX_A};
    uint32_t y;
    if (r->index >= MT_N) {
        int64_t kk;
        for (kk = 0; kk < MT_N - MT_M; ++kk) {
            y = (r->mt[kk] & MT_UPPER_MASK) | (r->mt[kk + 1] & MT_LOWER_MASK);
            r->mt[kk] = r->mt[kk + MT_M] ^ (y >> 1) ^ mag01[y & 1U];
        }
        for (; kk < MT_N - 1; ++kk) {
            y = (r->mt[kk] & MT_UPPER_MASK) | (r->mt[kk + 1] & MT_LOWER_MASK);
            r->mt[kk] = r->mt[kk + (MT_M - MT_N)] ^ (y >> 1) ^ mag01[y & 1U];
        }
        y = (r->mt[MT_N - 1] & MT_UPPER_MASK) | (r->mt[0] & MT_LOWER_MASK);
        r->mt[MT_N - 1] = r->mt[MT_M - 1] ^ (y >> 1) ^ mag01[y & 1U];
        r->index = 0;
    }
    y = r->mt[r->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= y >> 18;
    return y;
}

static double mt_next_double(PyAdamasRandom* r) {
    uint32_t a = mt_next32(r) >> 5;
    uint32_t b = mt_next32(r) >> 6;
    return ((double)a * 67108864.0 + (double)b) * (1.0 / 9007199254740992.0);
}

static uint64_t mt_getrandbits(PyAdamasRandom* r, int64_t k) {
    if (k <= 0) return 0;
    if (k <= 32) return (uint64_t)(mt_next32(r) >> (32 - k));
    uint64_t res = 0;
    int64_t i = 0;
    while (k > 0) {
        uint32_t rv = mt_next32(r);
        if (k < 32) rv >>= (32 - k);
        res |= (uint64_t)rv << i;
        i += 32;
        k -= 32;
    }
    return res;
}

/* CPython 的 _randbelow：取比 n 多一位的随机数，落在区间外就重抽。 */
static uint64_t mt_randbelow(PyAdamasRandom* r, uint64_t n) {
    if (n == 0) return 0;
    int64_t k = 0;
    for (uint64_t t = n; t; t >>= 1) k++;
    uint64_t v = mt_getrandbits(r, k);
    while (v >= n) v = mt_getrandbits(r, k);
    return v;
}

static void mt_time_seed(PyAdamasRandom* r) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t x = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^
                 ((uint64_t)getpid() << 40);
    mt_seed_i64(r, (int64_t)(x >> 1));
}

PyAdamasRandom* pa_random_new(int64_t seed) {
    PyAdamasRandom* r = (PyAdamasRandom*)rc_alloc((int64_t)sizeof(PyAdamasRandom), NULL, NULL,
                                          PYADAMAS_RC_OBJ);
    pa_random_seed(r, seed);
    return r;
}

void pa_random_seed(PyAdamasRandom* r, int64_t seed) {
    // 未传 seed 时调用点填的是 INT64_MIN 哨兵：用时间 + pid 播种。
    if (seed == INT64_MIN)
        mt_time_seed(r);
    else
        mt_seed_i64(r, seed);
}

double pa_random_next(PyAdamasRandom* r) { return mt_next_double(r); }

int64_t pa_random_randint(PyAdamasRandom* r, int64_t a, int64_t b) {
    if (a > b) pa_raise_c("ValueError", "empty range for randrange()");
    uint64_t width = (uint64_t)b - (uint64_t)a + 1;
    return (int64_t)((uint64_t)a + mt_randbelow(r, width));
}

/* 返回借用：调用点按静态元素类型决定是否补一次 rc_inc。 */
int64_t pa_random_choice(PyAdamasRandom* r, PyAdamasList* seq) {
    if (!seq || seq->len == 0)
        pa_raise_c("IndexError", "cannot choose from an empty sequence");
    return seq->data[mt_randbelow(r, (uint64_t)seq->len)];
}

double pa_random_uniform(PyAdamasRandom* r, double a, double b) {
    return a + (b - a) * mt_next_double(r);
}

void pa_random_shuffle(PyAdamasRandom* r, PyAdamasList* seq) {
    for (int64_t i = seq->len - 1; i > 0; --i) {
        uint64_t j = mt_randbelow(r, (uint64_t)(i + 1));
        int64_t tmp = seq->data[i];
        seq->data[i] = seq->data[(int64_t)j];
        seq->data[(int64_t)j] = tmp;
    }
}

/* ── 模块级函数共用一把全局 RNG ─────────────────────────────── */

static PyAdamasRandom g_rng;
static int g_rng_ready = 0;

static PyAdamasRandom* global_rng(void) {
    if (!g_rng_ready) {
        pa_random_seed(&g_rng, INT64_MIN);
        g_rng_ready = 1;
    }
    return &g_rng;
}

void pa_random_gseed(int64_t seed) {
    pa_random_seed(global_rng(), seed);
}

double pa_random_gnext(void) { return mt_next_double(global_rng()); }

int64_t pa_random_grandint(int64_t a, int64_t b) {
    return pa_random_randint(global_rng(), a, b);
}

int64_t pa_random_gchoice(PyAdamasList* seq) { return pa_random_choice(global_rng(), seq); }

double pa_random_guniform(double a, double b) {
    return pa_random_uniform(global_rng(), a, b);
}

void pa_random_gshuffle(PyAdamasList* seq) { pa_random_shuffle(global_rng(), seq); }

/* ── 补充的分布与采样 ─────────────────────────────────────────
   统一用「r 为 NULL 就走全局 rng」的约定：模块级函数传 0，实例方法传实例。 */

static PyAdamasRandom* rng_of(PyAdamasRandom* r) { return r ? r : global_rng(); }

/* randrange(a, b, step)：step 非零，方向由调用点保证。 */
int64_t pa_random_randrange(PyAdamasRandom* r, int64_t a, int64_t b, int64_t step) {
    PyAdamasRandom* g = rng_of(r);
    int64_t n;
    if (step > 0) {
        if (b <= a) pa_raise_c("ValueError", "empty range for randrange()");
        n = (b - a + step - 1) / step;
    } else {
        if (b >= a) pa_raise_c("ValueError", "empty range for randrange()");
        n = (a - b + (-step) - 1) / (-step);
    }
    int64_t k = (int64_t)(mt_next_double(g) * (double)n);
    if (k >= n) k = n - 1;
    return a + step * k;
}

int64_t pa_random_getrandbits(PyAdamasRandom* r, int64_t k) {
    if (k <= 0) pa_raise_c("ValueError", "number of bits must be greater than zero");
    if (k > 62) k = 62;
    int64_t v = (int64_t)(mt_next_double(rng_of(r)) * (double)((uint64_t)1 << k));
    return v;
}

double pa_random_gauss(PyAdamasRandom* r, double mu, double sigma) {
    PyAdamasRandom* g = rng_of(r);
    double u1 = mt_next_double(g);
    double u2 = mt_next_double(g);
    if (u1 < 1e-300) u1 = 1e-300;
    /* Box-Muller；2*pi 直接写常量，免得依赖 M_PI 的宏开关。 */
    double z = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
    return mu + sigma * z;
}

double pa_random_expovariate(PyAdamasRandom* r, double lambd) {
    if (lambd <= 0) pa_raise_c("ValueError", "lambda must be positive");
    double u = mt_next_double(rng_of(r));
    if (u <= 0) u = 1e-300;
    return -log(u) / lambd;
}

double pa_random_triangular(PyAdamasRandom* r, double low, double high, double mode) {
    if (high < low) pa_raise_c("ValueError", "high must be >= low");
    if (mode < low || mode > high) pa_raise_c("ValueError", "mode must be between low and high");
    double u = mt_next_double(rng_of(r));
    double c = (mode - low) / (high - low);
    if (u > c) {
        u = 1.0 - u;
        c = 1.0 - c;
        double t = low;
        low = high;
        high = t;
    }
    return low + (high - low) * sqrt(u * c);
}

/* 无放回采样：复制一份再部分洗牌，取前 k 个。 */
PyAdamasList* pa_random_sample(PyAdamasRandom* r, PyAdamasList* seq, int64_t k) {
    int64_t n = seq ? seq->len : 0;
    if (k < 0) pa_raise_c("ValueError", "sample size must be non-negative");
    if (k > n) pa_raise_c("ValueError", "sample larger than population");
    PyAdamasRandom* g = rng_of(r);
    PyAdamasList* pool = pa_list_copy(seq);
    for (int64_t i = 0; i < k; ++i) {
        int64_t j = i + (int64_t)(mt_next_double(g) * (double)(n - i));
        if (j >= n) j = n - 1;
        int64_t t = pool->data[i];
        pool->data[i] = pool->data[j];
        pool->data[j] = t;
    }
    // 截断掉 [k, n) 的副本引用，否则无条件复制会让多余元素泄漏。
    for (int64_t i = k; i < n; ++i) pa_own_dec(pool->elem_kind, pool->data[i]);
    pool->len = k;
    return pool;
}

/* 有放回采样。 */
PyAdamasList* pa_random_choices(PyAdamasRandom* r, PyAdamasList* seq, int64_t k) {
    int64_t n = seq ? seq->len : 0;
    if (n == 0) pa_raise_c("IndexError", "Cannot choose from an empty sequence");
    if (k < 0) pa_raise_c("ValueError", "number of choices must be non-negative");
    PyAdamasRandom* g = rng_of(r);
    PyAdamasList* out = pa_list_new(k, seq->elem_kind);
    for (int64_t i = 0; i < k; ++i) {
        int64_t j = (int64_t)(mt_next_double(g) * (double)n);
        if (j >= n) j = n - 1;
        int64_t v = seq->data[j];
        pa_own_inc(seq->elem_kind, v);
        pa_list_append(out, v);
    }
    return out;
}
