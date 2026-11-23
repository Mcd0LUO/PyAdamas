import state


def touch() -> int:
    state.bump()
    return state.counter
