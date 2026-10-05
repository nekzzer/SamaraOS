#!/bin/sh
# Builds userland/sysroot.tar for x86_64: samara.h, demo sources, the samara apps
# (static musl via x64cc.sh, needs build-x64sdk.sh first). The kernel unpacks it into
# the ramfs at boot; the real userland (gcc, bash, curl...) comes from build-x64root.sh.
# The old i386 tcc/musl/bash/curl tarball is in git history.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SAM=$ROOT/userland/samara
CC=$ROOT/userland/x64cc.sh
OUT=$ROOT/build/sysroot
rm -rf "$OUT"
mkdir -p "$OUT/usr/bin" "$OUT/usr/include" "$OUT/usr/src/samara" "$OUT/usr/src/games" "$OUT/usr/src/fm" "$OUT/usr/games" "$OUT/etc"

cp "$SAM/samara.h" "$OUT/usr/include/"
cp "$SAM/hello.c" "$SAM/hello.py" "$OUT/usr/src/samara/"
cp "$SAM/tetris.c" "$SAM/breakout.py" "$OUT/usr/src/games/"
cp "$SAM/breakout.py" "$OUT/usr/games/"
cp "$SAM/fm.c" "$SAM/fm_be.h" "$SAM/fm_samara.c" "$SAM/fm_img.c" "$OUT/usr/src/fm/"
cp "$SAM/samara-fm.conf" "$OUT/etc/samara-fm.conf"

$CC -w -I"$SAM" "$SAM/tetris.c" -o "$OUT/usr/games/tetris"
$CC -w "$SAM/samarafetch.c" -o "$OUT/usr/bin/samarafetch"
$CC -w -I"$SAM" "$SAM/fm.c" "$SAM/fm_samara.c" "$SAM/fm_img.c" -o "$OUT/usr/bin/fm"
$CC -w -I"$SAM" "$SAM/hello.c" -o "$OUT/usr/bin/samara-hello"
strip "$OUT"/usr/bin/* "$OUT"/usr/games/tetris

cat > "$OUT/etc/nanorc" <<'RC'
include "/usr/share/nano/*.nanorc"
set autoindent
set tabsize 4
set constantshow
RC

tar -C "$OUT" --owner=0 --group=0 -cf "$ROOT/userland/sysroot.tar" .
ls -l "$ROOT/userland/sysroot.tar"
