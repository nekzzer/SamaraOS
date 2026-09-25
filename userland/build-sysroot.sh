#!/bin/sh
# Builds userland/sysroot.tar: TCC + musl (headers, crt, libc.a) laid out as
# /usr/... inside SamaraOS. The kernel unpacks it into the ramfs at boot.
#
# Needs: toolchain/musl/i686-linux-musl-cross (https://musl.cc), musl 1.2.4
# built non-PIC in toolchain/musl-1.2.4 (see below), and a tcc
# 0.9.27 tree in toolchain/tcc-0.9.27 built with:
#   ./configure --prefix=/usr --cc=<musl-gcc> --ar=<musl-ar> --cpu=i386 \
#     --config-musl --extra-cflags="-O2 -fno-pie -DCONFIG_TCC_STATIC" \
#     --extra-ldflags="-static -no-pie -s" \
#     --sysincludepaths=/usr/lib/tcc/include:/usr/include \
#     --libpaths=/usr/lib/tcc:/usr/lib --crtprefix=/usr/lib
#   make tcc libtcc1.a
# (libtcc.c patched so static_link defaults to 1.)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MUSL=$ROOT/toolchain/musl/i686-linux-musl-cross/i686-linux-musl
TCC=$ROOT/toolchain/tcc-0.9.27
OUT=$ROOT/build/sysroot
rm -rf "$OUT"
mkdir -p "$OUT/usr/bin" "$OUT/usr/lib/tcc/include" "$OUT/usr/include" "$OUT/usr/src"

cp "$TCC/tcc" "$OUT/usr/bin/tcc"
cp "$TCC/libtcc1.a" "$OUT/usr/lib/tcc/"
# libc.a (gcc-built) calls two 64-bit division helpers from libgcc that
# libtcc1 lacks. Those two members are position-dependent; the rest of
# musl.cc's libgcc is PIC, which tcc 0.9.27 cannot link statically.
XBIN=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
LIBGCC=$("$XBIN/i686-linux-musl-gcc" -print-libgcc-file-name)
TMP=$(mktemp -d)
(cd "$TMP" && "$XBIN/i686-linux-musl-ar" x "$LIBGCC" _divmoddi4.o _udivmoddi4.o &&
 "$XBIN/i686-linux-musl-strip" -g ./*.o &&
 "$XBIN/i686-linux-musl-ar" q "$OUT/usr/lib/tcc/libtcc1.a" ./*.o && "$XBIN/i686-linux-musl-ranlib" "$OUT/usr/lib/tcc/libtcc1.a")
rm -rf "$TMP"
cp "$TCC"/include/*.h "$OUT/usr/lib/tcc/include/"
# musl built from source WITHOUT -fPIC (musl.cc's libc.a is PIC and tcc
# 0.9.27 leaves its PLT/GOT unrelocated in static links):
#   cd toolchain/musl-1.2.4 && CC=<musl-gcc> CFLAGS="-O2 -fno-pic -fno-pie -g0" \
#     ./configure --target=i386-linux-musl --prefix=/usr --disable-shared &&
#   make lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o
MUSLSRC=$ROOT/toolchain/musl-1.2.4
for f in crt1.o crti.o crtn.o libc.a; do cp "$MUSLSRC/lib/$f" "$OUT/usr/lib/"; done

cd "$MUSL/include"
cp ./*.h "$OUT/usr/include/"
for d in bits sys arpa net netinet netpacket; do cp -r "$d" "$OUT/usr/include/"; done

cat > "$OUT/usr/src/hello.c" <<'C'
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("Hello from TCC on SamaraOS! pid=%d\n", getpid());
    for (int i = 1; i < argc; i++) printf("  arg %d: %s\n", i, argv[i]);
    return 0;
}
C

cat > "$OUT/usr/src/demo.c" <<'C'
/* Exercises malloc, qsort, stdio files, 64-bit math and floating point. */
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    int n = 20000, *v = malloc(n * sizeof *v);
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        v[i] = (i * 7919) % 10007;
        h = (h ^ (unsigned)v[i]) * 1099511628211ULL;
    }
    qsort(v, n, sizeof *v, cmp);
    FILE *f = fopen("demo.out", "w");
    fprintf(f, "min=%d max=%d hash=%llu div=%llu\n", v[0], v[n - 1], h, h / 12345);
    fclose(f);
    char buf[128];
    f = fopen("demo.out", "r");
    fgets(buf, sizeof buf, f);
    fclose(f);
    printf("%s", buf);
    printf("pi ~ %.6f\n", 355.0 / 113.0);
    free(v);
    return 0;
}
C

