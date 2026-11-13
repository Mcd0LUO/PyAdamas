# binary_trees —— 完美二叉树分配与递归统计（Benchmarks Game 经典基准）
N = 12


class Node:
    def __init__(self, left, right):
        self.left = left
        self.right = right


def make(depth: int) -> "Optional[Node]":
    if depth == 0:
        return None
    return Node(make(depth - 1), make(depth - 1))


def check(node: "Optional[Node]") -> int:
    if node is None:
        return 1
    return 1 + check(node.left) + check(node.right)


root = make(N)
print(check(root))
