#!/usr/bin/env python3
"""Pack a plugin folder into a .flua file.

    python tools/flua_pack.py examples/plugins/hello            # -> example.hello.flua
    python tools/flua_pack.py examples/plugins/hello -o out.flua

Checks the same things the client checks before it loads a package:
manifest.json, id, entry file, known permissions, no native files and
size limits. A .flua is a plain zip with manifest.json at its root.
"""
import argparse
import json
import os
import re
import sys
import zipfile

KNOWN_PERMISSIONS = {
    "player",
    "messages.read", "messages.send", "ui", "storage", "links",
    "actions", "display", "chats.read", "compose", "mtproto",
    "messages.manage",
}
KNOWN_CATEGORIES = {"appearance", "chats", "media", "privacy", "tools", "integrations", "fun", "libraries"}
NET_HOST = r"^(\*\.)?[a-z0-9]([a-z0-9\-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9\-]*[a-z0-9])?)+$"
NATIVE = (".dll", ".so", ".dylib", ".exe", ".pyd")
MAX_FILE = 4 * 1024 * 1024
MAX_TOTAL = 16 * 1024 * 1024
MAX_FILES = 512
API_VERSION = 1


def fail(message):
    print("error: " + message, file=sys.stderr)
    sys.exit(1)


def collect(root):
    files = []
    for folder, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for name in names:
            if name.startswith("."):
                continue
            full = os.path.join(folder, name)
            relative = os.path.relpath(full, root).replace(os.sep, "/")
            if relative.lower().endswith(NATIVE):
                fail("native code is not allowed: " + relative)
            if os.path.getsize(full) > MAX_FILE:
                fail("file is too large: " + relative)
            files.append((relative, full))
    if len(files) > MAX_FILES:
        fail("too many files")
    if sum(os.path.getsize(full) for _, full in files) > MAX_TOTAL:
        fail("package is too large")
    return sorted(files)


def check_manifest(root, files):
    path = os.path.join(root, "manifest.json")
    if not os.path.exists(path):
        fail("manifest.json is missing")
    with open(path, encoding="utf-8") as stream:
        try:
            manifest = json.load(stream)
        except ValueError as error:
            fail("manifest.json: " + str(error))
    plugin_id = manifest.get("id", "")
    if not re.match(r"^[a-z0-9][a-z0-9_.\-]{1,63}$", plugin_id):
        fail("bad id, use 2-64 of a-z 0-9 _ . -")
    if not manifest.get("name"):
        fail("name is missing")
    names = {relative for relative, _ in files}
    entry = manifest.get("entry", "init.lua")
    if entry not in names:
        fail("entry file is missing: " + entry)
    icon = manifest.get("icon", "")
    if icon and icon not in names:
        fail("icon file is missing: " + icon)
    api = manifest.get("api", {})
    low, high = api.get("min", 1), api.get("max", api.get("min", 1))
    if low > API_VERSION or high < API_VERSION:
        fail("needs plugin API %d-%d, the client has %d" % (low, high, API_VERSION))
    categories = manifest.get("categories", [])
    if isinstance(manifest.get("category"), str):
        categories = categories + [manifest["category"]]
    for category in categories:
        if str(category).strip().lower() not in KNOWN_CATEGORIES:
            print("warning: unknown category, the catalog skips it: " + str(category),
                  file=sys.stderr)
    for permission in manifest.get("permissions", []):
        if permission.startswith("net:"):
            if not re.match(NET_HOST, permission[4:]):
                fail("bad network permission: " + permission)
        elif permission not in KNOWN_PERMISSIONS:
            fail("unknown permission: " + permission)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder")
    parser.add_argument("-o", "--output")
    args = parser.parse_args()

    root = os.path.abspath(args.folder)
    files = collect(root)
    manifest = check_manifest(root, files)
    output = args.output or (manifest["id"] + ".flua")
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for relative, full in files:
            archive.write(full, relative)
    print("%s: %s %s, %d files" % (
        output, manifest["id"], manifest.get("version", ""), len(files)))


if __name__ == "__main__":
    main()
