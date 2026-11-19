def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""c4_sales -- csv 分录型：销售透视聚合。

生成销售 CSV（区域/月份/产品/件数/单价分），按 区域|月份 复合 str 键
聚合营收（整数分），按产品汇总并排名（int 键单次排序），写区域×月份
透视 CSV 读回打印。
"""
import csv
import os

SRC: str = "_c4_sales.csv"
OUT: str = "_c4_pivot.csv"
REGIONS: list[str] = ["east", "west", "north", "south"]
MONTHS: list[str] = ["01", "02", "03", "04", "05", "06"]
PRODUCTS: list[str] = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta"]


def gen_rows(n: int) -> list[list[str]]:
    rows: list[list[str]] = []
    state: int = 314159265
    for i in range(n):
        state = lcg(state)
        region: str = REGIONS[state % len(REGIONS)]
        state = lcg(state)
        month: str = MONTHS[state % len(MONTHS)]
        state = lcg(state)
        prod: str = PRODUCTS[state % len(PRODUCTS)]
        state = lcg(state)
        units: int = 1 + state % 50
        state = lcg(state)
        price: int = 100 + state % 9900
        rows.append([region, month, prod, str(units), str(price)])
    return rows


def main() -> None:
    rows: list[list[str]] = gen_rows(1000)
    with open(SRC, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["region", "month", "product", "units", "price_cents"])
        w.writerows(rows)
    with open(SRC, "r", newline="") as f:
        table: list[list[str]] = list(csv.reader(f))
    data: list[list[str]] = table[1:]
    pivot: dict[str, int] = {}
    prod_rev: dict[str, int] = {}
    prod_units: dict[str, int] = {}
    grand: int = 0
    for row in data:
        region: str = row[0]
        month: str = row[1]
        prod: str = row[2]
        units: int = int(row[3])
        price: int = int(row[4])
        rev: int = units * price
        grand += rev
        key: str = region + "|" + month
        pivot[key] = pivot.get(key, 0) + rev
        prod_rev[prod] = prod_rev.get(prod, 0) + rev
        prod_units[prod] = prod_units.get(prod, 0) + units
    print("rows", len(data), "grand_revenue_cents", grand)
    print("-- product ranking --")
    prods: list[str] = sorted(prod_rev.keys())

    def neg_rev(p: str) -> int:
        return -prod_rev[p]

    ranked: list[str] = sorted(prods, key=neg_rev)
    for p in ranked:
        print(p, prod_rev[p], prod_units[p])
    print("-- pivot region x month --")
    out_rows: list[list[str]] = []
    header: list[str] = ["region"]
    for m in MONTHS:
        header.append(m)
    out_rows.append(header)
    for region in REGIONS:
        row: list[str] = [region]
        for m in MONTHS:
            key: str = region + "|" + m
            row.append(str(pivot.get(key, 0)))
        out_rows.append(row)
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerows(out_rows)
    with open(OUT, "r", newline="") as f:
        back: list[list[str]] = list(csv.reader(f))
    for r in back:
        print(",".join(r))
    os.remove(SRC)
    os.remove(OUT)


if __name__ == "__main__":
    main()
