# queue.Queue 基本 put/get/empty/qsize 对拍。
import queue
q = queue.Queue()
q.put(1)
q.put(2)
print("get", q.get(), q.get())
print("empty", q.empty())
q.put(3)
print("size", q.qsize())

# --- expect ---
# get 1 2
# empty True
# size 1
