#!/bin/bash
# host build of the browser layout (src/apps/browser.c) for quick tests:
#   ./tools/browser-test/build.sh && python3 tools/browser-test/inl.py https://news.ycombinator.com/ /tmp/hn.html
#   /tmp/bt /tmp/hn.html 940 40      (page, width, how many runs/boxes to dump)
cd "$(dirname "$0")/../.."
D=tools/browser-test
F="-O1 -g -ffreestanding -fno-builtin -fno-stack-protector -Wno-builtin-declaration-mismatch -Isrc"
gcc -c $F $D/wrap.c -o /tmp/bt-wrap.o || exit 1
gcc -c $F src/gfx/uifont.c -o /tmp/bt-uifont.o
gcc -c $F src/core/string.c -o /tmp/bt-string.o
gcc -c $F src/apps/imgdec.c -o /tmp/bt-imgdec.o
gcc -c -O1 $D/stubs.c -o /tmp/bt-stubs.o 2>/dev/null
gcc /tmp/bt-wrap.o /tmp/bt-imgdec.o /tmp/bt-uifont.o /tmp/bt-string.o /tmp/bt-stubs.o -o /tmp/bt
