#include <cstdio>
#include <vector>

static const int N = 12;

static long long place(int row, int n, std::vector<int>& cols, std::vector<int>& d1,
                       std::vector<int>& d2) {
    if (row == n) return 1;
    long long total = 0;
    for (int c = 0; c < n; ++c) {
        if (cols[c] == 0 && d1[row + c] == 0 && d2[row - c + n] == 0) {
            cols[c] = d1[row + c] = d2[row - c + n] = 1;
            total += place(row + 1, n, cols, d1, d2);
            cols[c] = d1[row + c] = d2[row - c + n] = 0;
        }
    }
    return total;
}

int main() {
    std::vector<int> cols(N, 0), d1(2 * N, 0), d2(2 * N, 0);
    std::printf("%lld\n", place(0, N, cols, d1, d2));
    return 0;
}
