#!/bin/sh
# samara-wl: dynamic x86_64 musl binary against the void libs in the sdk (toolchain/x64sdk or /tmp/int-x64sdk)
# needs: libglvnd-devel libgbm-devel in the sdk too (xbps-install -r toolchain/x64sdk)
# needs: build-x64sdk.sh + wayland-devel wayland-protocols libxkbcommon-devel libxcb-devel in the sdk, host wayland-scanner
set -e
D=$(cd "$(dirname "$0")" && pwd)
R=$D/../../toolchain/x64sdk
[ -d $R/usr ] || R=/tmp/int-x64sdk
G=${GEN:-/tmp/wl-gen}
OUT=${1:-$D/../../build/samara-wl}
P=$R/usr/share/wayland-protocols
mkdir -p $G
wayland-scanner server-header $P/stable/xdg-shell/xdg-shell.xml $G/xdg-shell-server-protocol.h
wayland-scanner private-code $P/stable/xdg-shell/xdg-shell.xml $G/xdg-shell-protocol.c
wayland-scanner server-header $P/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $G/xdg-decoration-server-protocol.h
wayland-scanner private-code $P/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $G/xdg-decoration-protocol.c
wayland-scanner server-header $P/stable/linux-dmabuf/linux-dmabuf-v1.xml $G/linux-dmabuf-v1-server-protocol.h
wayland-scanner private-code $P/stable/linux-dmabuf/linux-dmabuf-v1.xml $G/linux-dmabuf-v1-protocol.c
GI=$(gcc -print-file-name=include)
gcc -O2 -Wall -Wno-unused-parameter -nostdinc -isystem $GI -isystem $R/usr/include -I$G -I$D -fno-stack-protector -fno-pie -no-pie \
    -nostdlib -o $OUT $R/usr/lib/crt1.o $R/usr/lib/crti.o \
    $D/main.c $D/comp.c $D/seat.c $D/xdg.c $D/xwm.c $D/gpu.c $G/linux-dmabuf-v1-protocol.c $G/xdg-shell-protocol.c $G/xdg-decoration-protocol.c \
    -L$R/usr/lib -Wl,-rpath-link,$R/usr/lib -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
    -lwayland-server -lxkbcommon -lxcb -lxcb-composite -lxcb-xfixes -lEGL -lgbm -lGLESv2 -lpthread -lc $(gcc -print-libgcc-file-name) $R/usr/lib/crtn.o
echo "built $OUT"
