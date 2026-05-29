#ifndef _LIBC_ASSERT_H
#define _LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __libc_assert_fail(const char* expr, const char* file, int line);
#define assert(expr) ((expr) ? (void)0 : __libc_assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
