#include "proc/userland.h"
#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/bootmod.h"
#include "core/vmm.h"

/* userland/busybox and its applet list, linked in by objcopy (Makefile). */
extern const char _binary_userland_busybox_start[], _binary_userland_busybox_end[];
extern const char _binary_userland_busybox_applets_start[], _binary_userland_busybox_applets_end[];

extern const char _binary_userland_fm_start[], _binary_userland_fm_end[];
extern const char _binary_userland_samara_samara_fm_conf_start[];
extern const char _binary_userland_samara_samara_fm_conf_end[];
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

/* boot-made files point at the kernel's own string: to the ext2 root disk
   they look like sysroot files and don't get saved until somebody edits
   them. before, every /etc file and xsamara went to disk once and that
   stale copy won over the kernel's newer one forever */
static fs_node_t* put_static(const char* path, const char* text, uint16_t mode) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (!n) n = fs_create(fs_root(), path, FS_FILE);
    if (!n) return NULL;
    fs_set_static(n, text, strlen(text));
    n->mode = mode;
    return n;
}

static void put_text(const char* path, const char* text) {
    put_static(path, text, 0644);
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

    /* file manager, x86_64 static (make fm) */
    mkdir_p("/etc");
    fs_unlink(fs_root(), "/usr/bin/fm");   // tar has an old one, create would fail
    fs_node_t* fm = fs_create(fs_root(), "/usr/bin/fm", FS_FILE);
    if (fm) { fs_set_static(fm, _binary_userland_fm_start, _binary_userland_fm_end - _binary_userland_fm_start); fm->mode = 0755; }
    fs_unlink(fs_root(), "/etc/samara-fm.conf");
    fs_node_t* fc = fs_create(fs_root(), "/etc/samara-fm.conf", FS_FILE);
    if (fc) fs_set_static(fc, _binary_userland_samara_samara_fm_conf_start,
                          _binary_userland_samara_samara_fm_conf_end - _binary_userland_samara_samara_fm_conf_start);

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
             "telnetd -l /bin/login -p 23\n"
             "# runit (xbps-install runit runit-void) supervises the rest, sv status/stop/start\n"
             "if [ -x /usr/bin/runsvdir ]; then\n"
             "    SV=/etc/runit/runsvdir/default\n"
             "    mkdir -p /run/lock /run/runit /var/log/socklog $SV /etc/sv/dropbear\n"
             "    rm -f $SV/agetty-*\n"
             "    printf '#!/bin/sh\\nexec dropbear -F -B -r %s -p 0.0.0.0:22 2>&1\\n' $K > /etc/sv/dropbear/run\n"
             "    chmod 755 /etc/sv/dropbear/run\n"
             "    for s in dropbear crond; do [ -d /etc/sv/$s ] && ln -sf /etc/sv/$s $SV/; done\n"
             "    mkdir -p /run/runit/runsvdir\n"
             "    ln -sfn $SV /run/runit/runsvdir/current\n"
             "    exec runsvdir -P /run/runit/runsvdir/current\n"
             "fi\n"
             "dropbear -B -r $K -p 0.0.0.0:22\n");
    fs_node_t* rc = fs_resolve(fs_root(), "/etc/rc");
    if (rc) rc->mode = 0755;
    put_text("/etc/group", "root:x:0:\n");
    put_text("/etc/fstab", "none / ramfs rw 0 0\n");   /* btop stats it */
    if (!fs_resolve_nf(fs_root(), "/etc/mtab")) fs_symlink(fs_resolve(fs_root(), "/etc"), "mtab", "/proc/mounts");
    /* X11 on the framebuffer, once you apk add xorg-server & co. evdev on our
       two input devices, no udev, no vt switching */
    mkdir_p("/etc/X11");
    put_text("/etc/X11/xorg.conf",
             "Section \"ServerFlags\"\n    Option \"AutoAddDevices\" \"false\"\n"
             "    Option \"AutoEnableDevices\" \"false\"\n    Option \"DontVTSwitch\" \"true\"\nEndSection\n"
             "Section \"Device\"\n    Identifier \"fb\"\n    Driver \"fbdev\"\n    Option \"fbdev\" \"/dev/fb0\"\nEndSection\n"
             "Section \"Screen\"\n    Identifier \"s\"\n    Device \"fb\"\nEndSection\n"
             "Section \"InputDevice\"\n    Identifier \"kbd\"\n    Driver \"evdev\"\n    Option \"Device\" \"/dev/input-kbd\"\n"
             "    Option \"XkbLayout\" \"us,ru\"\n    Option \"XkbOptions\" \"grp:alt_shift_toggle\"\nEndSection\n"   /* alt+shift like the rest of the os */
             "Section \"InputDevice\"\n    Identifier \"mouse\"\n    Driver \"evdev\"\n    Option \"Device\" \"/dev/input-mouse\"\nEndSection\n"
             "Section \"ServerLayout\"\n    Identifier \"l\"\n    Screen \"s\"\n"
             "    InputDevice \"kbd\" \"CoreKeyboard\"\n    InputDevice \"mouse\" \"CorePointer\"\nEndSection\n");
    {
        const char* xs =
            "#!/bin/sh\n"
            "# X on the framebuffer. Ctrl+Alt+Backspace gets the keyboard back if it hangs\n"
            "if ! command -v Xorg >/dev/null; then\n"
            "    echo 'no Xorg here. apk add xorg-server xf86-video-fbdev xf86-input-evdev xterm twm xsetroot font-misc-misc font-cursor-misc mesa-dri-gallium llvm17-libs mesa-demos'\n"
            "    exit 1\n"
            "fi\n"
            "D=${XDISPLAY:-0}\n"
            "Xorg :$D -nolisten tcp -keeptty -novtswitch vt1 -logfile /tmp/Xorg.$D.log >/dev/null 2>&1 &\n"
            "X=$!\n"
            "i=0\n"
            "while [ ! -S /tmp/.X11-unix/X$D ] && [ $i -lt 150 ]; do sleep 0.1; i=$((i+1)); done\n"
            "export DISPLAY=:$D\n"
            "# grey root and an arrow, the default black X on black is invisible.\n"
            "# right after the socket shows up X still paints the root black over it\n"
            "command -v xsetroot >/dev/null && (sleep 3; xsetroot -solid '#3b3f4a' -cursor_name left_ptr) &\n"
            "# their chatter would land on the console, which draws right over X\n"
            "if [ -x \"$HOME/.xinitrc\" ]; then \"$HOME/.xinitrc\" >/tmp/xsamara.log 2>&1\n"
            "else twm >/tmp/xsamara.log 2>&1 & xterm -geometry 80x24+20+20 >>/tmp/xsamara.log 2>&1; fi\n"
            "kill $X 2>/dev/null; wait $X 2>/dev/null\n";
        put_static("/usr/bin/xsamara", xs, 0755);
    }
    /* a bit of /sys: xorg's fbdevhw readlinks /sys/class/graphics/fb0 and
       refuses anything that isn't there or sits on pci */
    mkdir_p("/sys/class/graphics");
    mkdir_p("/sys/devices/platform/vesa-framebuffer.0/graphics/fb0");
    if (!fs_resolve_nf(fs_root(), "/sys/class/graphics/fb0"))
        fs_symlink(fs_resolve(fs_root(), "/sys/class/graphics"), "fb0", "../../devices/platform/vesa-framebuffer.0/graphics/fb0");
    put_text("/etc/hosts", "127.0.0.1\tlocalhost\n10.0.2.15\tsamara\n10.0.2.2\thost gateway\n");
    put_text("/etc/resolv.conf", "nameserver 10.0.2.3\nnameserver 1.1.1.1\n");
    put_text("/etc/services", "http\t80/tcp\nhttps\t443/tcp\ndomain\t53/udp\n");
    put_text("/etc/shells", "/bin/sh\n/bin/ash\n/usr/bin/bash\n");
    put_text("/etc/motd", "");                    /* login shells show samarafetch instead */
    /* every interactive sh ($ENV): gradient prompt, colours */
    put_text("/etc/shrc",
             "case $- in *i*) ;; *) return 0 2>/dev/null ;; esac\n"
             "export COLORTERM=truecolor\n"
             "PS1=\"$(samarafetch --ps1 2>/dev/null || echo '\\u@\\h:\\w\\$ ')\"\n"
             "alias ls='ls --color=auto' ll='ls -l --color=auto' la='ls -la --color=auto'\n"
             "alias grep='grep --color=auto'\n"
             "if [ -x /usr/local/bin/python3 ]; then alias python=python3; else alias python=micropython python3=micropython; fi\n");
    /* login shells (ssh, telnet): the logo + system info, then the above */
    put_text("/etc/profile",
             "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/games:/usr/local/bin:/opt/gcc/bin\n"
             "export ENV=/etc/shrc LANG=C.UTF-8\n"
             "case $- in *i*) samarafetch 2>/dev/null ;; esac\n"
             ". /etc/shrc\n");
    /* zsh from apk got our bash style PS1 from the env and printed \u@\h raw */
    const char* zrc = "PROMPT='%F{magenta}%n@%m%f:%F{blue}%~%f%# '\n"
                      "alias ls='ls --color=auto' ll='ls -l --color=auto' la='ls -la --color=auto'\n";
    put_text("/root/.zshrc", zrc);
    put_text("/home/user/.zshrc", zrc);
    return count;
}
