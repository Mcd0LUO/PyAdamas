/* 0.562：数组类型的 extern 变量 —— 验证符号地址与 C 侧一致。 */
int arr_values[3] = {10, 20, 30};

int* arr_addr(void) { return arr_values; }
