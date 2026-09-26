#include "proc/userland.h"
#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/bootmod.h"
#include "core/vmm.h"

/* userland/busybox and its applet list, linked in by objcopy (Makefile). */
extern const char _binary_userland_busybox_start[], _binary_userland_busybox_end[];
extern const char _binary_userland_busybox_applets_start[], _binary_userland_busybox_applets_end[];

extern const char _binary_userland_sysroot_tar_start[], _binary_userland_sysroot_tar_end[];


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

static int tar_path(const char* h, char* path, int cap) {
    int n = 0;
    path[n++] = '/';
    if (h[345]) {                                       /* ustar prefix */
        for (int i = 0; i < 155 && h[345 + i] && n < cap - 102; i++) path[n++] = h[345 + i];
        path[n++] = '/';
    }
    for (int i = 0; i < 100 && h[i] && n < cap - 1; i++) path[n++] = h[i];
    while (n > 1 && path[n - 1] == '/') n--;
    path[n] = 0;
    return n;
}

/* A link entry: hard links name the target from the archive root, symlinks
   relative to their own directory. The ramfs has no links, so the new node
   shares the target's contents (zero-copy when those are borrowed). */
static bool tar_link(const char* h, const char* path, bool zero_copy) {
    char tgt[256];
    int n = 0;
    const char* ln = h + 157;
    if (h[156] == '2' && ln[0] != '/') {
        int cut = (int)strlen(path);
        while (cut > 0 && path[cut - 1] != '/') cut--;
        for (int i = 0; i < cut && n < 200; i++) tgt[n++] = path[i];
    } else if (ln[0] != '/') {
        tgt[n++] = '/';
    }
    for (int i = 0; i < 100 && ln[i] && n < 254; i++) tgt[n++] = ln[i];
    tgt[n] = 0;
    fs_node_t* t = fs_resolve(fs_root(), tgt);
    if (!t || t->type != FS_FILE) return false;          /* dir symlinks: not representable */
    fs_node_t* d = fs_resolve(fs_root(), path);
    if (!d) d = fs_create(fs_root(), path, FS_FILE);
    if (!d || d->type != FS_FILE) return false;
    if (zero_copy && t->data && !t->cap) fs_set_static(d, t->data, t->size);
    else fs_write(d, t->data ? t->data : "", t->size);
    d->mode = t->mode;
    return true;
}

/* Unpack a ustar archive into the ramfs root. zero_copy: file nodes point
   straight into the archive (which must stay put: kernel image or a boot
   module). Links are resolved in later passes, once their targets exist. */
static int untar_ex(const char* start, const char* end, bool zero_copy) {
    int files = 0, pending = 0;
    for (const char* p = start; p + 512 <= end;) {
        const char* h = p;
        if (!h[0]) break;                                   /* end-of-archive block */
        char path[256];
        tar_path(h, path, sizeof(path));
        uint32_t size = octal(h + 124, 12);
        uint16_t mode = (uint16_t)(octal(h + 100, 8) & 07777);
        char type = h[156];
        p += 512;
        if (type == '5') {
            mkdir_p(path);
        } else if (type == '0' || type == 0) {
            if (p + size > end) break;
            if (zero_copy) {
                fs_node_t* n = fs_resolve(fs_root(), path);
                if (!n) n = fs_create(fs_root(), path, FS_FILE);
                if (n && n->type == FS_FILE) {
                    fs_set_static(n, p, size);
                    n->mode = mode ? mode : 0644;
                    files++;
                }
            } else if (put_file(path, p, size, mode ? mode : 0644)) {
                files++;
            }
        } else if (type == '1' || type == '2') {
            pending++;
        }
        if (type == '1' || type == '2') size = 0;
        p += (size + 511) & ~511u;
    }
    for (int pass = 0; pass < 4 && pending; pass++) {
        int left = 0;
        for (const char* p = start; p + 512 <= end;) {
            const char* h = p;
            if (!h[0]) break;
            char type = h[156];
            uint32_t size = (type == '1' || type == '2') ? 0 : octal(h + 124, 12);
            p += 512 + ((size + 511) & ~511u);
            if (type != '1' && type != '2') continue;
            char path[256];
            tar_path(h, path, sizeof(path));
            fs_node_t* d = fs_resolve(fs_root(), path);
            if (d && d->type == FS_FILE && d->data) continue;  /* done in an earlier pass */
            if (tar_link(h, path, zero_copy)) files++;
            else left++;
        }
        if (left == pending) break;                       /* dangling / dir links */
        pending = left;
    }
    return files;
}

static int untar(const char* p, const char* end) { return untar_ex(p, end, true); }

