#!/bin/sh
# desktop latency numbers (us): tools/wmlat.sh in the guest, or tools/wmlat.sh build/wmbench.log on the host
# wm_frame: cost of a wm frame that drew something, wm_gap: time between presented frames,
# in2present: input irq -> next frame on screen, in2content: input irq -> frame after a window commit,
# wl_gap / wl_in: the same from samara-wl's side (commit interval, input event -> commit)
if [ -n "$1" ]; then
    tr -d '\r' < "$1" | sed -n '/^\[lat\] begin/h;/^\[lat\] begin/!H;/^\[lat\] end/{x;p;q}' | grep -v '^\[lat\]'
else
    echo reset > /dev/null
    cat /proc/samara/lat
fi
