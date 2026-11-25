from pathlib import Path

p: Path = Path('/tmp/x.txt')
print(p.name, p.stem, p.suffix)

# --- expect ---
# x.txt x .txt
