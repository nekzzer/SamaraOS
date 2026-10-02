#!/bin/sh
# Bigger programs for SamaraOS, static i386 musl: Lua 5.4, zlib, git, vim,
# CPython 3.12. They don't fit into the kernel image, so they go onto an
# ext2 disk labelled "/usr/local" (the kernel mounts it by its label):
#
#   ./userland/build-ports.sh            -> build/ports.img
#   make run SELF=0 VDISK=build/ports.img
#
# Sources in toolchain/ports/ (lua-5.4.7, zlib-1.3.1, git-2.46.2,
# vim-9.1.0750, Python-3.12.7, htop-3.3.0, fastfetch-2.21.3). Pass names
# to build only some: lua git ...
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
CC=$X/i686-linux-musl-gcc
P=$ROOT/toolchain/ports
OUT=$ROOT/build/ports             # becomes /usr/local
Z=$P/zlib-out                     # static zlib for git and python
NC=$ROOT/toolchain/ncurses-out/usr
J=$(nproc)
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share"
want() { [ -z "$ONLY" ] || echo " $ONLY " | grep -q " $1 "; }
ONLY="$*"

if want zlib || want git || want python; then
    if [ ! -f "$Z/lib/libz.a" ]; then
        cd "$P/zlib-1.3.1"
        CC=$CC CFLAGS="-O2" ./configure --static --prefix="$Z" >/dev/null
        make -j$J >/dev/null && make install >/dev/null
    fi
fi

if want lua; then
    cd "$P/lua-5.4.7"
    make -j$J CC="$CC -std=gnu99" MYCFLAGS="-DLUA_USE_POSIX" MYLDFLAGS="-static" MYLIBS="" linux-noreadline >/dev/null 2>&1 || \
    make -j$J CC="$CC -std=gnu99" MYCFLAGS="-DLUA_USE_POSIX" MYLDFLAGS="-static" MYLIBS="" posix
    cp src/lua src/luac "$OUT/bin/"
    $X/i686-linux-musl-strip "$OUT/bin/lua" "$OUT/bin/luac"
fi

if want git; then
    cd "$P/git-2.46.2"
    # no curl/expat/perl/python/tcl: local repos, git clone over file:// and ssh
    make -j$J CC=$CC AR=$X/i686-linux-musl-ar uname_S=Linux uname_M=i686 \
        CFLAGS="-O2 -I$Z/include" LDFLAGS="-static -L$Z/lib" \
        NO_CURL=1 NO_EXPAT=1 NO_PERL=1 NO_PYTHON=1 NO_TCLTK=1 NO_GETTEXT=1 NO_OPENSSL=1 \
        NO_ICONV=1 NO_REGEX=NeedsStartEnd NO_INSTALL_HARDLINKS=1 NO_UNIX_SOCKETS=1 \
        prefix=/usr/local gitexecdir=/usr/local/libexec/git-core \
        DESTDIR="$ROOT/build/ports-git" install >/dev/null
    mkdir -p "$OUT/libexec"
    rm -rf "$OUT/libexec/git-core"
    cp -a "$ROOT/build/ports-git/usr/local/bin/git" "$OUT/bin/"
    cp -a "$ROOT/build/ports-git/usr/local/libexec/git-core" "$OUT/libexec/"
    # git-core has ~140 names for the same binary: keep them as symlinks
    cd "$OUT/libexec/git-core"
    for f in *; do if cmp -s "$f" "$OUT/bin/git" 2>/dev/null; then rm "$f"; ln -s ../../bin/git "$f"; fi; done
    $X/i686-linux-musl-strip "$OUT/bin/git" 2>/dev/null || true
    for f in git-*; do [ -L "$f" ] || $X/i686-linux-musl-strip "$f" 2>/dev/null || true; done
    mkdir -p "$OUT/share/git-core/templates"
fi

if want vim; then
    cd "$P/vim-9.1.0750/src"
    [ -f auto/config.mk ] || CC=$CC CFLAGS="-O2 -fno-pie -I$NC/include" LDFLAGS="-static -no-pie -L$NC/lib" \
        vim_cv_toupper_broken=no vim_cv_terminfo=yes vim_cv_tgetent=zero vim_cv_getcwd_broken=no \
        vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes vim_cv_bcopy_handles_overlap=yes \
        vim_cv_memcpy_handles_overlap=yes \
        ./configure --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr/local \
        --with-features=normal --disable-gui --without-x --disable-nls --disable-netbeans \
        --disable-channel --disable-terminal --disable-xsmp --disable-gpm --disable-sysmouse \
        --with-tlib=ncurses --enable-multibyte >/dev/null
    # osdef.h redeclares libc/termcap functions with old prototypes: not needed with musl
    make auto/osdef.h >/dev/null 2>&1; : > auto/osdef.h
    make -j$J >/dev/null
    cp vim "$OUT/bin/vim"
    $X/i686-linux-musl-strip "$OUT/bin/vim"
    ln -sf vim "$OUT/bin/vi"
    mkdir -p "$OUT/share/vim"
    rm -rf "$OUT/share/vim/vim91"
    cp -r ../runtime "$OUT/share/vim/vim91"
    rm -rf "$OUT/share/vim/vim91/doc" "$OUT/share/vim/vim91/lang" "$OUT/share/vim/vim91/spell" "$OUT/share/vim/vim91/tutor"
    printf 'set nocompatible\nsyntax on\nset ruler\nset backspace=indent,eol,start\n' > "$OUT/share/vim/vimrc"
fi

