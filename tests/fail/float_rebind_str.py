# expect: cannot rebind to
# 未标注变量先存 float 再存堆值（或反过来）。float 在槽里是裸 double，
# str/list 是堆指针 —— 同一个槽装不下两种表示，以前会打印位模式或段错误。
v = "x"
print(v)
v = 1.5
print(v)
