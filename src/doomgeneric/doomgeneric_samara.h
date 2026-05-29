#ifndef SAMARA_DOOMGENERIC_H
#define SAMARA_DOOMGENERIC_H

#include "../types.h"

/* High-level entry: load WAD, init libc-shim FILE backing, open WM window
   and run doomgeneric_Create + tick loop. Caller must already be in a graphics
   mode (i.e. desktop is running). Returns 0 on success.
*/
int  samara_doom_launch(void);

/* Returns last status string for shell display. */
const char* samara_doom_status(void);

#endif
