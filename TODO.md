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

## Integration (ag/int: x64 + drm + ipv6 + fm)

* merged ag/drm: modern virtio pci, virtio-gpu 2d, /dev/dri/card0 (virtio + bochs). `mmio_map()` in
  vmm.c = direct map below 4G, a few 2M uncached slots hung into it above (no more bar_lo hack)
* merged ag/ipv6: v6 sockets, nd/slaac, netlink v6, SIOC*, /proc/net. 64 bit msghdr, ifreq is 40
  bytes, ifconf has the pointer at +8, x86_64 syscalls are separate (no socketcall)
* merged ag/fm: static x86_64 `userland/fm` embedded as /usr/bin/fm (`make fm`, musl from void
  musl-devel in /tmp/int-x64sdk). samara.h structs have 64 bit pointers now
* tested headless: drmtest on virtio-gpu and -vga std, `ip -6`/ping6/wget on rtl8139, e1000, virtio-net, fm in the DE
* merged ag/uring: io_uring on x86_64 (425/426/427, ring frames via pmm_alloc_run + P2V, mapped with vmm_map_frame,
  64 bit iovec/addrs, msghdr check 56). liburing static musl: io_uring-test/cp, link-cp (md5 ok), test/ nop probe link
  link-timeout timeout fsync connect accept-link cq-full eventfd poll-many all rc=0. IORING_OP_STATX works only via uring
  (syscall 332 is still ENOSYS)
* void userland: `build/root-x64.img` (userland/build-x64root.sh, 97 void x86_64-musl packages, ext2 root), xbps-install/remove
  work (gcc, xterm, nano installs checked, e2fsck clean), gcc compiles and runs. sysroot.tar is x86_64 now (samara apps + sources).
  fork is COW, kernel faults on user addresses return EFAULT. no hard links in the fs (the image build copies them),
  fallocate/flock are stubs, mremap grow often ENOMEM (musl copes)
* merged ag/smp + ag/xbps: live pte changes in vmm.c (pte_put/tlb_inval) shoot down the other cpus (IPI 0xF1), range
  loops and fork's cow marking batch it (sd_defer). EFAULT longjmp keeps the BKL (kernel fault, lock stays held).
  tested -smp 4 kvm + -smp 2 tcg: nproc, xbps-install/remove nano, cow, fork+threads cow (pthreads write the heap while
  the parent forks), kill/spin, fork/exec stress in 4 shells, uring tests, drmtest virtio-gpu, ping6, desktop+fm
* left: ping6 prints ttl=-1 (no IPV6_HOPLIMIT cmsg); bochs drm has no hw cursor
  (ENXIO, expected); fm right click / menus only checked by eye on the first screen, not clicked through

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

1. **SMP** works (`-smp N`, up to 8, kvm and tcg): ACPI MADT, LAPIC timer
   1 kHz, IOAPIC (irqs to the BSP), AP trampoline (`boot/smp_tramp.S`),
   per-CPU GDT/TSS/gs block, per-CPU scheduler with an idle task per cpu.
   One big kernel lock (`core/smp.c`, ticket lock): taken on every entry
   from ring 3 and held in ring 0 except in `cpu_wait()` (hlt) and at
   `task_yield()` when another cpu waits. So only user code runs in
   parallel. Next steps: finer locks (heap, pmm, fs), no BKL for pure
   user faults, x2apic, no-ACPI machines stay on the PIC with one cpu.
2. **Boot with interrupts off:** uptime is TSC based now (calibrated against
   the PIT in `apic_init`), the lost-tick drift is gone with APIC. The
   `lost_tick_policy` flag stays for the PIC fallback.
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

* io_uring: no SQPOLL, no provided buffer rings (PBUF_RING), no registered wait
  regions, multishot poll is sampled by workers (no real wakeups), UDP sends
  over MSS fail (send_recv test)
