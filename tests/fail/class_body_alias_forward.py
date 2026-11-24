# expect: undefined name 'short'
# 类体是顺序求值：别名只能看到**前面**已经执行过的 def。
class A:
    __str__ = short

    def short(self) -> str:
        return "x"