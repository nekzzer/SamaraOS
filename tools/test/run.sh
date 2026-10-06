#!/bin/sh
# make test: headless boot on a temp COPY of the root image, runs tools/test/guest.sh inside, prints PASS/FAIL.
# env: ROOTDISK (source image), KEEP=1 keeps the copy, TLIM (vm timeout, s)
cd "$(dirname "$0")/../.." || exit 1
SRC=${ROOTDISK:-build/root-x64.img}
IMG=build/test-root.img
LOG=build/test.log
TLIM=${TLIM:-420}
D=/usr/sbin/debugfs
T0=$(date +%s)

userland/x64cc.sh -Wall -o build/tt tools/test/tt.c || exit 1
userland/x64cc.sh -O2 -o build/locktest tools/locktest.c -lpthread || exit 1

echo "copying $SRC"
rm -f $IMG
cp --sparse=always $SRC $IMG || exit 1
cat > build/test.dbg <<EOT
mkdir /root/t
rm /root/t/tt
rm /root/t/locktest
rm /root/t/guest.sh
rm /root/t/hello.c
rm /root/t/tt.c
write build/tt /root/t/tt
write build/locktest /root/t/locktest
write tools/test/guest.sh /root/t/guest.sh
write tools/test/hello.c /root/t/hello.c
write tools/test/tt.c /root/t/tt.c
sif /root/t/tt mode 0100755
sif /root/t/locktest mode 0100755
EOT
$D -w -f build/test.dbg $IMG >/dev/null 2>&1

echo "booting (limit ${TLIM}s)"
rm -f build/test-disk.img
# setsid: the whole tree (timeout, make, qemu) dies with one kill of the group
setsid /home/eralp/Projects/vmslot.sh timeout $TLIM make run SELF=0 GPU=virgl GCC_MEM=1800 AUDIO= MUSIC_DRIVE= GLIBCDISK= \
    ROOTDISK=$IMG DISK_IMG=build/test-disk.img \
    QMONITOR="-monitor unix:$PWD/build/qm-test,server,nowait" \
    NET_DRIVE="-netdev user,id=n0 -device rtl8139,netdev=n0" QDISPLAY="-display egl-headless" \
    APPEND="autogfx autosh=sh /root/t/guest.sh $*" > $LOG 2>&1 &
vp=$!
while kill -0 $vp 2>/dev/null; do
    sleep 2
    if grep -q "^\[PANIC\]\|^\[autosh done\]\|autosh done" $LOG; then sleep 3; kill -TERM -$vp 2>/dev/null; break; fi
done
wait $vp
vmrc=$?
T1=$(date +%s)

tr -d '\r' < $LOG > build/test.clean
plan=$(sed -n 's/^@PLAN //p' build/test.clean | head -n 1)
echo
echo "== results (vm rc=$vmrc, $((T1 - T0))s so far)"
fails=0
for n in $plan; do
    l=$(grep "^@T $n " build/test.clean | tail -n 1)
    if [ -z "$l" ]; then
        printf "%-10s FAIL  (never finished)\n" $n
        fails=$((fails + 1))
        continue
    fi
    set -- $l
    printf "%-10s %s  %6s ms  %s\n" $2 $3 $4 "$(echo "$l" | cut -d' ' -f6-)"
    [ $3 = PASS ] || fails=$((fails + 1))
done

# things the kernel printed on its own
bad=$(grep -c "^\[heap\]" build/test.clean)
if [ "$bad" -gt 0 ]; then
    echo "kheap      FAIL  $bad [heap] lines on COM1:"
    grep "^\[heap\]" build/test.clean | head -n 8
    fails=$((fails + 1))
else
    echo "kheap      PASS  nothing on COM1"
fi
grep -E "unimplemented syscall|^\[proc\] pid .*(rip=|addr=)" build/test.clean | sort | uniq -c | sort -rn | head -n 8 | sed 's/^/  log: /'
grep "^@H " build/test.clean | sed 's/^@H /  /'
grep -q "^@END" build/test.clean || { echo "guest never reached @END"; fails=$((fails + 1)); }

e2fsck -fn $IMG > build/test.fsck 2>&1
frc=$?
if [ $frc = 0 ]; then echo "e2fsck     PASS"; else echo "e2fsck     FAIL  rc=$frc"; head -n 12 build/test.fsck; fails=$((fails + 1)); fi

[ -n "$KEEP" ] || rm -f $IMG build/test-disk.img
T2=$(date +%s)
if [ $fails = 0 ]; then echo "ALL PASS in $((T2 - T0))s"; exit 0; fi
echo "$fails FAILED in $((T2 - T0))s (log: $LOG)"
exit 1
