#!/bin/sh
# x86_64 musl sysroot from void into toolchain/x64sdk (musl-devel, headers, crt, libc.a)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=$ROOT/toolchain/x64sdk
mkdir -p $SDK/var/db/xbps/keys
cp -n /var/db/xbps/keys/* $SDK/var/db/xbps/keys/ 2>/dev/null || true
XBPS_ARCH=x86_64-musl xbps-install -S -y -r $SDK -R https://repo-default.voidlinux.org/current/musl \
    musl-devel kernel-libc-headers "$@"
echo "sdk: $SDK"