cat > "$OUT/usr/src/sigtest.c" <<'C'
/* Signal delivery: sync + async handlers, SIGCHLD, sigsuspend, EINTR. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static volatile sig_atomic_t usr1, spin_hit, chld;
static void on_usr1(int s) { usr1++; }
static void on_int(int s) { spin_hit = 1; }
static void on_chld(int s, siginfo_t *si, void *uc) { chld++; }

int main(void) {
    signal(SIGUSR1, on_usr1);
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR1);
    printf("1. self SIGUSR1 x2 -> handler ran %d times\n", usr1);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_chld;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGCHLD, &sa, 0);
    signal(SIGINT, on_int);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c == 0) {                       /* child: poke the busy parent, then exit */
        usleep(100000);
        kill(parent, SIGINT);
        _exit(7);
    }
    double x = 1.0;
    while (!spin_hit) x = x * 1.0000001;  /* pure ring-3 loop: async delivery */
    printf("2. SIGINT interrupted a busy loop (fpu intact: %s)\n", x > 1.0 ? "yes" : "no");
    int st;
    waitpid(c, &st, 0);
    printf("3. child exit=%d, SIGCHLD handler ran %d time(s)\n", WEXITSTATUS(st), chld);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    if (fork() == 0) { usleep(50000); kill(getppid(), SIGUSR1); _exit(0); }
    usr1 = 0;
    sigsuspend(&old);                   /* atomically unblock + wait */
    printf("4. sigsuspend woke by SIGUSR1: %s\n", usr1 ? "yes" : "no");
    wait(0);

    int fds[2];
    pipe(fds);
    signal(SIGUSR1, on_usr1);           /* no SA_RESTART with signal()? musl uses it */
    sa.sa_handler = on_usr1; sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, 0);
    sigprocmask(SIG_SETMASK, &old, 0);
    if (fork() == 0) { usleep(50000); kill(getppid(), SIGUSR1); _exit(0); }
    char b;
    int r = read(fds[0], &b, 1);
    printf("5. blocking read interrupted: r=%d errno=%s\n", r, r < 0 && errno == EINTR ? "EINTR" : "?");
    wait(0);
    return 0;
}
C

cat > "$OUT/usr/src/net.c" <<'C'
/* TCP echo over loopback + an HTTP GET: sockets from C.
   usage: net                 (self-test on 127.0.0.1)
          net <host> [path] [port]   (HTTP GET, e.g. net example.org /) */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int selftest(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7777) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { perror("listen"); return 1; }
    if (fork() == 0) {                       /* client */
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof a)) { perror("connect"); _exit(1); }
        const char *msg = "ping over TCP loopback";
        write(c, msg, strlen(msg));
        char buf[64] = {0};
        int n = read(c, buf, sizeof buf - 1);
        printf("client got %d bytes: %s\n", n, buf);
        close(c);
        _exit(0);
    }
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    int s = accept(ls, (struct sockaddr *)&peer, &pl);
    char buf[64] = {0};
    int n = read(s, buf, sizeof buf - 1);
    printf("server accepted %s:%d, got \"%s\"\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), buf);
    for (int i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    write(s, buf, n);
    close(s);
    wait(0);
    close(ls);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return selftest();
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    int e = getaddrinfo(argv[1], argc > 3 ? argv[3] : "80", &hints, &ai);
    if (e) { fprintf(stderr, "resolve %s: %s\n", argv[1], gai_strerror(e)); return 1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, ai->ai_addr, ai->ai_addrlen)) { perror("connect"); return 1; }
    char req[256];
    int len = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", argc > 2 ? argv[2] : "/", argv[1]);
    write(s, req, len);
    char buf[1024];
    int n;
    while ((n = read(s, buf, sizeof buf)) > 0) fwrite(buf, 1, n, stdout);
    close(s);
    return 0;
}
C

