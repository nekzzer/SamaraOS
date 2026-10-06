# /deskrun.sh for tools/test/wmbench.sh: runs inside the desktop boot (kernel cmdline "deskrun")
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 HOME=/root
sleep 10
foot >/dev/null 2>&1 &
sleep 8
echo reset > /proc/samara/lat
echo "mark ready" > /proc/samara/lat
sleep 50
echo dump > /proc/samara/lat
sync
poweroff