if want python; then
    PY=$P/Python-3.12.7
    HOSTPY=$P/python-host
    # cross builds need the same version on the build machine
    if [ ! -x "$HOSTPY/bin/python3.12" ]; then
        rm -rf "$P/py-host-build" && mkdir -p "$P/py-host-build" && cd "$P/py-host-build"
        "$PY/configure" --prefix="$HOSTPY" >/dev/null && make -j$J >/dev/null && make install >/dev/null
    fi
    rm -rf "$P/py-cross" && mkdir -p "$P/py-cross" && cd "$P/py-cross"
    # everything static: extension modules are built into the interpreter
    rm -f "$PY/Modules/Setup.local"
    # host pkg-config would hand us the build machine's zlib/bz2: off; what
    # we have no library for is marked missing
    CONFIG_SITE=/dev/null ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no PKG_CONFIG=false \
    py_cv_module__bz2=n/a py_cv_module__lzma=n/a py_cv_module__ssl=n/a py_cv_module__hashlib=n/a \
    py_cv_module__sqlite3=n/a py_cv_module__tkinter=n/a py_cv_module__dbm=n/a py_cv_module__gdbm=n/a \
    py_cv_module__uuid=n/a py_cv_module_readline=n/a py_cv_module__ctypes=n/a py_cv_module__curses=n/a \
    py_cv_module__curses_panel=n/a py_cv_module_nis=n/a py_cv_module__crypt=n/a \
    ZLIB_CFLAGS="-I$Z/include" ZLIB_LIBS="-L$Z/lib -lz" \
    CC=$CC AR=$X/i686-linux-musl-ar READELF=$X/i686-linux-musl-readelf \
    CFLAGS="-O2" CPPFLAGS="-I$Z/include" LDFLAGS="-static -L$Z/lib" LINKFORSHARED=" " \
    MODULE_BUILDTYPE=static \
    "$PY/configure" --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr/local \
        --with-build-python="$HOSTPY/bin/python3.12" --disable-shared --disable-ipv6 \
        --without-ensurepip --disable-test-modules >/dev/null
    make -j$J LINKFORSHARED=" " >/dev/null 2>"$P/py-cross/make.err" || { tail -30 "$P/py-cross/make.err"; exit 1; }
    make install DESTDIR="$ROOT/build/ports-py" >/dev/null 2>&1 || true
    cp "$P/py-cross/python" "$OUT/bin/python3.12"
    $X/i686-linux-musl-strip "$OUT/bin/python3.12"
    ln -sf python3.12 "$OUT/bin/python3"
    rm -rf "$OUT/lib/python3.12"
    cp -r "$ROOT/build/ports-py/usr/local/lib/python3.12" "$OUT/lib/"
    rm -rf "$OUT/lib/python3.12/test" "$OUT/lib/python3.12/idlelib" "$OUT/lib/python3.12/tkinter" \
           "$OUT/lib/python3.12/turtledemo" "$OUT/lib/python3.12/ensurepip" "$OUT/lib/python3.12/lib2to3"
    find "$OUT/lib/python3.12" -name __pycache__ -prune -exec rm -rf {} +
fi

# htop and fastfetch: dynamic (ld-musl + /lib/libc.so from the sysroot),
# ncurses still static. sources: htop-3.3.0, fastfetch-2.21.3 from github
if want htop; then
    cd "$P/htop-3.3.0"
    # ncurses.a is not PIC: no-pie, else textrels and ld-musl segfaults on them
    [ -f Makefile ] || CC=$CC CFLAGS="-O2 -fno-pie -I$NC/include" LDFLAGS="-no-pie -L$NC/lib" \
        HTOP_NCURSES_CONFIG_SCRIPT=false PKG_CONFIG=false \
        ./configure --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr/local \
        --disable-unicode --disable-sensors --disable-capabilities --disable-delayacct \
        --disable-hwloc --disable-affinity --disable-unwind >/dev/null
    make -j$J >/dev/null
    cp htop "$OUT/bin/"
    $X/i686-linux-musl-strip "$OUT/bin/htop"
fi

if want fastfetch; then
    cd "$P/fastfetch-2.21.3"
    # no X/wayland/dbus/gpu libs here, all off. dlopen would fail anyway
    OFF=""
    for o in VULKAN WAYLAND XCB_RANDR XCB XRANDR X11 DRM GIO DCONF DBUS XFCONF SQLITE3 RPM \
        IMAGEMAGICK7 IMAGEMAGICK6 CHAFA ZLIB EGL GLX OSMESA OPENCL FREETYPE PULSE DDCUTIL \
        DIRECTX_HEADERS ELF THREADS LTO; do OFF="$OFF -DENABLE_$o=OFF"; done
    mkdir -p build-i686 && cd build-i686
    [ -f build.ninja ] || cmake .. -G Ninja -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=i686 \
        -DCMAKE_C_COMPILER=$CC -DCMAKE_BUILD_TYPE=Release -DIS_MUSL=ON \
        -DCMAKE_INSTALL_PREFIX=/usr/local -DPKG_CONFIG_EXECUTABLE=/bin/false $OFF >/dev/null
    ninja fastfetch >/dev/null
    cp fastfetch "$OUT/bin/"
    $X/i686-linux-musl-strip "$OUT/bin/fastfetch"
fi

# the disk: ext2, 4k blocks, labelled with where it goes
IMG=$ROOT/build/ports.img
SZ=$(du -sm "$OUT" | cut -f1)
SZ=$((SZ + SZ / 2 + 64))
rm -f "$IMG"
truncate -s ${SZ}M "$IMG"
mke2fs -q -F -t ext2 -b 4096 -L /usr/local -d "$OUT" -E root_owner=0:0 "$IMG"
echo "built: $IMG (${SZ} MB). make run SELF=0 VDISK=build/ports.img"
