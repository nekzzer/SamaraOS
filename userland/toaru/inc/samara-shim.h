/* forced into every toaru file (-include). toaru libc bits musl doesn't have */
#pragma once
#include <limits.h>
#include <signal.h>
#define SIGWINEVENT SIGURG    // toaru-only signal, urg is unused here
int samara_mouse_fd(void);
int samara_kbd_fd(void);
// toaru's TRACE prints tv_sec with %d, musl has 64-bit time_t -> args shift, %s gets garbage, segfault.
// trace.h keeps ours since it's #ifndef. YUTANI_TRACE=1 to see it
#include <stdio.h>
#include <stdlib.h>
#define TRACE(msg, ...) do { if (getenv("YUTANI_TRACE")) fprintf(stderr, "[" TRACE_APP_NAME "] " msg "\n", ##__VA_ARGS__); } while (0)
