#include <cstdio>

static const int N = 350;
static const int MAXITER = 50;

static long long run(int size, int maxiter) {
    long long total = 0;
    for (int y = 0; y < size; ++y) {
        double ci = 2.0 * y / size - 1.0;
        for (int x = 0; x < size; ++x) {
            double cr = 2.0 * x / size - 1.5;
            double zr = 0.0, zi = 0.0;
            int i = 0;
            while (i < maxiter && zr * zr + zi * zi <= 4.0) {
                double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                ++i;
            }
            total += i;
        }
    }
    return total;
}

int main() {
    std::printf("%lld\n", run(N, MAXITER));
    return 0;
}
