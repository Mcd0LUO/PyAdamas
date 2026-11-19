# NEW BUG N2 (silent wrong): text-mode file write folds CRLF->LF and CR->LF and
# ignores the newline= write conversion. newline="" and newline="\r\n" both wrong.
# CPython writes 'a\r\nb'; pyadamas writes 'a\nb'. Breaks csv.writer default
# lineterminator="\r\n" (emits LF). Suspect: rt_file write applies read-side
# universal-newline folding; newline param not honored on write.
# Workaround: csv.writer(..., lineterminator="\n"); avoid \r in written content.
import os
p: str = "_n2.csv"
with open(p, "w", newline="") as f:
    f.write("a\r\nb\r\n")
with open(p, "r", newline="") as f:
    print("direct write:", repr(f.read()))   # CPython 'a\r\nb\r\n', pa 'a\nb\n'
with open(p, "w", newline="\r\n") as f:
    f.write("a\nb")
with open(p, "r", newline="") as f:
    print("newline=CRLF:", repr(f.read()))   # CPython 'a\r\nb', pa 'a\nb'
os.remove(p)
