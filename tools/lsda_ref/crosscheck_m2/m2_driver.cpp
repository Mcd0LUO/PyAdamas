// 与 mini_m2_*.S 配套的 C++ 驱动：外层用真正的 C++ catch(int) 捕获。
// 关键观察：如果 M2 形态的 LSDA 被 __gxx_personality_v0 当成 handler，
// 外层 catch 就不该拿到异常；如果按 ABI 当成 cleanup，landing pad 会先跑，
// 然后调用 _Unwind_Resume 继续展开，最后由外层 catch 拿到 7。
#include <cstdio>

extern "C" void m2_like_fn();
extern "C" long m2_lp_runs;

extern "C" void throw_int() {
    throw 7;
}

int main() {
    int caught = 0;
    try {
        m2_like_fn();
    } catch (int v) {
        caught = v;
    }
    std::fprintf(stderr, "[driver] caught=%d m2_lp_runs=%ld\n", caught,
                 m2_lp_runs);
    return (caught == 7 && m2_lp_runs == 1) ? 0 : 1;
}
