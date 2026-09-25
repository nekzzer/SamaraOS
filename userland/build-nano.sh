#!/bin/sh
# Builds GNU nano 7.2 as a static i386 binary for SamaraOS:
# ncurses 6.4 (static, narrow, no terminfo database: "linux" and "vt100"
# compiled in) + nano, both with the musl.cc i686 cross compiler.
# Sources are expected in toolchain/ncurses-6.4 and toolchain/nano-7.2.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
NCOUT=$ROOT/toolchain/ncurses-out

cd "$ROOT/toolchain/ncurses-6.4"
CC=$X/i686-linux-musl-gcc AR=$X/i686-linux-musl-ar RANLIB=$X/i686-linux-musl-ranlib \
CFLAGS="-O2 -fno-pie" LDFLAGS="-static -no-pie" \
./configure --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
    --without-shared --without-cxx --without-cxx-binding --without-ada \
    --without-tests --without-progs --without-manpages --disable-widec \
    --disable-database --with-fallbacks=linux,vt100 --without-debug
make -j"$(nproc)"
make DESTDIR="$NCOUT" install.libs install.includes

cd "$ROOT/toolchain/nano-7.2"
NC=$NCOUT/usr
CC=$X/i686-linux-musl-gcc CFLAGS="-O2 -fno-pie -I$NC/include" \
LDFLAGS="-static -no-pie -s -L$NC/lib" NCURSES_CFLAGS="-I$NC/include" \
NCURSES_LIBS="-L$NC/lib -lncurses" PKG_CONFIG=false \
./configure --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
    --sysconfdir=/etc --disable-nls --disable-utf8 --disable-speller \
    --disable-libmagic --enable-color --enable-nanorc
make -j"$(nproc)"
echo "built: $ROOT/toolchain/nano-7.2/src/nano (run userland/build-sysroot.sh next)"
