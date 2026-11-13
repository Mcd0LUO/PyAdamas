#include <cstdio>
#include <vector>

static const int N = 120;

static std::vector<long long> makeMatrix(int n) {
    std::vector<long long> out(static_cast<size_t>(n) * n);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<long long>((i * 7 + 3) % 11);
    return out;
}

int main() {
    auto a = makeMatrix(N), b = makeMatrix(N);
    std::vector<long long> c(static_cast<size_t>(N) * N, 0);
    for (int i = 0; i < N; ++i) {
        for (int k = 0; k < N; ++k) {
            long long aik = a[i * N + k];
            if (aik != 0) {
                for (int j = 0; j < N; ++j)
                    c[i * N + j] += aik * b[k * N + j];
            }
        }
    }
    long long total = 0;
    for (long long v : c) total += v;
    std::printf("%lld\n", total);
    return 0;
}
