def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""c5_csv_clean -- csv 分录型：脏 CSV 清洗去重。

生成含引号/内嵌逗号/首尾空白/重复行的脏 CSV，csv.reader 读回，
逐字段规范化（strip/lower/title），按规范化键去重（dict[str,int]），
按名字自然序重建行（标量排序避开堆元素 str 键排序），QUOTE_ALL 写净 CSV。
"""
import csv
import os

SRC: str = "_c5_dirty.csv"
OUT: str = "_c5_clean.csv"
FIRST: list[str] = ["alice", "bob", "carol", "dave", "erin", "frank"]
LAST: list[str] = ["smith", "jones", "brown", "white", "black", "green"]
CITIES: list[str] = ["new york", "los angeles", "chicago", "houston", "phoenix"]


def gen_rows(n: int) -> list[list[str]]:
    rows: list[list[str]] = []
    state: int = 271828182
    for i in range(n):
        state = lcg(state)
        first: str = FIRST[state % len(FIRST)]
        state = lcg(state)
        last: str = LAST[state % len(LAST)]
        state = lcg(state)
        city: str = CITIES[state % len(CITIES)]
        state = lcg(state)
        note: str = "note, with comma" if state % 3 == 0 else 'quote"inside'
        if i % 4 == 0:
            first = "  " + first.upper() + " "
            last = last.upper()
        if i % 7 == 0:
            city = " " + city + "  "
        rows.append([first, last, city, note])
    return rows


def norm_name(s: str) -> str:
    return s.strip().lower().capitalize()


def main() -> None:
    rows: list[list[str]] = gen_rows(500)
    with open(SRC, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["first", "last", "city", "note"])
        w.writerows(rows)
    with open(SRC, "r", newline="") as f:
        table: list[list[str]] = list(csv.reader(f))
    data: list[list[str]] = table[1:]
    print("dirty_rows", len(data))
    seen: dict[str, int] = {}
    clean_by_key: dict[str, list[str]] = {}
    dupes: int = 0
    for row in data:
        first: str = norm_name(row[0])
        last: str = norm_name(row[1])
        city: str = row[2].strip().lower().title()
        note: str = row[3].strip()
        key: str = first + "|" + last + "|" + city
        if key in seen:
            dupes += 1
            continue
        seen[key] = 1
        clean_by_key[key] = [first, last, city, note]
    print("unique_rows", len(seen), "dupes_removed", dupes)
    keys: list[str] = sorted(seen.keys())
    out_rows: list[list[str]] = []
    for key in keys:
        out_rows.append(clean_by_key[key])
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f, quoting=csv.QUOTE_ALL, lineterminator="\n")
        w.writerow(["first", "last", "city", "note"])
        w.writerows(out_rows)
    with open(OUT, "r", newline="") as f:
        back: list[list[str]] = list(csv.reader(f))
    print("clean_rows", len(back) - 1)
    print("-- sample clean --")
    for r in back[1:6]:
        print(",".join(r))
    with open(OUT, "r", newline="") as f:
        raw: str = f.read()
    print("first_raw_line", repr(raw.splitlines()[1]))
    os.remove(SRC)
    os.remove(OUT)


if __name__ == "__main__":
    main()
