#!/usr/bin/env python3
"""Fills SD_CARD/ with the game's files, from your own copy of the game.

    python tools/package_sd.py "C:/Games/Return to Castle Wolfenstein"

Copies the pk3 files the two games use to SD_CARD/switch/RtCWNX/main/, with lower
case names, after checking each one. Run the build first for the NROs: the result
is what goes on the root of the SD card.
"""

import argparse
import pathlib
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "SD_CARD" / "switch" / "RtCWNX" / "main"

# What each game cannot start without. sp_pak4 and the later mp_pak files are the
# last patch's, which GOG's and Steam's copies already have.
SINGLE_PLAYER = ["pak0.pk3", "sp_pak1.pk3", "sp_pak2.pk3", "sp_pak3.pk3", "sp_pak4.pk3"]
MULTIPLAYER = [
    "mp_bin.pk3", "mp_pak0.pk3", "mp_pak1.pk3", "mp_pak2.pk3", "mp_pak3.pk3", "mp_pak4.pk3", "mp_pak5.pk3",
    "mp_pakmaps0.pk3", "mp_pakmaps1.pk3", "mp_pakmaps2.pk3", "mp_pakmaps3.pk3", "mp_pakmaps4.pk3",
    "mp_pakmaps5.pk3", "mp_pakmaps6.pk3",
]


def find_main(game_dir):
    """The game's Main folder, however it is written."""
    for candidate in (game_dir, *sorted(game_dir.iterdir())):
        if candidate.is_dir() and candidate.name.lower() == "main":
            return candidate
    return None


def check_pak(path, seen, deep):
    """Returns the problems of one pk3: damage, and names that would clash without case."""
    problems = []
    try:
        with zipfile.ZipFile(path) as pak:
            if deep:
                bad = pak.testzip()
                if bad:
                    problems.append("%s: %s is damaged" % (path.name, bad))
            for info in pak.infolist():
                if info.is_dir():
                    continue
                key = info.filename.lower()
                if key in seen and seen[key] != info.filename:
                    problems.append("%s: %s and %s differ only by case" % (path.name, seen[key], info.filename))
                seen.setdefault(key, info.filename)
    except zipfile.BadZipFile as error:
        problems.append("%s: not a valid pk3 (%s)" % (path.name, error))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game_dir", type=pathlib.Path, help="the folder your copy of the game is installed in")
    parser.add_argument("--no-multiplayer", action="store_true", help="leave the multiplayer game's 170 MB out")
    parser.add_argument("--deep", action="store_true", help="also decompress every file, to find damaged ones")
    args = parser.parse_args()

    if not args.game_dir.is_dir():
        sys.exit("%s is not a folder" % args.game_dir)
    source = find_main(args.game_dir)
    if not source:
        sys.exit("%s has no Main folder: is it the game's folder?" % args.game_dir)

    available = {path.name.lower(): path for path in source.glob("*.pk3")}
    wanted = SINGLE_PLAYER + ([] if args.no_multiplayer else MULTIPLAYER)
    missing = [name for name in wanted if name not in available]
    if missing:
        sys.exit("Missing from %s: %s" % (source, ", ".join(missing)))

    TARGET.mkdir(parents=True, exist_ok=True)
    seen = {}
    problems = []
    total = 0
    for name in wanted:
        path = available[name]
        problems += check_pak(path, seen, args.deep)
        target = TARGET / name
        size = path.stat().st_size
        total += size
        if target.exists() and target.stat().st_size == size and target.stat().st_mtime >= path.stat().st_mtime:
            print("  have %s" % name)
            continue
        print("  copy %s (%.1f MB)" % (name, size / 1048576))
        shutil.copy2(path, target)

    for problem in problems:
        print("ERROR: " + problem)
    if problems:
        sys.exit(1)

    print("%d files, %.0f MB, %d files inside them" % (len(wanted), total / 1048576, len(seen)))
    print("Copy what is inside SD_CARD/ to the root of the SD card.")


if __name__ == "__main__":
    main()
