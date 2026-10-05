#ifndef SAMARA_SND_H
#define SAMARA_SND_H
#include "core/types.h"

/* alsa uapi over hda: /dev/snd/controlC0 and pcmC0D0p */

struct snd_fd;

void  snd_init(void);                       /* after hda_init and fs */
bool  snd_present(void);
struct snd_fd* snd_open(int pcm);           /* 0 control, 1 playback */
void  snd_close(struct snd_fd* s);
int   snd_ioctl(struct snd_fd* s, uint32_t req, void* arg, bool nb);
bool  snd_writable(struct snd_fd* s);

/* for the kernel's own wav player, stereo s16 at 44100 or 48000 */
int   snd_kopen(uint32_t rate);
int   snd_kwrite(const int16_t* pcm, uint32_t frames);   /* blocks */
void  snd_kclose(bool drain);

#endif
