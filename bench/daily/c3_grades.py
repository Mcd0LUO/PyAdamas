def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""c3_grades -- csv 分录型：成绩表统计与排名。

生成成绩 CSV（学生/科目/分数），按学生聚合分数列表，用 math.fsum
算平均（避开 float dict 与 sum 浮点精度差异），按平均排名（单次堆排序，
名字预排序保证 tie-break），等级分桶，写排名 CSV 读回打印。
"""
import csv
import os
import math

SRC: str = "_c3_grades.csv"
OUT: str = "_c3_rank.csv"
SUBJECTS: list[str] = ["math", "physics", "chemistry", "english", "history"]


class Student:
    name: str
    scores: list[int]
    avg: float

    def __init__(self, name: str) -> None:
        self.name = name
        self.scores = []
        self.avg = 0.0

    def finish(self) -> None:
        if len(self.scores) == 0:
            self.avg = 0.0
        else:
            self.avg = math.fsum([float(s) for s in self.scores]) / float(len(self.scores))


def gen_rows(n: int) -> list[list[str]]:
    rows: list[list[str]] = []
    state: int = 556677889
    nstudents: int = 60
    for i in range(n):
        state = lcg(state)
        student: str = "student-" + str(state % nstudents).zfill(2)
        state = lcg(state)
        subj: str = SUBJECTS[state % len(SUBJECTS)]
        state = lcg(state)
        score: int = 40 + state % 61
        rows.append([student, subj, str(score)])
    return rows


def grade_bucket(avg: float) -> str:
    if avg >= 90.0:
        return "A"
    if avg >= 80.0:
        return "B"
    if avg >= 70.0:
        return "C"
    if avg >= 60.0:
        return "D"
    return "F"


def main() -> None:
    rows: list[list[str]] = gen_rows(400)
    with open(SRC, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["student", "subject", "score"])
        w.writerows(rows)
    with open(SRC, "r", newline="") as f:
        table: list[list[str]] = list(csv.reader(f))
    data: list[list[str]] = table[1:]
    students: dict[str, Student] = {}
    subj_sum: dict[str, int] = {}
    subj_n: dict[str, int] = {}
    for row in data:
        name: str = row[0]
        subj: str = row[1]
        score: int = int(row[2])
        if name not in students:
            students[name] = Student(name)
        students[name].scores.append(score)
        subj_sum[subj] = subj_sum.get(subj, 0) + score
        subj_n[subj] = subj_n.get(subj, 0) + 1
    for name in students.keys():
        students[name].finish()
    print("rows", len(data), "students", len(students))
    print("-- subject averages --")
    for subj in sorted(subj_sum.keys()):
        avg: float = float(subj_sum[subj]) / float(subj_n[subj])
        print(subj, format(avg, ".3f"), subj_n[subj])
    # build in name order, then ONE stable sort by -avg (tie-break = name)
    names: list[str] = sorted(students.keys())
    ordered_students: list[Student] = []
    for name in names:
        ordered_students.append(students[name])

    def neg_avg(s: Student) -> float:
        return -s.avg

    ranked: list[Student] = sorted(ordered_students, key=neg_avg)
    buckets: dict[str, int] = {}
    out_rows: list[list[str]] = []
    rank: int = 0
    for s in ranked:
        rank += 1
        b: str = grade_bucket(s.avg)
        buckets[b] = buckets.get(b, 0) + 1
        out_rows.append([str(rank), s.name, format(s.avg, ".3f"), b])
    print("-- top 10 --")
    for r in out_rows[:10]:
        print(",".join(r))
    print("-- buckets --")
    for b in sorted(buckets.keys()):
        print(b, buckets[b])
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["rank", "student", "avg", "grade"])
        w.writerows(out_rows)
    with open(OUT, "r", newline="") as f:
        back: list[list[str]] = list(csv.reader(f))
    print("rank_rows", len(back) - 1)
    os.remove(SRC)
    os.remove(OUT)


if __name__ == "__main__":
    main()
