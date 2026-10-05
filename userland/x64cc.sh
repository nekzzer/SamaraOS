#!/bin/sh
# usage: x64cc.sh [gcc flags] src.c -o out   (static x86_64 musl, sysroot from build-x64sdk.sh)
R=$(cd "$(dirname "$0")/.." && pwd)/toolchain/x64sdk
[ -d $R/usr ] || R=/tmp/int-x64sdk
GI=$(gcc -print-file-name=include)
exec gcc -O2 -static -nostdinc -isystem $GI -isystem $R/usr/include -nostdlib -fno-stack-protector -fno-pie -no-pie "$@" \
  $R/usr/lib/crt1.o $R/usr/lib/crti.o -Wl,--start-group $R/usr/lib/libc.a $(gcc -print-libgcc-file-name) -Wl,--end-group $R/usr/lib/crtn.o
