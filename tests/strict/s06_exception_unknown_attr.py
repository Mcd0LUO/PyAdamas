# expect: has no attribute 'nosuchattr'
# 内置异常值只实现 args / __cause__ 两个属性。任意其它属性名以前一律
# retype 成 unknown 放行，运行期把异常对象指针当整数打印（静默算错）。
def f() -> None:
    try:
        raise ValueError("boom")
    except ValueError as e:
        print(e.nosuchattr)


f()
