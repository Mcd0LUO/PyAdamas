# calendar 子集：闰年/星期/月历/时间戳换算
import calendar

print(calendar.isleap(2000), calendar.isleap(1900), calendar.isleap(2024))
print(calendar.isleap(2023), calendar.leapdays(2000, 2025))
print(calendar.leapdays(1970, 2000))
print(calendar.weekday(2025, 1, 1))
print(calendar.weekday(2024, 2, 29))
print(calendar.weekday(1969, 7, 20))
print(calendar.weekday(0, 1, 1))
d, n = calendar.monthrange(2025, 2)
print(int(d), n)
d, n = calendar.monthrange(2024, 2)
print(int(d), n)
d, n = calendar.monthrange(2025, 12)
print(int(d), n)
print(calendar.mdays[2], calendar.mdays[1])
print(calendar.day_name[0], calendar.day_name[6], calendar.day_abbr[3])
print(calendar.month_name[1], calendar.month_name[12], repr(calendar.month_name[0]))
print(len(calendar.day_name), len(calendar.month_name))
print(calendar.weekheader(3))
print(calendar.weekheader(9))
print(calendar.weekheader(2))

print(calendar.monthcalendar(2025, 1))
print(calendar.monthcalendar(2024, 2))
print(calendar.monthcalendar(2025, 6))

print(calendar.timegm((1970, 1, 1, 0, 0, 0)))
print(calendar.timegm((2025, 1, 1, 0, 0, 0)))
print(calendar.timegm((2024, 2, 29, 12, 30, 15)))
print(calendar.timegm((1969, 12, 31, 23, 59, 59)))

print(calendar.firstweekday())
calendar.setfirstweekday(6)
print(calendar.firstweekday())
print(calendar.monthcalendar(2025, 1)[0])
calendar.setfirstweekday(0)

print(calendar.month(2025, 1))
print(calendar.month(2024, 2, 3, 1))
print(calendar.calendar(2025))
print(calendar.calendar(2025, 2, 1, 2, 2))

try:
    calendar.monthrange(2025, 13)
except ValueError:
    print("bad month ValueError")
try:
    calendar.setfirstweekday(9)
except ValueError:
    print("bad weekday ValueError")
print(calendar.error is ValueError)


