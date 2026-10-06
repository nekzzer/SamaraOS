// guest-side checks for make test: tt <name>. prints "ok ..." and exits 0, or a reason and exits 1
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/inotify.h>

static int fail(const char* m) { printf("FAIL %s (errno %d)\n", m, errno); exit(1); }
#define CK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); exit(1); } } while (0)

static long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static void fill(char* b, int n, int seed) { for (int i = 0; i < n; i++) b[i] = (char)(seed * 31 + i * 7 + (i >> 8)); }

static int fileops(void) {
    char p[64], buf[8192], rb[8192];
    mkdir("/root/ft", 0755);
    for (int d = 0; d < 20; d++) {
        snprintf(p, sizeof p, "/root/ft/d%d", d);
        CK(mkdir(p, 0755) == 0);
        for (int i = 0; i < 15; i++) {
            snprintf(p, sizeof p, "/root/ft/d%d/f%d", d, i);
            int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0644);
            CK(fd >= 0);
            int n = 100 + d * 37 + i * 211;
            fill(buf, n, d * 16 + i);
            CK(write(fd, buf, n) == n);
            close(fd);
        }
    }
    // one big file written in odd sized chunks, then read back
    int fd = open("/root/ft/big", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CK(fd >= 0);
    for (int i = 0; i < 256; i++) {
        fill(buf, 4096 + i, i);
        long w = write(fd, buf, 4096 + i);
        if (w != 4096 + i) { printf("FAIL big file chunk %d: write %ld of %d (errno %d)\n", i, w, 4096 + i, errno); return 1; }
    }
    CK(lseek(fd, 0, SEEK_SET) == 0);
    for (int i = 0; i < 256; i++) {
        fill(buf, 4096 + i, i);
        CK(read(fd, rb, 4096 + i) == 4096 + i);
        CK(!memcmp(buf, rb, 4096 + i));
    }
    CK(ftruncate(fd, 100000) == 0);
    struct stat st;
    CK(fstat(fd, &st) == 0 && st.st_size == 100000);
    CK(pwrite(fd, "abc", 3, 99999) == 3);
    CK(pread(fd, rb, 3, 99999) == 3 && !memcmp(rb, "abc", 3));
    close(fd);
    // rename / link / symlink / unlink
    CK(rename("/root/ft/d1/f1", "/root/ft/d2/moved") == 0);
    CK(link("/root/ft/d2/moved", "/root/ft/d2/hard") == 0);
    CK(symlink("moved", "/root/ft/d2/sym") == 0);
    CK(stat("/root/ft/d2/sym", &st) == 0 && st.st_nlink == 2);
    for (int d = 0; d < 20; d += 2)
        for (int i = 0; i < 15; i += 2) {
            snprintf(p, sizeof p, "/root/ft/d%d/f%d", d, i);
            if (unlink(p)) fail(p);
        }
    // survivors still hold their data
    for (int d = 3; d < 20; d += 2) {
        snprintf(p, sizeof p, "/root/ft/d%d/f%d", d, 3);
        int n = 100 + d * 37 + 3 * 211;
        fd = open(p, O_RDONLY);
        CK(fd >= 0 && read(fd, rb, n) == n);
        fill(buf, n, d * 16 + 3);
        CK(!memcmp(buf, rb, n));
        close(fd);
    }
    DIR* dd = opendir("/root/ft/d5");
    CK(dd);
    int cnt = 0;
    struct dirent* e;
    while ((e = readdir(dd))) cnt++;
    closedir(dd);
    CK(cnt == 17);
    printf("ok 300 files + big\n");
    return 0;
}

static int forkstorm(void) {
    for (int r = 0; r < 25; r++) {
        pid_t k[12];
        for (int i = 0; i < 12; i++) {
            k[i] = fork();
            CK(k[i] >= 0);
            if (!k[i]) {
                if (i & 1) { char* a[] = { "/bin/true", NULL }; execv("/bin/true", a); _exit(99); }
                _exit(i + r);
            }
        }
        for (int i = 0; i < 12; i++) {
            int st;
            CK(waitpid(k[i], &st, 0) == k[i]);
            CK(WIFEXITED(st));
            CK(WEXITSTATUS(st) == ((i & 1) ? 0 : i + r));
        }
    }
    // plain fork with some memory dirtied in child (cow)
    char* m = malloc(1 << 20);
    memset(m, 1, 1 << 20);
    for (int i = 0; i < 40; i++) {
        pid_t p = fork();
        if (!p) { memset(m, 2, 1 << 20); _exit(m[12345] == 2 ? 0 : 1); }
        int st;
        CK(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0);
        CK(m[12345] == 1);
    }
    printf("ok 300 forks+exec, 40 cow\n");
    return 0;
}

