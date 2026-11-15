// M1 链接用：提供 main 和 probe 里引用的外部符号。
// probe 函数本身不会被调用（本阶段不要求运行）。
extern void maybe_throw(int);
void maybe_throw(int) {}

struct Guard {
    ~Guard();
};
Guard::~Guard() {}

// 给 mini_direct_xo.S 用的 personality 桩：_URC_CONTINUE_UNWIND = 5
extern "C" int ext_personality(int, int, unsigned long long, void *, void *) {
    return 5;
}

int main() {
    return 0;
}
