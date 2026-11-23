# stdlib-a regression batch
import errno
import stat
import shlex
import getpass
import platform
import csv
import struct

print('errno', errno.EPERM, errno.ENOENT, errno.ESRCH, errno.EINTR, errno.EIO, errno.ENOMEM, errno.EACCES, errno.ENOEXEC, errno.EAGAIN, errno.ENOTDIR, errno.EISDIR, errno.EINVAL, errno.EMFILE, errno.EEXIST, errno.ENOSPC, errno.EPIPE, errno.EDOM, errno.ERANGE, errno.ETIMEDOUT)
print('errno_alias', errno.EWOULDBLOCK, errno.EWOULDBLOCK == errno.EAGAIN)
print('errno_code', errno.errorcode[2], errno.errorcode[13], errno.errorcode[110])
print('stat_bits', stat.S_IFMT, stat.S_IFDIR, stat.S_IFREG, stat.S_IFLNK, stat.S_IFIFO, stat.S_IFSOCK, stat.S_IRUSR, stat.S_IWUSR, stat.S_IXUSR, stat.S_IRGRP, stat.S_IWGRP, stat.S_IXGRP, stat.S_IROTH, stat.S_IWOTH, stat.S_IXOTH)
print('stat_is', stat.S_ISDIR(stat.S_IFDIR), stat.S_ISDIR(stat.S_IFREG), stat.S_ISREG(stat.S_IFREG | 511), stat.S_ISLNK(stat.S_IFLNK), stat.S_ISLNK(stat.S_IFDIR), stat.S_ISFIFO(stat.S_IFIFO), stat.S_ISSOCK(stat.S_IFSOCK), stat.S_ISSOCK(stat.S_IFREG))

D = chr(34)
S = chr(39)
TAB = chr(9)
BSL = chr(92)
print('shlex1', shlex.split('a b c'))
print('shlex2', shlex.split('  a   b' + TAB + 'c  '))
print('shlex3', shlex.split(D + 'a b' + D + ' c'), shlex.split(S + 'a b' + S + ' c'))
print('shlex4', shlex.split('a' + BSL + ' b'), shlex.split('a' + D + 'b c' + D + 'd'), shlex.split('a' + S + 'b c' + S + 'd'))
print('shlex5', shlex.split(''), shlex.split(D + D), shlex.split(S + S))
print('shlex6', shlex.split('a' + BSL + D + 'b'), shlex.split('a' + BSL + S + 'b'), shlex.split(D + 'a' + BSL + D + 'b' + D))
print('shlex7', shlex.quote(''), shlex.quote('abc'), shlex.quote('a b'))
print('shlex8', shlex.quote('it' + S + 's'), shlex.split(shlex.quote('it' + S + 's')))
parts = ['a', 'b c', 'd' + S + 'e', 'f' + D + 'g', '', 'h' + BSL + 'i']
j = shlex.join(parts)
print('shlex9', j, shlex.split(j))

print('getuser', getpass.getuser())
print('plat', platform.system(), platform.machine(), repr(platform.release()), platform.python_version())

text = 'name,age,city' + chr(10) + 'alice,30,NY' + chr(10) + 'bob,25,' + chr(10)
print('csvdr', csv.DictReader(text))
dw = csv.DictWriterText(['name', 'age'])
dw.writeheader()
dw.writerow({'name': 'alice', 'age': '30'})
dw.writerows([{'name': 'bob', 'age': '25'}, {'name': 'x'}])
print('csvdw', repr(dw.getvalue()))

_HEX = '0123456789abcdef'
def hx(b: bytes) -> str:
    out = ''
    for i in range(len(b)):
        v = b[i]
        out = out + _HEX[(v >> 4) & 15] + _HEX[v & 15]
    return out

print('st1', hx(struct.pack('<bBhHiI', -1, 255, -2, 65535, -3, 4294967295)), struct.calcsize('<bBhHiI'))
print('st2', struct.unpack('<bBhHiI', struct.pack('<bBhHiI', -1, 255, -2, 65535, -3, 4294967295)))
print('st3', hx(struct.pack('>bBhHiI', -1, 255, -2, 65535, -3, 4294967295)), struct.calcsize('>bBhHiI'))
print('st4', hx(struct.pack('!qQ', -1234567890123, 1234567890123)), struct.calcsize('!qQ'))
print('st5', struct.unpack('!qQ', struct.pack('!qQ', -1234567890123, 1234567890123)))
print('st6', hx(struct.pack('<qQ', -1234567890123, 1234567890123)))
print('st7', hx(struct.pack('<f', 1.5)), hx(struct.pack('>f', 1.5)))
print('st8', hx(struct.pack('<d', 3.141592653589793)), hx(struct.pack('>d', 3.141592653589793)))
print('st9', hx(struct.pack('<5s', 'hello')), hx(struct.pack('>3s', 'abc')))
print('st10', hx(struct.pack('<i', -42)), hx(struct.pack('>i', -42)))

# --- expect ---
# errno 1 2 3 4 5 12 13 8 11 20 21 22 24 17 28 32 33 34 110
# errno_alias 11 True
# errno_code ENOENT EACCES ETIMEDOUT
# stat_bits 61440 16384 32768 40960 4096 49152 256 128 64 32 16 8 4 2 1
# stat_is True False True True False True True False
# shlex1 ['a', 'b', 'c']
# shlex2 ['a', 'b', 'c']
# shlex3 ['a b', 'c'] ['a b', 'c']
# shlex4 ['a b'] ['ab cd'] ['ab cd']
# shlex5 [] [''] ['']
# shlex6 ['a"b'] ["a'b"] ['a"b']
# shlex7 '' abc 'a b'
# shlex8 'it'"'"'s' ["it's"]
# shlex9 a 'b c' 'd'"'"'e' 'f"g' '' 'h\i' ['a', 'b c', "d'e", 'f"g', '', 'h\\i']
# getuser celestea
# plat Linux x86_64 '' 3.12.0
# csvdr [{'name': 'alice', 'age': '30', 'city': 'NY'}, {'name': 'bob', 'age': '25', 'city': ''}]
# csvdw 'name,age\r\nalice,30\r\nbob,25\r\nx,\r\n'
# st1 fffffefffffffdffffffffffffff 14
# st2 (-1, 255, -2, 65535, -3, 4294967295)
# st3 fffffffefffffffffffdffffffff 14
# st4 fffffee08e04fb350000011f71fb04cb 16
# st5 (-1234567890123, 1234567890123)
# st6 35fb048ee0feffffcb04fb711f010000
# st7 0000c03f 3fc00000
# st8 182d4454fb210940 400921fb54442d18
# st9 68656c6c6f 616263
# st10 d6ffffff ffffffd6
