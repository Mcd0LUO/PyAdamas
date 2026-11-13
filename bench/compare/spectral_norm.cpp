#include <cmath>
#include <cstdio>
#include <vector>

static const int N = 600;

static double evalA(int i, int j) {
    return 1.0 / static_cast<double>((i + j) * (i + j + 1) / 2 + i + 1);
}

static void mulATA(const std::vector<double>& v, std::vector<double>& out, int n) {
    for (int i = 0; i < n; ++i) {
        double total = 0.0;
        for (int j = 0; j < n; ++j) total += evalA(i, j) * v[j];
        out[i] = total;
    }
}

static void mulA(const std::vector<double>& u, std::vector<double>& out, int n) {
    for (int i = 0; i < n; ++i) {
        double total = 0.0;
        for (int j = 0; j < n; ++j) total += evalA(j, i) * u[j];
        out[i] = total;
    }
}

int main() {
    std::vector<double> u(N, 1.0), v(N, 0.0), tmp(N, 0.0);
    for (int k = 0; k < 10; ++k) {
        mulATA(u, v, N);
        mulA(v, tmp, N);
    }
    double vbv = 0.0, vvv = 0.0;
    for (int i = 0; i < N; ++i) {
        vbv += u[i] * tmp[i];
        vvv += tmp[i] * tmp[i];
    }
    std::printf("%lld\n", static_cast<long long>(std::sqrt(vbv / vvv) * 1000000.0));
    return 0;
}
