// sieve —— 埃氏筛计数（list[int] 逐元素写 + 跨步内层循环）
#include <cstdio>
#include <vector>

static const int N = 500000;

int main() {
    std::vector<int> flags(N, 1);
    flags[0] = 0;
    flags[1] = 0;
    for (int p = 2; p * p < N; ++p) {
        if (flags[p] == 1)
            for (long long q = 1LL * p * p; q < N; q += p)
                flags[(size_t)q] = 0;
    }
    int count = 0;
    for (int i = 0; i < N; ++i)
        if (flags[i] == 1) ++count;
    std::printf("%d\n", count);
}
