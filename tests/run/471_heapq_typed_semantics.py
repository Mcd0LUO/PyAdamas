# heapq 核心操作语义钉桩：单元素、重复值、空堆 pushpop、nlarge/nsmall、merge。
import heapq

h: list[int] = []
heapq.heapify(h)
print("empty", len(h))
print("ph", heapq.heappushpop(h, 7))
print("after_ph", len(h))

h = [5, 3, 8, 1, 9, 2, 1]
heapq.heapify(h)
out: list[int] = []
while len(h) > 0:
    out.append(heapq.heappop(h))
print("sorted", out)

h = [1]
print("single_pop", heapq.heappop(h))
h = [3, 1, 2]
print("replace", heapq.heapreplace(h, 0))
print("heap_after_replace", h)

print("nl", heapq.nlargest(4, [5, 1, 8, 3, 8, 2]))
print("ns", heapq.nsmallest(4, [5, 1, 8, 3, 8, 2]))
print("nl_big", heapq.nlargest(20, [2, 1]))
print("merge", list(heapq.merge([1, 4, 7], [2, 5, 8], [3, 6, 9])))
print("merge_empty", list(heapq.merge([], [1, 2], [])))

words = ["aaa", "b", "cccc", "dd", "ee"]
def wlen(x): return len(x)
print("key_nl", heapq.nlargest(3, words, key=wlen))
print("key_ns", heapq.nsmallest(3, words, key=wlen))
# --- expect ---
# empty 0
# ph 7
# after_ph 0
# sorted [1, 1, 2, 3, 5, 8, 9]
# single_pop 1
# replace 3
# heap_after_replace [0, 1, 2]
# nl [8, 8, 5, 3]
# ns [1, 2, 3, 5]
# nl_big [2, 1]
# merge [1, 2, 3, 4, 5, 6, 7, 8, 9]
# merge_empty [1, 2]
# key_nl ['cccc', 'aaa', 'dd']
# key_ns ['b', 'dd', 'ee']
