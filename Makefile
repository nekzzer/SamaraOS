# SamaraOS Makefile
# Toolchain: i686-elf-gcc, built repo-locally by toolchain/build-cross.sh
# into toolchain/cross/ (see that script to reproduce on another machine).
# Run target: system qemu-system-i386.

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CROSS    ?= $(MAKEFILE_DIR)toolchain/cross/bin/i686-elf-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

QEMU     ?= qemu-system-i386

# Wire the PC speaker (PIT channel 2) to a real audio backend. Without these
# flags QEMU silently drops the speaker output even though the OS programs it.
# pa = PulseAudio (Linux). Override with AUDIO= for another backend/host.
AUDIO    ?= -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 -device sb16,audiodev=snd0

KCFLAGS  := -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
            -nostdlib -mno-red-zone \
            -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 \
            -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
            -std=gnu11 -Isrc

# DOOM compile flags. Permissive so id Software's 1993 K&R C compiles.
# x87 FP allowed (a few % format strings use it); SSE/MMX off.
DCFLAGS  := -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
            -nostdlib -mno-red-zone \
            -mno-mmx -mno-sse -mno-sse2 \
            -O2 -std=gnu11 -fcommon \
            -DNORMALUNIX -D_DEFAULT_SOURCE \
            -isystem src/libc/include -Isrc \
            -Idoomgeneric-master/doomgeneric \
            -w

LDFLAGS  := -m elf_i386 -nostdlib -T linker.ld
LIBGCC   := $(shell $(CC) -m32 -print-libgcc-file-name)

# ---------- Kernel sources ----------
# Organized by subsystem: core/ (heap, task, string, kernel entry), boot/
# (gdt, idt, paging, pic, pit, fpu, libgcc div/mod helpers), drivers/
# (device I/O), net/ (protocol stack), fs/ (filesystem), gfx/ (framebuffer,
# font, terminal), gui/ (desktop, window manager), apps/ (programs), shell/
# (command interpreter + shell/commands/ builtins).

