# 0.537（f32 方案①）：32 位 buffer 只能传给裸 ptr，不能冒充 ptr[float]。
cimport cbuflib as c
b = buffer_f32(4)
c.memset(b, 0, 16)
