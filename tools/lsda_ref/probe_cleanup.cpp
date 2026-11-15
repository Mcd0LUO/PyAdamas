// M1 侦察用例 3：只有清理（析构），没有 catch。
// 用来观察 action 表的 cleanup 形态（Z3 的 finally 要用）。
extern void maybe_throw(int);

struct Guard {
    ~Guard();
};

int probe_cleanup(int n) {
    Guard g;
    maybe_throw(n);
    return g, 0;  // 保证 g 被使用、析构不因 -O1 被删
}
