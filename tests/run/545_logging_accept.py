# logging 等级与 Logger 基础 API 对拍。
import logging
print("lvl", logging.DEBUG, logging.INFO, logging.WARNING, logging.ERROR, logging.CRITICAL)
lg = logging.getLogger("t")
print("name", lg.name)
lg.setLevel(logging.INFO)
print("enabled", lg.isEnabledFor(logging.INFO), lg.isEnabledFor(logging.DEBUG))
print("ln", logging.getLevelName(logging.WARNING))

# --- expect ---
# lvl 10 20 30 40 50
# name t
# enabled True False
# ln WARNING
