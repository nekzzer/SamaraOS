# SamaraOS

A hobby operating system for 32-bit x86 (i686), written in C.
It boots via Multiboot and runs in QEMU.

The kernel is about 30k lines of own code (38k with userland, tools and
build scripts, not counting DOOM and generated font tables). User programs
are ordinary static Linux binaries (musl): the kernel runs them through a
Linux i386 compatible system call interface. So busybox, bash, nano, gcc,
curl, QuickJS, MicroPython and dropbear SSH work inside SamaraOS without
changes.

## Highlights

* graphical desktop with a compositing window manager at 60 FPS
* ~190 Linux system calls: `fork`, `execve`, `clone` threads, `futex`,
  signals, pipes, sockets, ptys
* own TCP/IP stack, internet through QEMU NAT, SSH and telnet servers
* HTTPS (curl + mbedTLS) and a web browser with its own CSS engine and
  JavaScript (QuickJS + a small DOM)
* bash 5.2, busybox, nano, gcc 11 inside the OS, and the OS can rebuild
  its own kernel
* any number of terminal windows, stacked into one taskbar button
* FAT32 disk with read and write support
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
| `make run VIDEO=1920x1080` | screen size (default 1600x900) |
| `make run APPEND="..."` | extra kernel options |
| `make run QDISPLAY="-display none"` | headless |
| `make run-doom` | run with `Doom1.WAD` |
| `make run-debug` | no KVM, logs interrupts |
| `make iso`, `make run-iso` | bootable GRUB ISO |

`make run` has internet through QEMU user mode NAT (SLIRP). The host is
`10.0.2.2`, DNS `10.0.2.3`, the guest is `10.0.2.15`. Connect from the
host while the OS is running:

```
ssh -p 2222 root@localhost
telnet localhost 2323
scp -O -P 2222 file root@localhost:/tmp/
curl http://localhost:8080/        # guest port 80
```

There is no root password. Ports are open only on the host loopback.

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
* copy on fork, stack that grows on demand, `mmap`, `munmap`, `brk`

**Processes and threads**

* preemptive multitasking, kernel tasks and ring 3 processes
* ~190 Linux i386 syscalls via `int 0x80`
* real threads: `clone` with shared memory, `futex`, `set_thread_area` (TLS),
  so musl `pthread` programs work
* `prlimit64`, `getrlimit`, `sched_*`, `clock_gettime`, `poll`, `select`, ...
* ELF loader for `ET_EXEC` and static PIE binaries
* up to 48 processes and 1024 file descriptors per process
* sessions, process groups, controlling terminal, job control
* background jobs with `cmd &`
* `strace=PID` kernel option logs syscalls to COM1

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
| `/` | ramfs in memory, cleared on reboot |
| `/mnt` | FAT32 on `disk.img`, read and write, long names, persistent |
| `/proc` | `meminfo`, `cpuinfo`, `uptime`, `mounts`, `/proc/<pid>/...` |
| `/dev` | `null zero random tty fb0 input ptmx pts/N hda sda ...` |
| `/opt/gcc` | gcc 11, binutils, make |
| `/usr/src/samaraos` | kernel sources |

## Drivers

* PS/2 keyboard with EN and RU layouts (Alt+Shift to switch)
* PS/2 mouse with scroll wheel
* USB: UHCI host controller (polled) with a USB HID mouse
* VGA text mode, VBE framebuffer up to 1920x1080x32, `/dev/fb0`
* ATA PIO and AHCI (SATA with DMA)
* SoundBlaster 16 and PC speaker
* RTL8139 network card
* PCI config space access is atomic (see "Recent work")

## Network

* own stack: Ethernet, ARP, IPv4, ICMP, TCP, UDP, loopback
* static address `10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`
* BSD sockets for programs
* SSH server and client (dropbear), telnet, `nc`, `wget`, `curl`
* HTTPS: `curl` and an `openssl s_client` stand-in (used by busybox `wget`)
  on mbedTLS, CA roots in `/etc/ssl/certs` (`userland/build-curl.sh`)
* `sshd` and `telnetd` start on boot from `/etc/rc`
* the network status goes to COM1 on boot: `samara: net: up (...)`

## Shell

The built in shell has Tab completion, history and background jobs.
There are about 80 built in commands, everything else is searched in `PATH`.
`bash` (5.2) and busybox `sh` are there too.

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

