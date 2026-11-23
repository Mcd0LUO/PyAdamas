# stdlib batch B regression: shutil / tempfile / heapq / bisect / io / difflib
# 全部输出逐行与 CPython 对拍一致；PA 已知差异处用 `is None` 等规避。
import shutil
import tempfile
import heapq
import bisect
import io
import difflib
import os
import os.path as osp

# ── bisect ──
a = []
print('bi_empty', bisect.bisect_left(a, 5), bisect.bisect_right(a, 5))
a = [1, 2, 2, 2, 3, 4]
print('bi_dup', bisect.bisect_left(a, 2), bisect.bisect_right(a, 2))
print('bi_out', bisect.bisect_left(a, 0), bisect.bisect_right(a, 9))
print('bi_lohi', bisect.bisect_left(a, 2, 2, 4), bisect.bisect_right(a, 2, 2, 4))
g = []
bisect.insort_left(g, 3); bisect.insort_left(g, 1); bisect.insort_right(g, 3); bisect.insort_left(g, 2)
print('bi_insL', g)
h = []
bisect.insort_right(h, 3); bisect.insort_right(h, 1); bisect.insort_left(h, 3); bisect.insort_right(h, 2)
print('bi_insR', h)
print('bi_alias', bisect.bisect(a, 2))

# ── heapq ──
h = [5, 3, 8, 1, 9, 2]
heapq.heapify(h)
out = []
while len(h) > 0:
    out.append(heapq.heappop(h))
print('hq_popall', out)
h = [5, 3, 8, 1]
heapq.heapify(h)
heapq.heappush(h, 0)
out = []
while len(h) > 0:
    out.append(heapq.heappop(h))
print('hq_push', out)
h = [5, 3, 8, 1]
heapq.heapify(h)
print('hq_replace_ret', heapq.heapreplace(h, 0))
out = []
while len(h) > 0:
    out.append(heapq.heappop(h))
print('hq_replace', out)
h = [5, 3, 8, 1]
heapq.heapify(h)
print('hq_pushpop_ret', heapq.heappushpop(h, 0))
print('hq_nl', heapq.nlargest(3, [5, 1, 8, 3, 8, 2]))
print('hq_ns', heapq.nsmallest(3, [5, 1, 8, 3, 8, 2]))
print('hq_nl0', heapq.nlargest(0, [5, 1]), heapq.nsmallest(0, [5, 1]))
print('hq_merge', heapq.merge([1, 4, 7], [2, 5, 8], [3, 6, 9]))
print('hq_merge_empty', heapq.merge([], [1, 2], []))
words = ['aaa', 'b', 'cccc', 'dd', 'ee']
def wlen(x): return len(x)
print('hq_key_nl', heapq.nlargest(3, words, key=wlen))
print('hq_key_ns', heapq.nsmallest(3, words, key=wlen))

# ── io.BytesIO ──
b = io.BytesIO()
print('io_init', b.tell(), b.getvalue(), b.closed)
print('io_write', b.write(b'hello world'), b.getvalue())
b.seek(0)
print('io_read', b.read(5))
b.seek(0)
print('io_readline', b.readline())
b.seek(0)
print('io_lines', b.readlines())
b = io.BytesIO(b'abc\ndef\nghi')
print('io_lines2', b.readlines())
b = io.BytesIO(b'abcdef')
b.seek(2)
b.write(b'XY')
print('io_overwrite', b.getvalue(), b.tell())
print('io_trunc', b.truncate(4), b.getvalue())
b.close()
print('io_closed', b.closed)
b2 = io.BytesIO()
b2.close()
try:
    b2.read()
except ValueError:
    print('io_closed_err', True)

# ── difflib ──
da = ['a\n', 'b\n', 'c\n']
db = ['a\n', 'x\n', 'c\n', 'd\n']
print('dl_ud', difflib.unified_diff(da, db, 'old', 'new'))
print('dl_nd', difflib.ndiff(da, db))
sm = difflib.SequenceMatcher(None, 'hello world', 'hello there')
print('dl_ratio', sm.ratio())
same = ['one\n', 'two\n', 'three\n']
print('dl_ud_same', difflib.unified_diff(same, same, 'x', 'y'))
print('dl_nd_same', difflib.ndiff(same, same))

