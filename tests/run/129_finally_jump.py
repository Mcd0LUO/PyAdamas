# finally 体内的 return / break / continue。

import asyncio


def ret_override():
    try:
        return 1
    finally:
        return 2


def swallow():
    try:
        raise ValueError("x")
    finally:
        return 3


def after_swallow():
    try:
        try:
            raise ValueError("x")
        finally:
            return 4
    except ValueError:
        return 5


def loop_break():
    out = 0
    for i in range(5):
        try:
            out = out + i
        finally:
            if i == 2:
                break
    return out


def loop_continue():
    out = 0
    for i in range(4):
        try:
            out = out + 10
        finally:
            if i % 2 == 0:
                continue
        out = out + 1
    return out


def nested():
    try:
        try:
            return "inner"
        finally:
            print("inner-finally")
    finally:
        print("outer-finally")


def gen_return():
    try:
        yield 1
    finally:
        return


def gen_break():
    for i in range(3):
        try:
            yield i
        finally:
            if i == 1:
                break


print("override", ret_override())
print("swallow", swallow())
print("after", after_swallow())
print("break", loop_break())
print("continue", loop_continue())
nv = nested()
print("nested", nv)

g = gen_return()
print("g first", next(g))
try:
    next(g)
except StopIteration:
    print("g stop")

h = gen_break()
print("h", next(h))
print("h", next(h))
try:
    next(h)
except StopIteration:
    print("h stop")


async def aco():
    try:
        return 10
    finally:
        return 20


async def aswallow():
    try:
        raise ValueError("x")
    finally:
        return 30


async def aawait():
    try:
        return 1
    finally:
        await asyncio.sleep(0)
        return 2


async def amain():
    print("aco", await aco())
    print("aswallow", await aswallow())
    print("aawait", await aawait())


asyncio.run(amain())

# --- expect ---
# override 2
# swallow 3
# after 4
# break 3
# continue 42
# inner-finally
# outer-finally
# nested inner
# g first 1
# g stop
# h 0
# h 1
# h stop
# aco 20
# aswallow 30
# aawait 2
