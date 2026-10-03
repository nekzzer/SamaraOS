# SamaraOS

A hobby operating system for 32-bit x86 (i686), written in C.
It boots via Multiboot and runs in QEMU.

About 32k lines of own kernel code (43k with userland, tools and build
scripts, not counting DOOM and generated font tables). The kernel speaks
the Linux i386 system call ABI, so ordinary Linux programs built against
musl run as they are: static ones, dynamic ones with `.so` libraries, and
packages straight from the Alpine Linux repositories through `apk`.

```
apk update
apk add python3 git vim btop zsh openssh openjdk8-jre-base
curl -Lso- bench.sh | bash
xsamara        # X11 + Mesa, once the X packages are in (see "X11")
```

## Highlights

* `apk` from Alpine Linux (x86) out of the box: thousands of ready packages,
  kept across reboots on an ext2 root disk
* X.Org 21 on the framebuffer with keyboard and mouse, Mesa llvmpipe
  (OpenGL 4.5), glxgears at ~47 FPS in QEMU
* graphical desktop with a compositing window manager at 60 FPS
* ~210 Linux system calls: `fork`, `execve`, `clone` threads, `futex`,
  signals, pipes, unix and inet sockets (with fd passing), ptys, netlink,
  `epoll`, SysV shm
* dynamic linking (`ld-musl`), PIE and static PIE binaries
* own TCP/IP stack with DHCP, several network cards (RTL8139, Intel e1000,
  virtio-net), SSH and telnet servers
* HTTPS (curl + mbedTLS) and a web browser with its own CSS engine and
  JavaScript (QuickJS + a small DOM)
* bash 5.2, zsh, busybox, nano, gcc 11 inside the OS, and the OS can
  rebuild its own kernel
* FAT32 and ext2 with write support, ext3/ext4 read only, virtio-blk disks
* any number of terminal windows, stacked into one taskbar button
* USB mouse (UHCI), SoundBlaster 16
* DOOM, Tetris, Snake, Breakout, Paint, a music player

## Quick start

You need `qemu-system-i386` and an `i686-elf` cross compiler.
Build the cross compiler once (it goes into `toolchain/cross/`):

```
./toolchain/build-cross.sh
```

Then:

```
make                 # build the kernel (build/samara.elf)
make run SELF=0      # run in QEMU with the kernel you just built
```

Userland (optional, the repo ships `userland/sysroot.tar`):

```
./userland/build-quickjs.sh && ./userland/build-sysroot.sh && make
```

| Command | What it does |
|---|---|
| `make run` | runs the newest kernel (host build or the one the OS built itself) |
| `make run SELF=0` | always the host built kernel (use this while developing) |
| `make run NIC=e1000` | network card: `rtl8139` (default), `e1000`, `virtio-net-pci` |
| `make run NICS=3` | extra virtio-net cards, each on its own NAT (eth1, eth2) |
| `make run VDISK=build/ports.img` | attach an ext2 disk as virtio-blk (`/dev/vda`) |
| `make run ROOTDISK=` | without the persistent root disk (`build/root.img`) |
| `make run VIDEO=1920x1080` | screen size (default 1600x900) |
| `make run APPEND="..."` | extra kernel options |
| `make run QDISPLAY="-display none"` | headless |
| `make run-doom` | run with `Doom1.WAD` |
| `make run-debug` | no KVM, logs interrupts |
| `make iso`, `make run-iso` | bootable GRUB ISO |

`make run` has internet through QEMU user mode NAT (SLIRP). The OS gets
its address by DHCP (`10.0.2.15`, gateway `10.0.2.2`). Connect from the
host while the OS is running:

```
ssh -p 2222 root@localhost
telnet localhost 2323
scp -O -P 2222 file root@localhost:/tmp/
curl http://localhost:8080/        # guest port 80
```

There is no root password. Ports are open only on the host loopback.

KVM note: the Makefile passes `-global kvm-pit.lost_tick_policy=discard`.
Without it KVM replays the timer ticks lost while the kernel boots with
interrupts off, the uptime jumps seconds ahead and short timeouts (DHCP on
e1000) fire at once.

## Packages (apk)

