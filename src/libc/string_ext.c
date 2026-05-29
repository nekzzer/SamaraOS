/* Extra string functions on top of src/string.c (memset/memcpy/etc. live there) */
#include "../types.h"

/* declared in src/libc/include/string.h, src/string.h provides core ones */
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* d, const char* s);
char*  strncpy(char* d, const char* s, size_t n);

extern void* kmalloc(size_t);
extern void  kfree(void*);

static int lc(int c) { if (c >= 'A' && c <= 'Z') c += 32; return c; }
static int up(int c) { if (c >= 'a' && c <= 'z') c -= 32; return c; }

int strcasecmp(const char* a, const char* b) {
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    while (n && *a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; n--; }
    if (!n) return 0;
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == 0) return (char*)s;
    return (char*)last;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (*dst) dst++;
    while (n-- && *src) *dst++ = *src++;
    *dst = 0;
    return r;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* r = (char*)kmalloc(n);
    if (!r) return NULL;
    for (size_t i = 0; i < n; i++) r[i] = s[i];
    return r;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    for (; *s; s++, n++) {
        const char* a = accept;
        while (*a && *a != *s) a++;
        if (!*a) return n;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    for (; *s; s++, n++) {
        const char* r = reject;
        while (*r) { if (*r == *s) return n; r++; }
    }
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++) {
        const char* a = accept;
        while (*a) { if (*a == *s) return (char*)s; a++; }
    }
    return NULL;
}

static char* tok_save = NULL;
char* strtok(char* str, const char* delim) {
    char* s = str ? str : tok_save;
    if (!s) return NULL;
    /* skip leading delim */
    while (*s) {
        const char* d = delim;
        int hit = 0;
        while (*d) { if (*s == *d) { hit = 1; break; } d++; }
        if (!hit) break;
        s++;
    }
    if (!*s) { tok_save = NULL; return NULL; }
    char* tok = s;
    while (*s) {
        const char* d = delim;
        while (*d) { if (*s == *d) { *s = 0; tok_save = s + 1; return tok; } d++; }
        s++;
    }
    tok_save = NULL;
    return tok;
}

static const char* errmsg(int e) {
    switch (e) {
        case 0:  return "Success";
        case 1:  return "Operation not permitted";
        case 2:  return "No such file or directory";
        case 5:  return "I/O error";
        case 9:  return "Bad file descriptor";
        case 12: return "Out of memory";
        case 13: return "Permission denied";
        case 17: return "File exists";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 28: return "No space left on device";
        default: return "Unknown error";
    }
}

char* strerror(int err) { return (char*)errmsg(err); }