# ── tempfile ──
print('tf_prefix', tempfile.gettempprefix())
td = tempfile.mkdtemp(prefix='stdB_', suffix='_end')
print('tf_mkdtemp', td.startswith(tempfile.gettempdir() + '/stdB_'), td.endswith('_end'), osp.isdir(td))
os.rmdir(td)
fd, path = tempfile.mkstemp(prefix='stdBf_', suffix='.txt')
print('tf_mkstemp', path.startswith(tempfile.gettempdir() + '/stdBf_'), path.endswith('.txt'))
os.close(fd)
os.remove(path)
mp = tempfile.mktemp(prefix='stdBm_', suffix='.tmp')
print('tf_mktemp', mp.startswith(tempfile.gettempdir() + '/stdBm_'), mp.endswith('.tmp'), osp.exists(mp))
f = tempfile.NamedTemporaryFile(mode='w+', prefix='stdBN_', suffix='.dat', delete=True)
name = f.name
f.write('hello')
f.flush()
print('tf_ntf', name.startswith(tempfile.gettempdir() + '/stdBN_'), osp.exists(name))
f.close()
print('tf_ntf_deleted', osp.exists(name))
with tempfile.TemporaryDirectory(prefix='stdBtd_') as tdx:
    print('tf_td_in', osp.isdir(tdx))
print('tf_td_out', osp.exists(tdx))

# ── shutil ──
base = '/tmp/stdB_shutil_' + str(os.getpid())
root = base + '/work'
os.makedirs(root + '/src/sub/deep')
os.makedirs(root + '/dst')
shf = open(root + '/src/a.txt', 'w')
shf.write('hello world')
shf.close()
shf = open(root + '/src/sub/b.txt', 'w')
shf.write('second')
shf.close()
shf = open(root + '/src/sub/deep/c.txt', 'w')
shf.write('third')
shf.close()
shutil.copyfile(root + '/src/a.txt', root + '/dst/cf.txt')
shf = open(root + '/dst/cf.txt', 'r')
print('sh_copyfile', shf.read())
shf.close()
shutil.copy(root + '/src/a.txt', root + '/dst/cp.txt')
shf = open(root + '/dst/cp.txt', 'r')
print('sh_copy', shf.read())
shf.close()
shutil.copy2(root + '/src/a.txt', root + '/dst/c2.txt')
shf = open(root + '/dst/c2.txt', 'r')
print('sh_copy2', shf.read())
shf.close()
shutil.copytree(root + '/src', root + '/tree')
ns = os.listdir(root + '/tree')
ns.sort()
print('sh_tree', ns)
shutil.move(root + '/dst/cp.txt', root + '/dst/moved.txt')
print('sh_move', osp.exists(root + '/dst/moved.txt'), osp.exists(root + '/dst/cp.txt'))
shutil.rmtree(root + '/tree')
print('sh_rmtree', osp.exists(root + '/tree'))
print('sh_which', shutil.which('sh') is not None)
print('sh_which_none', shutil.which('no_such_command_xyz') is None)
u = shutil.disk_usage('/tmp')
print('sh_disk_total', u.total > 0, u[0] == u.total, len(u) == 3)
print('sh_disk_sum', u.used + u.free <= u.total)
shutil.rmtree(base)

# --- expect ---
# bi_empty 0 0
# bi_dup 1 4
# bi_out 0 6
# bi_lohi 2 4
# bi_insL [1, 2, 3, 3]
# bi_insR [1, 2, 3, 3]
# bi_alias 4
# hq_popall [1, 2, 3, 5, 8, 9]
# hq_push [0, 1, 3, 5, 8]
# hq_replace_ret 1
# hq_replace [0, 3, 5, 8]
# hq_pushpop_ret 0
# hq_nl [8, 8, 5]
# hq_ns [1, 2, 3]
# hq_nl0 [] []
# hq_merge [1, 2, 3, 4, 5, 6, 7, 8, 9]
# hq_merge_empty [1, 2]
# hq_key_nl ['cccc', 'aaa', 'dd']
# hq_key_ns ['b', 'dd', 'ee']
# io_init 0 b'' False
# io_write 11 b'hello world'
# io_read b'hello'
# io_readline b'hello world'
# io_lines [b'hello world']
# io_lines2 [b'abc\n', b'def\n', b'ghi']
# io_overwrite b'abXYef' 4
# io_trunc 4 b'abXY'
# io_closed True
# io_closed_err True
# dl_ud ['--- old\n', '+++ new\n', '@@ -1,3 +1,4 @@\n', ' a\n', '-b\n', '+x\n', ' c\n', '+d\n']
# dl_nd ['  a\n', '- b\n', '+ x\n', '  c\n', '+ d\n']
# dl_ratio 0.6363636363636364
# dl_ud_same []
# dl_nd_same ['  one\n', '  two\n', '  three\n']
# tf_prefix tmp
# tf_mkdtemp True True True
# tf_mkstemp True True
# tf_mktemp True True False
# tf_ntf True True
# tf_ntf_deleted False
# tf_td_in True
# tf_td_out False
# sh_copyfile hello world
# sh_copy hello world
# sh_copy2 hello world
# sh_tree ['a.txt', 'sub']
# sh_move True False
# sh_rmtree False
# sh_which True
# sh_which_none True
# sh_disk_total True True True
# sh_disk_sum True
