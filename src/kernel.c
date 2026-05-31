#include "types.h"
#include "io.h"
#include "vga.h"
#include "string.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "fpu.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "heap.h"
#include "task.h"
#include "fs.h"
#include "shell.h"
#include "multiboot.h"
#include "font.h"
#include "desktop.h"
#include "ata.h"
#include "doom.h"
#include "sb16.h"
#include "synth.h"
#include "embed.h"

/* ---------- Multiboot 1 header (with framebuffer request) ---------- */
#define MB_MAGIC 0x1BADB002
/* bit 0 = page-aligned modules, bit 1 = mem map, bit 2 = video mode */
#define MB_FLAGS 0x00000007
#define MB_CHK  (uint32_t)(-(MB_MAGIC + MB_FLAGS))

__attribute__((section(".multiboot"), used, aligned(4)))
static const struct {
    uint32_t magic, flags, checksum;
    /* video mode fields (bit 2) */
    uint32_t mode_type;     /* 0 = linear graphics, 1 = text */
    uint32_t width, height, depth;
} multiboot_header = {
    MB_MAGIC, MB_FLAGS, MB_CHK,
    0, 1920, 1080, 32
};

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

extern void kmain(uint32_t magic, uint32_t mb_info_addr);

/* Entry point: GRUB / qemu -kernel jumps here.
   eax = 0x2BADB002, ebx = multiboot info ptr. */
__attribute__((naked, section(".text.start"), used))
void _start(void) {
    __asm__ volatile (
        "mov $boot_stack + 4194304, %esp\n"
        "xor %ebp, %ebp\n"
        "cld\n"
        "push %ebx\n"               /* arg 2: mb_info_addr */
        "push %eax\n"               /* arg 1: magic */
        "call kmain\n"
        "1: cli\n"
        "   hlt\n"
        "   jmp 1b\n"
    );
}

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

void kmain(uint32_t magic, uint32_t mb_info_addr) {
    (void)magic;

    /* Plant the stack-overflow sentinel at the very bottom of boot_stack. */
    *boot_stack_sentinel = BOOT_STACK_GUARD;

    /* Heap lives immediately past the end of .bss (linker symbol
       _heap_start, 4 MiB aligned). 64 MiB long. 1920x1080x32 back buffer
       alone is ~8 MiB; DOOM zone + WAD blobs + browser response buffers
       take more. */
    extern char _heap_start[];
    heap_init((void*)_heap_start, 0x4000000);

    vga_init();
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("=== SamaraOS booting ===\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    /* Capture font from VGA plane 2 BEFORE any mode change later on. The
       font_init also overlays our CP866 Cyrillic glyphs; we then push them
       back to plane 2 so the text-mode shell can render Russian right
       from boot, not just after the desktop has been entered. */
    vga_puts("[*] font extract..."); font_init();   vga_puts(" ok\n");
    font_restore();

    vga_puts("[*] gdt+tss..."); gdt_init();    vga_puts(" ok\n");
    vga_puts("[*] idt..."); idt_init();        vga_puts(" ok\n");
    vga_puts("[*] paging..."); paging_init();
    vga_printf(" ok (cr0=0x%x cr4=0x%x)\n", paging_cr0(), paging_cr4());
    vga_puts("[*] fpu..."); fpu_init();
    vga_puts(fpu_present() ? " ok\n" : " absent\n");
    vga_puts("[*] pic..."); pic_remap();       vga_puts(" ok\n");
    vga_puts("[*] fs...");  fs_init();         vga_puts(" ok\n");
    vga_puts("[*] kbd...");  kbd_init();       vga_puts(" ok\n");
    vga_puts("[*] mouse..."); mouse_init();    vga_puts(" ok\n");
    vga_puts("[*] tasks..."); task_init();     vga_puts(" ok\n");
    vga_puts("[*] pit 100Hz..."); pit_init(100); vga_puts(" ok\n");
    vga_puts("[*] ata...");
    ata_init_all();
    for (int i = 0; i < 4; i++) {
        if (ata_drive_present(i)) {
            const char* labels[4] = { "pri-mst", "pri-slv", "sec-mst", "sec-slv" };
            vga_printf(" [%s:%u]", labels[i], ata_drive_sectors(i));
        }
    }
    if (!ata_drive_present(0) && !ata_drive_present(1) &&
        !ata_drive_present(2) && !ata_drive_present(3)) {
        vga_puts(" no disks");
    }
    vga_putc('\n');

    vga_puts("[*] sb16...");
    if (sb16_init()) vga_printf(" ok (DSP %u.%u)\n",
                                 0u, 0u);   /* version not pretty-printed here */
    else             vga_printf(" %s\n", sb16_status());

    vga_puts("[*] synth wavs..."); synth_install_demo_wavs(); vga_puts(" ok\n");
    vga_puts("[*] embed wavs..."); embed_install();             vga_puts(" ok\n");

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
        desktop_install_mbi((multiboot_info_t*)mb_info_addr);
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

    sti();

    shell_run();

    while (1) hlt();
}
