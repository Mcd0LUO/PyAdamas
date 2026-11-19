# NEW BUG N5 (silent wrong): datetime.strftime weekday/day-of-year fields wrong:
# %a/%A/%w/%u/%j/%U/%W. 2026-11-19 is Thursday; pa says Sunday, %j=001 (should 323).
# %Z/%z emit local tz (CST/+0000) for NAIVE datetime (CPython: empty).
# %b/%B month names and numeric directives are correct. date.weekday() is correct.
# Suspect: strftime builds a C tm without computing tm_wday/tm_yday (no mktime),
# and uses local timezone for naive values. Workaround: isoformat() + weekday()
# + a manual weekday-name table; avoid %a/%A/%j/%U/%W/%w/%u/%Z/%z.
import datetime
dt = datetime.datetime(2026, 11, 19, 14, 30, 5)
print("%A:", dt.strftime("%A"))   # CPython Thursday, pa Sunday
print("%a:", dt.strftime("%a"))   # CPython Thu, pa Sun
print("%j:", dt.strftime("%j"))   # CPython 323, pa 001
print("%w:", dt.strftime("%w"))   # CPython 4, pa 0
print("%Z:", repr(dt.strftime("%Z")))  # CPython '', pa 'CST'
print("weekday():", dt.weekday())      # both 3 (correct)
