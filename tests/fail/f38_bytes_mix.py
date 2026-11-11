# expect: cannot mix bytes and nonbytes literals
x = b"a" "b"
print(x)
