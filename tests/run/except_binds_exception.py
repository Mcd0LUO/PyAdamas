# except E as e：e 的静态类型是 E —— 富异常类按类方法面检查，内置异常按
# Exception 值处理；except (A, B) as e 全部分支同类型时用那个类型。
#
# 说明：内置异常值的**成员访问**仍是宽松的（CPython 允许给异常实例挂任意
# 属性，静态收成封闭成员表会误杀 e.errno / e.user_id 这类写法），见
# docs/frontend-gaps.md。


class MyErr(Exception):
    code: int

    def __init__(self, code: int) -> None:
        self.code = code

    def info(self) -> str:
        return "info:" + str(self.code)


def need_int(x: int) -> None:
    print(x)


def check(mode: int) -> None:
    try:
        if mode == 0:
            raise MyErr(3)
        raise ValueError("v")
    except MyErr as e:
        # 富异常：字段与方法都按类的方法面解析。
        print(e.info(), e.code)
    except (ValueError, TypeError) as e:
        # 两个内置异常分支（都是 Exception 值）：str/args 可用。
        print(str(e), e.args)


check(0)
check(1)

# e 的类型真的是 E：把它当 int 传给形参会报类型错（这里故意不写，见
# tests/strict/except_binds_exception_type.py）。
try:
    raise MyErr(9)
except MyErr as e:
    print(e.code + 1)

# --- expect ---
# info:3 3
# v ('v',)
# 10
