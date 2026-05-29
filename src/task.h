#ifndef SAMARA_TASK_H
#define SAMARA_TASK_H
#include "types.h"

#define MAX_TASKS  16
#define TASK_STACK_SZ 8192

typedef enum { T_FREE = 0, T_READY, T_BLOCKED, T_DEAD } task_state_t;

typedef struct task {
    uint32_t      esp;
    uint8_t*      stack;
    task_state_t  state;
    int           id;
    char          name[32];
    uint32_t      ticks;       /* total ticks consumed */
} task_t;

void  task_init(void);
int   task_spawn(const char* name, void (*entry)(void));
void  task_exit(void);
void  task_yield(void);
task_t* task_current(void);
void  task_dump(void (*emit)(const char*));
int   task_count(void);
task_t* task_at(int idx);

/* Called from PIT ISR — installed by task_init. Not for direct use. */
void task_install_timer(void);

#endif
