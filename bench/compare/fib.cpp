#include <cstdio>

static const int N = 32;

static long long fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    std::printf("%lld\n", fib(N));
    return 0;
}
