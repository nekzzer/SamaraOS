#ifndef SAMARA_BROWSER_H
#define SAMARA_BROWSER_H
#include "core/types.h"

/* Open the browser window. If `url` is non-NULL, immediately navigate to it.
   URL format is restricted to http://A.B.C.D[:port][/path] — no DNS. */
int browser_open(const char* url);

#endif
