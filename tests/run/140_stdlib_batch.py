import bisect
import binascii
import base64
import heapq
import statistics
import textwrap

h: list = [5, 3, 8, 1, 9, 2]
heapq.heapify(h)
print(heapq.heappop(h), heapq.heappop(h), heapq.heappop(h))
heapq.heappush(h, 4)
print(heapq.nsmallest(2, [9, 1, 5, 3]), heapq.nlargest(2, [9, 1, 5, 3]))
print(heapq.heappushpop([1, 2, 3], 0), heapq.heapreplace([1, 2, 3], 5))

xs: list = [1, 3, 3, 7]
print(bisect.bisect_left(xs, 3), bisect.bisect_right(xs, 3), bisect.bisect(xs, 4))
bisect.insort_left(xs, 4)
print(xs)

print(binascii.hexlify(b"hi"))
print(binascii.unhexlify(b"6869"))
print(base64.b16encode(b"hi"), base64.b16decode(b"6869"))
print(base64.b32encode(b"hi"), base64.b32decode(b"NBUQ===="))
print(base64.standard_b64encode(b"hi"), base64.standard_b64decode(b"aGk="))
print(base64.encodebytes(b"hi"))
print(base64.decodebytes(b"aGk=\n"))
print(statistics.geometric_mean([1.0, 4.0, 16.0]))

for line in textwrap.wrap("one two three four", 9):
    print(line)
print(textwrap.fill("one two three four", 9))
print(textwrap.shorten("hello world foo", 10, "..."))

# --- expect ---
# 1 2 3
# [1, 3] [9, 5]
# 0 1
# 1 3 3
# [1, 3, 3, 4, 7]
# b'6869'
# b'hi'
# b'6869' b'hi'
# b'NBUQ====' b'hi'
# b'aGk=' b'hi'
# b'aGk=\n'
# b'hi'
# 4.0
# one two
# three
# four
# one two
# three
# four
# hello...
