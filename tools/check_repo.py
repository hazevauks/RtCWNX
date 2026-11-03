#!/usr/bin/env python3
"""Fails if the repository holds game data or personal information.

Checks what git tracks or is about to commit. Run it before a commit; CI runs it on every push.
"""

import pathlib
import re
import subprocess
import sys

# game data and things made from it
FORBIDDEN_NAME = re.compile(
    r"\.(pk3|bsp|aas|mdc|mds|md3|roq|wav|tga|shader|menu|exe|dll|so|dylib|dksh|nro|elf|nacp|cdkey)$"
    r"|(^|/)rtcwkey$|(^|/)hunkusage\.dat$|Documentation\.txt$",
    re.IGNORECASE,
)

# personal information: paths from a home folder, and mail addresses that are not GitHub's own
FORBIDDEN_TEXT = [
    (re.compile(rb"[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s\"']+"), "a path inside a home folder"),
    (re.compile(rb"/home/[a-z][\w.-]+/|/c/Users/"), "a path inside a home folder"),
    (re.compile(rb"[\w.+-]+@(gmail|hotmail|outlook|yahoo|proton(mail)?|icloud)\.[a-z.]+", re.IGNORECASE), "a personal mail address"),
]

# Upstream's files keep their authors' addresses: only what this port wrote is checked for them.
UPSTREAM = "third_party/"


def git_files():
    output = subprocess.run(["git", "ls-files", "--cached", "-z"], capture_output=True, check=True).stdout
    return [name.decode("utf-8", "replace") for name in output.split(b"\0") if name]


def main():
    problems = []
    names = git_files()
    for name in names:
        if FORBIDDEN_NAME.search(name):
            problems.append("%s: not allowed in the repository" % name)
            continue
        path = pathlib.Path(name)
        if not path.is_file():
            continue
        if path.stat().st_size > 8 * 1024 * 1024:
            problems.append("%s: larger than 8 MB" % name)
            continue
        if name.startswith(UPSTREAM) or path.name == "check_repo.py" or path.suffix.lower() in (".jpg", ".png", ".webp"):
            continue
        data = path.read_bytes()
        for pattern, what in FORBIDDEN_TEXT:
            if pattern.search(data):
                problems.append("%s: contains %s" % (name, what))

    for problem in problems:
        print("ERROR: " + problem)
    print("%d files checked, %d problems" % (len(names), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
