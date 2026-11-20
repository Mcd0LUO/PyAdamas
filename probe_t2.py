s = ""
n = 0
if n > 0 and s[0] == "x":
    print("a")
print("after1")
if len(s) > 0 and s[0] == "x":
    print("b")
print("after2")
if False and s[0] == "x":
    print("c")
print("after3")
if len(s) and s[0] == "x":
    print("d")
print("after4")
flag = False
if flag and s[0] == "x":
    print("e")
print("after5")
