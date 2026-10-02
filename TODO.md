# SamaraOS: state and TODO

A hobby 32-bit x86 OS in C (~31k lines of kernel, ~40k total without DOOM
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

## Done recently (not committed yet, the user commits himself)

* browser: own CSS engine (cascade, selectors, flex, grid with areas, tables,
  floats, positioning), images (PNG/JPEG/GIF), live JavaScript through a
  resident domjs, big pages (Wikipedia) get their CSS
* network: several cards (rtl8139 + virtio-net), routing by subnet, per-card
  ifconfig; PCI config access made atomic (network was dead on 2 of 3 boots)
* virtio-blk disks (/dev/vda..), ext2 read/write (ext3/4 read-only),
  symlinks in the VFS, file names up to 127 chars
* ports on an ext2 disk labelled /usr/local: CPython 3.12, git 2.46, vim 9.1, Lua 5.4
* many terminals (sh on ptys), stacked in one taskbar button; idle shells
  no longer spin the CPU

## TODO for the next session

1. **SMP** (the big one): ACPI MADT parse, LAPIC + IOAPIC, AP trampoline
   (real mode -> protected), per-CPU GDT/TSS/stacks and current task,
   spinlocks instead of `irq_save` in the scheduler, heap, fs, net, tty/pty,
   then a scheduler that runs tasks on all cores. QEMU `-smp 4`. Do it in
   small steps: first APs just start and halt, then idle loops, then tasks.
2. **virtio-gpu** (skipped): needs the modern virtio PCI transport (caps,
   MMIO BARs). 2D: resource, backing, scanout, transfer+flush from the gfx
   back buffer; later hardware cursor and mode change.
3. (done) ssh PATH: /usr/local/bin entries are symlinked into /usr/bin at mount.
4. **e1000 driver** (the Makefile NET example mentions it), DHCP client.
5. **Browser leftovers:** progressive JPEG, SVG (at least icons), `z-index`,
   `box-shadow`, `transform`, `:hover`. Bigger pages are slow-ish in QEMU
   (Wikipedia ~3 s).
6. **ext2:** writes lay out the whole volume again (fine for small disks,
   slow for GBs). No ext4 writes. `lost+found` loses its preallocation.
7. **Flaky:** once saw new pty shells spin in ash's job control loop
   (TIOCGPGRP went to the console tty). Not reproduced after a clean build,
   keep an eye on it.
8. README.md: add the virtio/ext2/ports/multi-NIC part (README has the
   browser and terminals already).
9. Debug output to remove when stable: `samara: gw arp ...` (net.c),
   `kdbg()` helper in pty.c.
10. git in the repo is broken: HEAD was on an empty `dev` ref, switched to
    `main`; nothing committed. Commit in small pieces when the user asks.

## Style

Code like a person wrote it (see CLAUDE.md, local only): snake_case, short
names, few comments with some life in them, no docstrings, no banners, no
over-engineering. Talk to the user in Russian, informally.
