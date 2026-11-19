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

/* CPython _randommodule.c 的 getrandbits：k<=32 走快路（uint32 右移），
   否则按 32 位字逐个生成、最后一个字右移（32 - k%32）。**取多少字必须
   与 CPython 完全一致**——下游 randrange/sample/choice 的序列全靠这个
   对齐。返回值按 uint64 截断（PyAdamas 没有大整数，k>64 丢高位，但抽
   取的字数仍与 CPython 相同，后续序列不漂）。 */
static uint64_t mt_getrandbits(PyAdamasRandom* r, int64_t k) {
    if (k <= 0) return 0;
    if (k <= 32) return (uint64_t)(mt_next32(r) >> (32 - k));
    uint64_t res = 0;
    int64_t left = k;
    int64_t shift = 0;
    while (left > 0) {
        uint32_t rv = mt_next32(r);
        if (left < 32) rv >>= (32 - left);
        if (shift < 64) res |= (uint64_t)rv << shift;
        shift += 32;
        left -= 32;
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
    /* 0.303：洗牌交换元素位置，kind2 位图无法跟随，保守丢弃。 */
    if (seq->elem_kind == 2) pa_list_drop_bits(seq);
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
    /* CPython randrange：start + step * _randbelow(width)。用 getrandbits
       的拒绝采样，不是 random()*n —— 两者序列不同，语料对拍会分叉。 */
    int64_t k = (int64_t)mt_randbelow(g, (uint64_t)n);
    return a + step * k;
}

int64_t pa_random_getrandbits(PyAdamasRandom* r, int64_t k) {
    if (k <= 0) pa_raise_c("ValueError", "number of bits must be greater than zero");
    /* 与 CPython 同一算法、同一消耗：抽取的字数逐字对齐，所以后续序列
       不漂。PyAdamas 没有无符号/大整数，k>63 时只保留低 63 位（非负）——
       128 位的 uuid4 因此是 63 位随机（字符串仍是合法 32 位 hex，只是熵
       少于 CPython，已在 stdlib 文档声明边界）。 */
    uint64_t bits = mt_getrandbits(rng_of(r), k);
    return (int64_t)(bits & 0x7fffffffffffffffULL);
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

/* CPython sample() 的两条算法路径按同一门槛选择：种群小就整表洗牌取前 k，
   大就用「已选下标集合」重抽。门槛、抽数方式都一样，序列才逐位对得上。 */
static int64_t sample_setsize(int64_t k) {
    if (k <= 5) return 21;
    double x = log((double)k * 3.0) / log(4.0);
    int64_t c = (int64_t)ceil(x);
    int64_t p = 1;
    for (int64_t i = 0; i < c && p < (INT64_MAX / 4); ++i) p *= 4;
    return 21 + p;
}

/* 无放回采样：复制一份再部分洗牌，取前 k 个（CPython 的 pool 分支）。 */
PyAdamasList* pa_random_sample(PyAdamasRandom* r, PyAdamasList* seq, int64_t k) {
    int64_t n = seq ? seq->len : 0;
    if (k < 0) pa_raise_c("ValueError", "sample size must be non-negative");
    if (k > n) pa_raise_c("ValueError", "sample larger than population");
    PyAdamasRandom* g = rng_of(r);
    if (n > sample_setsize(k)) {
        /* 大种群：CPython 用 set 记录已选下标，重复就重抽；这里用同构的
           开地址集合（下标都是非负 int64，-1 表示空）。 */
        int64_t cap = 8;
        while (cap < k * 2) cap *= 2;
        int64_t* tab = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
        if (!tab) panic_c("out of memory");
        for (int64_t i = 0; i < cap; ++i) tab[i] = -1;
        PyAdamasList* out = pa_list_new(k, seq->elem_kind);
        pa_gc_pin(out);
        for (int64_t i = 0; i < k; ++i) {
            int64_t j = (int64_t)mt_randbelow(g, (uint64_t)n);
            for (;;) {
                int64_t h = (int64_t)(((uint64_t)j * 0x9e3779b97f4a7c15ULL) >> 40) & (cap - 1);
                while (tab[h] != -1 && tab[h] != j) h = (h + 1) & (cap - 1);
                if (tab[h] == -1) {
                    tab[h] = j;
                    break;
                }
                j = (int64_t)mt_randbelow(g, (uint64_t)n);
            }
            int64_t v = seq->data[j];
            pa_own_inc(seq->elem_kind, v);
            pa_list_append(out, v, seq->elem_kind == 3 ? 2 : (int)seq->elem_kind);
        }
        pa_gc_unpin(out);
        free(tab);
        return out;
    }
    /* CPython 3.11+ 的 pool 分支：不变量是「未选中项都在 pool[0:n-i]」。
       每轮抽 j ∈ [0, n-i)，取走 pool[j]，把尾部未选中项填进空位。
       选中项先落临时区，循环结束后再搬到表头 —— 边抽边写表头会覆盖还
       没抽到的候选（n 与 k 接近时）。 */
    PyAdamasList* pool = pa_list_copy(seq);
    int64_t* sel = k > 0 ? (int64_t*)malloc(sizeof(int64_t) * (size_t)k) : NULL;
    if (k > 0 && !sel) panic_c("out of memory");
    for (int64_t i = 0; i < k; ++i) {
        int64_t j = (int64_t)mt_randbelow(g, (uint64_t)(n - i));
        sel[i] = pool->data[j];
        pool->data[j] = pool->data[n - i - 1];
    }
    for (int64_t i = 0; i < k; ++i) pool->data[i] = sel[i];
    free(sel);
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
        pa_list_append(out, v, 2);
    }
    return out;
}
