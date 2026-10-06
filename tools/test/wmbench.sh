#!/bin/sh
# desktop responsiveness benchmark: boots the desktop on a temp copy of the root image, injects mouse/keys
# through the qemu monitor, reads /proc/samara/lat. numbers come out on COM1 (build/wmbench.log)
cd "$(dirname "$0")/../.." || exit 1
SRC=${ROOTDISK:-build/root-x64.img}
IMG=build/wmb-root.img
LOG=build/wmbench.log
D=/usr/sbin/debugfs
[ -f build/samara-wl ] || userland/samara-wl/build.sh || exit 1
rm -f $IMG
cp --sparse=always $SRC $IMG || exit 1
cat > build/wmb.dbg <<EOT
rm /deskrun.sh
write tools/test/deskrun.sh /deskrun.sh
rm /usr/bin/samara-wl
write build/samara-wl /usr/bin/samara-wl
sif /usr/bin/samara-wl mode 0100755
EOT
$D -w -f build/wmb.dbg $IMG >/dev/null 2>&1
rm -f build/qm-wmb build/wmb-disk.img
setsid /home/eralp/Projects/vmslot.sh timeout 300 make run SELF=0 GPU=virgl GCC_MEM=1800 AUDIO= MUSIC_DRIVE= GLIBCDISK= \
    ROOTDISK=$IMG DISK_IMG=build/wmb-disk.img \
    QMONITOR="-monitor unix:$PWD/build/qm-wmb,server,nowait" \
    NET_DRIVE="-netdev user,id=n0 -device rtl8139,netdev=n0" QDISPLAY="-display egl-headless" \
    APPEND="deskrun" > $LOG 2>&1 &
vp=$!
i=0
while ! grep -q "^\[lat\] ready" $LOG 2>/dev/null; do
    sleep 2; i=$((i + 2))
    if [ $i -gt 240 ] || ! kill -0 $vp 2>/dev/null; then echo "desktop never came up"; kill -TERM -$vp 2>/dev/null; exit 1; fi
done
echo "desktop up after ~${i}s, injecting input"
python3 tools/test/wmbench.py build/qm-wmb $PWD/build/wmb-shot.ppm
i=0
while ! grep -q "^\[lat\] end" $LOG 2>/dev/null && [ $i -lt 90 ]; do sleep 2; i=$((i + 2)); done
sleep 3
kill -TERM -$vp 2>/dev/null
wait $vp 2>/dev/null
echo
tools/wmlat.sh $LOG
[ -n "$KEEP" ] || rm -f $IMG build/wmb-disk.img
