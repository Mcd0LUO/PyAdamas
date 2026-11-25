import subprocess


def spawn() -> subprocess.Popen:
    return subprocess.Popen(["true"])
