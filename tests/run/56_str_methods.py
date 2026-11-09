s = "  Hello, World  "
print(s.strip().upper())
print(s.strip().lower())
print("xxHixx".strip("x"))
parts = "a,b,c".split(",")
print(len(parts), parts[0], parts[2])
words = "  a  b  ".split()
print(len(words), words[0], words[1])
print(",".join(["a", "b", "c"]))
print("banana".replace("a", "o"))
print("banana".replace("a", "o", 2))
print("hello".find("l"), "hello".rfind("l"), "hello".find("z"))
print("hello".startswith("he"), "hello".endswith("lo"))
print("banana".count("an"))
print("7".zfill(3), "-7".zfill(4))
print("ab".ljust(4, "."), "ab".rjust(4, "."), "ab".center(6, "-"))
print("hELLO wORLD".capitalize(), "hELLO wORLD".title())
print("abc123".isalpha(), "abc123".isalnum(), "123".isdigit(), "   ".isspace())
print("hello".removeprefix("he"), "hello".removesuffix("lo"))
lines = "a\nb\r\nc".splitlines()
print(len(lines), lines[0], lines[2])
try:
    print("hello".index("z"))
except ValueError as e:
    print("ValueError", str(e))

# --- expect ---
# HELLO, WORLD
# hello, world
# Hi
# 3 a c
# 2 a b
# a,b,c
# bonono
# bonona
# 2 3 -1
# True True
# 2
# 007 -007
# ab.. ..ab --ab--
# Hello world Hello World
# False True True True
# llo hel
# 3 a c
# ValueError substring not found
