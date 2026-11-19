"""b1_log_parse -- 字符串处理型：Web 访问日志解析与聚合。

确定性生成 N 条 apache 风格日志，用 re 提取字段，统计状态码分布、
热门路径、总流量、按小时直方图、方法分布。字符串/分配密集。
"""
import re

LOG_RE = re.compile(
    r'(\d+\.\d+\.\d+\.\d+) - - \[([^\]]+)\] "(\w+) (\S+) HTTP/1\.1" (\d{3}) (\d+)'
)


def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


def gen_logs(n: int) -> list[str]:
    paths: list[str] = ["/", "/index.html", "/api/users", "/api/orders", "/static/app.js",
                        "/static/style.css", "/login", "/logout", "/health", "/api/search",
                        "/images/logo.png", "/docs", "/favicon.ico", "/api/metrics"]
    methods: list[str] = ["GET", "GET", "GET", "POST", "PUT", "DELETE", "HEAD"]
    statuses: list[str] = ["200", "200", "200", "200", "301", "304", "404", "500", "403", "201"]
    lines: list[str] = []
    state: int = 123456789
    for i in range(n):
        state = lcg(state)
        ip: str = str(10 + state % 200) + "." + str(state % 256) + "." + str((state >> 8) % 256) + "." + str((state >> 16) % 256)
        state = lcg(state)
        path: str = paths[state % len(paths)]
        state = lcg(state)
        method: str = methods[state % len(methods)]
        state = lcg(state)
        status: str = statuses[state % len(statuses)]
        state = lcg(state)
        nbytes: int = 200 + state % 50000
        hour: int = (i * 7) % 24
        day: int = 1 + (i % 28)
        date: str = str(day).zfill(2) + "/Nov/2026:" + str(hour).zfill(2) + ":15:30 +0800"
        lines.append(ip + ' - - [' + date + '] "' + method + " " + path + ' HTTP/1.1" ' + status + " " + str(nbytes))
    return lines


def main() -> None:
    lines: list[str] = gen_logs(4000)
    status_counts: dict[str, int] = {}
    path_counts: dict[str, int] = {}
    method_counts: dict[str, int] = {}
    hour_bytes: dict[str, int] = {}
    total_bytes: int = 0
    matched: int = 0
    errors: int = 0
    for line in lines:
        m = LOG_RE.search(line)
        if m is None:
            errors += 1
            continue
        matched += 1
        method: str = m.group(3)
        path: str = m.group(4)
        status: str = m.group(5)
        nbytes: int = int(m.group(6))
        date: str = m.group(2)
        hour: str = date[12:14]
        total_bytes += nbytes
        status_counts[status] = status_counts.get(status, 0) + 1
        path_counts[path] = path_counts.get(path, 0) + 1
        method_counts[method] = method_counts.get(method, 0) + 1
        hour_bytes[hour] = hour_bytes.get(hour, 0) + nbytes
    print("lines", len(lines), "matched", matched, "errors", errors)
    print("total_bytes", total_bytes)
    print("-- status --")
    for s in sorted(status_counts.keys()):
        print(s, status_counts[s])
    print("-- methods --")
    for meth in sorted(method_counts.keys()):
        print(meth, method_counts[meth])
    print("-- top paths --")
    alpha: list[str] = sorted(path_counts.keys())

    def neg_count(p: str) -> int:
        return -path_counts[p]

    top: list[str] = sorted(alpha, key=neg_count)
    for p in top[:8]:
        print(p, path_counts[p])
    print("-- busiest hours --")
    hours: list[str] = sorted(hour_bytes.keys())

    def neg_bytes(h: str) -> int:
        return -hour_bytes[h]

    for h in sorted(hours, key=neg_bytes)[:5]:
        print(h, hour_bytes[h])
    avg: float = float(total_bytes) / float(matched)
    print("avg_bytes", format(avg, ".2f"))


if __name__ == "__main__":
    main()
