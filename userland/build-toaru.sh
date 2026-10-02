#!/bin/sh
# yutani (toaruos compositor) + a few of its demo apps, on top of
# userland/toaru/samara.c. toaru sources: git clone github.com/klange/toaruos
# into toolchain/ports/toaruos. everything goes to the ports disk:
#   ./userland/build-toaru.sh && ./userland/build-ports.sh none
# inside: yutani drawlines   (Ctrl+Alt+Q gets the screen back if it hangs)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin
CC=$X/i686-linux-musl-gcc
T=$ROOT/toolchain/ports/toaruos
S=$ROOT/userland/toaru
B=$ROOT/build/toaru
OUT=$ROOT/build/ports
rm -rf "$B" && mkdir -p "$B/src" "$OUT/bin" "$OUT/lib" "$OUT/share/toaru/fonts" "$OUT/share/toaru/cursor"

LIBS="graphics yutani hashmap list kbd text decorations menu inflate markup markup_text icon_cache"
APPS="yutani drawlines julia"
for f in $LIBS png jpeg; do cp "$T/lib/$f.c" "$B/src/"; done
for f in $APPS; do cp "$T/apps/$f.c" "$B/src/app-$f.c"; done

# our flip/fullscreen live in samara.c
sed -i -e 's/^void flip(gfx_context_t \* ctx) {/void toaru_flip(gfx_context_t * ctx) {/' \
       -e 's/^gfx_context_t \* init_graphics_fullscreen() {/static gfx_context_t * toaru_igf_unused() {/' \
       -e 's/^void reinit_graphics_fullscreen(gfx_context_t \* out) {/static void toaru_rgf_unused(gfx_context_t * out) {/' \
       -e 's/^uint32_t framebuffer_stride(void) {/static uint32_t toaru_fbs_unused(void) {/' "$B/src/graphics.c"
sed -i -e 's|open("/dev/mouse", O_RDONLY \| O_CLOEXEC)|samara_mouse_fd()|' \
       -e 's|open("/dev/kbd", O_RDONLY \| O_CLOEXEC)|samara_kbd_fd()|' \
       -e 's|"/usr/share/cursor/|"/usr/local/share/toaru/cursor/|' "$B/src/app-yutani.c"
sed -i 's|"/usr/share/fonts/default/%s"|"/usr/local/share/toaru/fonts/%s"|' "$B/src/text.c"
grep -q samara_mouse_fd "$B/src/app-yutani.c"

CF="-O2 -msse2 -std=gnu11 -fplan9-extensions -w -I$S/inc -include samara-shim.h"
# libtoaru.so: the libs + glue. apps link against it dynamically
for f in $LIBS; do $CC $CF -fPIC -c "$B/src/$f.c" -o "$B/$f.o"; done
$CC $CF -fPIC -c "$S/samara.c" -o "$B/samara.o"
$CC -shared -o "$B/libtoaru.so" $(for f in $LIBS samara; do echo "$B/$f.o"; done) -lm -lpthread
# png/jpeg are dlopen()ed by load_sprite as libtoaru_<fmt>.so, like on toaru
for f in png jpeg; do
    $CC $CF -fPIC -c "$B/src/$f.c" -o "$B/p-$f.o"
    $CC -shared -o "$B/libtoaru_$f.so" "$B/p-$f.o" -L"$B" -ltoaru -lm
done
for a in $APPS; do
    $CC $CF -c "$B/src/app-$a.c" -o "$B/app-$a.o"
    $CC -o "$B/$a" "$B/app-$a.o" -L"$B" -ltoaru -lm -lpthread
done

cp "$B/libtoaru.so" "$B/libtoaru_png.so" "$B/libtoaru_jpeg.so" "$OUT/lib/"
for a in $APPS; do cp "$B/$a" "$OUT/bin/"; done
$X/i686-linux-musl-strip "$OUT/lib/libtoaru.so" "$OUT/lib/libtoaru_png.so" "$OUT/lib/libtoaru_jpeg.so"
for a in $APPS; do $X/i686-linux-musl-strip "$OUT/bin/$a"; done

# fonts under the names toaru asks for (createramdisk.krk makes the same links)
D=$T/base/usr/share/fonts/truetype/dejavu
cp "$D/DejaVuSans.ttf" "$OUT/share/toaru/fonts/sans-serif"
cp "$D/DejaVuSans-Bold.ttf" "$OUT/share/toaru/fonts/sans-serif.bold"
cp "$D/DejaVuSans-Oblique.ttf" "$OUT/share/toaru/fonts/sans-serif.italic"
cp "$D/DejaVuSans-BoldOblique.ttf" "$OUT/share/toaru/fonts/sans-serif.bolditalic"
cp "$D/DejaVuSansMono.ttf" "$OUT/share/toaru/fonts/monospace"
cp "$D/DejaVuSansMono-Bold.ttf" "$OUT/share/toaru/fonts/monospace.bold"
cp "$D/DejaVuSansMono-Oblique.ttf" "$OUT/share/toaru/fonts/monospace.italic"
cp "$D/DejaVuSansMono-BoldOblique.ttf" "$OUT/share/toaru/fonts/monospace.bolditalic"
cp "$T"/base/usr/share/cursor/*.png "$OUT/share/toaru/cursor/"
cp "$T/LICENSE" "$OUT/share/toaru/LICENSE"
echo "toaru: $APPS -> $OUT (repack: build-ports.sh none)"
