# fannkuch —— 全排列 + 前缀翻转（Benchmarks Game 经典基准）
N = 8


def fannkuch(n):
    perm = []
    count = []
    i = 0
    while i < n:
        perm.append(i)
        count.append(0)
        i += 1
    max_flips = 0
    checksum = 0
    perm_count = 0
    while True:
        k = perm[0]
        if k != 0:
            temp = []
            i = 0
            while i < n:
                temp.append(perm[i])
                i += 1
            flips = 0
            while temp[0] != 0:
                m = temp[0]
                i = 0
                while i < m:
                    t = temp[i]
                    temp[i] = temp[m]
                    temp[m] = t
                    i += 1
                    m -= 1
                flips += 1
            if flips > max_flips:
                max_flips = flips
            if perm_count % 2 == 0:
                checksum += flips
            else:
                checksum -= flips
        perm_count += 1
        i = 1
        while True:
            if i >= n:
                return checksum, max_flips
            if count[i] < i:
                break
            count[i] = 0
            i += 1
        first = perm[0]
        j = 0
        while j < i:
            perm[j] = perm[j + 1]
            j += 1
        perm[i] = first
        count[i] += 1


c, m = fannkuch(N)
print(c)
print(m)
