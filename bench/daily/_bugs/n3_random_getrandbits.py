# NEW BUG N3 (silent wrong): random.getrandbits(k) diverges for k>=31.
# k=8/16 match; k=31/32 differ in low bits; k=33/64 differ structurally (word
# assembly). randrange/sample/choices sequences diverge downstream.
# random()/randint/uniform/choice/shuffle match (MT19937 top bits correct).
# Suspect: multi-word getrandbits assembly endianness + tempering low-bit bug;
# randrange/sample consume RNG differently. Workaround: use random()/randint/
# uniform/choice/shuffle; avoid getrandbits(>16)/randrange/sample/choices.
import random
random.seed(7)
print("bits8:", random.getrandbits(8))     # matches
print("bits16:", random.getrandbits(16))   # matches
print("bits31:", random.getrandbits(31))   # CPython 695425564, pa 695425567
print("bits32:", random.getrandbits(32))   # CPython 1390851128, pa 1390851134
print("bits64:", random.getrandbits(64))   # diverges
random.seed(7)
print("randrange:", random.randrange(10), random.randrange(10))  # CPython 5 2, pa 3 1
print("sample:", random.sample([10, 20, 30, 40], 2))             # diverges
