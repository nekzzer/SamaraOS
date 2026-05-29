/* Replaces doomgeneric/i_system.c — same exported symbols, but freestanding. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "d_ticcmd.h"

#define DEFAULT_RAM 6
#define MIN_RAM     6

extern void doomgen_panic(const char* msg) __attribute__((noreturn));

typedef struct atexit_s atexit_t;
struct atexit_s { atexit_func_t func; boolean run_on_error; atexit_t* next; };
static atexit_t* exit_funcs = NULL;

void I_AtExit(atexit_func_t f, boolean run_on_error) {
    atexit_t* e = (atexit_t*)malloc(sizeof(*e));
    if (!e) return;
    e->func = f; e->run_on_error = run_on_error; e->next = exit_funcs;
    exit_funcs = e;
}

void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

byte* I_ZoneBase(int* size) {
    int mb = DEFAULT_RAM;
    int p = M_CheckParmWithArgs("-mb", 1);
    if (p > 0) mb = atoi(myargv[p+1]);
    if (mb < MIN_RAM) mb = MIN_RAM;
    byte* mem = NULL;
    while (mb >= 1) {
        *size = mb * 1024 * 1024;
        mem = (byte*)malloc(*size);
        if (mem) break;
        mb--;
    }
    if (!mem) {
        doomgen_panic("I_ZoneBase: cannot allocate zone");
    }
    printf("zone memory: %p, %x allocated for zone\n", mem, *size);
    return mem;
}

boolean I_ConsoleStdout(void) { return 0; }

ticcmd_t* I_BaseTiccmd(void) {
    static ticcmd_t emptycmd;
    return &emptycmd;
}

void I_Quit(void) {
    atexit_t* e = exit_funcs;
    while (e) { e->func(); e = e->next; }
    doomgen_panic("I_Quit");
}

void I_Init(void) { /* timer/joystick init not needed */ }
void I_BindVariables(void) { }

void I_PrintBanner(char* msg) {
    int sp = 35 - (strlen(msg) / 2);
    for (int i = 0; i < sp; i++) putchar(' ');
    puts(msg);
}

void I_PrintDivider(void) {
    for (int i = 0; i < 75; i++) putchar('=');
    putchar('\n');
}

void I_PrintStartupBanner(char* desc) {
    I_PrintDivider();
    I_PrintBanner(desc);
    I_PrintDivider();
}

static boolean already_quitting = 0;

void I_Error(char* err, ...) {
    char buf[512];
    va_list ap;
    if (already_quitting) doomgen_panic("recursive I_Error");
    already_quitting = 1;
    va_start(ap, err);
    vsnprintf(buf, sizeof(buf), err, ap);
    va_end(ap);
    fprintf(stderr, "DOOM ERROR: %s\n", buf);
    atexit_t* e = exit_funcs;
    while (e) { if (e->run_on_error) e->func(); e = e->next; }
    doomgen_panic(buf);
}

boolean I_GetMemoryValue(unsigned int offset, void* value, int size) {
    static const unsigned char dump[10] = {0x9E,0x0F,0xC9,0x00,0x65,0x04,0x70,0x00,0x16,0x00};
    if (offset + size > sizeof(dump)) return 0;
    switch (size) {
        case 1: *(unsigned char*)value = dump[offset]; return 1;
        case 2: *(unsigned short*)value = dump[offset] | (dump[offset+1] << 8); return 1;
        case 4: *(unsigned int*)value = dump[offset] | (dump[offset+1]<<8) | (dump[offset+2]<<16) | (dump[offset+3]<<24); return 1;
    }
    return 0;
}