# GNU nano (static, ncurses with a built-in "linux" terminfo entry), built by
# userland/build-nano.sh, plus a few syntax definitions.
NANO=$ROOT/toolchain/nano-7.2
if [ -x "$NANO/src/nano" ]; then
    cp "$NANO/src/nano" "$OUT/usr/bin/nano"
    mkdir -p "$OUT/usr/share/nano" "$OUT/etc"
    for f in c sh makefile markdown nanorc default; do
        [ -f "$NANO/syntax/$f.nanorc" ] && cp "$NANO/syntax/$f.nanorc" "$OUT/usr/share/nano/"
    done
    cat > "$OUT/etc/nanorc" <<'RC'
include "/usr/share/nano/*.nanorc"
set autoindent
set tabsize 4
set constantshow
RC
fi

# MicroPython (static, built by userland/build-micropython.sh) + demos.
MPY=$ROOT/toolchain/micropython/ports/unix/build-standard/micropython
if [ -x "$MPY" ]; then
    cp "$MPY" "$OUT/usr/bin/micropython"
    mkdir -p "$OUT/usr/src/py"
    cat > "$OUT/usr/src/py/hello.py" <<'PY'
import sys, os, time
print("Hello from MicroPython on SamaraOS!")
print("version:", sys.version)
print("pid:", os.getpid(), " cwd:", os.getcwd())
print("args:", sys.argv[1:])
print("files in /usr/src:", os.listdir("/usr/src"))
t = time.ticks_ms()
print("primes < 2000:", len([n for n in range(2, 2000) if all(n % d for d in range(2, int(n ** 0.5) + 1))]))
print("took", time.ticks_diff(time.ticks_ms(), t), "ms;  2**100 =", 2 ** 100)
PY
    cat > "$OUT/usr/src/py/files.py" <<'PY'
# File I/O + json round-trip in the ramfs (use /mnt/... to persist on disk).
import json
data = {"os": "SamaraOS", "lang": "MicroPython", "nums": [1, 2, 3], "pi": 3.14159}
with open("/tmp/data.json", "w") as f:
    json.dump(data, f)
with open("/tmp/data.json") as f:
    back = json.load(f)
print("wrote and read back:", back)
print("equal:", back == data)
PY
    cat > "$OUT/usr/src/py/http.py" <<'PY'
# HTTP GET over the SamaraOS TCP stack.
# usage: micropython http.py [host] [path] [port]   (default: host:8080 /, see serve.py)
import sys, socket
host = sys.argv[1] if len(sys.argv) > 1 else "host"
path = sys.argv[2] if len(sys.argv) > 2 else "/"
port = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
addr = socket.getaddrinfo(host, port)[0][-1]
s = socket.socket()
s.connect(addr)
s.send(b"GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path.encode(), host.encode()))
while True:
    chunk = s.recv(1024)
    if not chunk:
        break
    sys.stdout.buffer.write(chunk)
s.close()
PY
fi

# samara.h (desktop windows from C) + demos and games. tetris is prebuilt
# with musl-gcc so the desktop icon works out of the box; the same source
# builds inside the OS: tcc /usr/src/games/tetris.c -o /usr/bin/tetris
SAM=$ROOT/userland/samara
mkdir -p "$OUT/usr/src/samara" "$OUT/usr/src/games" "$OUT/usr/games"
cp "$SAM/samara.h" "$OUT/usr/include/samara.h"
cp "$SAM/hello.c" "$SAM/hello.py" "$OUT/usr/src/samara/"
cp "$SAM/tetris.c" "$SAM/breakout.py" "$OUT/usr/src/games/"
cp "$SAM/breakout.py" "$OUT/usr/games/breakout.py"
"$XBIN/i686-linux-musl-gcc" -static -no-pie -O2 -s -I"$SAM" "$SAM/tetris.c" -o "$OUT/usr/games/tetris"

cd "$OUT"
tar --format=ustar --owner=0 --group=0 -cf "$ROOT/userland/sysroot.tar" usr $( [ -d etc ] && echo etc )
ls -la "$ROOT/userland/sysroot.tar"
