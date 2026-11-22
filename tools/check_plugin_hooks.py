#!/usr/bin/env python3
"""Check that every plugin hook is still in place.

Each call from client code into plugins/plugins_hooks.h is marked with
"// PLUGIN-HOOK: <name>". After merging a new AyuGram / Telegram Desktop
version run this script: it fails if a marker got lost, appeared twice,
or a new one was added without listing it in tools/plugin_hooks.txt.
"""
import os
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = os.path.join(ROOT, "Telegram", "SourceFiles")
LIST = os.path.join(ROOT, "tools", "plugin_hooks.txt")
MARKER = re.compile(r"//\s*PLUGIN-HOOK:\s*([a-z0-9_]+)")


def expected():
    with open(LIST, encoding="utf-8") as stream:
        return [
            line.split("#", 1)[0].strip()
            for line in stream
            if line.split("#", 1)[0].strip()
        ]


def found():
    result = Counter()
    places = {}
    for folder, _, files in os.walk(SOURCES):
        if os.path.relpath(folder, SOURCES).replace("\\", "/").startswith("plugins"):
            continue
        for name in files:
            if not name.endswith((".cpp", ".h", ".mm")):
                continue
            path = os.path.join(folder, name)
            with open(path, encoding="utf-8", errors="replace") as stream:
                for number, line in enumerate(stream, 1):
                    match = MARKER.search(line)
                    if match:
                        hook = match.group(1)
                        result[hook] += 1
                        places.setdefault(hook, []).append(
                            "%s:%d" % (os.path.relpath(path, ROOT), number))
    return result, places


def main():
    want = expected()
    have, places = found()
    errors = []
    for hook in want:
        if have[hook] == 0:
            errors.append("missing: %s" % hook)
        elif have[hook] > 1:
            errors.append("duplicated: %s (%s)" % (hook, ", ".join(places[hook])))
    for hook in sorted(set(have) - set(want)):
        errors.append("not listed in tools/plugin_hooks.txt: %s (%s)" % (
            hook, ", ".join(places[hook])))
    if errors:
        print("\n".join(errors))
        return 1
    print("plugin hooks: %d in place" % len(want))
    return 0


if __name__ == "__main__":
    sys.exit(main())
