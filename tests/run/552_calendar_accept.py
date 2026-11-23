# calendar isleap/monthrange/weekday 常量对拍；weekday 常量本轮补齐。
import calendar
print("leap", calendar.isleap(2024), calendar.isleap(2023))
r = calendar.monthrange(2024, 2)
print("mr", r[0] == calendar.THURSDAY, r[1])
print("md", calendar.monthrange(2024, 2)[1])

# --- expect ---
# leap True False
# mr True 29
# md 29
