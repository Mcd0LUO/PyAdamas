// M1 侦察用例 2：catch(...)，catch-all 是我们最关心的形态。
extern void maybe_throw(int);

int probe_catch_all(int n) {
    try {
        maybe_throw(n);
        return 1;
    } catch (...) {
        return 2;
    }
}
