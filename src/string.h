#ifndef SAMARA_STRING_H
#define SAMARA_STRING_H
#include "types.h"

void* memset(void* dst, int v, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strchr(const char* s, int c);
char* strstr(const char* hay, const char* needle);
int   atoi(const char* s);
void  itoa(int v, char* buf, int base);
void  utoa(uint32_t v, char* buf, int base);

#endif
