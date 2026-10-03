# SamaraOS: state and TODO

A hobby 32-bit x86 OS in C (~33k lines of kernel, ~44k total without DOOM
and generated font tables). Runs in QEMU. User programs are static Linux
i386 musl binaries, the kernel speaks the Linux syscall ABI. See README.md
for the full feature list.

## How to run and test

```
./toolchain/build-cross.sh                  # once
make                                        # kernel: build/samara.elf
make run SELF=0                             # SELF=0: always the host build
make run SELF=0 VDISK=build/ports.img       # + ports disk at /usr/local (python3, git, vim, lua)
make run SELF=0 NICS=3                      # + virtio-net cards eth1, eth2
./userland/build-quickjs.sh && ./userland/build-sysroot.sh && make   # userland in the kernel
./userland/build-ports.sh [lua git vim python]                       # ports disk
```

* ssh `-p 2222 root@localhost` (no password), telnet 2323, guest :80 on host 8080
* headless tests: `APPEND="autosh=<cmd>"` runs a shell command, prints to COM1, powers off;
  `-display none -monitor unix:/tmp/samara-qmon,server,nowait`, then
  `python3 tools/browser-test/mon.py type 'desktop\n'`, `... shot out.png`
* browser layout on the host: `tools/browser-test/build.sh`,
  `python3 tools/browser-test/inl.py <url> page.html`, `/tmp/bt page.html 1290 0 out.ppm`
* CSS test pages: `cd www/css && python3 -m http.server 8000`, in the OS `10.0.2.2:8000`
* phone notifications: `tools/ntfy.sh msg|pic|read` (topic in ~/.config/samara-ntfy-topic)
* headers are tracked (`-MMD`), a plain `make` is enough after editing a .h

## Done recently (all committed and pushed)

* apk (Alpine 3.20 x86) in the sysroot; root disk `build/root.img` (ext2
  labelled `/`) merged into the root, incremental ext2 write back, lazy file
  reads + eviction when the file arena is full, in place growth of file buffers
* network: e1000 driver, DHCP on every card, PCI bus walk cached, netlink
  route dumps (getifaddrs), recvmsg msg_name fix (musl 1.2.5 DNS)
* X11: named AF_UNIX sockets + SCM_RIGHTS, epoll, fb0 mmap and ioctls,
  /sys/class/graphics/fb0, SysV shm, evdev devices `/dev/input-kbd` and
  `/dev/input-mouse`, `xsamara` + `/etc/X11/xorg.conf`; console stays off
  the screen while /dev/fb0 is owned
* eventfd, timerfd, memfd_create; fork shares MAP_SHARED/shm pages; capget;
  mremap answers like linux (musl stack probe); java (openjdk8) and nodejs 20 run
* boot-made /etc files and xsamara are static (not saved to the root disk
  unless edited)

## TODO for the next session

1. **SMP** (the big one): ACPI MADT parse, LAPIC + IOAPIC, AP trampoline
   (real mode -> protected), per-CPU GDT/TSS/stacks and current task,
   spinlocks instead of `irq_save` in the scheduler, heap, fs, net, tty/pty,
   then a scheduler that runs tasks on all cores. QEMU `-smp 4`. llvmpipe and
   java want it badly.
2. **Boot runs with interrupts off** for a long time: KVM replays the lost
   PIT ticks afterwards and the uptime jumps (worked around with
   `kvm-pit.lost_tick_policy=discard` in the Makefile).
3. **virtio-gpu** (skipped): needs the modern virtio PCI transport.
4. **Browser leftovers:** progressive JPEG, SVG (at least icons), `z-index`,
   `box-shadow`, `transform`, `:hover`.
5. **X:** no mode switching (boot resolution), no
   signalfd. The X root background needs `xsetroot` a few seconds late.
6. **ext2:** a type clash between a boot file and a disk entry keeps the
   boot file; old root disks still carry stale boot-made /etc files (rm once).
7. **Flaky:** once saw new pty shells spin in ash's job control loop
   (TIOCGPGRP went to the console tty). Not reproduced, keep an eye on it.
8. Debug output to remove when stable: `samara: gw arp ...` (net.c),
   `kdbg()` helper in pty.c.

## Style

Code like a person wrote it (see CLAUDE.md, local only): snake_case, short
names, few comments with some life in them, no docstrings, no banners, no
over-engineering. Talk to the user in Russian, informally.
