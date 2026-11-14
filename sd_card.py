#!/usr/bin/env python3
"""sd_card.py - gather the latest build of every OCKM plugin into one folder for the SD card.

    python3 sd_card.py

Result (not committed, rebuilt each time):

    SD Card/
      VST2 Plugins/
        OCKM - VST - OCKM-03/
        OCKM - VST - OCKM Gate/
        ...

On the SD card, open (or create) the VST2 Plugins folder, then drag everything inside
"SD Card/VST2 Plugins" into it. Don't drag the whole VST2 Plugins folder onto the card:
Finder would replace the card's folder and remove any other plugins already on it.

It copies what is already built; it does not build. A plugin whose code, UI or skin
changed after its last build is still copied, with a warning to rebuild it.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "SD Card", "VST2 Plugins")
SOURCES = ("src", "ui.json", "plugin.json", "skins", "assets", "Content")


def newest_mtime(path):
    """Latest modification time of a file, or of any file under a folder."""
    if os.path.isfile(path):
        return os.path.getmtime(path)
    latest = 0.0
    for root, _, files in os.walk(path):
        for f in files:
            if f != ".DS_Store":
                latest = max(latest, os.path.getmtime(os.path.join(root, f)))
    return latest


def main():
    # Start clean so a renamed or retired plugin doesn't linger. Only this output folder is removed.
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    copied, stale, missing = [], [], []
    for name in sorted(os.listdir(HERE)):
        project = os.path.join(HERE, name)
        if not os.path.isfile(os.path.join(project, "plugin.json")):
            continue
        build = os.path.join(project, "build")
        outputs = [d for d in os.listdir(build) if d.startswith("OCKM - VST - ")] if os.path.isdir(build) else []
        if len(outputs) != 1:
            missing.append(name)
            continue
        built = os.path.join(build, outputs[0])
        built_at = newest_mtime(os.path.join(built, "VST"))
        changed_at = max(newest_mtime(os.path.join(project, s)) for s in SOURCES
                         if os.path.exists(os.path.join(project, s)))
        if changed_at > built_at:
            stale.append(name)
        shutil.copytree(built, os.path.join(OUT, outputs[0]), ignore=shutil.ignore_patterns(".DS_Store"))
        copied.append(outputs[0])

    print("Copied into %s:" % OUT)
    for c in copied:
        print("  " + c)
    for s in stale:
        print("WARNING: %s changed after its last build. Rebuild it, then run this again." % s)
    for m in missing:
        print("WARNING: %s has no build yet; skipped." % m)
    print("\nOn the SD card: drag everything inside 'SD Card/VST2 Plugins' into the card's VST2 Plugins folder.")
    return 1 if (stale or missing) else 0


if __name__ == "__main__":
    sys.exit(main())