# Shell builtins, split out of shell.c into one file per command group.
SHELL_CMD_SRC := $(wildcard src/shell/commands/*.c)

KERN_SRC := \
    src/core/string.c \
    src/drivers/vga.c \
    src/boot/gdt.c \
    src/boot/idt.c \
    src/boot/paging.c \
    src/boot/fpu.c \
    src/boot/pic.c \
    src/boot/pit.c \
    src/drivers/keyboard.c \
    src/drivers/mouse.c \
    src/drivers/input.c \
    src/drivers/fbdev.c \
    src/core/heap.c \
    src/core/task.c \
    src/core/vmm.c \
    src/core/clock.c \
    src/proc/tty.c \
    src/proc/file.c \
    src/proc/proc.c \
    src/proc/syscall.c \
    src/proc/userland.c \
    src/proc/procfs.c \
    src/proc/signal.c \
    src/fs/fs.c \
    src/gfx/font.c \
    src/gfx/gfx.c \
    src/gfx/gfx_term.c \
    src/gfx/uifont.c \
    src/gui/desktop.c \
    src/gui/wm.c \
    src/gui/uwin.c \
    src/drivers/ata.c \
    src/drivers/ahci.c \
    src/apps/doom.c \
    src/apps/mediaplayer.c \
    src/apps/paint.c \
    src/apps/clock.c \
    src/drivers/sb16.c \
    src/apps/wav.c \
    src/apps/synth.c \
    src/apps/embed.c \
    src/fs/fat.c \
    src/fs/fatfs.c \
    src/drivers/pci.c \
    src/drivers/rtl8139.c \
    src/net/net.c \
    src/net/sock.c \
    src/apps/snake.c \
    src/apps/browser.c \
    $(SHELL_CMD_SRC) \
    src/shell/shell.c \
    src/boot/libgcc_div.c \
    src/core/kernel.c

# ---------- libc-shim sources ----------
LIBC_SRC := \
    src/libc/string_ext.c \
    src/libc/ctype.c \
    src/libc/stdlib.c \
    src/libc/stdio.c \
    src/libc/misc_stubs.c

LIBC_ASM := src/libc/setjmp.S

# ---------- DOOM platform layer ----------
DGEN_LOCAL := \
    src/doomgeneric/samara_wad.c \
    src/doomgeneric/samara_system.c \
    src/doomgeneric/samara_net_stub.c \
    src/doomgeneric/samara_zone.c \
    src/doomgeneric/doomgeneric_samara.c

# ---------- DOOM engine sources (from doomgeneric upstream) ----------
DGEN_DIR  := doomgeneric-master/doomgeneric

DGEN_SRC := \
    $(DGEN_DIR)/am_map.c $(DGEN_DIR)/d_event.c $(DGEN_DIR)/d_items.c \
    $(DGEN_DIR)/d_iwad.c $(DGEN_DIR)/d_loop.c $(DGEN_DIR)/d_main.c \
    $(DGEN_DIR)/d_mode.c $(DGEN_DIR)/d_net.c $(DGEN_DIR)/doomdef.c \
    $(DGEN_DIR)/doomstat.c $(DGEN_DIR)/dstrings.c $(DGEN_DIR)/dummy.c \
    $(DGEN_DIR)/f_finale.c $(DGEN_DIR)/f_wipe.c $(DGEN_DIR)/g_game.c \
    $(DGEN_DIR)/hu_lib.c $(DGEN_DIR)/hu_stuff.c $(DGEN_DIR)/i_cdmus.c \
    $(DGEN_DIR)/i_endoom.c $(DGEN_DIR)/i_joystick.c $(DGEN_DIR)/i_scale.c \
    $(DGEN_DIR)/i_sound.c $(DGEN_DIR)/i_timer.c $(DGEN_DIR)/info.c \
    $(DGEN_DIR)/memio.c $(DGEN_DIR)/m_argv.c $(DGEN_DIR)/m_bbox.c \
    $(DGEN_DIR)/m_cheat.c $(DGEN_DIR)/m_config.c $(DGEN_DIR)/m_controls.c \
    $(DGEN_DIR)/m_fixed.c $(DGEN_DIR)/m_menu.c $(DGEN_DIR)/m_misc.c \
    $(DGEN_DIR)/m_random.c \
    $(DGEN_DIR)/p_ceilng.c $(DGEN_DIR)/p_doors.c $(DGEN_DIR)/p_enemy.c \
    $(DGEN_DIR)/p_floor.c $(DGEN_DIR)/p_inter.c $(DGEN_DIR)/p_lights.c \
    $(DGEN_DIR)/p_map.c $(DGEN_DIR)/p_maputl.c $(DGEN_DIR)/p_mobj.c \
    $(DGEN_DIR)/p_plats.c $(DGEN_DIR)/p_pspr.c $(DGEN_DIR)/p_saveg.c \
    $(DGEN_DIR)/p_setup.c $(DGEN_DIR)/p_sight.c $(DGEN_DIR)/p_spec.c \
    $(DGEN_DIR)/p_switch.c $(DGEN_DIR)/p_telept.c $(DGEN_DIR)/p_tick.c \
    $(DGEN_DIR)/p_user.c \
    $(DGEN_DIR)/r_bsp.c $(DGEN_DIR)/r_data.c $(DGEN_DIR)/r_draw.c \
    $(DGEN_DIR)/r_main.c $(DGEN_DIR)/r_plane.c $(DGEN_DIR)/r_segs.c \
    $(DGEN_DIR)/r_sky.c $(DGEN_DIR)/r_things.c \
    $(DGEN_DIR)/s_sound.c $(DGEN_DIR)/sha1.c $(DGEN_DIR)/sounds.c \
    $(DGEN_DIR)/statdump.c $(DGEN_DIR)/st_lib.c $(DGEN_DIR)/st_stuff.c \
    $(DGEN_DIR)/tables.c $(DGEN_DIR)/v_video.c $(DGEN_DIR)/w_checksum.c \
    $(DGEN_DIR)/w_file.c $(DGEN_DIR)/w_file_stdc.c $(DGEN_DIR)/w_main.c \
    $(DGEN_DIR)/w_wad.c $(DGEN_DIR)/wi_stuff.c \
    $(DGEN_DIR)/i_input.c $(DGEN_DIR)/i_video.c $(DGEN_DIR)/doomgeneric.c

KERN_OBJ  := $(KERN_SRC:.c=.o)

# Embedded WAVs (objcopy -I binary). Variables only here — rules live below
# the default target so 'make' without arguments still builds the kernel.
EMBED_WAVS := $(wildcard embed/*.wav)
EMBED_OBJS := $(EMBED_WAVS:.wav=.wav.o)

LIBC_OBJ  := $(LIBC_SRC:.c=.o) $(LIBC_ASM:.S=.o)
DGEN_LOC_OBJ := $(DGEN_LOCAL:.c=.o)
DGEN_OBJ  := $(DGEN_SRC:.c=.o)

# Userland: a static i686 busybox (musl) + its applet list, and the TCC +
# musl sysroot tarball (userland/build-sysroot.sh), linked into the kernel
# image and unpacked into the ramfs at boot (src/proc/userland.c).
USERLAND_BINS := userland/busybox userland/busybox.applets userland/sysroot.tar
USERLAND_OBJS := $(addsuffix .bin.o,$(USERLAND_BINS))

ALL_OBJ := $(KERN_OBJ) $(LIBC_OBJ) $(DGEN_LOC_OBJ) $(DGEN_OBJ) $(EMBED_OBJS) $(USERLAND_OBJS)

KERNEL := build/samara.elf

all: $(KERNEL)

# Auto-generated list of EMBED(name, sym) pairs included from src/apps/embed.c
src/apps/embed_list.h: $(EMBED_WAVS) Makefile
	@printf '/* auto-generated, see embed dir */\n' > $@
	@for f in $(EMBED_WAVS); do                                                  \
	  base=$$(basename "$$f");                                                   \
	  sym=$$(printf '%s' "$$f" | sed 's|[^A-Za-z0-9_]|_|g');                     \
	  printf 'EMBED("%s", %s)\n' "$$base" "$$sym" >> $@;                         \
	done

# objcopy: turn raw .wav into a relocatable ELF blob with _binary_*_start/end
embed/%.wav.o: embed/%.wav
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 $< $@

# Symbols come out as _binary_userland_busybox_start etc.
userland/%.bin.o: userland/%
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 --rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

# Font atlases baked by tools/mkfont.py (committed, regenerate on demand)
src/gfx/uifont.o: src/gfx/uifont_data.h src/gfx/uifont.h

# embed.c includes the generated header
src/apps/embed.o: src/apps/embed_list.h

build:
	@mkdir -p build

# Kernel files: strict warnings
$(KERN_OBJ): %.o: %.c | build
	$(CC) $(KCFLAGS) -c $< -o $@

# libc-shim files: x87 FP allowed (sqrt/atof use it). Kernel includes needed.
src/libc/%.o: src/libc/%.c | build
	$(CC) -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	    -nostdlib -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	    -O2 -std=gnu11 -fcommon \
	    -isystem src/libc/include -Isrc \
	    -Wno-builtin-declaration-mismatch -w \
	    -c $< -o $@

src/libc/%.o: src/libc/%.S | build
	$(CC) -m32 -c $< -o $@

# Local DOOM platform layer: permissive, sees DOOM headers
src/doomgeneric/%.o: src/doomgeneric/%.c | build
	$(CC) $(DCFLAGS) -c $< -o $@

# DOOM engine sources from upstream
$(DGEN_DIR)/%.o: $(DGEN_DIR)/%.c | build
	$(CC) $(DCFLAGS) -c $< -o $@

$(KERNEL): $(ALL_OBJ) linker.ld | build
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJ) $(LIBGCC)

