#!/usr/bin/env python3
"""Which kernel should `make run` boot?

    pick-kernel.py <disk.img> <host kernel> <extract path>

If the disk (the OS's persistent /mnt) holds /samara.elf - put there by
`selfbuild` inside SamaraOS - and it is newer than the host build, extract it
and print its path; otherwise print the host kernel. Status goes to stderr.

The guest stamps FAT entries with its RTC, which QEMU runs in UTC; mtools
reads FAT times as local time, so the offset is added back before comparing.
"""
import os, subprocess, sys, time

disk, host, out = sys.argv[1:4]
if os.environ.get("SELF", "1") == "0" or not os.path.exists(disk):
    print(host)
    sys.exit(0)
r = subprocess.run(["mcopy", "-m", "-o", "-n", "-i", disk, "::/samara.elf", out],
                   capture_output=True)
if r.returncode != 0 or not os.path.exists(out):
    print(host)                                   # no self-built kernel on the disk
    sys.exit(0)
self_t = os.stat(out).st_mtime + time.localtime().tm_gmtoff
host_t = os.stat(host).st_mtime if os.path.exists(host) else 0
fmt = lambda t: time.strftime("%Y-%m-%d %H:%M", time.localtime(t))
if self_t > host_t:
    sys.stderr.write(f"*** booting the SELF-BUILT kernel from {disk}:/samara.elf "
                     f"({fmt(self_t)}, newer than {host} {fmt(host_t)}).\n"
                     f"*** host build instead: make run SELF=0\n")
    print(out)
else:
    sys.stderr.write(f"(ignoring {disk}:/samara.elf from {fmt(self_t)}: "
                     f"the host build {fmt(host_t)} is newer)\n")
    print(host)