`apk` is the static `apk-tools` from Alpine 3.20 x86, with the Alpine keys,
the `main` and `community` repositories and an empty package database, so
`apk add` works right after boot. Alpine packages are i386 musl binaries,
the same ABI the kernel runs.

Tested: python3, git, vim, lua, jq, tree, neofetch, btop, zsh, openssh,
figlet. What it took on the kernel side:

* `recvmsg` now fills `msg_name`: musl 1.2.5 drops DNS answers without a
  source address, every lookup timed out
* `flock`, `fallocate`, `setfsuid32`/`setfsgid32`
* `link`/`linkat` (the file system has no hard links, so it copies the file)
* `rename` over an existing file used the cwd instead of the directory fd
* `AF_NETLINK` route dumps for `getifaddrs()` (btop)
* `LANG=C.UTF-8`, `/etc/fstab`, `/etc/mtab`

`make run` attaches `build/root.img` (2 GB ext2 labelled `/`, created on the
first run). It is merged into the root at boot, so installed packages and
changes in `/usr`, `/lib`, `/etc`, `/root` survive a reboot. Files are read
from it on first use, not at mount, and clean ones are dropped again when
memory runs low. When a program hits a missing syscall the kernel prints
`[sys] pid N (prog) unimplemented syscall NNN` on the console.

## X11

```
apk add xorg-server xf86-video-fbdev xf86-input-evdev xterm twm font-misc-misc \
        mesa-dri-gallium llvm17-libs mesa-demos
xsamara
```

`xsamara` starts Xorg on `/dev/fb0` with `/etc/X11/xorg.conf` (both ship
with the OS), runs `~/.xinitrc` or twm + xterm, and stops X when that
ends. Input comes from `/dev/input-kbd` and `/dev/input-mouse`, evdev style
devices for the Xorg evdev driver. Ctrl+Alt+Q takes the keyboard back if
something hangs.

What X needed from the kernel: named AF_UNIX sockets with `SCM_RIGHTS`,
`epoll`, `mmap` of the framebuffer, `FBIOPUT_VSCREENINFO`,
`/sys/class/graphics/fb0`, SysV shm for MIT-SHM, evdev ioctls.

## Kernel

**Boot and CPU**

* Multiboot 1 (QEMU `-kernel` or GRUB), GDT, TSS, IDT, PIC, 1 kHz PIT
* paging with 4 MB pages
* x87 FPU, SSE and SSE2 (FXSAVE on task switch)
* Multiboot modules (`-initrd a.tar,b.tar`) are moved to the top of RAM
  and unpacked into the file system without copying

**Memory**

* kernel heap plus a large arena for file data
* up to 1 GB of physical memory mapped directly, programs can use all RAM
* copy on fork, stack that grows on demand, `mmap` (with real `PROT_NONE`),
  `mprotect`, `munmap`, `brk`

**Processes and threads**

* preemptive multitasking, kernel tasks and ring 3 processes
* ~210 Linux i386 syscalls via `int 0x80`
* real threads: `clone` with shared memory, `futex`, `set_thread_area` (TLS),
  so musl `pthread` programs work
* ELF loader for `ET_EXEC`, PIE and static PIE; `PT_INTERP` loads
  `ld-musl-i386.so.1`, which maps the `.so` files itself
* `prlimit64`, `getrlimit`, `sched_*`, `clock_gettime`, `poll`, `select`, ...
* sessions, process groups, controlling terminal, job control
* background jobs with `cmd &`
* `strace` / `strace=PID` kernel option logs syscalls to COM1

**Terminals**

