# expect: undefined name
# 模块级未定义名字：check 宽松（标准库基线需要），但 build/run 必须报错。
# 否则降级成初值为 0 的全局，运行期静默算出 0 而不是 NameError。
print(totally_undefined_xyz)
