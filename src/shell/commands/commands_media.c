#include "../shell_priv.h"
#include "apps/wav.h"
#include "core/heap.h"
#include "core/string.h"
#include "drivers/sb16.h"
#include "drivers/vga.h"
#include "fs/fat.h"
#include "fs/fs.h"

static const char *wav_err_str(int r) {
  switch (r) {
  case 0:
    return "ok";
  case -1:
    return "bad args";
  case -2:
    return "missing RIFF tag (not a WAV file)";
  case -3:
    return "missing WAVE tag";
  case -4:
    return "chunk runs off end (truncated file?)";
  case -5:
    return "not PCM (compressed: try saving as 'PCM WAV')";
  case -6:
    return "data before fmt chunk";
  case -7:
    return "no data chunk found";
  case -10:
    return "SB16 not initialised";
  case -11:
    return "fmt chunk has zero frame size";
  case -12:
    return "SB16 play failed";
  default:
    return "unknown error";
  }
}

static void fat_list_visit(const char *name, uint32_t size, void *user) {
  (void)user;
  vga_puts("  ");
  vga_puts(name);
  int pad = 13 - (int)strlen(name);
  for (int i = 0; i < pad; i++)
    vga_putc(' ');
  vga_printf(" %u bytes\n", size);
}

void cmd_fatmount(int argc, char **argv) {
  int idx = 2; /* secondary master by default */
  if (argc > 1)
    idx = atoi(argv[1]);
  if (fat_mount(idx)) {
    vga_printf("fat: mounted drive %d\n", idx);
  } else {
    vga_printf("fat: mount drive %d failed (%s)\n", idx, fat_status());
  }
}

void cmd_fatls(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!fat_mounted()) {
    vga_puts("fat: not mounted (run: fatmount)\n");
    return;
  }
  vga_puts("FAT root:\n");
  fat_list(fat_list_visit, NULL);
}

void cmd_fatload(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("fatload: usage: fatload <FAT-name> [ramfs-name]\n");
    return;
  }
  if (!fat_mounted()) {
    vga_puts("fatload: not mounted\n");
    return;
  }
  uint8_t *buf = NULL;
  uint32_t sz = 0;
  int r = fat_read_file(argv[1], &buf, &sz);
  if (r != 0) {
    vga_printf("fatload: error %d\n", r);
    return;
  }
  const char *dst = argc > 2 ? argv[2] : argv[1];
  fs_node_t *f = fs_resolve(cwd, dst);
  if (!f)
    f = fs_create(cwd, dst, FS_FILE);
  if (!f || f->type != FS_FILE) {
    vga_puts("fatload: cannot create ramfs file\n");
    kfree(buf);
    return;
  }
  fs_write(f, (const char *)buf, sz);
  kfree(buf);
  vga_printf("fatload: %s -> %s (%u bytes)\n", argv[1], dst, sz);
}

/* Convenience: mount + find + play in one go, without copying to ramfs. */
void cmd_playfat(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("playfat: usage: playfat <FAT-name>\n");
    return;
  }
  if (!sb16_present()) {
    vga_puts("playfat: SB16 not available\n");
    return;
  }
  if (!fat_mounted()) {
    if (!fat_mount(2)) {
      vga_printf("playfat: auto-mount failed (%s)\n", fat_status());
      return;
    }
  }
  uint8_t *buf = NULL;
  uint32_t sz = 0;
  int r = fat_read_file(argv[1], &buf, &sz);
  if (r != 0) {
    vga_printf("playfat: not found (%d)\n", r);
    return;
  }
  vga_printf("playfat: %s, %u bytes, playing...\n", argv[1], sz);
  int pr = wav_play(buf, sz);
  if (pr != 0)
    vga_printf("playfat: %s (err %d)\n", wav_err_str(pr), pr);
  kfree(buf);
}

void cmd_playwav(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("playwav: usage: playwav <file>\n");
    return;
  }
  if (!sb16_present()) {
    vga_puts("playwav: SB16 not available (");
    vga_puts(sb16_status());
    vga_puts(")\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[1]);
  if (!f || f->type != FS_FILE || !f->data) {
    vga_puts("playwav: no such file\n");
    return;
  }
  vga_printf("playwav: %u bytes, playing...\n", (uint32_t)f->size);
  int r = wav_play((const uint8_t *)f->data, (uint32_t)f->size);
  if (r != 0)
    vga_printf("playwav: %s (err %d)\n", wav_err_str(r), r);
}

void cmd_stopwav(int argc, char **argv) {
  (void)argc;
  (void)argv;
  sb16_stop();
  vga_puts("playwav: stopped\n");
}

void cmd_sbinfo(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("sb16: ");
  vga_puts(sb16_status());
  vga_putc('\n');
}
