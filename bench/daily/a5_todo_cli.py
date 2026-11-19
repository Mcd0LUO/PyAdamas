"""a5_todo_cli -- 快速往返型：内存待办事项管理器。

确定性生成一批任务，按 argv 命令执行 list/add/done/stats/by-priority，
用类存任务、按 key 排序、过滤统计。无参数时跑完整演示流程。
"""
import sys


class Task:
    tid: int
    title: str
    priority: int
    done: bool
    tag: str

    def __init__(self, tid: int, title: str, priority: int, tag: str) -> None:
        self.tid = tid
        self.title = title
        self.priority = priority
        self.done = False
        self.tag = tag

    def __repr__(self) -> str:
        mark: str = "x" if self.done else " "
        return "[" + mark + "] #" + str(self.tid) + " p" + str(self.priority) + " " + self.title + " (" + self.tag + ")"


def by_priority(t: Task) -> int:
    return -t.priority


def by_title(t: Task) -> str:
    return t.title


def seed_tasks() -> list[Task]:
    titles: list[str] = [
        "write report", "fix login bug", "review PR", "update docs",
        "deploy staging", "backup db", "refactor parser", "add tests",
        "sync calendar", "clean inbox", "plan sprint", "audit logs",
    ]
    tags: list[str] = ["work", "dev", "ops", "admin"]
    tasks: list[Task] = []
    i: int = 0
    for title in titles:
        pr: int = (i * 7 + 3) % 5 + 1
        tag: str = tags[(i * 3) % 4]
        tasks.append(Task(i + 1, title, pr, tag))
        i += 1
    return tasks


def cmd_list(tasks: list[Task]) -> None:
    for t in tasks:
        print(repr(t))


def cmd_done(tasks: list[Task], tid: int) -> None:
    for t in tasks:
        if t.tid == tid:
            t.done = True
            print("completed", tid)
            return
    print("no such task", tid)


def cmd_stats(tasks: list[Task]) -> None:
    total: int = len(tasks)
    done: int = 0
    by_tag: dict[str, int] = {}
    pr_sum: int = 0
    for t in tasks:
        if t.done:
            done += 1
        pr_sum += t.priority
        if t.tag not in by_tag:
            by_tag[t.tag] = 0
        by_tag[t.tag] += 1
    print("total", total, "done", done, "open", total - done)
    print("avg_priority", format(float(pr_sum) / float(total), ".3f"))
    for tag in sorted(by_tag.keys()):
        print("tag", tag, by_tag[tag])


def cmd_priority(tasks: list[Task]) -> None:
    ordered: list[Task] = sorted(tasks, key=by_priority)
    for t in ordered:
        if not t.done:
            print(repr(t))


def main() -> None:
    tasks: list[Task] = seed_tasks()
    args: list[str] = sys.argv[1:]
    if len(args) == 0:
        cmd_done(tasks, 2)
        cmd_done(tasks, 5)
        cmd_done(tasks, 99)
        cmd_stats(tasks)
        print("-- by priority --")
        cmd_priority(tasks)
        print("-- alpha --")
        for t in sorted(tasks, key=by_title):
            print(t.title)
    else:
        cmd: str = args[0]
        if cmd == "list":
            cmd_list(tasks)
        elif cmd == "stats":
            cmd_stats(tasks)
        elif cmd == "priority":
            cmd_priority(tasks)
        elif cmd == "done" and len(args) > 1:
            cmd_done(tasks, int(args[1]))
        else:
            print("unknown command", cmd)


if __name__ == "__main__":
    main()
