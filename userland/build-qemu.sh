#!/bin/sh
# QEMU for SamaraOS. WIP: i386-softmmu, tcg only, -nographic / curses,
# fully static (glib, pixman etc. are .a), goes to the ports disk:
#
#   ./userland/build-qemu.sh && ./userland/build-ports.sh lua   # any name, just repacks the img
#
# Deps get built into toolchain/qemu-deps (prefix /usr inside):
# libffi, pcre2, glib (needs meson, from a venv), pixman.
# qemu 8.2: last one that doesn't nag about 32-bit hosts, has its own meson.
#
# Kernel side, what qemu wants:
#  - threads (clone CLONE_VM|CLONE_THREAD), futex: have them
#  - eventfd/signalfd: missing, qemu falls back to pipes + a signal thread on ENOSYS
#  - coroutines: musl has no ucontext, so --with-coroutine=sigaltstack
#    (sigaltstack + SIGUSR2 on the alt stack, check that it really works)
#  - big mmap for the tcg buffer (-accel tcg,tb-size=16 to keep it small)
#  - TODO: timerfd, memfd_create, display backend over samara.h windows
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
CC=$X/i686-linux-musl-gcc
P=$ROOT/toolchain/ports
D=$ROOT/toolchain/qemu-deps
Z=$P/zlib-out
NC=$ROOT/toolchain/ncurses-out/usr
OUT=$ROOT/build/ports
J=$(nproc)
mkdir -p "$P" "$D"
cd "$P"

get() { [ -f "$(basename "$1")" ] || curl -sLO "$1"; }
get https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz
get https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.bz2
get https://download.gnome.org/sources/glib/2.80/glib-2.80.5.tar.xz
get https://cairographics.org/releases/pixman-0.43.4.tar.gz
get https://download.qemu.org/qemu-8.2.7.tar.xz

# meson + ninja for the deps (qemu brings its own meson)
VENV=$ROOT/toolchain/meson-venv
[ -x "$VENV/bin/meson" ] || { python3 -m venv "$VENV" && "$VENV/bin/pip" -q install meson==1.5.2 packaging distlib; }
export PATH=$VENV/bin:$PATH

cat > "$D/cross.txt" <<EOF
[binaries]
c = '$CC'
cpp = '$X/i686-linux-musl-g++'
ar = '$X/i686-linux-musl-ar'
strip = '$X/i686-linux-musl-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-O2', '-I$D/usr/include', '-I$Z/include']
c_link_args = ['-L$D/usr/lib', '-L$Z/lib']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
# only our .pc files, never the host's
export PKG_CONFIG_LIBDIR=$D/usr/lib/pkgconfig:$Z/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=
unset PKG_CONFIG_PATH

if [ ! -f "$D/usr/lib/libffi.a" ]; then
    rm -rf libffi-3.4.6 && tar xf libffi-3.4.6.tar.gz && cd libffi-3.4.6
    CC=$CC CFLAGS="-O2" ./configure --host=i686-linux-musl --build=x86_64-linux-gnu \
        --prefix=/usr --disable-shared --enable-static --disable-docs >/dev/null
    make -j$J >/dev/null && make install DESTDIR="$D" >/dev/null
    cd "$P"
fi

if [ ! -f "$D/usr/lib/libpcre2-8.a" ]; then
    rm -rf pcre2-10.44 && tar xf pcre2-10.44.tar.bz2 && cd pcre2-10.44
    CC=$CC CFLAGS="-O2" ./configure --host=i686-linux-musl --build=x86_64-linux-gnu \
        --prefix=/usr --disable-shared --enable-static >/dev/null
    make -j$J >/dev/null && make install DESTDIR="$D" >/dev/null
    cd "$P"
fi

# fix .pc prefixes to point into $D
fixpc() { sed -i "s|^prefix=/usr\$|prefix=$D/usr|" "$D"/usr/lib/pkgconfig/*.pc; }
fixpc

if [ ! -f "$D/usr/lib/libglib-2.0.a" ]; then
    rm -rf glib-2.80.5 && tar xf glib-2.80.5.tar.xz && cd glib-2.80.5
    meson setup b --cross-file "$D/cross.txt" --prefix=/usr --default-library=static \
        -Dtests=false -Dintrospection=disabled -Dnls=disabled -Dselinux=disabled \
        -Dxattr=false -Dlibmount=disabled -Dman-pages=disabled -Dsysprof=disabled \
        -Ddocumentation=false -Dglib_debug=disabled >/dev/null
    ninja -C b >/dev/null && DESTDIR="$D" ninja -C b install >/dev/null
    fixpc
    cd "$P"
fi

if [ ! -f "$D/usr/lib/libpixman-1.a" ]; then
    rm -rf pixman-0.43.4 && tar xf pixman-0.43.4.tar.gz && cd pixman-0.43.4
    # no sse2/mmx: kernel saves fxsr state, but keep it dumb for now
    meson setup b --cross-file "$D/cross.txt" --prefix=/usr --default-library=static \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
        -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled >/dev/null
    ninja -C b >/dev/null && DESTDIR="$D" ninja -C b install >/dev/null
    fixpc
    cd "$P"
fi

[ -d qemu-8.2.7 ] || tar xf qemu-8.2.7.tar.xz
cd qemu-8.2.7
mkdir -p b-samara && cd b-samara
[ -f build.ninja ] || PKG_CONFIG=pkg-config ../configure --cross-prefix=$X/i686-linux-musl- --cpu=i386 --static \
    --prefix=/usr/local --target-list=i386-softmmu \
    --extra-cflags="-O2 -I$D/usr/include -I$Z/include -I$NC/include" \
    --extra-ldflags="-no-pie -L$D/usr/lib -L$Z/lib -L$NC/lib" \
    --with-coroutine=sigaltstack --enable-tcg --disable-kvm --disable-xen --disable-werror \
    --disable-docs --disable-tools --disable-guest-agent --disable-user --disable-sdl --disable-gtk \
    --disable-vnc --disable-opengl --disable-spice --disable-usb-redir --disable-libusb \
    --disable-slirp --disable-curl --disable-gnutls --disable-nettle --disable-gcrypt \
    --disable-linux-aio --disable-linux-io-uring --disable-cap-ng --disable-attr --disable-seccomp \
    --disable-vhost-user --disable-vhost-net --disable-vhost-kernel --disable-vhost-crypto \
    --disable-vhost-vdpa --disable-plugins --disable-debug-info --disable-libudev \
    --disable-alsa --disable-pa --disable-oss --disable-jack --disable-sndio --disable-pipewire \
    --disable-curses --disable-iconv --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy \
    --disable-libssh --disable-bpf --disable-fuse --disable-tpm --disable-membarrier --disable-dbus-display \
    --audio-drv-list= --disable-pie
# TODO: --enable-curses with our narrow ncurses (qemu wants ncursesw), slirp for -net user
ninja -j$J qemu-system-i386
cp qemu-system-i386 "$OUT/bin/"
$X/i686-linux-musl-strip "$OUT/bin/qemu-system-i386"
# bios + vga roms, without them it won't even start
mkdir -p "$OUT/share/qemu"
for f in bios-256k.bin bios.bin vgabios-stdvga.bin vgabios.bin kvmvapic.bin linuxboot.bin linuxboot_dma.bin \
         multiboot.bin multiboot_dma.bin efi-e1000.rom; do
    [ -f ../pc-bios/$f ] && cp ../pc-bios/$f "$OUT/share/qemu/"
done
cp pc-bios/*.bin "$OUT/share/qemu/" 2>/dev/null || true
echo "qemu: $OUT/bin/qemu-system-i386 (repack the ports disk)"
