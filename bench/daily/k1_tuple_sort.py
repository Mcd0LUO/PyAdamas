"""k1_tuple_sort -- 已知 bug 覆盖（0.312 正在修）：元组列表自然排序 no-op。

排行榜场景：list[tuple[str, int]] 按分数自然排序。CPython 正确排序，
pyadamas 0.311 的 .sort()/sorted() 对元组列表是 no-op（原样返回）。
本脚本故意覆盖该已知 bug：自然排序段输出会不一致（标注「已知」），
并给出 sorted(key=int 函数) 的绕行对照（该路径正确）。
"""


def by_score(t: tuple[str, int]) -> int:
    return -t[1]


def by_name(t: tuple[str, int]) -> str:
    return t[0]


def main() -> None:
    board: list[tuple[str, int]] = [("carol", 20), ("alice", 30), ("dave", 10), ("bob", 10)]
    print("input", board)
    # 已知 bug：元组列表自然 .sort() 是 no-op
    copy1: list[tuple[str, int]] = list(board)
    copy1.sort()
    print("natural .sort()", copy1)
    # 已知 bug：sorted() 自然序同样 no-op（含 reverse）
    print("natural sorted()", sorted(board))
    print("natural sorted(reverse)", sorted(board, reverse=True))
    # 绕行：key 函数返回 int，正确
    print("workaround sorted(key=-score)", sorted(board, key=by_score))
    # 名字 tie-break：先按名字（标量 str 自然序正确）再稳定按分数
    alpha: list[tuple[str, int]] = sorted(board, key=by_name)
    print("name then score", sorted(alpha, key=by_score))


if __name__ == "__main__":
    main()
