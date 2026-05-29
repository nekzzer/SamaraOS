#ifndef SAMARA_EMBED_H
#define SAMARA_EMBED_H

/* Registers every WAV file that the build linked in via objcopy as a ramfs
   entry under /home/user/. Wavs come from the project's embed/ directory.
   Safe no-op if nothing was embedded. Call once after fs_init(). */
void embed_install(void);

#endif
