#include <cstdio>
#include <string>

static const int N = 40000;

static std::string build(int n) {
    long long seed = 42;
    std::string s;
    s.reserve(static_cast<size_t>(n));
    int total = 0;
    while (total < n) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        switch (seed % 4) {
            case 0: s += "ACGT"; break;
            case 1: s += "AAAA"; break;
            case 2: s += "CCCC"; break;
            default: s += "GGGG"; break;
        }
        total += 4;
    }
    return s;
}

int main() {
    std::string s = build(N);
    long long h = 0;
    for (int i = 0; i < N; ++i)
        h = (h * 31 + static_cast<unsigned char>(s[static_cast<size_t>(i)])) % 1000000007LL;
    std::printf("%d\n%lld\n", N, h);
    return 0;
}
