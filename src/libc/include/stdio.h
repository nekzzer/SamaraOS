#ifndef _LIBC_STDIO_H
#define _LIBC_STDIO_H

#include "../../core/types.h"
#include "stdarg.h"

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _IO_FILE FILE;
typedef long fpos_t;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* path, const char* mode);
int   fclose(FILE* fp);
size_t fread(void* p, size_t sz, size_t n, FILE* fp);
size_t fwrite(const void* p, size_t sz, size_t n, FILE* fp);
int   fseek(FILE* fp, long off, int whence);
long  ftell(FILE* fp);
void  rewind(FILE* fp);
int   fflush(FILE* fp);
int   feof(FILE* fp);
int   ferror(FILE* fp);
int   fgetc(FILE* fp);
char* fgets(char* s, int n, FILE* fp);
int   fputc(int c, FILE* fp);
int   fputs(const char* s, FILE* fp);
int   ungetc(int c, FILE* fp);
int   remove(const char* path);
int   rename(const char* a, const char* b);
FILE* tmpfile(void);

int   printf(const char* fmt, ...);
int   fprintf(FILE* fp, const char* fmt, ...);
int   sprintf(char* buf, const char* fmt, ...);
int   snprintf(char* buf, size_t n, const char* fmt, ...);
int   vprintf(const char* fmt, va_list ap);
int   vfprintf(FILE* fp, const char* fmt, va_list ap);
int   vsprintf(char* buf, const char* fmt, va_list ap);
int   vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

int   sscanf(const char* str, const char* fmt, ...);
int   vsscanf(const char* str, const char* fmt, va_list ap);

int   putchar(int c);
int   getchar(void);
int   puts(const char* s);
void  perror(const char* s);

#endif
