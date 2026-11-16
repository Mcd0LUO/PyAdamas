# mandelbrot —— Mandelbrot 迭代计数（浮点 / while 循环）
N = 350
MAXITER = 50


def run(size: int, maxiter: int) -> int:
    total = 0
    for y in range(size):
        ci = 2.0 * y / size - 1.0
        for x in range(size):
            cr = 2.0 * x / size - 1.5
            zr = 0.0
            zi = 0.0
            i = 0
            while i < maxiter and zr * zr + zi * zi <= 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                i += 1
            total += i
    return total


print(run(N, MAXITER))
