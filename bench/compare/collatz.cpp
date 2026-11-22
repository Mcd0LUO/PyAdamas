// collatz —— 1..N 的 Collatz 步数总和与最长链（整数分支 / 除法）
#include <cstdio>
#include <cstdint>

static const int N = 50000;

static int64_t steps(int64_t n) {
    int64_t c = 0;
    while (n != 1) {
        if (n % 2 == 0)
            n /= 2;
        else
            n = 3 * n + 1;
        ++c;
    }
    return c;
}

int main() {
    int64_t total = 0, best = 0, bestn = 1;
    for (int64_t i = 1; i < N; ++i) {
        int64_t s = steps(i);
        total += s;
        if (s > best) {
            best = s;
            bestn = i;
        }
    }
    std::printf("%lld %lld %lld\n", (long long)total, (long long)best, (long long)bestn);
}
