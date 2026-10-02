#!/bin/sh
# Builds QuickJS (2024-01-13) for SamaraOS: qjs (plain engine), node (engine +
# node api layer from userland/js/node/*.js) and domjs (script runner for the
# browser, userland/js/dom.js). Static i386 musl binaries into build/js/.
# Source expected in toolchain/quickjs-2024-01-13.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
Q=$ROOT/toolchain/quickjs-2024-01-13
J=$ROOT/userland/js
OUT=$ROOT/build/js
mkdir -p "$OUT"
cd "$OUT"

# bytecode compiler runs on the host, the generated C is arch independent
V=$(cat "$Q/VERSION")
HDEF="-D_GNU_SOURCE -DCONFIG_VERSION=\"$V\" -DCONFIG_BIGNUM"
gcc -O2 $HDEF -I"$Q" -o qjsc_host "$Q/qjsc.c" "$Q/quickjs.c" "$Q/libregexp.c" "$Q/libunicode.c" \
    "$Q/cutils.c" "$Q/quickjs-libc.c" "$Q/libbf.c" -lm -ldl -lpthread
./qjsc_host -c -o repl.c -m "$Q/repl.js"
./qjsc_host -fbignum -c -o qjscalc.c "$Q/qjscalc.js"
( echo "(function () { 'use strict';"; cat "$J"/node/[1-9]_*.js; echo "})();" ) > node_all.js
./qjsc_host -c -o node_boot.c -N node_boot node_all.js
./qjsc_host -c -o dom_boot.c -N dom_boot "$J/dom.js"

CC=$X/i686-linux-musl-gcc
CF="-O2 -fno-pie -msse2 -mfpmath=sse -D_GNU_SOURCE -DCONFIG_VERSION=\"$V\" -DCONFIG_BIGNUM -I$Q -w"
LIB="$Q/quickjs.c $Q/libregexp.c $Q/libunicode.c $Q/cutils.c $Q/quickjs-libc.c $Q/libbf.c"
$CC $CF -static -no-pie -s -o qjs "$Q/qjs.c" repl.c qjscalc.c $LIB -lm
$CC $CF -static -no-pie -s -o node "$J/node.c" node_boot.c $LIB -lm
$CC $CF -static -no-pie -s -o domjs "$J/domjs.c" dom_boot.c $LIB -lm
ls -la qjs node domjs
