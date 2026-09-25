#!/bin/sh
# Builds MicroPython (ports/unix) as a static i386 musl binary for SamaraOS.
# The kernel's Linux i386 syscall layer runs it unmodified; it ends up in
# /usr/bin/micropython via userland/build-sysroot.sh (run that next).
#
# Disabled: ffi (needs libffi), ssl (mbedtls submodule), btree (berkeley-db
# submodule), _thread (pthreads/CLONE_THREAD), FAT/littlefs VFS (not needed:
# the unix port uses the host filesystem = SamaraOS ramfs + /mnt).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin/i686-linux-musl-
MP=$ROOT/toolchain/micropython
MP_TAG=${MP_TAG:-v1.26.1}
# `samara` module (desktop windows): userland/samara/micropython/samara/
USERMOD=$ROOT/userland/samara/micropython

if [ ! -d "$MP" ]; then
    git clone --depth 1 -b "$MP_TAG" https://github.com/micropython/micropython.git "$MP"
fi
(cd "$MP" && git submodule update --init --depth 1 lib/micropython-lib)

make -C "$MP/mpy-cross" -j"$(nproc)"
make -C "$MP/ports/unix" -j"$(nproc)" CROSS_COMPILE="$X" \
    MICROPY_PY_FFI=0 MICROPY_PY_BTREE=0 MICROPY_PY_SSL=0 MICROPY_SSL_MBEDTLS=0 \
    MICROPY_PY_THREAD=0 MICROPY_VFS_FAT=0 MICROPY_VFS_LFS1=0 MICROPY_VFS_LFS2=0 \
    USER_C_MODULES="$USERMOD" \
    CFLAGS_EXTRA="-fno-pie" LDFLAGS_EXTRA="-static -no-pie"
"${X}strip" "$MP/ports/unix/build-standard/micropython"
ls -l "$MP/ports/unix/build-standard/micropython"
echo "built MicroPython (run userland/build-sysroot.sh next)"
