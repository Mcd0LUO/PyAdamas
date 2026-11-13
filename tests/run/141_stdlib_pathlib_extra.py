from pathlib import Path

p = Path("/tmp/pyadamas_path_test.txt")
for part in p.parts:
    print(part)
print(str(p.with_suffix(".md")))
p.write_text("hi")
print(p.read_text(), p.exists())
p.unlink()
print(p.exists())

d = Path("/tmp/pyadamas_dir_test")
d.mkdir(exist_ok=True)
print(d.exists())
d.rmdir()
print(d.exists())

# --- expect ---
# /
# tmp
# pyadamas_path_test.txt
# /tmp/pyadamas_path_test.md
# hi True
# False
# True
# False
