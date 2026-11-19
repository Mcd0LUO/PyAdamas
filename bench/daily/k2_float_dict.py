"""k2_float_dict -- 已知 bug 覆盖（0.313 正在修）：float 值 dict 损坏。

dict[str, float] 的字面量/计算值存储、下标读、get、增量赋值。
pyadamas 0.311 下标读返回错值、非字面量存储后查找丢失、+= 损坏。
本脚本故意覆盖该已知 bug（输出会不一致，标注「已知」），
并给出整数分（int 值 dict）绕行对照（该路径正确）。
"""


def main() -> None:
    d: dict[str, float] = {}
    d["a"] = 1.5
    v: float = 0.1 + 0.2
    d["b"] = v
    print("float dict subscript:", d["a"], d["b"])
    print("float dict get:", d.get("a"), d.get("b"), d.get("zz", 9.5))
    d["c"] = d["a"] + d["b"]
    print("float dict computed store:", d["c"], d.get("c", -1.0))
    d["a"] += 0.5
    print("float dict augassign:", d["a"], d.get("a"))
    # 绕行：金额用整数分，dict[str, int] 正确
    cents: dict[str, int] = {}
    cents["a"] = 150
    cents["b"] = 30
    cents["a"] += 50
    print("int cents workaround:", cents["a"], cents["b"], cents.get("a"), cents.get("zz", 0))


if __name__ == "__main__":
    main()