/* Boot modules (QEMU -initrd): ustar archives, e.g. the gcc toolchain. */
int userland_install_modules(void) {
    const bootmod_t* m;
    int n = boot_modules(&m), files = 0;
    for (int i = 0; i < n; i++)
        files += untar_ex((const char*)P2V(m[i].start), (const char*)P2V(m[i].end), true);
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
    fs_node_t* bb = fs_resolve(fs_root(), "/bin/busybox");
    if (!bb) bb = fs_create(fs_root(), "/bin/busybox", FS_FILE);
    if (!bb) return -1;
    fs_set_static(bb, _binary_userland_busybox_start, size);   /* straight from the kernel image */
    bb->mode = 0755;

    /* Every applet is busybox itself - the same bytes, like hard links - so
       it dispatches on argv[0] as execve() was given it. That keeps login
       shells ("-sh" from sshd, telnetd, login) working, which a
       "#!/bin/busybox" stub would turn into plain "busybox /bin/sh". */
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
        fs_node_t* an = fs_create(fs_root(), path, FS_FILE);
        if (an) { fs_set_static(an, _binary_userland_busybox_start, size); an->mode = 0755; count++; }
    }

    /* TCC + musl headers/libc (userland/sysroot.tar, see build-sysroot.sh). */
    untar(_binary_userland_sysroot_tar_start, _binary_userland_sysroot_tar_end);

    put_text("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n");
    /* root has no password: logins over ssh/telnet only reach it through
       QEMU's port forwards on the host's loopback (see Makefile NET_DRIVE). */
    put_text("/etc/shadow", "root::19000:0:99999:7:::\n");
    mkdir_p("/etc/dropbear");
    put_text("/etc/rc",
             "#!/bin/sh\n"
             "# SamaraOS services, started in the background at boot (not with\n"
             "# 'noservices' on the kernel command line).\n"
             "K=/etc/dropbear/dropbear_ed25519_host_key\n"
             "# keep the ssh host key on the disk so it survives reboots\n"
             "if grep -q ' /mnt ' /proc/mounts; then\n"
             "    mkdir -p /mnt/etc/dropbear\n"
             "    [ -f /mnt$K ] || dropbearkey -t ed25519 -f /mnt$K >/dev/null 2>&1\n"
             "    cp /mnt$K $K 2>/dev/null\n"
             "fi\n"
             "[ -f $K ] || dropbearkey -t ed25519 -f $K >/dev/null 2>&1\n"
             "# ssh client key (ssh user@10.0.2.2 without a password): kept on the\n"
             "# disk, installed for both homes (the SamaraOS shell uses /home/user)\n"
             "C=/mnt/etc/dropbear/id_client\n"
             "if grep -q ' /mnt ' /proc/mounts; then\n"
             "    [ -f $C ] || dropbearkey -t ed25519 -f $C >/dev/null 2>&1\n"
             "    for h in /root /home/user; do mkdir -p $h/.ssh; cp $C $h/.ssh/id_dropbear; done\n"
             "fi\n"
             "dropbear -B -r $K -p 22\n"
             "telnetd -l /bin/login -p 23\n");
    fs_node_t* rc = fs_resolve(fs_root(), "/etc/rc");
    if (rc) rc->mode = 0755;
    put_text("/etc/group", "root:x:0:\n");
    put_text("/etc/hosts", "127.0.0.1\tlocalhost\n10.0.2.15\tsamara\n10.0.2.2\thost gateway\n");
    put_text("/etc/resolv.conf", "nameserver 10.0.2.3\nnameserver 1.1.1.1\n");
    put_text("/etc/services", "http\t80/tcp\nhttps\t443/tcp\ndomain\t53/udp\n");
    put_text("/etc/shells", "/bin/sh\n/bin/ash\n");
    put_text("/etc/motd", "");                    /* login shells show samarafetch instead */
    /* every interactive sh ($ENV): gradient prompt, colours */
    put_text("/etc/shrc",
             "case $- in *i*) ;; *) return 0 2>/dev/null ;; esac\n"
             "export COLORTERM=truecolor\n"
             "PS1=\"$(samarafetch --ps1 2>/dev/null || echo '\\u@\\h:\\w\\$ ')\"\n"
             "alias ls='ls --color=auto' ll='ls -l --color=auto' la='ls -la --color=auto'\n"
             "alias grep='grep --color=auto'\n"
             "alias python=micropython python3=micropython\n");
    /* login shells (ssh, telnet): the logo + system info, then the above */
    put_text("/etc/profile",
             "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/games:/opt/gcc/bin\n"
             "export ENV=/etc/shrc\n"
             "case $- in *i*) samarafetch 2>/dev/null ;; esac\n"
             ". /etc/shrc\n");
    return count;
}
