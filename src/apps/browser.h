#ifndef SAMARA_BROWSER_H
#define SAMARA_BROWSER_H
#include "core/types.h"

/* Open the browser window. If `url` is non-NULL, immediately navigate to it.
   Accepts full URLs, bare host names (google.com) or search words;
   pages are fetched with the userland curl (DNS + HTTPS). */
int browser_open(const char* url);

#endif
