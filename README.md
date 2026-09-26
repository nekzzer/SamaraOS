# SamaraOS

A hobby operating system for 32-bit x86 (i686), written in C.
It boots via Multiboot and runs in QEMU.

The kernel is about 26k lines of own code. User programs are ordinary static
Linux binaries (musl): the kernel runs them through a Linux i386 compatible
system call interface. So busybox, nano, gcc, MicroPython and dropbear SSH
work inside SamaraOS without changes.

## Highlights

* graphical desktop with a compositing window manager at 60 FPS
* ~190 Linux system calls, `fork`, `execve`, signals, pipes, sockets
* own TCP/IP stack, SSH and telnet servers
* gcc 11 inside the OS, and the OS can rebuild its own kernel
* FAT32 disk with read and write support
* DOOM, Tetris, Snake, a web browser, Paint, a music player

## Quick start

You need `qemu-system-i386` and an `i686-elf` cross compiler.
Build the cross compiler once (it goes into `toolchain/cross/`):

```
./toolchain/build-cross.sh
```

Then:

```
make          # build the kernel (build/samara.elf)
make run      # run in QEMU
```

| Command | What it does |
|---|---|
| `make run` | normal run, loads the newest kernel (host build or self build) |
| `make run SELF=0` | always use the host built kernel |
| `make run VIDEO=1920x1080` | screen size (default 1600x900) |
| `make run APPEND="..."` | extra kernel options |
| `make run-doom` | run with `Doom1.WAD` |
| `make run-debug` | no KVM, logs interrupts |
| `make iso`, `make run-iso` | bootable GRUB ISO |

Connect from the host while the OS is running:

```
ssh -p 2222 root@localhost
telnet localhost 2323
scp -O -P 2222 file root@localhost:/tmp/
```

There is no root password. Ports are open only on the host loopback.

## Kernel

**Boot and CPU**

* Multiboot 1 (QEMU `-kernel` or GRUB), GDT, TSS, IDT, PIC, 1 kHz PIT
* paging with 4 MB pages
* x87 FPU, SSE and SSE2 (FXSAVE on task switch)
* Multiboot modules (`-initrd a.tar,b.tar`) are unpacked into the file system

**Memory**

* 64 MB kernel heap plus a large arena for file data
* 1 GB of physical memory mapped directly, so programs can use all RAM
* copy on fork, stack that grows on demand, `mmap`, `munmap`, `brk`

**Processes**

* preemptive multitasking, kernel tasks and ring 3 processes
* ~190 Linux i386 syscalls via `int 0x80`
* ELF loader for `ET_EXEC` and static PIE binaries
* up to 48 processes and 1024 file descriptors per process
* sessions, process groups, controlling terminal
* background jobs with `cmd &`
* `strace=PID` kernel option logs syscalls to COM1

**Terminals**

* console tty with termios (canonical mode, echo, `^C`, `^\`, `^D`)
* pseudo terminals `/dev/ptmx` and `/dev/pts/N` for sshd and telnetd
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
* VGA text mode, VBE framebuffer up to 1920x1080x32, `/dev/fb0`
* ATA PIO and AHCI (SATA with DMA)
* SoundBlaster 16 and PC speaker
* RTL8139 network card

## Network

* own stack: Ethernet, ARP, IPv4, ICMP, TCP, UDP, loopback
* static address `10.0.2.15`, gateway `10.0.2.2` (the host in QEMU)
* BSD sockets for programs
* SSH server and client (dropbear), telnet, `nc`, `wget`
* `sshd` and `telnetd` start on boot

## Shell

The built in shell has Tab completion, history and background jobs.
There are 70 built in commands, everything else is searched in `PATH`.

| Group | Commands |
|---|---|
| Files | `ls cd pwd cat mkdir touch rm cp mv head tail wc grep find df` |
| System | `help clear uptime mem ps uname date shutdown reboot neofetch` |
| Programs | `sh python nano edit calc` |
| Graphics | `desktop wallpaper paint clock browser snake doom player` |
| Network | `ifconfig ping wget` |
| Sound | `beep playwav stopwav` |
| Disks | `disk fatmount fatls fatload` |

The console uses DejaVu Sans Mono in 4 sizes. Ctrl + and Ctrl - change
the font size, the mouse wheel scrolls history.

## Programs

| Program | What it is |
|---|---|
| busybox | 402 applets: `sh`, `vi`, `less`, `top`, `awk`, `sed`, `tar`, ... |
| nano 7.2 | text editor |
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
* desktop icons (games from `/usr/games` show up by themselves)
* BMP wallpapers

Apps: Terminal, Web Browser (HTTP), Music Player, Paint, Clock, Snake,
Bounce, DOOM.

## Self build

SamaraOS can build its own kernel:

```
selfbuild
```

It compiles the sources in `/usr/src/samaraos` with gcc 11 and saves
`/mnt/samara.elf`. The next `make run` on the host boots it if it is newer.
The result is bit for bit the same as the host build.

## Kernel options

Pass them with `make run APPEND="..."`.

| Option | What it does |
|---|---|
| `video=WxH` | screen size |
| `textmode` | old VGA text console |
| `noservices` | do not start sshd and telnetd |
| `autosh=<script>` | run a script, print to COM1, power off |
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
| programs | ^C, ^\, ^D | SIGINT, SIGQUIT, end of input |
| games | Ctrl+Alt+Q | get the keyboard back |

## Source layout

| Folder | Contents |
|---|---|
| `src/boot` | GDT, IDT, paging, FPU and SSE, PIC, PIT |
| `src/core` | `kmain`, heap, tasks, vmm, strings |
| `src/drivers` | keyboard, mouse, VGA, ATA, AHCI, PCI, RTL8139, SB16 |
| `src/fs` | ramfs, FAT12/16, FAT32 |
| `src/proc` | processes, syscalls, signals, tty, pty, procfs |
| `src/net` | TCP/IP stack, sockets |
| `src/gfx` | graphics, fonts, terminal |
| `src/gui` | desktop, window manager, program windows |
| `src/shell` | shell, commands, Tab completion |
| `src/apps` | browser, player, Paint, clock, snake, DOOM |
| `userland` | sysroot, `samara.h`, samarafetch, games |
| `tools` | font generators, module builders |

## Limits

* one CPU core, no threads sharing memory
* no HTTPS and no DHCP
* only `/mnt` survives a reboot
* wide characters (CJK, emoji) are not drawn

## Recent update

* pseudo terminals, so SSH and telnet work with real terminals
* dropbear SSH server and client
* Tab completion in the shell
* new terminal font with 4 sizes and Unicode
* 1 GB of RAM for programs
* gcc 11 inside the OS and `selfbuild`
* samarafetch and gradient prompt
* desktop icons, BMP wallpapers, window resize and maximize
* `samara.h` window API and the MicroPython `samara` module
* Tetris and Breakout
