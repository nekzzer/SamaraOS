#include "../core/types.h"

/* signal — no-op */
typedef void (*__sighandler_t)(int);
__sighandler_t signal(int sig, __sighandler_t h) { (void)sig; (void)h; return (__sighandler_t)0; }
int raise(int sig) { (void)sig; return 0; }

/* time / clock — driven by PIT uptime */
extern uint32_t pit_uptime_ms(void);

int32_t time(int32_t* t) {
    int32_t s = (int32_t)(pit_uptime_ms() / 1000);
    if (t) *t = s;
    return s;
}
int32_t clock(void) { return (int32_t)pit_uptime_ms(); }

/* unistd stubs */
int   isatty(int fd) { (void)fd; return 0; }
int   close(int fd)  { (void)fd; return 0; }
int   unlink(const char* p) { (void)p; return 0; }
int   access(const char* p, int m) { (void)p; (void)m; return -1; }
int   fileno(void* fp) { (void)fp; return 1; }
unsigned int sleep(unsigned int s) { (void)s; return 0; }
int   usleep(unsigned int u) { (void)u; return 0; }

/* sys/stat */
int   mkdir(const char* p, int m) { (void)p; (void)m; return 0; }
int   stat(const char* p, void* st) { (void)p; (void)st; return -1; }

/* fcntl */
int open(const char* p, int f, ...) { (void)p; (void)f; return -1; }

/* math: rough double routines used sparsely by DOOM */
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) { long ip = (long)x; if (x < 0 && (double)ip != x) ip--; return (double)ip; }
double ceil(double x) { long ip = (long)x; if (x > 0 && (double)ip != x) ip++; return (double)ip; }

double sqrt(double x) {
    if (x <= 0) return 0;
    double r = x;
    for (int i = 0; i < 20; i++) r = 0.5 * (r + x / r);
    return r;
}

/* DOOM uses sin/cos via tables.c -- these are unused but link-safe */
double sin(double x) { (void)x; return 0; }
double cos(double x) { (void)x; return 1; }
double atan2(double y, double x) { (void)y; (void)x; return 0; }
double pow(double x, double y) { (void)y; return x; }

/* GCC / lib helpers */
void __stack_chk_fail(void) {
    extern void doomgen_panic(const char*) __attribute__((noreturn));
    doomgen_panic("__stack_chk_fail");
}
unsigned long __stack_chk_guard = 0xdeadbeefUL;
