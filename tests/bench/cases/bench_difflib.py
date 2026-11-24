"""difflib 基准：unified_diff + SequenceMatcher + get_close_matches。"""
import difflib

a = []
b = []
i = 0
while i < 120:
    a.append("line " + str(i))
    if i % 7 == 0:
        b.append("changed " + str(i))
    else:
        b.append("line " + str(i))
    i += 1

total = 0
r = 0
while r < 150:
    total += len(list(difflib.unified_diff(a, b, "a", "b")))
    sm = difflib.SequenceMatcher(a, b)
    total += len(sm.get_opcodes())
    total += len(difflib.get_close_matches("line 42", a, 3))
    r += 1
print(total)
