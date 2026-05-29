Drop .wav files here. The Makefile attaches this directory as a
virtual FAT disk to QEMU (-drive file=fat:music). Inside SamaraOS:

  fatmount             # mount secondary IDE (auto on first playfat)
  fatls                # list files
  playfat song.wav     # stream straight from FAT to SB16

Format: PCM WAV (mono/stereo, 8 or 16 bit, ≤ 44100 Hz).
