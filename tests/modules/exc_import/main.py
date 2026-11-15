from lib import LibError, SubError, RichError
from lib import boom, boom_sub, boom_rich
import lib

# 1) 导入的异常类出现在 except 名字位置，接住 lib 里抛的异常
try:
    boom()
except LibError as e:
    print("1", e)

# 2) 导入的异常类出现在 raise 名字位置（本模块构造并抛）
try:
    raise LibError("local")
except LibError as e:
    print("2", e)

# 3) 裸 raise 导入类（无消息）
try:
    raise SubError
except SubError as e:
    print("3", e)

# 4) 抛导入的子类，父类 except 能接
try:
    boom_sub()
except LibError as e:
    print("4", e)

# 5) 本模块定义子类，继承导入的异常类（运行期注册 LocalSub -> LibError）
class LocalSub(LibError):
    pass

try:
    raise LocalSub("local-sub")
except LibError as e:
    print("5", e)

# 6) 模块属性形式：except lib.LibError / raise lib.LibError(...)
try:
    raise lib.LibError("attr")
except lib.LibError as e:
    print("6", e)

# 7) 导入的富异常：raise 构造 + except as e 取回实例
try:
    raise RichError(11, "made")
except RichError as e:
    print("7", e.code, e.name, e.describe())

# 8) 富异常从 lib 抛出，本模块按类接
try:
    boom_rich()
except RichError as e:
    print("8", e.code, e.describe())

# 9) 导入异常按内置父类 ValueError 接
try:
    boom()
except ValueError as e:
    print("9", e)

# 10) 多参异常：导入类也能走 args 语义
try:
    raise LibError("a", "b")
except LibError as e:
    print("10", len(e.args), e.args[0], e.args[1])

# --- expect ---
# 1 from lib
# 2 local
# 3 
# 4 from lib sub
# 5 local-sub
# 6 attr
# 7 11 made 11:made
# 8 7 7:rich
# 9 from lib
# 10 2 a b
