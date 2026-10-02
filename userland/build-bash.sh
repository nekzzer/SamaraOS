#!/bin/sh
# Builds GNU bash 5.2 as a static i386 binary for SamaraOS (musl.cc cross).
# Source expected in toolchain/bash-5.2.21.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin

cd "$ROOT/toolchain/bash-5.2.21"
# cross compile: configure cant run tests, so answer the ones that matter by hand
CC=$X/i686-linux-musl-gcc CFLAGS="-O2 -fno-pie -std=gnu17 -Wno-implicit-function-declaration -Wno-int-conversion" \
LDFLAGS="-static -no-pie" \
bash_cv_func_strtoimax=present ac_cv_func_strtoimax=yes ac_cv_func_strtoumax=yes ac_cv_func_strtol=yes ac_cv_func_strtoul=yes ac_cv_func_strtoll=yes ac_cv_func_strtoull=yes \
bash_cv_getcwd_malloc=yes bash_cv_job_control_missing=present \
bash_cv_sys_named_pipes=present bash_cv_func_sigsetjmp=present \
bash_cv_printf_a_format=yes bash_cv_wcontinued_broken=no \
bash_cv_dup2_broken=no bash_cv_pgrp_pipe=no bash_cv_sys_siglist=no \
bash_cv_under_sys_siglist=no bash_cv_unusable_rtsigs=no \
bash_cv_func_strcoll_broken=no bash_cv_getenv_redef=yes \
bash_cv_must_reinstall_sighandlers=no bash_cv_ulimit_maxfds=yes \
./configure --host=i686-linux-musl --build=x86_64-linux-gnu --prefix=/usr \
    --without-bash-malloc --disable-nls --disable-rpath \
    --without-curses --disable-readline-shared \
    --enable-static-link
make -j"$(nproc)"
$X/i686-linux-musl-strip bash
echo "built: $ROOT/toolchain/bash-5.2.21/bash (run userland/build-sysroot.sh next)"