# --- expect ---
# True False True
# False 7
# 7
# 2
# 3
# 6
# 5
# 5 28
# 3 29
# 0 31
# 28 31
# Monday Sunday Thu
# January December ''
# 7 13
# Mon Tue Wed Thu Fri Sat Sun
#   Monday   Tuesday  Wednesday  Thursday   Friday   Saturday   Sunday 
# Mo Tu We Th Fr Sa Su
# [[0, 0, 1, 2, 3, 4, 5], [6, 7, 8, 9, 10, 11, 12], [13, 14, 15, 16, 17, 18, 19], [20, 21, 22, 23, 24, 25, 26], [27, 28, 29, 30, 31, 0, 0]]
# [[0, 0, 0, 1, 2, 3, 4], [5, 6, 7, 8, 9, 10, 11], [12, 13, 14, 15, 16, 17, 18], [19, 20, 21, 22, 23, 24, 25], [26, 27, 28, 29, 0, 0, 0]]
# [[0, 0, 0, 0, 0, 0, 1], [2, 3, 4, 5, 6, 7, 8], [9, 10, 11, 12, 13, 14, 15], [16, 17, 18, 19, 20, 21, 22], [23, 24, 25, 26, 27, 28, 29], [30, 0, 0, 0, 0, 0, 0]]
# 0
# 1735689600
# 1709209815
# -1
# 0
# 6
# [0, 0, 0, 1, 2, 3, 4]
#     January 2025
# Mo Tu We Th Fr Sa Su
#        1  2  3  4  5
#  6  7  8  9 10 11 12
# 13 14 15 16 17 18 19
# 20 21 22 23 24 25 26
# 27 28 29 30 31
# 
#        February 2024
# Mon Tue Wed Thu Fri Sat Sun
#               1   2   3   4
#   5   6   7   8   9  10  11
#  12  13  14  15  16  17  18
#  19  20  21  22  23  24  25
#  26  27  28  29
# 
#                                   2025
# 
#       January                   February                   March
# Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su
#        1  2  3  4  5                      1  2                      1  2
#  6  7  8  9 10 11 12       3  4  5  6  7  8  9       3  4  5  6  7  8  9
# 13 14 15 16 17 18 19      10 11 12 13 14 15 16      10 11 12 13 14 15 16
# 20 21 22 23 24 25 26      17 18 19 20 21 22 23      17 18 19 20 21 22 23
# 27 28 29 30 31            24 25 26 27 28            24 25 26 27 28 29 30
#                                                     31
# 
#        April                      May                       June
# Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su
#     1  2  3  4  5  6                1  2  3  4                         1
#  7  8  9 10 11 12 13       5  6  7  8  9 10 11       2  3  4  5  6  7  8
# 14 15 16 17 18 19 20      12 13 14 15 16 17 18       9 10 11 12 13 14 15
# 21 22 23 24 25 26 27      19 20 21 22 23 24 25      16 17 18 19 20 21 22
# 28 29 30                  26 27 28 29 30 31         23 24 25 26 27 28 29
#                                                     30
# 
#         July                     August                  September
# Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su
#     1  2  3  4  5  6                   1  2  3       1  2  3  4  5  6  7
#  7  8  9 10 11 12 13       4  5  6  7  8  9 10       8  9 10 11 12 13 14
# 14 15 16 17 18 19 20      11 12 13 14 15 16 17      15 16 17 18 19 20 21
# 21 22 23 24 25 26 27      18 19 20 21 22 23 24      22 23 24 25 26 27 28
# 28 29 30 31               25 26 27 28 29 30 31      29 30
# 
#       October                   November                  December
# Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su      Mo Tu We Th Fr Sa Su
#        1  2  3  4  5                      1  2       1  2  3  4  5  6  7
#  6  7  8  9 10 11 12       3  4  5  6  7  8  9       8  9 10 11 12 13 14
# 13 14 15 16 17 18 19      10 11 12 13 14 15 16      15 16 17 18 19 20 21
# 20 21 22 23 24 25 26      17 18 19 20 21 22 23      22 23 24 25 26 27 28
# 27 28 29 30 31            24 25 26 27 28 29 30      29 30 31
# 
#                    2025
# 
#       January               February
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#        1  2  3  4  5                  1  2
#  6  7  8  9 10 11 12   3  4  5  6  7  8  9
# 13 14 15 16 17 18 19  10 11 12 13 14 15 16
# 20 21 22 23 24 25 26  17 18 19 20 21 22 23
# 27 28 29 30 31        24 25 26 27 28
# 
#        March                 April
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#                 1  2      1  2  3  4  5  6
#  3  4  5  6  7  8  9   7  8  9 10 11 12 13
# 10 11 12 13 14 15 16  14 15 16 17 18 19 20
# 17 18 19 20 21 22 23  21 22 23 24 25 26 27
# 24 25 26 27 28 29 30  28 29 30
# 31
# 
#         May                   June
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#           1  2  3  4                     1
#  5  6  7  8  9 10 11   2  3  4  5  6  7  8
# 12 13 14 15 16 17 18   9 10 11 12 13 14 15
# 19 20 21 22 23 24 25  16 17 18 19 20 21 22
# 26 27 28 29 30 31     23 24 25 26 27 28 29
#                       30
# 
#         July                 August
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#     1  2  3  4  5  6               1  2  3
#  7  8  9 10 11 12 13   4  5  6  7  8  9 10
# 14 15 16 17 18 19 20  11 12 13 14 15 16 17
# 21 22 23 24 25 26 27  18 19 20 21 22 23 24
# 28 29 30 31           25 26 27 28 29 30 31
# 
#      September              October
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#  1  2  3  4  5  6  7         1  2  3  4  5
#  8  9 10 11 12 13 14   6  7  8  9 10 11 12
# 15 16 17 18 19 20 21  13 14 15 16 17 18 19
# 22 23 24 25 26 27 28  20 21 22 23 24 25 26
# 29 30                 27 28 29 30 31
# 
#       November              December
# Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su
#                 1  2   1  2  3  4  5  6  7
#  3  4  5  6  7  8  9   8  9 10 11 12 13 14
# 10 11 12 13 14 15 16  15 16 17 18 19 20 21
# 17 18 19 20 21 22 23  22 23 24 25 26 27 28
# 24 25 26 27 28 29 30  29 30 31
# 
# bad month ValueError
# bad weekday ValueError
# True
