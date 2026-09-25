#include "../shell_priv.h"
#include "boot/pit.h"
#include "core/string.h"
#include "drivers/vga.h"
#include "fs/fs.h"
#include "net/net.h"

/* Parse "a.b.c.d" -> uint32_t (host byte order). 1 on success. */
static int parse_ipv4(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int parts = 0, n = 0, have = 0;
  while (*s) {
    if (*s >= '0' && *s <= '9') {
      n = n * 10 + (*s - '0');
      if (n > 255)
        return 0;
      have = 1;
    } else if (*s == '.') {
      if (!have)
        return 0;
      v = (v << 8) | (uint32_t)n;
      n = 0;
      have = 0;
      parts++;
      if (parts > 3)
        return 0;
    } else {
      return 0;
    }
    s++;
  }
  if (!have || parts != 3)
    return 0;
  v = (v << 8) | (uint32_t)n;
  *out = v;
  return 1;
}

static void print_ip(uint32_t ip) {
  vga_printf("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

static void print_mac(const uint8_t *m) {
  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    char a = hex[m[i] >> 4], b = hex[m[i] & 0xF];
    vga_putc(a);
    vga_putc(b);
    if (i < 5)
      vga_putc(':');
  }
}

static int ensure_net(const char *who) {
  if (net_ready())
    return 0;
  int r = net_init();
  if (r != 0) {
    vga_puts(who);
    vga_puts(": ");
    vga_puts(net_status());
    vga_putc('\n');
    return -1;
  }
  return 0;
}

void cmd_ifconfig(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (ensure_net("ifconfig") != 0)
    return;
  vga_puts("eth0  HWaddr ");
  print_mac(net_mac());
  vga_putc('\n');
  vga_puts("      inet ");
  print_ip(net_ip());
  vga_puts("  mask 255.255.255.0  gw ");
  print_ip(net_gw());
  vga_putc('\n');
  vga_puts("      status: ");
  vga_puts(net_status());
  vga_putc('\n');
}

void cmd_ping(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("ping: usage: ping <ipv4> [count]\n");
    return;
  }
  if (ensure_net("ping") != 0)
    return;
  uint32_t ip;
  if (!parse_ipv4(argv[1], &ip)) {
    vga_puts("ping: bad IP (numeric only, no DNS)\n");
    return;
  }
  int count = (argc > 2) ? atoi(argv[2]) : 4;
  if (count < 1)
    count = 1;
  if (count > 32)
    count = 32;
  vga_puts("PING ");
  print_ip(ip);
  vga_puts(" 32 bytes\n");
  int sent = 0, recv_ok = 0, rtt_sum = 0;
  for (int i = 0; i < count; i++) {
    int rtt = net_ping(ip, 1500);
    sent++;
    if (rtt >= 0) {
      recv_ok++;
      rtt_sum += rtt;
      vga_printf("  seq=%d time=%d ms\n", i + 1, rtt);
    } else {
      vga_printf("  seq=%d timeout\n", i + 1);
    }
    /* small delay between pings */
    uint32_t until = pit_uptime_ms() + 250;
    while (pit_uptime_ms() < until)
      net_poll();
  }
  vga_printf("--- stats: %d sent, %d recv, %d%% loss", sent, recv_ok,
             sent ? (100 - 100 * recv_ok / sent) : 0);
  if (recv_ok)
    vga_printf(", avg %d ms", rtt_sum / recv_ok);
  vga_putc('\n');
}

/* URL parser for "http://(a.b.c.d|host|localhost|samara)[:port][/path]" — no
 * DNS. */
