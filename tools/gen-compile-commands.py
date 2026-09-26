#!/usr/bin/env python3
"""Generate compile_commands.json for clangd (Zed, VS Code, ...).

Runs `make -n -B all` (dry run, nothing is built), picks every compiler
invocation with the exact per-file flags from the Makefile, and adds the
userland/samara programs (built with the i686 musl cross gcc).

    make compile_commands.json      # or: python3 tools/gen-compile-commands.py
"""
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

out = subprocess.run(["make", "-n", "-B", "all"], capture_output=True, text=True)
if out.returncode != 0 and not out.stdout:
    sys.exit("make -n failed:\n" + out.stderr)

# join backslash-continued lines into whole commands
cmds, cur = [], ""
for line in out.stdout.splitlines():
    if line.endswith("\\"):
        cur += line[:-1] + " "
        continue
    cmds.append(cur + line)
    cur = ""

entries = {}
for cmd in cmds:
    try:
        argv = shlex.split(cmd)
    except ValueError:
        continue
    if not argv or not argv[0].endswith("gcc") or "-c" not in argv:
        continue
    src = argv[argv.index("-c") + 1]
    if not src.endswith((".c", ".S")):
        continue
    entries[src] = {"directory": ROOT, "file": os.path.join(ROOT, src), "arguments": argv}

# userland/samara: hosted C for SamaraOS userspace (musl, i686 linux)
musl = os.path.join(ROOT, "toolchain/musl/i686-linux-musl-cross/bin/i686-linux-musl-gcc")
sam = os.path.join(ROOT, "userland/samara")
if os.path.isdir(sam):
    for f in sorted(os.listdir(sam)):
        if f.endswith(".c"):
            src = os.path.join("userland/samara", f)
            entries[src] = {
                "directory": ROOT,
                "file": os.path.join(ROOT, src),
                "arguments": [musl, "-static", "-no-pie", "-O2", "-std=gnu11",
                              "-I" + sam, "-c", src, "-o", "/dev/null"],
            }

with open("compile_commands.json", "w") as fp:
    json.dump(list(entries.values()), fp, indent=1)
print(f"compile_commands.json: {len(entries)} files")