static int send_fds(int s, int* fds, int n) {
    char c = 'x';
    struct iovec iov = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int) * 64)];
    memset(cb, 0, sizeof cb);
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cb; m.msg_controllen = CMSG_SPACE(sizeof(int) * n);
    struct cmsghdr* h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(h), fds, sizeof(int) * n);
    return sendmsg(s, &m, 0);
}

static int recv_fds(int s, int* fds, int max) {
    char c;
    struct iovec iov = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int) * 64)];
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cb; m.msg_controllen = sizeof cb;
    if (recvmsg(s, &m, 0) <= 0) return -1;
    struct cmsghdr* h = CMSG_FIRSTHDR(&m);
    if (!h || h->cmsg_type != SCM_RIGHTS) return 0;
    int n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    if (n > max) n = max;
    memcpy(fds, CMSG_DATA(h), n * sizeof(int));
    return n;
}

static int mkfds(int* o, int n, int k) {
    for (int i = 0; i < n; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "/tmp/scm.%d.%d", k, i);
        int fd = open(nm, O_CREAT | O_RDWR | O_TRUNC, 0600);
        CK(fd >= 0);
        unlink(nm);
        int v = k * 100 + i;
        CK(write(fd, &v, 4) == 4);
        o[i] = fd;
    }
    return 0;
}

static int getfds(int s, int n, int k) {
    int got[40];
    int r = recv_fds(s, got, 40);
    if (r != n) { printf("FAIL msg %d: sent %d fds, got %d\n", k, n, r); return 1; }
    for (int i = 0; i < r; i++) {
        int v = -1;
        CK(pread(got[i], &v, 4, 0) == 4);
        CK(v == k * 100 + i);
        close(got[i]);
    }
    return 0;
}

static int scm(void) {
    int sv[2];
    CK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    // 4 messages queued before the first recv: 28 fds in flight (pipe_t holds 32, more get dropped)
    int counts[] = { 1, 8, 9, 10 };
    int mine[4][32];
    for (int k = 0; k < 4; k++) {
        mkfds(mine[k], counts[k], k);
        CK(send_fds(sv[0], mine[k], counts[k]) == 1);
    }
    // hammer the kernel heap a bit while they sit in the queue
    void* junk[200];
    for (int i = 0; i < 200; i++) { junk[i] = malloc(100 + i * 13); memset(junk[i], 0x5a, 100 + i * 13); }
    for (int i = 0; i < 200; i++) free(junk[i]);
    for (int k = 0; k < 4; k++) if (getfds(sv[1], counts[k], k)) return 1;
    // one big message
    int big[32];
    mkfds(big, 30, 7);
    CK(send_fds(sv[0], big, 30) == 1);
    if (getfds(sv[1], 30, 7)) return 1;
    for (int k = 0; k < 4; k++) for (int i = 0; i < counts[k]; i++) close(mine[k][i]);
    for (int i = 0; i < 30; i++) close(big[i]);
    // through a forked child too
    pid_t p = fork();
    if (!p) {
        int got[40];
        int n = recv_fds(sv[1], got, 40);
        _exit(n == 20 ? 0 : 1);
    }
    int fds[20];
    for (int i = 0; i < 20; i++) fds[i] = dup(0);
    CK(send_fds(sv[0], fds, 20) == 1);
    int st;
    CK(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0);
    printf("ok 28 fds in flight, 30 in one msg\n");
    return 0;
}

static int shm(void) {
    int fd = syscall(SYS_memfd_create, "tt", 0);
    CK(fd >= 0);
    CK(ftruncate(fd, 1 << 20) == 0);
    char* a = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char* b = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CK(a != MAP_FAILED && b != MAP_FAILED);
    for (int i = 0; i < (1 << 20); i += 4096) a[i] = (char)(i >> 12) | 1;
    for (int i = 0; i < (1 << 20); i += 4096) CK(b[i] == ((char)(i >> 12) | 1));
    pid_t p = fork();
    if (!p) { a[100] = 77; _exit(0); }
    int st;
    waitpid(p, &st, 0);
    CK(b[100] == 77);
    char rb[4];
    CK(pread(fd, rb, 1, 100) == 1 && rb[0] == 77);
    // private copy must not leak back
    char* c = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CK(c != MAP_FAILED && c[100] == 77);
    c[100] = 5;
    CK(a[100] == 77);
    munmap(a, 1 << 20); munmap(b, 1 << 20);
    close(fd);
    // posix shm
    int s = shm_open("/ttshm", O_CREAT | O_RDWR, 0600);
    CK(s >= 0);
    CK(ftruncate(s, 8192) == 0);
    int* q = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    CK(q != MAP_FAILED);
    q[1000] = 1234;
    CK(shm_unlink("/ttshm") == 0);
    CK(q[1000] == 1234);
    printf("ok memfd + shm_open\n");
    return 0;
}