static int parse_http_url(const char *url, uint32_t *ip, uint16_t *port,
                          const char **path) {
  const char *p = url;
  if ((p[0] | 0x20) == 'h' && (p[1] | 0x20) == 't' && (p[2] | 0x20) == 't' &&
      (p[3] | 0x20) == 'p' && p[4] == ':' && p[5] == '/' && p[6] == '/')
    p += 7;
  char host[64];
  int hn = 0;
  while (*p && *p != ':' && *p != '/' && hn < (int)sizeof(host) - 1)
    host[hn++] = *p++;
  host[hn] = 0;
  bool got = parse_ipv4(host, ip);
  if (!got) {
    static const struct {
      const char *name;
      uint32_t ip;
    } aliases[] = {
        {"host", IP4(10, 0, 2, 2)},    {"localhost", IP4(10, 0, 2, 2)},
        {"gateway", IP4(10, 0, 2, 2)}, {"samara", IP4(10, 0, 2, 15)},
        {"self", IP4(10, 0, 2, 15)},
    };
    for (int i = 0; i < (int)(sizeof(aliases) / sizeof(aliases[0])); i++) {
      bool eq = true;
      int j;
      for (j = 0; aliases[i].name[j]; j++) {
        char a = host[j] | 0x20;
        if (a != aliases[i].name[j]) {
          eq = false;
          break;
        }
      }
      if (eq && host[j] == 0) {
        *ip = aliases[i].ip;
        got = true;
        break;
      }
    }
  }
  if (!got)
    return 0;
  *port = 80;
  if (*p == ':') {
    p++;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      p++;
    }
    if (v < 1 || v > 65535)
      return 0;
    *port = (uint16_t)v;
  }
  if (*p == 0)
    *path = "/";
  else
    *path = p; /* p points at '/' */
  return 1;
}

#define WGET_BUF (256 * 1024)
static uint8_t wget_buf[WGET_BUF];

void cmd_wget(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("wget: usage: wget <http://a.b.c.d[:port]/path> [outfile]\n");
    return;
  }
  if (ensure_net("wget") != 0)
    return;
  uint32_t ip;
  uint16_t port;
  const char *path;
  if (!parse_http_url(argv[1], &ip, &port, &path)) {
    vga_puts("wget: bad URL (need numeric IP)\n");
    return;
  }
  vga_puts("wget: GET ");
  print_ip(ip);
  vga_printf(":%u%s\n", port, path);
  int n = net_http_get(ip, port, NULL, path, wget_buf, WGET_BUF);
  if (n < 0) {
    vga_printf("wget: failed (%d) — %s\n", n, net_status());
    return;
  }
  vga_printf("wget: got %d bytes\n", n);
  if (n >= WGET_BUF)
    vga_printf("wget: warning: file truncated at %d KB\n", WGET_BUF / 1024);

  /* strip HTTP headers: find "\r\n\r\n" */
  int body_off = 0;
  for (int i = 0; i + 3 < n; i++) {
    if (wget_buf[i] == '\r' && wget_buf[i + 1] == '\n' &&
        wget_buf[i + 2] == '\r' && wget_buf[i + 3] == '\n') {
      body_off = i + 4;
      break;
    }
  }
  int body_len = n - body_off;

  if (argc >= 3) {
    const char *outname = argv[2];
    fs_node_t *f = fs_resolve(cwd, outname);
    if (!f)
      f = fs_create(cwd, outname, FS_FILE);
    if (!f || f->type != FS_FILE) {
      vga_puts("wget: cannot create output file\n");
      return;
    }
    fs_write(f, (const char *)(wget_buf + body_off), body_len);
    /* downloaded ELF binaries are runnable as-is */
    if (body_len >= 4 && !memcmp(wget_buf + body_off, "\x7f" "ELF", 4))
      f->mode = 0755;
    vga_printf("wget: wrote %d bytes to %s\n", body_len, outname);
  } else {
    /* print body (up to 4KB) for quick test */
    int show = body_len < 4096 ? body_len : 4096;
    for (int i = 0; i < show; i++) {
      char c = (char)wget_buf[body_off + i];
      if (c == '\r')
        continue;
      if (c == '\n' || (c >= 0x20 && c < 0x7F))
        vga_putc(c);
      else
        vga_putc('.');
    }
    if (body_len > show)
      vga_printf("\n[...%d more bytes truncated]\n", body_len - show);
    else
      vga_putc('\n');
  }
}
