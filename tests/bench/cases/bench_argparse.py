"""argparse 基准：建 parser + 解析 + 生成 help 文本。"""
import argparse

total = 0
r = 0
while r < 200:
    p = argparse.ArgumentParser(prog="bench", description="a benchmark parser")
    p.add_argument("-v", "--verbose", action="store_true", help="be loud")
    p.add_argument("-n", "--num", type=int, default=3, help="a number")
    p.add_argument("name", help="the name")
    ns = p.parse_args(["-v", "-n", "7", "alpha"])
    total += ns.num
    total += len(p.format_help())
    r += 1
print(total)
