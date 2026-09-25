#!/usr/bin/env bash
# Builds a self-contained i686-elf cross-compiler (binutils + gcc) for
# SamaraOS, following the standard OSDev.org "GCC Cross-Compiler" recipe.
#
# Everything lands in toolchain/cross/ next to this script, so the result
# is fully repo-local: clone the repo on another machine, run this script
# there, and the Makefile picks the toolchain up automatically (see
# CROSS in ../Makefile). Nothing is installed system-wide except the
# handful of build dependencies below.
#
# Usage: ./toolchain/build-cross.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PREFIX="$HERE/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"

BINUTILS_VERSION=2.42
GCC_VERSION=14.2.0

# ---------------------------------------------------------------------
# 1. Build dependencies. This line is Void Linux (xbps) specific; on
#    another distro swap it for the equivalent (e.g. on Debian/Ubuntu:
#    apt install build-essential bison flex libgmp-dev libmpfr-dev
#    libmpc-dev texinfo wget). Will prompt for your sudo password.
# ---------------------------------------------------------------------
if command -v xbps-install >/dev/null; then
    sudo xbps-install -y gmp-devel mpfr-devel libmpc-devel wget bison flex texinfo xorriso mtools ImageMagick netpbm
else
    echo "Not Void Linux (no xbps-install found) — install gmp/mpfr/mpc/bison/flex/texinfo/wget/xorriso dev packages yourself, then re-run." >&2
fi

mkdir -p "$HERE/src"
cd "$HERE/src"

if [ ! -f "binutils-$BINUTILS_VERSION.tar.gz" ]; then
    wget -c "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.gz"
fi
if [ ! -d "binutils-$BINUTILS_VERSION" ]; then
    tar xf "binutils-$BINUTILS_VERSION.tar.gz"
fi

if [ ! -f "gcc-$GCC_VERSION.tar.gz" ]; then
    wget -c "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz"
fi
if [ ! -d "gcc-$GCC_VERSION" ]; then
    tar xf "gcc-$GCC_VERSION.tar.gz"
fi

JOBS="$(nproc)"

# ---------------------------------------------------------------------
# 2. binutils (as, ld, objcopy, ...) targeting i686-elf
# ---------------------------------------------------------------------
mkdir -p "$HERE/build-binutils"
cd "$HERE/build-binutils"
"$HERE/src/binutils-$BINUTILS_VERSION/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS"
make install

# ---------------------------------------------------------------------
# 3. gcc (freestanding, no libc) targeting i686-elf
# ---------------------------------------------------------------------
mkdir -p "$HERE/build-gcc"
cd "$HERE/build-gcc"
"$HERE/src/gcc-$GCC_VERSION/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers
make -j"$JOBS" all-gcc all-target-libgcc
make install-gcc install-target-libgcc

echo
echo "Cross-compiler installed at: $PREFIX/bin/$TARGET-gcc"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
