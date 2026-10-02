#include <stdio.h>
#include <stdlib.h>
void* kmalloc_big(unsigned n){ return calloc(1,n); }
void kfree(void*p){ free(p); }
void* kmalloc(unsigned n){ return calloc(1,n); }
const unsigned char* font_glyph(unsigned char c){ static unsigned char z[16]; return z; }
void* fs_resolve(void*a,const char*b){return 0;} void* fs_root(void){return 0;} int fs_unlink(void*a,const char*b){return 0;}
/* a tiny framebuffer so pages can be rendered to a picture */
int FW = 1300, FH = 20000;
unsigned* fb;
static int cx0, cy0, cx1, cy1;
void gfx_set_clip(int x, int y, int w, int h) { cx0 = x < 0 ? 0 : x; cy0 = y < 0 ? 0 : y; cx1 = x + w > FW ? FW : x + w; cy1 = y + h > FH ? FH : y + h; if (cx1 < cx0) cx1 = cx0; if (cy1 < cy0) cy1 = cy0; }
void gfx_get_clip(int* x, int* y, int* w, int* h) { *x = cx0; *y = cy0; *w = cx1 - cx0; *h = cy1 - cy0; }
static void px(int x, int y, unsigned c) { if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) fb[y * FW + x] = c; }
static unsigned mixc(unsigned a, unsigned b, int al) {
    int r = ((a >> 16 & 255) * (255 - al) + (b >> 16 & 255) * al) / 255, g = ((a >> 8 & 255) * (255 - al) + (b >> 8 & 255) * al) / 255, bb = ((a & 255) * (255 - al) + (b & 255) * al) / 255;
    return r << 16 | g << 8 | bb;
}
void gfx_rect_fill(int x, int y, int w, int h, unsigned c) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c); }
void gfx_rrect_fill(int x, int y, int w, int h, int r, int corners, unsigned c) { gfx_rect_fill(x, y, w, h, c); }
void gfx_rect(int x, int y, int w, int h, unsigned c) { gfx_rect_fill(x, y, w, 1, c); gfx_rect_fill(x, y + h - 1, w, 1, c); gfx_rect_fill(x, y, 1, h, c); gfx_rect_fill(x + w - 1, y, 1, h, c); }
void gfx_rect_blend(int x, int y, int w, int h, unsigned c, int al) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (i >= cx0 && i < cx1 && j >= cy0 && j < cy1) fb[j * FW + i] = mixc(fb[j * FW + i], c, al); }
void gfx_alpha_mask(int x, int y, int w, int h, const unsigned char* a, int stride, unsigned c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int X = x + i, Y = y + j; if (X >= cx0 && X < cx1 && Y >= cy0 && Y < cy1 && a[j * stride + i]) fb[Y * FW + X] = mixc(fb[Y * FW + X], c, a[j * stride + i]); }
}
void gfx_disc(int cx, int cy, int r, unsigned c) { for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) px(cx + i, cy + j, c); }
void gfx_circle(int cx, int cy, int r, unsigned c) { for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) { int d = i * i + j * j; if (d <= r * r && d >= (r - 1) * (r - 1)) px(cx + i, cy + j, c); } }
void gfx_blit_argb(int x, int y, int w, int h, const unsigned* s) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, s[j * w + i]); }
void gfx_glyph(){}
int gfx_h(void){return 900;} int gfx_w(void){return 1600;} void gfx_line(){} int gfx_ready(void){return 1;}
void* fs_create(){return 0;} int fs_append(){return 0;}
int kbd_is_ru(void){return 0;} void mouse_get(){} unsigned pit_uptime_ms(void){return 0;}
int proc_alive(int p){return 0;} void* proc_by_pid(int p){return 0;} void proc_send_signal(){} int proc_spawn_detached(){return -1;}
void wm_client_rect(){} void wm_close(){} void* wm_open_app_ex(){return 0;}
int h_run(const char* html, int n, int w, int dump);
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); static char buf[2<<20]; int n = fread(buf,1,sizeof buf,f); fclose(f);
    int w = argc > 2 ? atoi(argv[2]) : 940, d = argc > 3 ? atoi(argv[3]) : 60;
    fb = calloc(FW * FH, 4);
    h_run(buf, n, w, d);
    if (getenv("BT_ID")) { extern void h_why(const char*); h_why(getenv("BT_ID")); }
    if (argc > 4) {
        extern int h_paint(int);
        int hh = h_paint(FW);
        if (hh > FH) hh = FH;
        FILE* o = fopen(argv[4], "wb");
        fprintf(o, "P6 %d %d 255\n", FW, hh);
        for (int i = 0; i < FW * hh; i++) { fputc(fb[i] >> 16 & 255, o); fputc(fb[i] >> 8 & 255, o); fputc(fb[i] & 255, o); }
        fclose(o);
    }
    return 0;
}
