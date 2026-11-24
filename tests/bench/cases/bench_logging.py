"""logging 基准：Logger + Formatter 格式化（写 stderr）。"""
import logging

logging.basicConfig(level=logging.DEBUG)
lg = logging.getLogger("bench")

i = 0
while i < 3000:
    lg.info("message with some payload xxxxxxxxxxxxxxxx")
    i += 1
print("logged")