* console tty with termios (canonical mode, echo, `^C`, `^\`, `^D`)
* pseudo terminals `/dev/ptmx` and `/dev/pts/N`: sshd, telnetd and the
  extra desktop terminals run on them
* ANSI and VT100: scroll regions, true color, 256 colors, bold, underline,
  alternate screen, UTF-8
* SIGWINCH when a terminal window is resized

## File systems

| Path | What it is |
|---|---|
| `/` | ramfs in memory with symlinks, names up to 127 chars; the ext2 root disk is merged into it |
| `/mnt` | FAT32 on `disk.img`, read and write, long names, persistent |
| `/usr/local` | ext2 disk labelled `/usr/local` (`VDISK=build/ports.img`), read and write |
| `/proc` | `meminfo`, `cpuinfo`, `uptime`, `mounts`, `/proc/<pid>/` with `maps`, `io`, `task/` |
| `/dev` | `null zero random tty fb0 input ptmx pts/N hda sda vda ...` |
| `/opt/gcc` | gcc 11, binutils, make |
| `/usr/src/samaraos` | kernel sources |

ext2 disks are mounted by their label (`/` merges into the root,
`/usr/local` goes there); ext3 and ext4 mount read only. Writes are
incremental: a file keeps its inode and blocks, only changed blocks go to
the disk.

## Drivers

* PS/2 keyboard with EN and RU layouts (Alt+Shift to switch)
* PS/2 mouse with scroll wheel
* USB: UHCI host controller (polled) with a USB HID mouse
* VGA text mode, VBE framebuffer up to 1920x1080x32, `/dev/fb0`
* ATA PIO, AHCI (SATA with DMA), virtio-blk
* network: RTL8139, Intel e1000 (82540EM, 82545EM, 82574L), virtio-net
* SoundBlaster 16 and PC speaker
* PCI: one bus walk at boot, cached; config space access is atomic

## Network

* own stack: Ethernet, ARP, IPv4, ICMP, TCP, UDP, loopback
* DHCP client on every card, falls back to `10.0.2.15` if nobody answers;
  the DNS server from the lease goes to `/etc/resolv.conf`
* several cards at once, routing by subnet, `ifconfig` per card
* BSD sockets for programs, `AF_UNIX`, `AF_NETLINK` (route dumps)
* SSH server and client (dropbear), telnet, `nc`, `wget`, `curl`
* HTTPS: `curl` and an `openssl s_client` stand-in (used by busybox `wget`)
  on mbedTLS, CA roots in `/etc/ssl/certs` (`userland/build-curl.sh`)
* `sshd` and `telnetd` start on boot from `/etc/rc`
* the network status goes to COM1 on boot: `samara: net: up (...)`

## Shell

The built in shell has Tab completion, history and background jobs.
There are about 80 built in commands, everything else is searched in `PATH`.
Lines with pipes, redirects, quotes, `;`, `$` or `&&` are handed to
`/bin/sh -c`. `bash` (5.2) and busybox `sh` are there too, `zsh` and others
come from `apk`.

| Group | Commands |
|---|---|
| Files | `ls cd pwd cat mkdir touch rm cp mv head tail wc grep find df` |
| System | `help clear uptime mem ps procs uname date whoami shutdown reboot neofetch strace` |
| Programs | `sh run python python3 nano edit calc` |
| Graphics | `desktop term wallpaper paint clock browser www snake bounce doom player music` |
| Network | `ifconfig ping wget` |
| Sound | `beep playwav stopwav playfat sbinfo` |
| Disks | `disk disks fatmount fatls fatload` |
| Keyboard | `klayout kbdignore` |

The console uses DejaVu Sans Mono in 4 sizes. Ctrl + and Ctrl - change
the font size, the mouse wheel scrolls history.

## Programs

Built in (from `userland/sysroot.tar` and the busybox in the kernel):

| Program | What it is |
|---|---|
| apk | Alpine package manager (apk-tools 2.14, static) |
| busybox | 402 applets: `sh`, `vi`, `less`, `top`, `awk`, `sed`, `tar`, ... |
| bash 5.2 | `userland/build-bash.sh` |
| nano 7.2 | text editor |
| curl 8.15 | HTTP and HTTPS (mbedTLS 3.6) |
| htop, fastfetch | dynamic binaries, use `/lib/ld-musl-i386.so.1` |
| qjs | QuickJS JavaScript engine |
| node | QuickJS with a Node.js style API layer (`userland/js/node/*.js`): `fs`, `path`, `process`, `child_process`, `http`, ... |
| domjs | QuickJS + a small DOM, runs page scripts for the browser |
| micropython | Python 3 with the `samara` module for windows |
| tcc | small C compiler |
| gcc 11.2 | with binutils and GNU make |
| dropbear | `ssh`, `scp`, `dropbear` |
| samarafetch | system info with a gradient logo |
| tetris, breakout | games in `/usr/games` |

On the ports disk (`./userland/build-ports.sh`, `VDISK=build/ports.img`):
CPython 3.12, git 2.46, vim 9.1, Lua 5.4, and yutani, the toaruos
compositor, with some of its demos (`userland/build-toaru.sh`).

Write your own window programs with `/usr/include/samara.h`
(`sm_open`, `sm_rect`, `sm_text`, `sm_event`, `sm_present`).
Examples are in `/usr/src/samara/`.

## Desktop

Run `desktop` to start it.

* compositor with damage tracking and double buffering
* move, resize, maximize and minimize windows, F11 for fullscreen
* taskbar with menu, window buttons, network status, layout switch, clock
* windows of one kind are stacked into one taskbar button with a counter;
  clicking it walks through them
* desktop icons (games from `/usr/games` show up by themselves)
* BMP wallpapers

Apps: Terminal (as many as you want), Web Browser, Music Player, Paint,
Clock, Snake, Bounce, DOOM.

**Terminals.** The first Terminal window is the kernel shell. Every next
one (the Terminal icon again, the menu, or `term` in a terminal) is a new
window with `/bin/sh -i` on its own pty, with its own screen and a VT100
parser (`src/apps/pterm.c`). All of them share one "Terminal" button in the
taskbar.

## Web browser

`browser <url>` or the Browser icon. Pages are fetched by `curl` in the
background (HTTP, HTTPS, redirects), decoded (UTF-8 and windows-1251 to the
OS charset) and laid out by the browser itself.

**Pipeline**

1. `curl` downloads the page
2. if the page has `<script>` or stylesheets, `domjs` runs it: parses the
   HTML into a DOM, pulls external CSS into `<style>` (relative `url()`s
   fixed, `@import` inlined, `media` kept), runs scripts, `fetch`,
   `XMLHttpRequest` and promises, writes the result. Pages over 400 KB
   (Wikipedia articles) skip the DOM and scripts and only get their
   stylesheets pulled in
3. the browser parses the HTML into its own DOM tree, runs the CSS cascade
   and lays the page out
4. images are fetched one by one with `curl` and decoded in the kernel
   (PNG, baseline JPEG, GIF); the page is laid out again as they arrive
5. **live JavaScript:** pages with scripts keep their `domjs` running.
   Clicks, typed text and Enter go to it as DOM events (`click`, `input`,
   `change`, `keydown`, `submit` with default actions: links, forms,
   checkboxes), timers run on the real clock, and every change of the DOM
   comes back as a new render. `location.href = ...` navigates. The file
   protocol is in `/tmp/.browser-ev`, `-js`, `-js-seq`, `-nav`

**CSS engine** (`src/apps/browser.c`, integer only, the kernel has no FPU state)

* selectors: type, `.class`, `#id`, `*`, combinators ` `, `>`, `+`, `~`;
  attributes `[a]`, `=`, `^=`, `$=`, `*=`, `~=`, `|=`, `i` flag;
  `:first-child`, `:last-child`, `:only-child`, `:nth-child()`,
  `:nth-last-child()`, `:first/last/only-of-type`, `:nth-of-type()`,
  `:not()`, `:is()`, `:where()`, `:root`, `:empty`, `:link`, `:checked`,
  `:disabled`; `::before` and `::after` with `content`; `:hover`, `:focus`
  and the like never match
* cascade: UA stylesheet, presentational attributes (`bgcolor`, `width`,
  `align`, `<font>`, `cellpadding`, ...), `<style>`, external
  `<link rel=stylesheet>` (inlined by domjs), `style=""`, specificity,
  source order, `!important`; rules are bucketed by id, class and tag
* inheritance: a computed style per element, children inherit colour,
  fonts, text properties, `visibility`, `line-height`, list style, ...
* at-rules: `@media` (screen, print, `min/max-width`, range syntax,
  `prefers-color-scheme`, `prefers-reduced-motion`, `hover`, `pointer`,
  `orientation`, resolution), `@supports`, `@layer`; `@font-face`,
  `@keyframes`, `@import` in `<style>` are skipped
* values: custom properties `var(--x, fallback)` with inheritance,
  `calc()`, `min()`, `max()`, `clamp()`; units px, em, rem, %, pt, pc, in,
  cm, mm, vw, vh, vmin, vmax, ch, ex; colours hex 3/4/6/8, `rgb()`,
  `rgba()`, `hsl()`, `hsla()`, all 148 named colours, `currentColor`,
  `transparent`; gradients become the average of their stops
* boxes: `display` block, inline, inline-block, list-item, flex,
  inline-flex, grid, inline-grid, table and its parts, contents, none,
  flow-root; margin, padding and border on all sides (width, colour, style
  solid/dashed/dotted), `border-radius`, `width`, `min-width`, `max-width`,
  `height`, `min-height`, `box-sizing`, `margin: auto` centering,
  `overflow: hidden` clipping, `float` and `clear`,
  `position: relative/absolute/fixed/sticky` (simplified), `opacity`,
  `visibility`, `clip` / `clip-path` screen reader tricks
* flex: row and column, wrap, `gap`, `justify-content` (start, center, end,
  space-between, space-around, space-evenly), `align-items`, `flex`,
  `flex-grow`, `flex-basis`, `margin-left: auto`
* grid: `grid-template-columns` with px, %, `fr`, `auto`,
  `min/max-content`, `repeat()`, `minmax()`, `repeat(auto-fill, ...)`;
  `grid-template-areas` and `grid-area`, `grid-template` shorthand, line
  numbers in `grid-column` / `grid-row` (`2 / 4`, `1 / -1`, `span N`),
  auto placement, `gap`
* images: `<img>` (`src`, lazy `data-src`, `srcset`, width/height from
  attributes, CSS or the picture's own size, `max-width`, floats),
  `background-image: url()` with `cover`, `contain`, `no-repeat`, tiling
* tables: column widths from min- and max-content, `colspan`,
  `border-spacing`, row backgrounds, `vertical-align` in cells
* text: `font-size` (nearest of the UI fonts or scaled 2x/3x),
  `font-weight`, `font-style: italic` (drawn sheared), monospace families,
  `font` shorthand, `line-height`, `text-align`, `text-indent`,
  `letter-spacing`, `word-spacing`, `text-transform`, `text-decoration`
  (underline, line-through, overline), `white-space` (normal, nowrap, pre,
  pre-wrap, pre-line), `vertical-align`, `list-style-type` (disc, circle,
  square, decimal, alpha, roman)
* forms: text and password fields, submit buttons, GET and POST
* test pages: `www/css/` (`cd www/css && python3 -m http.server 8000`,
  then `10.0.2.2:8000` in the browser)
* host test harness: `tools/browser-test/` builds the layout code for the
  host and dumps runs and boxes of any page

## Self build

SamaraOS can build its own kernel:

```
selfbuild
```

It compiles the sources in `/usr/src/samaraos` with gcc 11 and saves
`/mnt/samara.elf`. The next `make run` on the host boots it if it is newer
(`SELF=0` to skip that). The result is bit for bit the same as the host build.

## Kernel options

Pass them with `make run APPEND="..."`.

| Option | What it does |
|---|---|
| `video=WxH` | screen size |
| `textmode` | old VGA text console |
| `noservices` | do not start sshd and telnetd |
| `autosh=<script>` | run a script, print to COM1, power off (headless tests) |
| `browser=<url>` | open the browser with this page when the desktop starts |
| `wmstats` | desktop FPS to COM1 |
| `strace`, `strace=PID` | trace all syscalls or one process |
| `kbd_ignore=35,2b` | ignore broken keys |

## Hotkeys

| Where | Keys | Action |
|---|---|---|
| everywhere | Alt+Shift | switch EN and RU |
| console | Ctrl + and Ctrl -, wheel, Tab | font size, history, completion |
| desktop | F11 | fullscreen window |
| desktop | Esc | leave desktop |
| browser | `/` or `l`, Backspace, PgUp/PgDn, Tab, F5 | address bar, back, scroll, next field, reload |
| programs | ^C, ^\, ^D | SIGINT, SIGQUIT, end of input |
| games | Ctrl+Alt+Q | get the keyboard back |

## Source layout

| Folder | Contents |
|---|---|
| `src/boot` | GDT, IDT, paging, FPU and SSE, PIC, PIT |
| `src/core` | `kmain`, heap, tasks, vmm, strings |
| `src/drivers` | keyboard, mouse, USB (UHCI), VGA, ATA, AHCI, PCI, RTL8139, e1000, virtio, SB16, fbdev, input |
| `src/fs` | ramfs, FAT12/16, FAT32, ext2 |
| `src/proc` | processes, threads, syscalls, signals, tty, pty, procfs, ELF loader |
| `src/net` | TCP/IP stack, DHCP, sockets |
| `src/gfx` | graphics, fonts, terminal |
| `src/gui` | desktop, window manager, program windows |
| `src/shell` | shell, commands, Tab completion |
| `src/apps` | browser, image decoders, extra terminals, player, Paint, clock, snake, DOOM |
| `userland` | sysroot, build scripts (bash, curl, dropbear, nano, QuickJS, MicroPython, ports, toaru), `samara.h`, `js/` (node layer, `dom.js`), games |
| `tools` | font generators, module builders, `browser-test/` |
| `www` | test pages (`www/css/`) and web demos |

## Limits

* one CPU core
* IPv4 only
* no `eventfd`, `timerfd`, `memfd` yet (nodejs and friends fail)
* X has no hardware acceleration (llvmpipe on one core) and no mode
  switching: it runs in the boot resolution
* the file cache is ~340 MB of a 1 GB guest; a single file bigger than
  that can't be opened
* wide characters (CJK, emoji) are not drawn
* browser: no progressive JPEG and no SVG, no `z-index`, `transform`,
  `box-shadow`, `:hover`; big pages (over 400 KB) get CSS but no scripts

## Recent work

**X11 and Mesa.** See "X11". glxgears went from 1.9 to 47 FPS once SysV
shm was there (MIT-SHM instead of every frame through the socket) and pipes
got 64 KB buffers with `memcpy`.

**Java.** openjdk8 (the only one Alpine has for x86) runs: `/proc/self/exe`
for `$ORIGIN` in RPATH, and the stack start in `/proc/self/stat` pointed
one byte past the stack.

**ext2 root disk.** Incremental write back, lazy file reads, eviction of
clean files when the file arena is full, in place growth of file buffers
(libLLVM is 161 MB).

**apk and Alpine packages.** See "Packages". The biggest one was DNS:
`recvmsg` passed a kernel stack pointer to the user pointer check when it
wrote the sender address, the check said no and nothing was written. musl
1.2.5 (the one in Alpine) throws away DNS answers that don't come from the
nameserver, so every lookup timed out. The musl.cc toolchain (1.2.4 era)
didn't care, that's why curl worked all along.

**`curl -Lso- bench.sh | bash` printed the script.** The kernel shell split
lines on spaces only, so curl got `|` and `bash` as two more URLs. Lines
with shell syntax now go to `/bin/sh -c`.

**Intel e1000 and DHCP.** New polled e1000 driver, a DHCP client on every
card. Getting DHCP to work on e1000 under QEMU+KVM took a while: QEMU's
e1000 holds received frames for one second after RX is enabled, and our
uptime ran far ahead of real time right after boot (KVM replays the PIT
ticks lost while interrupts were off), so all retries were done in a few
milliseconds. Fixed with `kvm-pit.lost_tick_policy=discard`.

**PCI bus walk.** `pci_find` walked all 256 buses on every call, a few
seconds when the device isn't there. Now the bus is walked once and
cached; the network comes up right after boot instead of ~6 s later.

**btop and zsh.** btop crashed when `getifaddrs()` failed (musl does it
with netlink) and refused to start without a UTF-8 locale. zsh printed
the bash style `PS1` from the environment as is, now it gets its own
`~/.zshrc`.

**Before that:** virtio-blk and virtio-net, ext2 read and write, several
network cards, symlinks, a ports disk with CPython, git, vim and Lua,
dynamic linking with `.so` files, htop and fastfetch, procfs threads and
maps, yutani, a CSS engine for the browser, many terminals, threads,
bash 5.2, USB mouse, QuickJS with `node` and `domjs`.

**Next:** SMP, `eventfd`/`timerfd`/`memfd`, interrupts off for too long
at boot (KVM then replays timer ticks, see the KVM note).