static int ino(void) {
    mkdir("/root/ino", 0755);
    int fd = inotify_init1(IN_NONBLOCK);
    CK(fd >= 0);
    CK(inotify_add_watch(fd, "/root/ino", IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO) >= 0);
    int f = open("/root/ino/a", O_CREAT | O_WRONLY, 0644);
    write(f, "hi", 2);
    close(f);
    rename("/root/ino/a", "/root/ino/b");
    unlink("/root/ino/b");
    usleep(20000);
    char buf[4096] __attribute__((aligned(8)));
    int n = read(fd, buf, sizeof buf), seen = 0;
    CK(n > 0);
    for (char* p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event*)p)->len)
        seen |= ((struct inotify_event*)p)->mask & (IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (seen != (IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) { printf("FAIL events %x\n", seen); return 1; }
    printf("ok create/modify/move/delete\n");
    return 0;
}

static int stx(void) {
    int fd = open("/root/stx", O_CREAT | O_RDWR | O_TRUNC, 0640);
    CK(fd >= 0 && write(fd, "12345", 5) == 5);
    struct statx x;
    CK(syscall(SYS_statx, AT_FDCWD, "/root/stx", 0, STATX_BASIC_STATS, &x) == 0);
    CK(x.stx_size == 5 && (x.stx_mode & 0777) == 0640 && S_ISREG(x.stx_mode));
    CK(x.stx_mtime.tv_sec > 0);
    CK(syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_SIZE, &x) == 0 && x.stx_size == 5);
    CK(syscall(SYS_statx, AT_FDCWD, "/root", 0, STATX_TYPE, &x) == 0 && S_ISDIR(x.stx_mode));
    CK(syscall(SYS_statx, AT_FDCWD, "/root/nope", 0, STATX_BASIC_STATS, &x) == -1 && errno == ENOENT);
    close(fd);
    unlink("/root/stx");
    printf("ok\n");
    return 0;
}

static int pollt(void) {
    long t = now_us();
    CK(poll(NULL, 0, 80) == 0);
    long d = (now_us() - t) / 1000;
    if (d < 75 || d > 200) { printf("FAIL poll(NULL,0,80) took %ld ms\n", d); return 1; }
    int pp[2];
    pipe(pp);
    struct pollfd f = { pp[0], POLLIN, 0 };
    t = now_us();
    CK(poll(&f, 1, 50) == 0);
    d = (now_us() - t) / 1000;
    if (d < 45 || d > 200) { printf("FAIL poll(pipe,50) took %ld ms\n", d); return 1; }
    write(pp[1], "x", 1);
    CK(poll(&f, 1, 1000) == 1 && (f.revents & POLLIN));
    printf("ok\n");
    return 0;
}

static int wait4t(void) {
    pid_t p = fork();
    if (!p) { usleep(60000); _exit(7); }
    int st;
    struct rusage ru;
    CK(wait4(p, &st, WNOHANG, &ru) == 0);
    CK(wait4(p, &st, 0, &ru) == p);
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    p = fork();
    if (!p) { for (;;) pause(); }
    usleep(20000);
    kill(p, SIGKILL);
    CK(wait4(-1, &st, 0, &ru) == p);
    CK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    CK(wait4(-1, &st, 0, &ru) == -1 && errno == ECHILD);
    printf("ok\n");
    return 0;
}

static int jit(void) {
    long sum = 0, mx = 0, mn = 1 << 30;
    int n = 50;
    for (int i = 0; i < n; i++) {
        long t = now_us();
        usleep(100000);
        long d = now_us() - t - 100000;
        sum += d;
        if (d > mx) mx = d;
        if (d < mn) mn = d;
    }
    printf("%s min %ld avg %ld max %ld us over 100 ms\n", (mx > 500000 || sum / n > 25000 || mn < -2000) ? "FAIL" : "ok", mn, sum / n, mx);
    return (mx > 500000 || sum / n > 25000 || mn < -2000);
}

int main(int argc, char** argv) {
    const char* w = argc > 1 ? argv[1] : "";
    if (!strcmp(w, "fileops")) return fileops();
    if (!strcmp(w, "fork")) return forkstorm();
    if (!strcmp(w, "scm")) return scm();
    if (!strcmp(w, "shm")) return shm();
    if (!strcmp(w, "inotify")) return ino();
    if (!strcmp(w, "statx")) return stx();
    if (!strcmp(w, "poll")) return pollt();
    if (!strcmp(w, "wait4")) return wait4t();
    if (!strcmp(w, "jitter")) return jit();
    printf("usage: tt fileops|fork|scm|shm|inotify|statx|poll|wait4|jitter\n");
    return 2;
}
