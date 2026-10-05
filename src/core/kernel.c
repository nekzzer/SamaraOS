#include "boot/apic.h"
#include "boot/acpi.h"
#include "core/smp.h"
#include "core/types.h"
#include "core/io.h"
#include "drivers/vga.h"
#include "core/string.h"
#include "boot/gdt.h"
#include "boot/idt.h"
#include "boot/paging.h"
#include "boot/fpu.h"
#include "boot/pic.h"
#include "boot/pit.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/usb.h"
#include "core/heap.h"
#include "core/task.h"
#include "proc/uring.h"
#include "fs/fs.h"
#include "shell/shell.h"
#include "boot/multiboot.h"
#include "core/bootmod.h"
#include "core/vmm.h"
#include "gfx/font.h"
#include "gui/desktop.h"
#include "gui/login.h"
#include "gfx/termfont.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "apps/doom.h"
#include "drivers/sb16.h"
#include "apps/synth.h"
#include "apps/embed.h"
#include "core/clock.h"
#include "core/vmm.h"
#include "proc/proc.h"
#include "proc/userland.h"
#include "net/sock.h"
#include "fs/fatfs.h"
#include "fs/ext2.h"
#include "shell/commands.h"
#include "drivers/drm.h"

/* ---------- boot stack ----------
   DOOM's R_RenderBSPNode is deeply recursive (especially when the BSP tree
   is dense). 4 MiB gives plenty of headroom and, crucially, lets us *detect*
   overflow via a sentinel check in the PIT IRQ before it corrupts other
   .bss objects. */
#define BOOT_STACK_BYTES (4u * 1024u * 1024u)
__attribute__((aligned(4096)))
uint8_t boot_stack[BOOT_STACK_BYTES];

/* Sentinel placed at the very bottom of the stack. The PIT handler reads it
   on every tick and panics on serial if it ever changes — that's how we
   notice a stack overflow before it scribbles over the page directory. */
#define BOOT_STACK_GUARD 0xDEADC0DEu
uint32_t* boot_stack_sentinel = (uint32_t*)boot_stack;

/* ---------- boot log helpers: "[*] name...... " then a status ---------- */
#define BOOT_LABEL_WIDTH 14

static void boot_step(const char* label) {
    vga_puts("[*] ");
    vga_puts(label);
    int pad = BOOT_LABEL_WIDTH - (int)strlen(label);
    for (int i = 0; i < pad; i++) vga_putc('.');
    vga_putc(' ');
}

static void boot_done(const char* status) {
    vga_puts(status);
    vga_putc('\n');
}

/* One line per init: BOOT_OK(label, call) runs `call` and prints "ok".
 * Prefer this over separate boot_step()/boot_done() calls when the status
 * is a plain "ok" -- keeps step+result atomic so they can't drift apart. */
#define BOOT_OK(label, call) do { boot_step(label); call; boot_done("ok"); } while (0)

/* ---------- background spinner task: proves multitasking ---------- */
static void task_blinker(void) {
    int n = 0;
    while (1) {
        const char glyphs[] = "|/-\\";
        uint8_t color = (VGA_BLACK << 4) | VGA_LMAGENTA;
        vga_putcell_at(VGA_WIDTH - 2, 0, glyphs[n & 3], color);
        n++;
        for (volatile int i = 0; i < 2000000; i++) { __asm__ volatile (""); }
    }
}

/* Kernel command line (multiboot / QEMU -append), copied before anything
   can overwrite the bootloader's copy. */
static char boot_cmdline[512];
const char* kernel_cmdline(void) { return boot_cmdline; }

/* "autosh=<script>" runs `/bin/sh -c <script>` at boot with the program's
   output mirrored to COM1, then powers off: headless tests from the host. */
static void boot_autorun(void) {
    const char* key = strstr(boot_cmdline, "autosh=");
    if (!key) return;
    extern bool tty_serial_mirror;
    tty_serial_mirror = true;
    /* "autogfx": framebuffer console first, so /dev/fb0 works (Xorg tests) */
    if (strstr(boot_cmdline, "autogfx")) console_gfx_start();
    char* argv[] = { "sh", "-c", (char*)key + 7, NULL };
    shell_exec_program("/bin/sh", 3, argv);
    ext2_sync_all(); fatfs_sync_all();   /* last writes were lost on poweroff */
    const char* bye = "\r\n[autosh done]\r\n";
    while (*bye) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *bye++); }
    outw(0x604, 0x2000);                 /* QEMU ACPI power off */
}

