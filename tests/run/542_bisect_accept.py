# bisect 常用 API 对拍（仅 int 列表）。
import bisect
a = [1, 3, 5]
print("bl", bisect.bisect_left(a, 3))
print("br", bisect.bisect_right(a, 3))
print("b", bisect.bisect(a, 3))
b = []
bisect.insort_left(b, 3)
bisect.insort_right(b, 1)
bisect.insort(b, 2)
print("ins", b)

# --- expect ---
# bl 1
# br 2
# b 2
# ins [1, 2, 3]
