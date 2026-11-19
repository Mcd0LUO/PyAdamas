# json_bench.py —— json 吞吐基准（0.314 优化轮取证用）。
# 三类负载：字符串密集 dict 列表 / 嵌套 dict[str, list[int]] / 纯 int 列表。
# 只打印正确性摘要（不打印时间），计时由外部 perf/taskset 完成。
import json

def make_users(n: int) -> list[dict[str, str]]:
    users: list[dict[str, str]] = []
    for i in range(n):
        u: dict[str, str] = {
            "name": "user_" + str(i),
            "email": "user" + str(i) + "@example.com",
            "city": "Springfield",
            "note": "line1\nline2\t\"quoted\" \\ backslash 中文",
        }
        users.append(u)
    return users

def make_nested(n: int, m: int) -> dict[str, list[int]]:
    d: dict[str, list[int]] = {}
    for i in range(n):
        xs: list[int] = []
        for j in range(m):
            xs.append(i * 1000 + j)
        d["key_" + str(i)] = xs
    return d

def make_ints(n: int) -> list[int]:
    xs: list[int] = []
    for i in range(n):
        xs.append(i * 7 - 3)
    return xs

def main() -> None:
    users = make_users(200)
    nested = make_nested(50, 20)
    ints = make_ints(2000)
    su = 0
    sn = 0
    si = 0
    for r in range(200):
        s = json.dumps(users)
        back = json.loads(s, [{"": ""}])
        su += len(s) + len(back)
        s2 = json.dumps(nested)
        back2 = json.loads(s2, {"": [0]})
        sn += len(s2) + len(back2)
        s3 = json.dumps(ints)
        back3 = json.loads(s3, [0])
        si += len(s3) + len(back3)
    print("users", su, len(users))
    print("nested", sn, len(nested))
    print("ints", si, len(ints))
    u0 = users[0]
    print(u0["name"], u0["email"])
    print(ints[1999], nested["key_49"][19])

main()
