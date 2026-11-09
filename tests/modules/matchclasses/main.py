import shapes


def kind(x) -> str:
    match x:
        case shapes.Circle():
            return "circle"
        case shapes.Shape():
            return "shape"
        case _:
            return "?"


print(kind(shapes.Circle()), kind(shapes.Shape()), kind(1))

# --- expect ---
# circle shape ?
