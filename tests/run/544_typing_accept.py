# typing 常用 API 对拍；@overload 运行期语义差异见 stdlib/typing.py 注释。
import typing
print("cast", typing.cast(int, 5))
print("TC", typing.TYPE_CHECKING)
T = typing.TypeVar("T")
print("tv", str(T) != "")
print("ov", typing.overload is not None)

# --- expect ---
# cast 5
# TC False
# tv True
# ov True
