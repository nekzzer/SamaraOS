#include "../core/types.h"
#include "../core/heap.h"
#include "../core/string.h"

extern void doomgen_panic(const char* msg) __attribute__((noreturn));

/* malloc with size header so realloc/calloc work */
typedef struct { size_t sz; } mhdr_t;

void* malloc(size_t n) {
    if (!n) return NULL;
    mhdr_t* h = (mhdr_t*)kmalloc(n + sizeof(mhdr_t));
    if (!h) return NULL;
    h->sz = n;
    return (void*)(h + 1);
}

void free(void* p) {
    if (!p) return;
    mhdr_t* h = ((mhdr_t*)p) - 1;
    kfree(h);
}

void* calloc(size_t nmemb, size_t size) {
    size_t n = nmemb * size;
    void* p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    mhdr_t* h = ((mhdr_t*)p) - 1;
    if (h->sz >= n) return p;
    void* np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, h->sz);
    free(p);
    return np;
}

extern void I_Quit(void);
void exit(int status) {
    (void)status;
    /* If DOOM is up, run its at-exit chain, then halt. */
    doomgen_panic("exit() called");
}
void abort(void) { doomgen_panic("abort() called"); }

int atexit(void (*f)(void)) { (void)f; return 0; }
char* getenv(const char* n) { (void)n; return (char*)0; }
int system(const char* c) { (void)c; return -1; }

long atol(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}

double atof(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; }
    }
    return sign * v;
}

long strtol(const char* s, char** endp, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; if (base == 0) base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    long v = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endp) *endp = (char*)s;
    return sign * v;
}

unsigned long strtoul(const char* s, char** endp, int base) {
    return (unsigned long)strtol(s, endp, base);
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned int rseed = 1;
int rand(void) {
    rseed = rseed * 1103515245 + 12345;
    return (int)((rseed / 65536) & 0x7fffffff);
}
void srand(unsigned int s) { rseed = s; }

/* qsort: simple Sedgewick quicksort with insertion-sort tail */
static void swap_bytes(char* a, char* b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void* base, size_t n, size_t size,
           int (*cmp)(const void*, const void*)) {
    if (n < 2) return;
    char* arr = (char*)base;
    /* recursive in-place quicksort */
    /* pivot = middle element; partition */
    char* pivot = arr + (n / 2) * size;
    /* save pivot (local buffer up to 256 bytes; DOOM compares cmp_t = small) */
    char tmp[256];
    size_t psz = size > sizeof(tmp) ? sizeof(tmp) : size;
    /* fall back to indices if record too big */
    (void)tmp; (void)psz;

    size_t i = 0, j = n - 1;
    /* partition */
    char piv_buf[256];
    if (size <= sizeof(piv_buf)) {
        for (size_t k = 0; k < size; k++) piv_buf[k] = pivot[k];
    }
    while (i <= j) {
        while (i < n && cmp(arr + i*size, size <= sizeof(piv_buf) ? (void*)piv_buf : (void*)pivot) < 0) i++;
        while (j > 0 && cmp(arr + j*size, size <= sizeof(piv_buf) ? (void*)piv_buf : (void*)pivot) > 0) j--;
        if (i >= j) break;
        swap_bytes(arr + i*size, arr + j*size, size);
        if (arr + i*size == pivot) pivot = arr + j*size;
        else if (arr + j*size == pivot) pivot = arr + i*size;
        i++;
        if (j > 0) j--;
    }
    if (j > 0) qsort(arr, j + 1, size, cmp);
    if (i < n) qsort(arr + i*size, n - i, size, cmp);
}