| Program | What it is |
|---|---|
| busybox | 402 applets: `sh`, `vi`, `less`, `top`, `awk`, `sed`, `tar`, ... |
| bash 5.2 | `userland/build-bash.sh` |
| nano 7.2 | text editor |
| curl 8.15 | HTTP and HTTPS (mbedTLS 3.6) |
| qjs | QuickJS JavaScript engine |
| node | QuickJS with a Node.js style API layer (`userland/js/node/*.js`): `fs`, `path`, `process`, `child_process`, `http`, ... |
| domjs | QuickJS + a small DOM, runs page scripts for the browser |
| micropython | Python 3 with the `samara` module for windows |
| tcc | small C compiler |
| gcc 11.2 | with binutils and GNU make |
| dropbear | `ssh`, `scp`, `dropbear` |
| samarafetch | system info with a gradient logo |
| tetris, breakout | games in `/usr/games` |

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
| `strace=PID` | trace syscalls |
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
| `src/drivers` | keyboard, mouse, USB (UHCI), VGA, ATA, AHCI, PCI, RTL8139, SB16, fbdev, input |
| `src/fs` | ramfs, FAT12/16, FAT32 |
| `src/proc` | processes, threads, syscalls, signals, tty, pty, procfs, ELF loader |
| `src/net` | TCP/IP stack, sockets |
| `src/gfx` | graphics, fonts, terminal |
| `src/gui` | desktop, window manager, program windows |
| `src/shell` | shell, commands, Tab completion |
| `src/apps` | browser, image decoders, extra terminals, player, Paint, clock, snake, DOOM |
| `userland` | sysroot, build scripts (bash, curl, dropbear, nano, QuickJS, MicroPython), `samara.h`, `js/` (node layer, `dom.js`), games |
| `tools` | font generators, module builders, `browser-test/` |
| `www` | test pages (`www/css/`) and web demos |

## Limits

* one CPU core
* no DHCP, one network card, IPv4 only
* only `/mnt` survives a reboot
* wide characters (CJK, emoji) are not drawn
* browser: no progressive JPEG and no SVG, no `z-index`, `transform`,
  `box-shadow`, `:hover`; big pages (over 400 KB) get CSS but no scripts

## Recent work

**Browser: a real CSS engine.** The old code handled a small subset
(colours, backgrounds, `display: none`, bold, three font sizes, alignment,
some margins) while streaming the HTML. It is replaced by:

* an HTML parser that builds a DOM (implied end tags for `p`, `li`, `dt`,
  `dd`, `tr`, `td`, `th`, `tbody`, `option`; raw text elements)
* a CSS parser (comments, strings, nesting skipped, `@media`, `@supports`,
  `@layer`) into rules with compound selectors, attribute conditions and
  specificity, bucketed for fast matching
* a computed style per element with inheritance, custom properties on a
  stack, `calc()`, two passes (font size and colour first, so `em` and
  `currentColor` are right)
* a layout engine: block flow with collapsing bottom margins, line boxes
  with baseline alignment and `vertical-align`, inline boxes with
  backgrounds and borders split per line, atomic inline-blocks, floats
  with line shortening and `clear`, absolute and relative positioning,
  flex, grid, tables with content-based column widths, list markers,
  `::before` / `::after`, intrinsic sizes with a per-node cache
* drawing: boxes (backgrounds, per-side borders, dashed and dotted, rounded
  corners, alpha) interleaved with text in document order, clip rectangles
  for `overflow: hidden`, scaled fonts, fake italic, letter spacing,
  underline / strike / overline

Hacker News, Wikipedia, CNN Lite, example.com and the pages in `www/css/`
were used for testing.

**Network was dead on 2 boots out of 3.** `netd` probes the RTL8139 while
`kmain` scans PCI for USB and AHCI. The PCI config space is a pair of
ports (address to `0xCF8`, data from `0xCFC`); a task switch between the
two made one side read the other's register, and the NIC came up with a
broken setup (no ARP answers, no ssh, "offline" in the taskbar). PCI
config reads and writes are now atomic. After the fix: 6 of 6 boots with
working network.

**Many terminals.** The desktop can open any number of terminals; the
extra ones are `/bin/sh` on a pty with their own VT100 screen. Windows of
one group share a taskbar button with a counter. New shell command `term`.

**Also:** threads (`clone`, `futex`, TLS), bash 5.2, USB UHCI mouse,
QuickJS with `node` and `domjs`, `prlimit64` fix (it returned garbage
because of the user pointer check on a kernel pointer), the network status
on COM1, the `tools/browser-test` host harness, CSS test pages.

**Images in the browser:** `src/apps/imgdec.c` (inflate + PNG filters and
palettes, baseline JPEG with integer IDCT, GIF LZW), fetched in the
background, scaled with a small box filter, alpha over the background.

**Live JavaScript:** `domjs` stays alive after the first render; see the
browser section. Promise jobs now run after every script, timer and event
(before, `fetch().then()` results never showed up).

**Wikipedia with styles:** big pages go through a light `domjs` mode that
only inlines stylesheets, the document may be up to 1400 px wide (so
desktop `@media` layouts apply), `grid-template-areas`, compounds with up to
6 classes, `mask-image` icons are not painted as squares. Wikipedia
articles render with the Vector 2022 layout: sidebar, header, infobox.

**Terminals are cheap now:** blocking reads on ptys and the console,
`poll` and `select` sleep instead of spinning on `task_yield` (5 idle
shells used to eat the CPU), a terminal repaints only the rows that
changed and only the cells under the clip, and ^C gets through even when
the output ring is full. `term N` opens N terminals at once.

**Build:** header dependencies (`-MMD`), so editing a `.h` rebuilds the
files that include it.

**Next:** virtio (block, net, gpu), ext2, bigger ports (Lua, CPython, git,
vim), SMP.
