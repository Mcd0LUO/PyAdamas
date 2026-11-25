import subprocess


def run() -> subprocess.CompletedProcess:
    return subprocess.run(["true"])
