#ifndef SAMARA_MEDIAPLAYER_H
#define SAMARA_MEDIAPLAYER_H
#include "types.h"

/* Opens the SamaraOS music player as a window in the WM. Returns 0 on success,
   -1 if no graphics available. Safe to call multiple times — singleton. */
int  mediaplayer_open(void);

/* Stop speaker and clear singleton — used when the WM is shut down. */
void mediaplayer_force_stop(void);

#endif
