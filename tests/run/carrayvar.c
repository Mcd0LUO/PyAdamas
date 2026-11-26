/* 0.563：数组类型的 extern 变量 —— 元素宽度由 array[T, N] 的 T 决定。 */
int vals3[3] = {10, 20, 30};
long vals64[3] = {100, 200, 300};

int* vals_addr(void) { return vals3; }
