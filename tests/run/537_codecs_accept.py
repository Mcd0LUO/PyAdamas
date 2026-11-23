# codecs 常用 API 对拍（CPython 3.14 同形脚本，逐字节 0 diff）
# errors="replace"/"ignore" 在 PA 不生效（str.encode 忽略 errors 参数），见报告。
import codecs

print("u8", codecs.encode("hello"))
print("u8dec", codecs.decode(b"hello"))
print("asc", codecs.encode("hello", "ascii"))
print("ascdec", codecs.decode(b"hello", "ascii"))
print("empty", codecs.encode(""))

# --- expect ---
# u8 b'hello'
# u8dec hello
# asc b'hello'
# ascdec hello
# empty b''
