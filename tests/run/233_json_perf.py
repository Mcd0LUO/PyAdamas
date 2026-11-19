# 233_json_perf.py —— json 优化路径正确性哨兵（0.314）。
# 只钉正确性不钉时间：三类负载（字符串密集 dict 列表 / 嵌套
# dict[str, list[int]] / 纯 int 列表）的 dumps+loads 循环，加上
# ensure_ascii=False 与 indent 路径。优化前后输出必须逐字节不变。
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
    users = make_users(100)
    nested = make_nested(50, 20)
    ints = make_ints(2000)
    su = 0
    sn = 0
    si = 0
    for r in range(300):
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
    # 优化路径抽查：内容逐字节往返
    u0 = json.loads(json.dumps(users[0]), {"": ""})
    print(u0["name"], u0["note"])
    n49 = json.loads(json.dumps(nested), {"": [0]})
    print(n49["key_49"][19], len(n49))
    i1999 = json.loads(json.dumps(ints), [0])
    print(i1999[1999], i1999[0], len(i1999))
    # ensure_ascii=False / indent / separators / 键转换 路径
    s = json.dumps(users[1], ensure_ascii=False, sort_keys=True)
    print(s)
    r2 = json.loads(s, {"": ""})
    print(r2["note"])
    print(json.dumps(nested["key_0"], indent=2))
    print(json.dumps({3: "c", 1: "a", 2: "b"}, sort_keys=True, separators=(",", ":")))
    print(json.dumps(ints[0:5], separators=(",", ":")))

main()

# --- expect ---
# users 4134000 100
# nested 2217000 50
# ints 4323300 2000
# user_0 line1
# line2	"quoted" \ backslash 中文
# 49019 50
# 13990 -3 2000
# {"city": "Springfield", "email": "user1@example.com", "name": "user_1", "note": "line1\nline2\t\"quoted\" \\ backslash 中文"}
# line1
# line2	"quoted" \ backslash 中文
# [
#   0,
#   1,
#   2,
#   3,
#   4,
#   5,
#   6,
#   7,
#   8,
#   9,
#   10,
#   11,
#   12,
#   13,
#   14,
#   15,
#   16,
#   17,
#   18,
#   19
# ]
# {"1":"a","2":"b","3":"c"}
# [-3,4,11,18,25]
