#!/bin/sh
# x86_64 musl root disk from void: build/root-x64.img (ext2 labelled "/", sparse)
# the kernel merges it into the root at boot (make run ROOTDISK=build/root-x64.img)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/build/x64root
IMG=${1:-$ROOT/build/root-x64.img}
SIZE=${SIZE:-4G}
REPO=${REPO:-https://repo-fastly.voidlinux.org/current/musl}
PKGS="base-files musl busybox coreutils findutils grep sed gawk tar gzip xz bash dash xbps \
      ca-certificates openssl curl wget nano htop dropbear openssh iproute2 file less \
      ncurses-base ncurses procps-ng util-linux shadow iputils iperf3 tcpdump \
      wayland libxkbcommon xkeyboard-config foot xterm xorg-server-xwayland \
      font-misc-misc dejavu-fonts-ttf fontconfig \
      mesa-dri mesa-demos xclock xeyes xcalc xev xclip xsel xdotool wl-clipboard wayland-utils \
      xcursor-themes adwaita-icon-theme gtk+3-demo \
      alsa-utils alsa-lib mpg123 mpv SDL2 \
      runit runit-void cronie e2fsprogs dosfstools strace"

# everything inside a user namespace: files end up owned by root:root without sudo
if [ -z "$IN_NS" ]; then
    IN_NS=1 exec unshare -r sh "$0" "$@"
fi

rm -rf $D
mkdir -p $D/var/db/xbps/keys $ROOT/build/xbps-cache
cp /var/db/xbps/keys/* $D/var/db/xbps/keys/ 2>/dev/null || true
XBPS_ARCH=x86_64-musl xbps-install -S -y -r $D -c $ROOT/build/xbps-cache -R $REPO $PKGS $EXTRA

cd $D
echo samara > etc/hostname
printf 'nameserver 10.0.2.3\n' > etc/resolv.conf
printf '127.0.0.1\tlocalhost\n::1\tlocalhost\n10.0.2.15\tsamara\n10.0.2.2\thost gateway\n' > etc/hosts
# root without a password (dropbear -B), only reachable through the qemu hostfwd
printf 'root::19000:0:99999:7:::\n' > etc/shadow
chmod 600 etc/shadow
cat > usr/lib/os-release <<EOT
NAME="SamaraOS"
ID=samara
ID_LIKE=void
PRETTY_NAME="SamaraOS (void userland)"
HOME_URL="https://github.com/"
EOT
printf 'repository=%s\n' "$REPO" > etc/xbps.d/00-repository-main.conf
mkdir -p var/cache/xbps root etc/dropbear tmp
chmod 1777 tmp
printf 'SamaraOS\n' > etc/issue
# no sysv ipc yet, so no dmix: default straight to the hda
printf 'pcm.!default {\n    type plug\n    slave.pcm "hw:0,0"\n}\nctl.!default {\n    type hw\n    card 0\n}\n' > etc/asound.conf
# mpv asks for float first and our plug path chokes on that
mkdir -p etc/mpv
printf 'ao=alsa\naudio-format=s16\n' > etc/mpv/mpv.conf
# installer (needs the iso for grub images)
sh $ROOT/userland/x64cc.sh -w $ROOT/userland/samara/mkdisk.c -o usr/bin/samara-mkdisk
install -m755 $ROOT/userland/samara-install usr/bin/samara-install
# wayland side of the desktop (userland/samara-wl/build.sh)
[ -x $ROOT/build/samara-wl ] && install -m755 $ROOT/build/samara-wl usr/bin/samara-wl

# fontconfig cache, else every font app scans all fonts at start (works in userns chroot, musl binary)
chroot $D /usr/bin/fc-cache -f 2>/dev/null || echo "fc-cache failed, skipped"
# xkbcomp output cache for Xwayland, must be writable
mkdir -p var/lib/xkb
chmod 1777 var/lib/xkb

rm -f $IMG
truncate -s $SIZE $IMG
mke2fs -q -t ext2 -b 4096 -O ^dir_index,^resize_inode -L / -d $D $IMG
e2fsck -fn $IMG | tail -1
echo "root disk: $IMG"
