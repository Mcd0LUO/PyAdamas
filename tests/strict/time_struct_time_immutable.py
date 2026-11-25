# time.struct_time 只读（0.520）：属性写与下标写都要在编译期拒绝。
import time

t = time.localtime(0)
t.tm_year = 1
t[0] = 1