/* ---- boot modules ---- */

#define MAX_BOOTMODS 8
static bootmod_t bootmods[MAX_BOOTMODS];
static int n_bootmods;
static uint64_t mods_floor;               /* lowest byte used by moved modules */

int boot_modules(const bootmod_t** out) { *out = bootmods; return n_bootmods; }

typedef struct { uint32_t start, end, string, reserved; } mb_mod_t;

typedef struct { uint32_t size; uint64_t base, len; uint32_t type; } __attribute__((packed)) mb_mmap_t;

/* usable ranges from the multiboot map (or just mem_upper) */
#define MAX_REG 16
static struct { uint64_t start, end; } reg[MAX_REG];
static int n_reg;
static uint64_t ram_top_g;                /* end of the highest usable range */
static uint64_t low_end;                  /* end of the range that holds the kernel, below 4 GiB */

static void read_memmap(multiboot_info_t* mbi) {
    if (mbi->flags & 64) {
        uint8_t* p = (uint8_t*)P2V(mbi->mmap_addr);
        uint8_t* e = p + mbi->mmap_length;
        while (p < e && n_reg < MAX_REG) {
            mb_mmap_t* m = (mb_mmap_t*)p;
            if (m->type == 1 && m->len) {
                uint64_t s = m->base, en = m->base + m->len;
                if (s < 0x100000) s = 0x100000;            /* low megabyte is bios/vga stuff */
                if (en > s) { reg[n_reg].start = s; reg[n_reg].end = en; n_reg++; }
            }
            p += m->size + 4;
        }
    }
    if (!n_reg) {
        reg[0].start = 0x100000;
        reg[0].end = 0x100000 + ((mbi->flags & 1) ? (uint64_t)mbi->mem_upper * 1024 : 0x7F00000);
        n_reg = 1;
    }
    for (int i = 0; i < n_reg; i++) {
        if (reg[i].end > ram_top_g) ram_top_g = reg[i].end;
        if (reg[i].start <= 0x100000 && reg[i].end > 0x100000) low_end = reg[i].end;
    }
    if (low_end > 0x100000000ull) low_end = 0x100000000ull;
}

/* The loader drops modules right after the kernel image - where our heap
   goes. Before anything allocates slide them to the top of the low RAM
   range, highest first so none overwrites another. */
static void relocate_modules(multiboot_info_t* mbi) {
    mods_floor = low_end;
    if (!(mbi->flags & 8) || !mbi->mods_count) return;
    mb_mod_t* m = (mb_mod_t*)P2V(mbi->mods_addr);
    int n = (int)mbi->mods_count;
    if (n > MAX_BOOTMODS) n = MAX_BOOTMODS;
    for (int i = 0; i < n; i++) {
        bootmods[i].start = m[i].start;
        bootmods[i].end = m[i].end;
        const char* nm = m[i].string ? (const char*)P2V(m[i].string) : "";
        int k = 0;
        while (nm[k] && k < 63) { bootmods[i].name[k] = nm[k]; k++; }
        bootmods[i].name[k] = 0;
    }
    /* order by source address, descending */
    for (int i = 1; i < n; i++) {
        bootmod_t v = bootmods[i];
        int j = i - 1;
        while (j >= 0 && bootmods[j].start < v.start) { bootmods[j + 1] = bootmods[j]; j--; }
        bootmods[j + 1] = v;
    }
    uint64_t top = low_end & ~0xFFFull;
    for (int i = 0; i < n; i++) {
        uint64_t len = bootmods[i].end - bootmods[i].start;
        uint64_t dst = (top - len) & ~0xFFFull;
        if (len > top || dst < 0x08000000u) { n = i; break; }  /* no room above 128 MiB */
        memmove(P2V(dst), P2V(bootmods[i].start), len);
        bootmods[i].start = dst;
        bootmods[i].end = dst + len;
        top = dst;
    }
    n_bootmods = n;
    mods_floor = top;
}

