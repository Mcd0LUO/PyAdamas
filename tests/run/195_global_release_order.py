# 全局堆对象覆盖：先持有新值，再写入全局，最后释放旧值。
def tracked(n: int):
    try:
        yield n
        yield n * 10
        yield n * 100
        yield n * 1000
    finally:
        if n == 1:
            observe_release()
        elif n == 3:
            print("release", n)


# 首次写入时，原生全局槽的旧值为空指针。
current = tracked(1)
saved = tracked(0)


def replace_current(n: int) -> None:
    global current
    current = tracked(n)


def observe_release() -> None:
    # 旧值析构时必须看见新值，重入写入也不能被外层覆盖。
    print("published", next(current))
    replace_current(3)
    print("reentered", next(current))


print("start", next(current))
replace_current(2)
print("after", next(current))

# 没有其他持有者时自赋值；随后让两个全局共享同一生成器。
current = current
saved = current
replace_current(4)
print("alias", next(saved))
current = saved
saved = tracked(0)
print("retained", next(current))
replace_current(0)
print("done")

# --- expect ---
# start 1
# published 2
# reentered 3
# after 30
# alias 300
# retained 3000
# release 3
# done
