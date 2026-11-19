def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""b5_re_extract -- 字符串处理型：正则信息抽取。

从确定性生成的杂乱记录里抽取日期/金额/邮箱/电话/IP，
用 fullmatch 校验、规范化、去重、排序后输出结构化结果。
覆盖 search/findall(单组)/sub/split/fullmatch 与命名组。
"""
import re

DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})")
MONEY_RE = re.compile(r"\$([0-9]+\.[0-9]{2})")
EMAIL_RE = re.compile(r"[a-z0-9_.]+@[a-z0-9.]+\.[a-z]{2,}")
PHONE_RE = re.compile(r"\d{3}-\d{3}-\d{4}")
IP_RE = re.compile(r"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}")
ISO_DATE = re.compile(r"\d{4}-\d{2}-\d{2}")


def gen_records(n: int) -> list[str]:
    users: list[str] = ["alice", "bob", "carol", "dave", "erin", "frank", "grace", "heidi"]
    domains: list[str] = ["example.com", "mail.test.org", "corp.internal", "demo.net"]
    recs: list[str] = []
    state: int = 135791113
    for i in range(n):
        state = lcg(state)
        user: str = users[state % len(users)]
        state = lcg(state)
        dom: str = domains[state % len(domains)]
        state = lcg(state)
        year: int = 2020 + state % 7
        state = lcg(state)
        month: int = 1 + state % 12
        state = lcg(state)
        day: int = 1 + state % 28
        state = lcg(state)
        amount: int = state % 100000
        state = lcg(state)
        phone_a: int = 100 + state % 900
        state = lcg(state)
        phone_b: int = 100 + state % 900
        state = lcg(state)
        phone_c: int = state % 10000
        state = lcg(state)
        ip1: int = state % 256
        state = lcg(state)
        ip2: int = state % 256
        date: str = str(year) + "-" + str(month).zfill(2) + "-" + str(day).zfill(2)
        money: str = "$" + str(amount // 100) + "." + str(amount % 100).zfill(2)
        phone: str = str(phone_a) + "-" + str(phone_b) + "-" + str(phone_c).zfill(4)
        ip: str = str(ip1) + "." + str(ip2) + ".0.1"
        email: str = user + "@" + dom
        rec: str = "REC " + date + " | " + email + " | " + money + " | " + phone + " | " + ip + " | note#" + str(i)
        if i % 9 == 0:
            rec = "  " + rec + "  "
        recs.append(rec)
    return recs


def norm_date(s: str) -> str:
    parts: list[str] = s.split("-")
    return parts[0] + "/" + parts[1] + "/" + parts[2]


def main() -> None:
    recs: list[str] = gen_records(1500)
    dates: list[str] = []
    emails: dict[str, int] = {}
    phones: dict[str, int] = {}
    ips: dict[str, int] = {}
    total_cents: int = 0
    valid_dates: int = 0
    for rec in recs:
        r: str = rec.strip()
        dm = DATE_RE.search(r)
        if dm is not None:
            iso: str = dm.group(0)
            if ISO_DATE.fullmatch(iso) is not None:
                valid_dates += 1
                dates.append(norm_date(iso))
        for em in EMAIL_RE.findall(r):
            emails[em] = 1
        for ph in PHONE_RE.findall(r):
            phones[ph] = 1
        for ipm in IP_RE.findall(r):
            ips[ipm] = 1
        mm = MONEY_RE.search(r)
        if mm is not None:
            amt: str = mm.group(1)
            pieces: list[str] = amt.split(".")
            total_cents += int(pieces[0]) * 100 + int(pieces[1])
    print("records", len(recs), "valid_dates", valid_dates)
    print("unique_emails", len(emails), "unique_phones", len(phones), "unique_ips", len(ips))
    print("total_cents", total_cents)
    print("-- sample emails --")
    for e in sorted(emails.keys())[:8]:
        print(e)
    print("-- sample dates --")
    for d in dates[:6]:
        print(d)
    print("-- date year histogram --")
    years: dict[str, int] = {}
    for d in dates:
        y: str = d[:4]
        years[y] = years.get(y, 0) + 1
    for y in sorted(years.keys()):
        print(y, years[y])
    print("-- redact demo --")
    redacted: str = EMAIL_RE.sub("[email]", recs[0].strip())
    redacted = PHONE_RE.sub("[phone]", redacted)
    print(redacted)
    print("-- split demo --")
    fields: list[str] = re.split(r"\s*\|\s*", recs[1].strip())
    print("field_count", len(fields))


if __name__ == "__main__":
    main()