void kmain(uint32_t magic, uint32_t mb_info_addr) {
    multiboot_info_t* mbi = (multiboot_info_t*)P2V(mb_info_addr);
    bool mb = magic == MB1_BOOTED_MAGIC && mb_info_addr;
    if (mb) { read_memmap(mbi); relocate_modules(mbi); }
    else { low_end = ram_top_g = 0x8000000; mods_floor = low_end; reg[0].start = 0x100000; reg[0].end = low_end; n_reg = 1; }
    if (mb) {
        if ((mbi->flags & 4) && mbi->cmdline) {
            const char* c = (const char*)P2V(mbi->cmdline);
            int i = 0;
            while (c[i] && i < (int)sizeof(boot_cmdline) - 1) { boot_cmdline[i] = c[i]; i++; }
            boot_cmdline[i] = 0;
        }
    }

    { extern bool g_wm_stats; g_wm_stats = strstr(boot_cmdline, "wmstats") != NULL; }
    { extern bool g_ftrace; g_ftrace = strstr(boot_cmdline, "ftrace") != NULL; }
    {   /* syscalls -> COM1: "strace" (all) or "strace=PID" */
        extern bool g_strace; extern int g_strace_pid;
        const char* k = strstr(boot_cmdline, "strace");
        if (k) {
            g_strace = true;
            if (k[6] == '=') for (k += 7; *k >= '0' && *k <= '9'; k++) g_strace_pid = g_strace_pid * 10 + (*k - '0');
        }
    }

    /* Plant the stack-overflow sentinel at the very bottom of boot_stack. */
    *boot_stack_sentinel = BOOT_STACK_GUARD;

    /* Heap lives immediately past the end of .bss (linker symbol
       _heap_start, 4 MiB aligned). 64 MiB long. 1920x1080x32 back buffer
       alone is ~8 MiB; DOOM zone + WAD blobs + browser response buffers
       take more. */
    extern char _heap_start[];
    const uint32_t heap_bytes = 0x4000000;
    heap_init((void*)_heap_start, heap_bytes);

    termfont_init();
    vga_init();
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("=== SamaraOS booting ===\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    /* Capture font from VGA plane 2 BEFORE any mode change later on. The
       font_init also overlays our CP866 Cyrillic glyphs; we then push them
       back to plane 2 so the text-mode shell can render Russian right
       from boot, not just after the desktop has been entered. */
    BOOT_OK("font extract", font_init());
    font_restore();

    BOOT_OK("gdt+tss", gdt_init());
    BOOT_OK("idt", idt_init());

    boot_step("paging"); paging_init(ram_top_g);
    vga_printf("ok (cr0=0x%lx cr4=0x%lx)\n", paging_cr0(), paging_cr4());

    boot_step("fpu"); fpu_init();
    boot_done(!fpu_present() ? "absent" : fpu_sse() ? "ok (x87 + SSE)" : "ok (x87)");

    BOOT_OK("pic", pic_remap());
    acpi_init();
    apic_init();
    if (apic_on) vga_puts("apic on\n");
    clock_init();
    BOOT_OK("fs", fs_init());
    BOOT_OK("kbd", kbd_init());
    /* "kbd_ignore=35,2b": hex scancodes of keys to ignore (broken keys). */
    for (const char* k = strstr(boot_cmdline, "kbd_ignore="); k; k = NULL) {
        k += 11;
        while (*k && *k != ' ') {
            uint32_t v = 0;
            int digits = 0;
            for (; *k && *k != ',' && *k != ' '; k++, digits++) {
                char c = *k;
                v = v * 16 + (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            if (digits && v < 0x80) { kbd_ignore_scancode((uint8_t)v); vga_printf("    ignoring scancode 0x%x\n", v); }
            if (*k == ',') k++;
        }
    }
    BOOT_OK("mouse", mouse_init());
    BOOT_OK("tasks", task_init());
    BOOT_OK("pit 1000Hz", pit_init(1000));

    /* Process frames: all usable RAM past the heap up to the boot modules, and
       whatever sits above 4 GiB. With RAM to spare the top 40% of the low
       range becomes the big heap arena for file contents (compilers,
       archives), reached through the direct map. */
    {
        uint64_t pool_start = (V2P(_heap_start) + heap_bytes + 0x1FFFFF) & ~0x1FFFFFull;
        uint64_t pool_end = mods_floor & ~0x1FFFFFull;
        uint64_t avail = pool_end > pool_start ? pool_end - pool_start : 0;
        uint64_t big = avail >= 0x10000000u ? (avail / 5 * 2) & ~0x1FFFFFull : 0;   /* >= 256 MiB */
        if (big) heap_add_big(P2V(pool_end - big), big);
        boot_step("user memory");
        pmm_init(ram_top_g);
        for (int i = 0; i < n_reg; i++) {
            uint64_t s = reg[i].start, e = reg[i].end;
            if (s <= 0x100000 && e > 0x100000) { s = pool_start; e = pool_end - big; }
            if (e > s) pmm_add(s, e);
        }
        vga_printf("ok (%u KB, files %u KB, modules %d)\n", (uint32_t)(pmm_total_frames() * 4), (uint32_t)(heap_big_total() / 1024),
                   n_bootmods);
    }
    BOOT_OK("syscalls", (proc_init(), syscall_init()));
    BOOT_OK("sockets", sock_init());
    BOOT_OK("io_uring", uring_init());
    boot_step("busybox");
    {
        int n = userland_install();
        if (n < 0) boot_done("failed");
        else vga_printf("ok (%d applets)\n", n);
    }
    if (n_bootmods) {
        boot_step("boot modules");
        vga_printf("ok (%d files)\n", userland_install_modules());
    }

    boot_step("disks");
    ata_init_all();
    if (ata_primary() < 0) {
        boot_done("none");
    } else {
        for (int i = 0; i < DISK_MAX; i++) {
            if (ata_drive_present(i))
                vga_printf("[%s:%uMB] ", ata_drive_name(i), ata_drive_sectors(i) / 2048);
        }
        vga_putc('\n');
    }
    vga_printf("    ahci: %s\n", ahci_status());
    fs_add_disk_nodes();                  /* /dev/hda.., /dev/sda.. */
    fatfs_init();
    /* A FAT-formatted first SATA disk becomes persistent storage at /mnt. */
    if (ata_drive_present(DISK_AHCI_BASE) && fatfs_probe(DISK_AHCI_BASE)) {
        int r = fatfs_mount(DISK_AHCI_BASE, fs_resolve(fs_root(), "/mnt"));
        vga_printf("    /mnt: %s\n", r == 0 ? "sda mounted (vfat, persistent)" : "mount failed");
    } else if (ata_drive_present(DISK_VIRTIO_BASE) && fatfs_probe(DISK_VIRTIO_BASE)) {
        int r = fatfs_mount(DISK_VIRTIO_BASE, fs_resolve(fs_root(), "/mnt"));
        vga_printf("    /mnt: %s\n", r == 0 ? "vda mounted (vfat, persistent)" : "mount failed");
    } else {
        /* installed system on a plain ide disk (see gui/install.c) */
        for (int i = 0; i < ATA_DRIVES; i++) {
            if (!ata_drive_present(i) || !fatfs_probe(i)) continue;
            int r = fatfs_mount(i, fs_resolve(fs_root(), "/mnt"));
            vga_printf("    /mnt: %s %s\n", ata_drive_name(i), r == 0 ? "mounted" : "mount failed");
            break;
        }
    }
    login_load_etc();

    /* ext2 disks show up under /disk/<name> */
    for (int i = 0; i < DISK_MAX; i++) {
        if (!ext2_probe(i)) continue;
        fs_node_t* d = fs_resolve(fs_root(), "/disk");
        if (!d) d = fs_create(fs_root(), "/disk", FS_DIR);
        char p[24] = "/disk/", lab[17];
        strcat(p, ata_drive_name(i));
        ext2_label(i, lab);
        if (lab[0] == '/') {                   /* label "/usr/local": mount it there */
            strcpy(p, lab);
            for (char* s = p + 1; *s; s++) {
                if (*s != '/') continue;
                *s = 0;
                if (!fs_resolve(fs_root(), p)) fs_create(fs_root(), p, FS_DIR);
                *s = '/';
            }
        }
        fs_node_t* at = fs_resolve(fs_root(), p);
        if (!at) at = fs_create(fs_root(), p, FS_DIR);
        int r = ext2_mount(i, at);
        vga_printf("    %s: %s\n", p, r == 0 ? "ext2 mounted" : r == 1 ? "ext mounted read-only" : "ext2 mount failed");
        /* /usr/local/bin/x -> /usr/bin/x too: ssh commands and the like use a short PATH */
        if (r >= 0 && !strcmp(p, "/usr/local")) {
            fs_node_t* lb = fs_resolve(fs_root(), "/usr/local/bin");
            fs_node_t* ub = fs_resolve(fs_root(), "/usr/bin");
            for (fs_node_t* c = lb ? lb->child : NULL; c && ub; c = c->next) {
                if (fs_child(ub, c->name)) continue;
                char tg[FS_NAME_MAX + 20] = "/usr/local/bin/";
                strcat(tg, c->name);
                fs_node_t* l = fs_symlink(ub, c->name, tg);
                if (l) l->mount_id = 0;
            }
        }
    }

    /* Background services from /etc/rc: the dropbear ssh server and telnetd
       (both on a pty per session). Skipped with "noservices". */
    if (!strstr(boot_cmdline, "noservices")) {
        fs_node_t* rc = fs_resolve(fs_root(), "/etc/rc");
        extern fs_node_t* cwd;
        fs_node_t* saved = cwd;
        cwd = fs_root();
        if (rc) {
            char* argv[] = { "sh", "/etc/rc", NULL };
            char* envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/root", NULL };
            boot_step("services");
            int pid = proc_spawn_detached("/bin/sh", argv, envp);
            if (pid > 0) vga_printf("ok (ssh :22, telnet :23)\n");
            else boot_done("failed");
        }
        cwd = saved;
    }
    for (const char* m = "samara: ahci: "; *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }
    for (const char* m = ahci_status(); *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }
    for (const char* m = "\r\n"; *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }

    boot_step("sb16");
    if (sb16_init()) vga_printf("ok (DSP %u.%u)\n", 0u, 0u); /* version not pretty-printed here */
    else             vga_printf("%s\n", sb16_status());

    boot_step("usb");
    usb_init();
    drm_init();
    boot_done("uhci (polled)");

    BOOT_OK("synth wavs", synth_install_demo_wavs());
    BOOT_OK("embed wavs", embed_install());

    /* COM1 init for serial diagnostics. */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    const char* m1 = "samara: boot\r\n";
    while (*m1) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m1++); }

    /* Pass multiboot info to desktop (used as last-resort FB source). */
    if (magic == MB1_BOOTED_MAGIC && mb_info_addr) {
        desktop_install_mbi(mbi);
    }
    /* Try loading DOOM WAD from disk so user sees it on boot if attached. */
    int dr = doom_load_from_disk();
    const char* dmsg = doom_status();
    const char* tag = "samara: doom: ";
    for (int i = 0; tag[i]; i++) { while (!(inb(0x3F8+5) & 0x20)){} outb(0x3F8, tag[i]); }
    for (int i = 0; dmsg[i]; i++) { while (!(inb(0x3F8+5) & 0x20)){} outb(0x3F8, dmsg[i]); }
    while (!(inb(0x3F8+5) & 0x20)){} outb(0x3F8, '\r');
    while (!(inb(0x3F8+5) & 0x20)){} outb(0x3F8, '\n');
    (void)dr;

    /* Probe Bochs VBE so we can tell the user upfront. */
    outw(0x01CE, 0);
    uint16_t vbe_id = inw(0x01CF);
    if (vbe_id >= 0xB0C0 && vbe_id <= 0xB0CF) {
        vga_set_color(VGA_LGREEN, VGA_BLACK);
        vga_puts("[*] graphics: Bochs VBE detected (1920x1080x32 ready)\n");
        vga_set_color(VGA_LGREY, VGA_BLACK);
    } else {
        vga_puts("[*] graphics: VBE not detected; mode 13h fallback (320x200)\n");
    }

    /* task_spawn("blinker", task_blinker); */  /* disabled — interferes with DOOM */
    (void)task_blinker;

    smp_init();
    sti();

    boot_autorun();
    /* The shell runs on the graphical console (true colour, the terminal
       font) unless "textmode" asks for plain VGA text. */
    if (!strstr(boot_cmdline, "textmode") && console_gfx_start() && !strstr(boot_cmdline, "nologin")) {
        /* booted from the ISO: offer to install first */
        if (fs_resolve(fs_root(), "/boot/grub/core.img")) install_screen();
        login_screen();
        shell_run_line("desktop");
    }
    shell_run();

    while (1) hlt();
}
