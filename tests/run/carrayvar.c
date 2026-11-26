/* 0.564：数组类型的 extern 变量 —— 用 C/Python 同形的 T[N] 声明。 */
int vals3[3] = {10, 20, 30};
long vals64[3] = {100, 200, 300};
char* names[2] = {"aa", "bb"};

int* vals_addr(void) { return vals3; }
