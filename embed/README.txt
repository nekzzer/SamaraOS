Drop .wav files here. The build will objcopy them straight into the
kernel image; at boot they appear in /home/user/ inside SamaraOS and
play with `playwav <filename>`.

Format: 8-bit unsigned or 16-bit signed PCM, mono or stereo, any
sample rate up to 44100 Hz. No MP3 yet.

Smaller is better — the entire kernel ELF is shipped to QEMU each
boot. For larger collections, drop them into ../music/ and use
`playfat <name>` instead (FAT mount over IDE).
