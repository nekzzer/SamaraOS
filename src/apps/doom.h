#ifndef SAMARA_DOOM_H
#define SAMARA_DOOM_H
#include "core/types.h"

/* Returns 0 on success, negative on error (-1 no ATA, -2 not a WAD,
   -3 missing PLAYPAL/TITLEPIC, -4 OOM). */
int  doom_load_from_disk(void);
bool doom_loaded(void);

/* Open the DOOM viewer in a WM window. Must be called inside desktop. */
void doom_open_window(void);

/* Status text for shell — last load result message. */
const char* doom_status(void);

/* Loads E1M1 (VERTEXES, LINEDEFS, THINGS) and opens a 3D play window. */
int  doom_play_e1m1(void);

#endif
