// smp scaling test: locktest <mm|futex|pipe|clock|all> [threads] [iters]
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

static int nt = 4, iters = 100000;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void* t_mm(void* a) {
    for (int i = 0; i < iters; i++) {
        volatile char* p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("mmap fail\n"); return 0; }
        p[0] = 1; p[4096] = 2;
        munmap((void*)p, 8192);
    }
    return 0;
}

static void* t_clock(void* a) {
    struct timespec t;
    for (int i = 0; i < iters * 5; i++) clock_gettime(CLOCK_REALTIME, &t);
    return 0;
}

static void* t_pf(void* a) {
    // page faults only: one big mapping, touch every page
    int n = 4096;
    for (int r = 0; r < iters / 4096 + 1; r++) {
        char* p = mmap(0, n * 4096L, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        for (int i = 0; i < n; i++) p[i * 4096L] = 1;
        munmap(p, n * 4096L);
    }
    return 0;
}

struct pp { volatile int turn; int fd[4]; };
static long fx(volatile int* a, int op, int v) { return syscall(SYS_futex, a, op, v, 0, 0, 0); }

static void run(const char* name, void* (*fn)(void*), long ops_per_thread) {
    pthread_t th[16];
    long t0 = now_ms();
    for (int i = 0; i < nt; i++) pthread_create(&th[i], 0, fn, 0);
    for (int i = 0; i < nt; i++) pthread_join(th[i], 0);
    long dt = now_ms() - t0;
    printf("%-6s thr=%d  %5ld ms  %ld ops/s\n", name, nt, dt, dt ? ops_per_thread * nt * 1000 / dt : 0);
}

// pairs need distinct ids per thread: wrap
struct arg { struct pp* p; int id; int kind; };
static void* wrap(void* v) {
    struct arg* a = v;
    struct pp* sh = a->p;
    int me = a->id;
    if (a->kind == 0) {
        for (int i = 0; i < iters; i++) {
            while (sh->turn != me) fx(&sh->turn, FUTEX_WAIT_PRIVATE, 1 - me);
            sh->turn = 1 - me;
            fx(&sh->turn, FUTEX_WAKE_PRIVATE, 1);
        }
    } else if (a->kind == 2) {
        uint64_t v = 1;
        for (int i = 0; i < iters; i++) {
            if (me == 0) { write(sh->fd[0], &v, 8); read(sh->fd[1], &v, 8); }
            else { read(sh->fd[0], &v, 8); write(sh->fd[1], &v, 8); }
        }
    } else {
        char c = 'x';
        for (int i = 0; i < iters; i++) {
            if (me == 0) { write(sh->fd[1], &c, 1); read(sh->fd[2], &c, 1); }
            else { read(sh->fd[0], &c, 1); write(sh->fd[3], &c, 1); }
        }
    }
    return 0;
}

static void runp(const char* name, int kind) {
    pthread_t th[16];
    static struct pp pp[8];
    static struct arg ar[16];
    int n = nt / 2 ? nt / 2 : 1;
    for (int i = 0; i < n; i++) {
        pp[i].turn = 0;
        if (kind == 2) { pp[i].fd[0] = eventfd(0, 0); pp[i].fd[1] = eventfd(0, 0); }
        else { pipe(pp[i].fd); pipe(pp[i].fd + 2); }
    }
    long t0 = now_ms();
    for (int i = 0; i < n * 2; i++) {
        ar[i].p = &pp[i / 2]; ar[i].id = i & 1; ar[i].kind = kind;
        pthread_create(&th[i], 0, wrap, &ar[i]);
    }
    for (int i = 0; i < n * 2; i++) pthread_join(th[i], 0);
    long dt = now_ms() - t0;
    printf("%-6s thr=%d  %5ld ms  %ld roundtrips/s\n", name, n * 2, dt, dt ? (long)iters * n * 1000 / dt : 0);
    for (int i = 0; i < n; i++) { close(pp[i].fd[0]); close(pp[i].fd[1]); close(pp[i].fd[2]); close(pp[i].fd[3]); }
}

// jitter <ms>: spin on getppid (still takes the big lock) and report the worst gap
static void jitter(int ms) {
    long t0 = now_ms(), last = t0, worst = 0, n = 0;
    while (now_ms() - t0 < ms) {
        getppid();
        long t = now_ms();
        if (t - last > worst) worst = t - last;
        last = t; n++;
    }
    printf("jitter %ld calls, worst gap %ld ms\n", n, worst);
}

int main(int argc, char** argv) {
    const char* w = argc > 1 ? argv[1] : "all";
    if (!strcmp(w, "jitter")) { jitter(argc > 2 ? atoi(argv[2]) : 3000); return 0; }
    if (argc > 2) nt = atoi(argv[2]);
    if (argc > 3) iters = atoi(argv[3]);
    if (!strcmp(w, "mm") || !strcmp(w, "all")) run("mm", t_mm, iters);
    if (!strcmp(w, "pf") || !strcmp(w, "all")) run("pf", t_pf, (iters / 4096 + 1) * 4096L);
    if (!strcmp(w, "clock") || !strcmp(w, "all")) run("clock", t_clock, iters * 5L);
    if (!strcmp(w, "futex") || !strcmp(w, "all")) runp("futex", 0);
    if (!strcmp(w, "pipe") || !strcmp(w, "all")) runp("pipe", 1);
    if (!strcmp(w, "efd") || !strcmp(w, "all")) runp("efd", 2);
    return 0;
}
