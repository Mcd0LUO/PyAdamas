// M1 侦察用例 1：catch(int)，有 TType 类型表。
// 编译方式见 gen.sh：g++ -O1 -S / -O1 -c -Wa,-L（保留 .L 局部符号便于对地址）。
extern void maybe_throw(int);

int probe_catch_int(int n) {
    try {
        maybe_throw(n);
        return 1;
    } catch (int) {
        return 2;
    }
}
