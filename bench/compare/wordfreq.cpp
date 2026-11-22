// wordfreq —— LCG 生成 N 个词并用 dict 计数（dict / str 路径）
#include <cstdio>
#include <string>
#include <unordered_map>

static const int N = 200000;

int main() {
    long long x = 12345;
    std::unordered_map<std::string, int> counts;
    for (int i = 0; i < N; ++i) {
        x = (x * 1103515245LL + 12345) % 2147483648LL;
        counts["w" + std::to_string(x % 1000)] += 1;
    }
    long long total = 0;
    int best = 0;
    for (const auto& kv : counts) {
        total += kv.second;
        if (kv.second > best) best = kv.second;
    }
    std::printf("%zu %lld %d\n", counts.size(), total, best);
}
