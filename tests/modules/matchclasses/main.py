import shapes


def kind(x) -> str:
    match x:
        case shapes.Circle():
            return "circle"
        case shapes.Shape():
            return "shape"
        case _:
            return "?"


def where(p) -> str:
    match p:
        case shapes.Point(0, y):
            return "axis-y" + str(y)
        case shapes.Point(x=1):
            return "one"
        case shapes.Point(x=v, y=w):
            return "at" + str(v) + "," + str(w)
        case _:
            return "?"


print(kind(shapes.Circle()), kind(shapes.Shape()), kind(1))
print(where(shapes.Point(0, 5)), where(shapes.Point(1, 2)), where(shapes.Point(3, 4)))

# --- expect ---
# circle shape ?
# axis-y5 one at3,4
