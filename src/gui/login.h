#ifndef SAMARA_LOGIN_H
#define SAMARA_LOGIN_H
#include "core/types.h"

void login_load_etc(void);         /* /mnt/etc/{passwd,shadow,...} -> /etc */
void login_screen(void);           /* blocks till someone logs in */
bool install_screen(void);         /* false = "try it" without installing */
void install_progress(const char* msg, int pct);

/* "$1$salt$hash" into out (>= 40 bytes), same as crypt() in musl */
void md5crypt(const char* pw, const char* salt, char* out);

/* install.c */
const char* install_run(int disk, const char* host, const char* user, const char* pass);

extern char login_user[32];

#endif
