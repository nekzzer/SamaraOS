#ifndef _LIBC_STDLIB_H
#define _LIBC_STDLIB_H

#include "../../types.h"

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(size_t n);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* p, size_t n);
void  free(void* p);

void  exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*f)(void));
char* getenv(const char* name);
int   system(const char* cmd);

int   atoi(const char* s);
long  atol(const char* s);
double atof(const char* s);
long  strtol(const char* s, char** endp, int base);
unsigned long strtoul(const char* s, char** endp, int base);

int   abs(int x);
long  labs(long x);

void  qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));

int   rand(void);
void  srand(unsigned int seed);

#endif