# Music directory served as a read-only virtual FAT16 disk on secondary IDE.
# Drop .wav files in music/ and use `playfat <name>` inside SamaraOS.
MUSIC_DIR ?= music
# snapshot=on writes go to a temp overlay so the host music/ directory is
# untouched even though QEMU's IDE attach demands a writable backing store.
MUSIC_DRIVE := -drive file=fat:$(MUSIC_DIR),format=raw,if=ide,index=2,snapshot=on

# RTL8139 NIC on QEMU user-mode SLIRP. Guest gets 10.0.2.15, gw 10.0.2.2.
# DNS at 10.0.2.3 is exposed but our stack doesn't use it (numeric IPs only).
NET_DRIVE := -netdev user,id=n0 -device rtl8139,netdev=n0

# Persistent 64 MiB FAT32 disk on AHCI: SamaraOS mounts it at /mnt and
# writes changes back, so files there survive reboots. Created on first run;
# on the host: `mdir -i disk.img ::` / `mcopy -i disk.img ::file .`
DISK_IMG ?= disk.img
DISK_DRIVE := -device ahci,id=ahci -drive id=sata0,file=$(DISK_IMG),format=raw,if=none \
              -device ide-hd,drive=sata0,bus=ahci.0

$(DISK_IMG):
	truncate -s 64M $@
	mkfs.fat -F 32 -n SAMARA $@ >/dev/null

