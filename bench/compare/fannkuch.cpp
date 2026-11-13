#include <cstdio>
#include <vector>

static const int N = 8;

static void fannkuch(int n, int& checksum, int& maxFlip) {
    std::vector<int> perm(n), count(n, 0), temp(n);
    for (int i = 0; i < n; ++i) perm[i] = i;
    maxFlip = 0;
    checksum = 0;
    int permCount = 0;
    for (;;) {
        int k = perm[0];
        if (k != 0) {
            for (int i = 0; i < n; ++i) temp[i] = perm[i];
            int flips = 0;
            while (temp[0] != 0) {
                int m = temp[0];
                for (int i = 0; i < m; ++i, --m) {
                    int t = temp[i];
                    temp[i] = temp[m];
                    temp[m] = t;
                }
                ++flips;
            }
            if (flips > maxFlip) maxFlip = flips;
            checksum += (permCount % 2 == 0) ? flips : -flips;
        }
        ++permCount;
        int i = 1;
        for (;;) {
            if (i >= n) return;
            if (count[i] < i) break;
            count[i] = 0;
            ++i;
        }
        int first = perm[0];
        for (int j = 0; j < i; ++j) perm[j] = perm[j + 1];
        perm[i] = first;
        ++count[i];
    }
}

int main() {
    int c = 0, m = 0;
    fannkuch(N, c, m);
    std::printf("%d\n%d\n", c, m);
    return 0;
}
