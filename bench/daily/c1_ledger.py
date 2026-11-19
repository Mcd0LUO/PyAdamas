def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""c1_ledger -- csv 分录型：财务日记账聚合。

确定性生成分录 CSV（日期/科目/摘要/金额分/借贷），写文件后用
csv.reader 读回，按科目与日期过滤、聚合净额（整数分避免浮点 dict），
试算平衡，写汇总 CSV 再读回打印。金额一律 int 分。
"""
import csv
import os
import datetime

SRC: str = "_c1_journal.csv"
OUT: str = "_c1_summary.csv"
ACCOUNTS: list[str] = ["cash", "receivable", "revenue", "expense", "payable", "inventory"]


def gen_rows(n: int) -> list[list[str]]:
    rows: list[list[str]] = []
    state: int = 1122334455
    base = datetime.date(2026, 1, 1)
    for i in range(n):
        state = lcg(state)
        acct: str = ACCOUNTS[state % len(ACCOUNTS)]
        state = lcg(state)
        cents: int = 100 + state % 500000
        state = lcg(state)
        kind: str = "debit" if state % 2 == 0 else "credit"
        day_off: int = i % 90
        d = base + datetime.timedelta(days=day_off)
        desc: str = "entry-" + str(i) + "-" + acct
        rows.append([d.isoformat(), acct, desc, str(cents), kind])
    return rows


def write_csv(path: str, rows: list[list[str]]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["date", "account", "description", "amount_cents", "kind"])
        w.writerows(rows)


def main() -> None:
    rows: list[list[str]] = gen_rows(800)
    write_csv(SRC, rows)
    with open(SRC, "r", newline="") as f:
        table: list[list[str]] = list(csv.reader(f))
    header: list[str] = table[0]
    data: list[list[str]] = table[1:]
    print("header", ",".join(header))
    print("rows", len(data))
    net: dict[str, int] = {}
    debit_total: int = 0
    credit_total: int = 0
    q1_count: int = 0
    for row in data:
        date: str = row[0]
        acct: str = row[1]
        cents: int = int(row[3])
        kind: str = row[4]
        signed: int = cents if kind == "debit" else -cents
        net[acct] = net.get(acct, 0) + signed
        if kind == "debit":
            debit_total += cents
        else:
            credit_total += cents
        if date < "2026-04-01":
            q1_count += 1
    print("debit_total", debit_total, "credit_total", credit_total)
    print("balanced", debit_total == credit_total)
    print("q1_entries", q1_count)
    print("-- net by account --")
    summary: list[list[str]] = []
    for acct in sorted(net.keys()):
        print(acct, net[acct])
        summary.append([acct, str(net[acct])])
    write_csv(OUT, summary)
    with open(OUT, "r", newline="") as f:
        back: list[list[str]] = list(csv.reader(f))
    print("summary_rows", len(back) - 1)
    print("summary_first", ",".join(back[1]))
    os.remove(SRC)
    os.remove(OUT)


if __name__ == "__main__":
    main()
