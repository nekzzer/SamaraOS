#include "proc/userland.h"
#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"

/* userland/busybox and its applet list, linked in by objcopy (Makefile). */
extern const char _binary_userland_busybox_start[], _binary_userland_busybox_end[];
extern const char _binary_userland_busybox_applets_start[], _binary_userland_busybox_applets_end[];

extern const char _binary_userland_sysroot_tar_start[], _binary_userland_sysroot_tar_end[];

static const char applet_stub[] = "#!/bin/busybox\n";

static fs_node_t* mkdir_p(const char* path) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (n) return n;
    char part[128];
    int len = 0;
    for (const char* p = path; ; p++) {
        if (*p == '/' || !*p) {
            part[len] = 0;
            if (len > 1 && !fs_resolve(fs_root(), part)) fs_create(fs_root(), part, FS_DIR);
            if (!*p) break;
        }
        if (len < (int)sizeof(part) - 1) part[len++] = *p;
    }
    return fs_resolve(fs_root(), path);
}

static fs_node_t* put_file(const char* path, const char* data, uint32_t len, uint16_t mode) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (!n) n = fs_create(fs_root(), path, FS_FILE);
    if (!n) return NULL;
    fs_write(n, data, len);
    n->mode = mode;
    return n;
}

static void put_text(const char* path, const char* text) {
    put_file(path, text, strlen(text), 0644);
}

static uint32_t octal(const char* s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (uint32_t)(s[i] - '0');
    }
    return v;
}

/* Unpack a ustar archive into the ramfs root (dirs + regular files). */
static int untar(const char* p, const char* end) {
    int files = 0;
    while (p + 512 <= end) {
        const char* h = p;
        if (!h[0]) break;                                   /* end-of-archive block */
        char path[256];
        int n = 0;
        path[n++] = '/';
        if (h[345]) {                                       /* ustar prefix */
            for (int i = 0; i < 155 && h[345 + i] && n < 200; i++) path[n++] = h[345 + i];
            path[n++] = '/';
        }
        for (int i = 0; i < 100 && h[i] && n < 254; i++) path[n++] = h[i];
        while (n > 1 && path[n - 1] == '/') n--;
        path[n] = 0;
        uint32_t size = octal(h + 124, 12);
        uint16_t mode = (uint16_t)(octal(h + 100, 8) & 07777);
        char type = h[156];
        p += 512;
        if (type == '5') {
            mkdir_p(path);
        } else if (type == '0' || type == 0) {
            if (put_file(path, p, size, mode ? mode : 0644)) files++;
        }
        p += (size + 511) & ~511u;
    }
    return files;
}

int userland_install(void) {
    uint32_t size = (uint32_t)(_binary_userland_busybox_end - _binary_userland_busybox_start);
    mkdir_p("/bin");
    mkdir_p("/sbin");
    mkdir_p("/usr/bin");
    mkdir_p("/usr/sbin");
    mkdir_p("/root");
    mkdir_p("/proc");
    mkdir_p("/var/log");
    mkdir_p("/mnt");
    mkdir_p("/tmp");
    mkdir_p("/home/user");
    if (!put_file("/bin/busybox", _binary_userland_busybox_start, size, 0755)) return -1;

    /* One "#!/bin/busybox" stub per applet: execve follows the shebang and
       busybox dispatches on the path it is handed ("busybox /bin/ls ..."). */
    int count = 0;
    const char* p = _binary_userland_busybox_applets_start;
    const char* end = _binary_userland_busybox_applets_end;
    while (p < end) {
        char path[96];
        int n = 0;
        path[n++] = '/';
        while (p < end && *p != '\n' && n < (int)sizeof(path) - 1) path[n++] = *p++;
        path[n] = 0;
        while (p < end && *p != '\n') p++;
        p++;
        if (n <= 1 || !strcmp(path, "/linuxrc") || !strcmp(path, "/bin/busybox")) continue;
        if (fs_resolve(fs_root(), path)) continue;
        if (put_file(path, applet_stub, sizeof(applet_stub) - 1, 0755)) count++;
    }

    /* TCC + musl headers/libc (userland/sysroot.tar, see build-sysroot.sh). */
    untar(_binary_userland_sysroot_tar_start, _binary_userland_sysroot_tar_end);

    put_text("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n");
    put_text("/etc/group", "root:x:0:\n");
    put_text("/etc/hosts", "127.0.0.1\tlocalhost\n10.0.2.15\tsamara\n10.0.2.2\thost gateway\n");
    put_text("/etc/resolv.conf", "nameserver 10.0.2.3\nnameserver 1.1.1.1\n");
    put_text("/etc/services", "http\t80/tcp\nhttps\t443/tcp\ndomain\t53/udp\n");
    put_text("/etc/shells", "/bin/sh\n/bin/ash\n");
    put_text("/etc/motd", "BusyBox on SamaraOS. Type 'exit' to return to the SamaraOS shell.\n");
    put_text("/etc/profile",
             "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/games\n"
             "export PS1='\\u@\\h:\\w\\$ '\n"
             "alias ll='ls -l'\n"
             "alias python=micropython python3=micropython\n");
    return count;
}
