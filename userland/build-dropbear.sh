#!/bin/sh
# Builds dropbear (SSH server + client) as one static i386 musl binary for
# SamaraOS: dropbear, dbclient (= ssh), dropbearkey, scp. build-sysroot.sh
# then puts it into /usr/bin as hard links (run that next).
#
# The kernel side is the pty driver (src/proc/pty.c): /dev/ptmx, /dev/pts/N,
# controlling terminals. /etc/rc starts `dropbear -B` (root, no password) and
# `telnetd -l /bin/login` at boot; QEMU forwards host 127.0.0.1:2222 -> 22 and
# :2323 -> 23 (Makefile NET_DRIVE), so:  ssh -p 2222 root@localhost
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin/i686-linux-musl-
V=${DROPBEAR_VERSION:-2025.88}
D=$ROOT/toolchain/dropbear-$V
cd "$ROOT/toolchain"
[ -f dropbear-$V.tar.bz2 ] || curl -sSLfO https://matt.ucc.asn.au/dropbear/releases/dropbear-$V.tar.bz2
[ -d "$D" ] || tar xjf dropbear-$V.tar.bz2
cd "$D"
cat > localoptions.h <<'OPT'
/* SamaraOS */
#define DEFAULT_PATH "/bin:/sbin:/usr/bin:/usr/sbin:/usr/games:/opt/gcc/bin"
#define DEFAULT_ROOT_PATH DEFAULT_PATH
#define DROPBEAR_PATH_SSH_PROGRAM "/usr/bin/dbclient"
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SFTPSERVER 0
#define DO_MOTD 1
#define MOTD_FILENAME "/etc/motd"
OPT
./configure --host=i686-linux-musl --disable-zlib --disable-syslog --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline \
    --disable-pututxline --enable-static CC=${X}gcc AR=${X}ar RANLIB=${X}ranlib \
    CFLAGS="-O2 -fno-pie" LDFLAGS="-static -no-pie" > /dev/null
make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 -j"$(nproc)" > /dev/null
${X}strip dropbearmulti
ls -l "$D/dropbearmulti"
echo "built dropbear (run userland/build-sysroot.sh next)"
