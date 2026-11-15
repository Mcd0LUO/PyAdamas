# expect: is not implemented yet
# 异常值的 __traceback__/__context__ 在 CPython 里是真实属性，本项目还没实现。
# 以前降级把异常对象指针当值放行（print(e.__traceback__) 打出 99498760223040），
# 是静默算错；现在 build 期明确拒绝，消息写「未实现」而不是「没有该属性」。
def f() -> None:
    try:
        raise ValueError("boom")
    except ValueError as e:
        print(e.__traceback__)


f()
