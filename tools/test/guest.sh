# runs inside samara as autosh, started by tools/test/run.sh. prints "@T name PASS|FAIL ms detail"
export HOME=/root PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
cd /root/t

ms() { read u _ < /proc/uptime; echo ${u%.*}${u#*.}0; }

ONLY=" $* "

t() {
    n=$1; lim=$2; shift 2
    case "$ONLY" in "  ") ;; *" $n "*) ;; *) return ;; esac
    s=$(ms)
    "$@" > /tmp/t.out 2>&1 &
    p=$!
    (sleep $lim; kill -9 $p 2>/dev/null) &
    w=$!
    wait $p; rc=$?
    kill $w 2>/dev/null
    e=$(ms)
    st=PASS; [ $rc = 0 ] || st=FAIL
    echo "@T $n $st $((e - s)) rc=$rc $(tail -n 1 /tmp/t.out | tr -d '\r')"
    [ $rc = 0 ] || sed 's/^/  | /' /tmp/t.out | tail -n 8
}

ALL="fileops fork pthread scm shm inotify statx poll wait4 jitter gcc sync ls chromium glxgears heap"
echo "@PLAN ${*:-$ALL}"

t fileops 60 ./tt fileops
t fork 90 ./tt fork
t pthread 120 ./locktest all 4 20000
t scm 30 ./tt scm
t shm 30 ./tt shm
t inotify 30 ./tt inotify
t statx 30 ./tt statx
t poll 30 ./tt poll
t wait4 30 ./tt wait4
t jitter 60 ./tt jitter

gcc_test() {
    gcc -O2 -o hello hello.c && [ "$(./hello)" = "hello 42" ] || return 1
    gcc -O1 -o tt2 tt.c || return 1
    ./tt2 poll
}
t gcc 240 gcc_test

sync_test() {
    dd if=/dev/zero of=sync.bin bs=1M count=24 2>/dev/null
    s=$(ms); sync; e=$(ms)
    rm -f sync.bin
    echo "sync of 24M dirty: $((e - s)) ms"
}
t sync 60 sync_test

ls_test() {
    n=$(ls -R /usr | wc -l)
    echo "$n lines"
    [ $n -gt 1000 ]
}
t ls 60 ls_test

chr_test() {
    rm -f shot.png
    chromium --headless=new --no-sandbox --disable-gpu-sandbox --no-zygote --disable-dev-shm-usage --screenshot=/root/t/shot.png "data:text/html,<h1>samara</h1>" >/dev/null 2>&1
    [ -s shot.png ] || return 1
    echo "png $(stat -c %s shot.png) bytes, magic $(dd if=shot.png bs=1 skip=1 count=3 2>/dev/null)"
    [ "$(dd if=shot.png bs=1 skip=1 count=3 2>/dev/null)" = PNG ]
}
t chromium 200 chr_test

gl_test() {
    sed -e 's|Driver "fbdev"|Driver "modesetting"|' -e 's|Option "fbdev" "/dev/fb0"|Option "kmsdev" "/dev/dri/card0"\n    Option "AccelMethod" "glamor"|' /etc/X11/xorg.conf > /tmp/ms.conf
    Xorg :1 -config /tmp/ms.conf -nolisten tcp -keeptty -novtswitch vt1 -logfile /tmp/Xorg.1.log >/dev/null 2>&1 &
    xp=$!
    i=0
    while [ $i -lt 70 ] && [ ! -e /tmp/.X11-unix/X1 ]; do sleep 1; i=$((i + 1)); done
    sleep 3
    fps=$(DISPLAY=:1 vblank_mode=0 timeout 20 glxgears 2>&1 | sed -n 's/.*= \([0-9.]*\) FPS.*/\1/p' | tail -n 1)
    kill $xp 2>/dev/null
    echo "glxgears $fps fps (xorg up after ${i}s)"
    [ -n "$fps" ] && [ ${fps%.*} -ge 10 ]
}
t glxgears 150 gl_test

heap_test() {
    echo check > /proc/samara/heap
    grep -q "errors 0" /proc/samara/heap || return 1
    echo selftest > /proc/samara/heap
    grep -q "selftest \([0-9]\)/\1 caught" /proc/samara/heap
}
t heap 30 heap_test
sed 's/^/@H /' /proc/samara/heap | head -n 14
sync
echo "@END"
