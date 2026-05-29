#include "string.h"

void* memset(void* dst, int v, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)v;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (uint8_t)*a - (uint8_t)*b;
}

char* strcpy(char* dst, const char* src) {
    char* r = dst;
    while ((*dst++ = *src++)) {}
    return r;
}

char* strncpy(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (n && (*dst++ = *src++)) n--;
    while (n--) *dst++ = 0;
    return r;
}

char* strcat(char* dst, const char* src) {
    char* r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return r;
}

char* strchr(const char* s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == 0) ? (char*)s : NULL;
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (const char* h = hay; *h; h++) {
        const char* n = needle;
        const char* hh = h;
        while (*n && *hh && *n == *hh) { n++; hh++; }
        if (!*n) return (char*)h;
    }
    return NULL;
}

int atoi(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

static void reverse(char* s, int n) {
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        char t = s[i]; s[i] = s[j]; s[j] = t;
    }
}

void utoa(uint32_t v, char* buf, int base) {
    const char* digits = "0123456789abcdef";
    int n = 0;
    if (v == 0) { buf[n++] = '0'; buf[n] = 0; return; }
    while (v) { buf[n++] = digits[v % (uint32_t)base]; v /= (uint32_t)base; }
    buf[n] = 0;
    reverse(buf, n);
}

void itoa(int v, char* buf, int base) {
    if (v < 0 && base == 10) {
        *buf++ = '-';
        utoa((uint32_t)(-v), buf, base);
    } else {
        utoa((uint32_t)v, buf, base);
    }
}
