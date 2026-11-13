#include <cmath>
#include <cstdio>
#include <vector>

static const int N = 50000;

static void advance(std::vector<double>& b, double dt, int steps) {
    for (int s = 0; s < steps; ++s) {
        for (int i = 0; i < 5; ++i) {
            for (int j = i + 1; j < 5; ++j) {
                double dx = b[i * 7] - b[j * 7];
                double dy = b[i * 7 + 1] - b[j * 7 + 1];
                double dz = b[i * 7 + 2] - b[j * 7 + 2];
                double d2 = dx * dx + dy * dy + dz * dz;
                double mag = dt / (d2 * std::sqrt(d2));
                b[i * 7 + 3] -= dx * b[j * 7 + 6] * mag;
                b[i * 7 + 4] -= dy * b[j * 7 + 6] * mag;
                b[i * 7 + 5] -= dz * b[j * 7 + 6] * mag;
                b[j * 7 + 3] += dx * b[i * 7 + 6] * mag;
                b[j * 7 + 4] += dy * b[i * 7 + 6] * mag;
                b[j * 7 + 5] += dz * b[i * 7 + 6] * mag;
            }
        }
        for (int i = 0; i < 5; ++i) {
            b[i * 7] += dt * b[i * 7 + 3];
            b[i * 7 + 1] += dt * b[i * 7 + 4];
            b[i * 7 + 2] += dt * b[i * 7 + 5];
        }
    }
}

static double energy(const std::vector<double>& b) {
    double e = 0.0;
    for (int i = 0; i < 5; ++i) {
        e += 0.5 * b[i * 7 + 6] * (b[i * 7 + 3] * b[i * 7 + 3] +
                                   b[i * 7 + 4] * b[i * 7 + 4] +
                                   b[i * 7 + 5] * b[i * 7 + 5]);
        for (int j = i + 1; j < 5; ++j) {
            double dx = b[i * 7] - b[j * 7];
            double dy = b[i * 7 + 1] - b[j * 7 + 1];
            double dz = b[i * 7 + 2] - b[j * 7 + 2];
            double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            e -= b[i * 7 + 6] * b[j * 7 + 6] / d;
        }
    }
    return e;
}

int main() {
    std::vector<double> b(35, 0.0);
    b[6] = 39.47841760435743;
    b[7] = 4.841431442464721;
    b[8] = -1.1603200440274284;
    b[9] = -0.10362204447112311;
    b[10] = 0.606326392995832;
    b[11] = 0.019301642645413026;
    b[12] = 0.0379118670433766;
    b[13] = 8.34336671824458;
    b[14] = 4.124798564124305;
    b[15] = -0.4035234171143214;
    b[16] = 0.010518566837692432;
    b[17] = 0.2046927615536005;
    b[18] = 0.0031132583343902;
    b[19] = 12.894369562139131;
    b[20] = -15.111151401698631;
    b[21] = -0.22330757889265574;
    b[22] = 0.015611249589010908;
    b[23] = 0.044136246667619354;
    b[24] = 0.002122611299774;
    b[25] = 15.379697114850917;
    b[26] = -25.919314609987964;
    b[27] = 0.17925877295037118;
    b[28] = 0.010300551650376;
    b[29] = 0.15024187765192692;
    b[30] = 0.002058885949389;
    b[31] = 19.9214925362507;
    b[32] = -20.0893886122242;
    b[33] = -0.052887120235923;
    b[34] = 0.015200054700887;

    advance(b, 0.01, N);
    std::printf("%lld\n", static_cast<long long>(energy(b) * 1000000.0));
    return 0;
}
