# heapq 常用 API 对拍。
import heapq
h = [5, 3, 8, 1, 9, 2]
heapq.heapify(h)
print("heap", h)
out = []
while len(h) > 0:
    out.append(heapq.heappop(h))
print("pop", out)
print("nl", heapq.nlargest(3, [1,5,3,9,2]))
print("ns", heapq.nsmallest(3, [1,5,3,9,2]))

# --- expect ---
# heap [1, 3, 2, 5, 9, 8]
# pop [1, 2, 3, 5, 8, 9]
# nl [9, 5, 3]
# ns [1, 2, 3]
