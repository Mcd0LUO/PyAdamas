s = ""
print("a", s[:2], s[0:2], len(s))
t = "abc"
print(t[:2])
flag = False
if flag:
    print("no")
elif t[:2] == "//":
    print("yes")
else:
    print("else")
if s and s[0] != "/":
    print("guard")
print("done")
