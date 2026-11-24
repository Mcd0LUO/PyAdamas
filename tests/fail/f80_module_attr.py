# expect: module 'json' has no attribute 'nonexistent_attr'
# pya-lsp 缺口 2（0.484）：check 阶段就要报模块属性错误，不能等到 build。
# 之前 check 不加载 import 图，`json.nonexistent_attr` 能过 check、build 才报。
import json

def main() -> None:
    x: int = json.nonexistent_attr(1)
    print(x)

main()