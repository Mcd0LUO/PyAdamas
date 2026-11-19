def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""c2_inventory -- csv 分录型：库存分组与补货。

生成库存 CSV（sku/仓库/数量/单位成本分/补货点），按仓库分组聚合
数量与库存价值（整数分），筛出需补货 sku 并按缺口排序，写补货 CSV。
分组用 dict[str, list[int]]（显式 guard，避开 setdefault 列表 bug）。
"""
import csv
import os

SRC: str = "_c2_inventory.csv"
OUT: str = "_c2_reorder.csv"
WAREHOUSES: list[str] = ["east", "west", "north", "south"]


def gen_rows(n: int) -> list[list[str]]:
    rows: list[list[str]] = []
    state: int = 998877665
    for i in range(n):
        state = lcg(state)
        wh: str = WAREHOUSES[state % len(WAREHOUSES)]
        state = lcg(state)
        qty: int = state % 200
        state = lcg(state)
        cost: int = 50 + state % 4950
        state = lcg(state)
        reorder: int = 20 + state % 60
        sku: str = "SKU-" + str(1000 + i)
        rows.append([sku, wh, str(qty), str(cost), str(reorder)])
    return rows


def main() -> None:
    rows: list[list[str]] = gen_rows(600)
    with open(SRC, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["sku", "warehouse", "qty", "unit_cost_cents", "reorder_point"])
        w.writerows(rows)
    with open(SRC, "r", newline="") as f:
        table: list[list[str]] = list(csv.reader(f))
    data: list[list[str]] = table[1:]
    wh_qty: dict[str, int] = {}
    wh_value: dict[str, int] = {}
    wh_skus: dict[str, list[int]] = {}
    reorder_rows: list[list[str]] = []
    total_value: int = 0
    for idx in range(len(data)):
        row: list[str] = data[idx]
        sku: str = row[0]
        wh: str = row[1]
        qty: int = int(row[2])
        cost: int = int(row[3])
        rp: int = int(row[4])
        value: int = qty * cost
        total_value += value
        wh_qty[wh] = wh_qty.get(wh, 0) + qty
        wh_value[wh] = wh_value.get(wh, 0) + value
        if wh not in wh_skus:
            wh_skus[wh] = []
        wh_skus[wh].append(idx)
        if qty < rp:
            gap: int = rp - qty
            reorder_rows.append([sku, wh, str(qty), str(rp), str(gap)])
    print("rows", len(data), "total_value_cents", total_value)
    print("-- by warehouse --")
    for wh in sorted(wh_qty.keys()):
        print(wh, "qty", wh_qty[wh], "value", wh_value[wh], "skus", len(wh_skus[wh]))
    print("-- reorder need --")
    print("reorder_count", len(reorder_rows))

    # single int composite-key sort (gap desc, sku asc tie-break);
    # str-returning sort keys over-release RC at scale (workaround: int key)
    def reorder_key(r: list[str]) -> int:
        sku_num: int = int(r[0][4:])
        return (999999 - int(r[4])) * 100000 + sku_num

    ordered: list[list[str]] = sorted(reorder_rows, key=reorder_key)
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["sku", "warehouse", "qty", "reorder_point", "gap"])
        w.writerows(ordered)
    with open(OUT, "r", newline="") as f:
        back: list[list[str]] = list(csv.reader(f))
    print("top_reorder")
    for r in back[1:6]:
        print(",".join(r))
    os.remove(SRC)
    os.remove(OUT)


if __name__ == "__main__":
    main()
