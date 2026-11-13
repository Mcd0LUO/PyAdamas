from pathlib import Path

p = Path("/tmp/pyadamas_pl.txt")
print(p.is_absolute(), Path("rel.txt").is_absolute())
p.write_bytes(b"abc")
print(p.read_bytes())
print(p.exists())
q = p.joinpath("sub")
print(str(q))
print(str(p.relative_to("/tmp")))
print(str(Path("/tmp/a/b").relative_to("/tmp/a")))
print(str(Path("x").absolute())[:1] == "/")
p.unlink()

t = Path("/tmp/pyadamas_touch.txt")
t.touch()
print(t.exists())
t.unlink()

# --- expect ---
# True False
# b'abc'
# True
# /tmp/pyadamas_pl.txt/sub
# pyadamas_pl.txt
# b
# True
# True