run: $(KERNEL) $(DISK_IMG)
	@mkdir -p $(MUSIC_DIR)
	$(QEMU) -kernel $(KERNEL) -m 256 -vga std -serial stdio $(AUDIO) $(MUSIC_DRIVE) $(NET_DRIVE) $(DISK_DRIVE)

# Run with DOOM1.WAD attached as the primary disk so 'doom' command works.
WAD ?= Doom1.WAD
run-doom: $(KERNEL)
	@mkdir -p $(MUSIC_DIR)
	$(QEMU) -kernel $(KERNEL) -m 256 -vga std -serial stdio $(AUDIO) \
	    -drive file=$(WAD),format=raw,if=ide,index=0 $(MUSIC_DRIVE) $(NET_DRIVE)

run-debug: $(KERNEL)
	@mkdir -p $(MUSIC_DIR)
	$(QEMU) -kernel $(KERNEL) -m 256 -vga std -serial stdio $(AUDIO) $(MUSIC_DRIVE) $(NET_DRIVE) -d int -no-reboot -no-shutdown

# Real, portable boot image: a GRUB rescue ISO carrying the multiboot kernel.
# Boots on any x86 PC/VM from CD/USB, not just via QEMU's -kernel shortcut.
ISO := build/samara.iso

$(ISO): $(KERNEL) iso/grub.cfg
	@mkdir -p build/isodir/boot/grub
	cp $(KERNEL) build/isodir/boot/samara.elf
	cp iso/grub.cfg build/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) build/isodir

iso: $(ISO)

# Boots the ISO exactly as a real PC/VM would (BIOS -> GRUB -> multiboot),
# instead of QEMU's -kernel shortcut which skips the bootloader entirely.
run-iso: $(ISO)
	@mkdir -p $(MUSIC_DIR)
	$(QEMU) -cdrom $(ISO) -boot d -m 256 -vga std -serial stdio $(AUDIO) \
	    -drive file=fat:$(MUSIC_DIR),format=raw,if=ide,index=3,snapshot=on $(NET_DRIVE)

# Same as run-doom but the WAD sits on a SATA disk behind an AHCI controller
# (exercises src/drivers/ahci.c; shows up as /dev/sda, disk index 4).
run-sata: $(KERNEL)
	@mkdir -p $(MUSIC_DIR)
	$(QEMU) -kernel $(KERNEL) -m 256 -vga std -serial stdio $(AUDIO) \
	    -device ahci,id=ahci -drive id=sata0,file=$(WAD),format=raw,if=none \
	    -device ide-hd,drive=sata0,bus=ahci.0 $(MUSIC_DRIVE) $(NET_DRIVE)

clean:
	rm -f $(ALL_OBJ) $(KERNEL)
	rm -rf build

.PHONY: all run run-doom run-sata run-debug iso run-iso clean build
