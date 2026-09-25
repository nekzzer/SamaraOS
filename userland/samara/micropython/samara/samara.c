// MicroPython `samara` module: desktop windows for SamaraOS (syscall 500).
// Mirrors userland/samara/samara.h. The pixel buffer lives in the program
// (system malloc, freed by the finaliser) and is copied by present().
//
//   import samara
//   w = samara.Window(320, 200, scale=2, title="Hi")
//   while True:
//       e = w.ev()
//       if e and e[0] == samara.EV_CLOSE: break
//       w.clear(samara.BLACK); w.text(10, 10, "Привет", samara.WHITE)
//       w.present(); samara.sleep_ms(16)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objstr.h"

#define SYS_SAMARA 500
enum {
    OP_OPEN = 1, OP_PRESENT, OP_EVENT, OP_CLOSE, OP_TEXT, OP_KEYS, OP_MOUSE, OP_TITLE, OP_FONT_H
};

typedef struct { int32_t w, h, scale, flags; uint32_t title; } sm_open_t;
typedef struct { int32_t type, a, b, c; } sm_event_t;
typedef struct { uint32_t buf; int32_t bw, bh, x, y, font; uint32_t color, str; } sm_text_t;

// Returns the raw result or -errno; retries/raises on EINTR (Ctrl+C).
static long sm_call(long op, long a, long b, long c, bool retry_eintr) {
    for (;;) {
        long r = syscall(SYS_SAMARA, op, a, b, c);
        if (r != -1) {
            return r;
        }
        int err = errno;
        if (err == EINTR) {
            mp_handle_pending(true);     // KeyboardInterrupt if SIGINT
            if (retry_eintr) {
                continue;
            }
        }
        return -err;
    }
}

typedef struct {
    mp_obj_base_t base;
    int handle;
    int w, h, scale;
    uint32_t *pix;
    bool closed;
} sm_win_obj_t;

static const mp_obj_type_t samara_window_type;

static sm_win_obj_t *win_of(mp_obj_t o) {
    return MP_OBJ_TO_PTR(o);
}

static inline void px_set(sm_win_obj_t *w, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)w->w && (unsigned)y < (unsigned)w->h) {
        w->pix[y * w->w + x] = c;
    }
}

static void fill_rect(sm_win_obj_t *w, int x, int y, int rw, int rh, uint32_t c) {
    int x1 = x + rw, y1 = y + rh;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x1 > w->w) {
        x1 = w->w;
    }
    if (y1 > w->h) {
        y1 = w->h;
    }
    for (int j = y; j < y1; j++) {
        uint32_t *p = w->pix + j * w->w;
        for (int i = x; i < x1; i++) {
            p[i] = c;
        }
    }
}

// ---- constructor / finaliser ----

static mp_obj_t win_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_w, ARG_h, ARG_scale, ARG_title };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_scale, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_title, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    int w = args[ARG_w].u_int, h = args[ARG_h].u_int, scale = args[ARG_scale].u_int;
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 8) {
        scale = 8;
    }
    const char *title = args[ARG_title].u_obj == mp_const_none ? "MicroPython"
                                                               : mp_obj_str_get_str(args[ARG_title].u_obj);
    sm_open_t o = { w, h, scale, 0, (uint32_t)(uintptr_t)title };
    long r = sm_call(OP_OPEN, (long)&o, 0, 0, false);
    if (r < 0) {
        if (r == -ENODEV) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no desktop (run `desktop` first)"));
        }
        mp_raise_OSError((int)-r);
    }
    uint32_t *pix = calloc((size_t)w * h, 4);
    if (!pix) {
        sm_call(OP_CLOSE, r, 0, 0, false);
        mp_raise_OSError(ENOMEM);
    }
    sm_win_obj_t *self = mp_obj_malloc_with_finaliser(sm_win_obj_t, type);
    self->handle = (int)r;
    self->w = w;
    self->h = h;
    self->scale = scale;
    self->pix = pix;
    self->closed = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t win_close(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    if (!self->closed) {
        self->closed = true;
        sm_call(OP_CLOSE, self->handle, 0, 0, false);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_close_obj, win_close);

static mp_obj_t win_del(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    win_close(self_in);
    free(self->pix);
    self->pix = NULL;
    self->w = self->h = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_del_obj, win_del);

static mp_obj_t win_enter(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_enter_obj, win_enter);

static mp_obj_t win_exit(size_t n_args, const mp_obj_t *args) {
    return win_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_exit_obj, 4, 4, win_exit);

// ---- drawing (pure C on the buffer) ----

static mp_obj_t win_clear(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    uint32_t c = n_args > 1 ? (uint32_t)mp_obj_get_int_truncated(args[1]) : 0;
    fill_rect(self, 0, 0, self->w, self->h, c);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_clear_obj, 1, 2, win_clear);

static mp_obj_t win_pixel(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    if (n_args == 3) {
        if ((unsigned)x < (unsigned)self->w && (unsigned)y < (unsigned)self->h) {
            return mp_obj_new_int_from_uint(self->pix[y * self->w + x]);
        }
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    px_set(self, x, y, (uint32_t)mp_obj_get_int_truncated(args[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_pixel_obj, 3, 4, win_pixel);

static mp_obj_t win_rect(size_t n_args, const mp_obj_t *args) {
    fill_rect(win_of(args[0]), mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
        mp_obj_get_int(args[3]), mp_obj_get_int(args[4]), (uint32_t)mp_obj_get_int_truncated(args[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_rect_obj, 6, 6, win_rect);

static mp_obj_t win_frame(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    int rw = mp_obj_get_int(args[3]), rh = mp_obj_get_int(args[4]);
    uint32_t c = (uint32_t)mp_obj_get_int_truncated(args[5]);
    if (rw > 0 && rh > 0) {
        fill_rect(self, x, y, rw, 1, c);
        fill_rect(self, x, y + rh - 1, rw, 1, c);
        fill_rect(self, x, y, 1, rh, c);
        fill_rect(self, x + rw - 1, y, 1, rh, c);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_frame_obj, 6, 6, win_frame);

static mp_obj_t win_line(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    int x0 = mp_obj_get_int(args[1]), y0 = mp_obj_get_int(args[2]);
    int x1 = mp_obj_get_int(args[3]), y1 = mp_obj_get_int(args[4]);
    uint32_t c = (uint32_t)mp_obj_get_int_truncated(args[5]);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 20000; guard++) {
        px_set(self, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_line_obj, 6, 6, win_line);

static mp_obj_t win_circle(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    int cx = mp_obj_get_int(args[1]), cy = mp_obj_get_int(args[2]), r = mp_obj_get_int(args[3]);
    uint32_t c = (uint32_t)mp_obj_get_int_truncated(args[4]);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px_set(self, cx + x, cy + y, c);
        px_set(self, cx - x, cy + y, c);
        px_set(self, cx + x, cy - y, c);
        px_set(self, cx - x, cy - y, c);
        px_set(self, cx + y, cy + x, c);
        px_set(self, cx - y, cy + x, c);
        px_set(self, cx + y, cy - x, c);
        px_set(self, cx - y, cy - x, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_circle_obj, 5, 5, win_circle);

static mp_obj_t win_disc(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    int cx = mp_obj_get_int(args[1]), cy = mp_obj_get_int(args[2]), r = mp_obj_get_int(args[3]);
    uint32_t c = (uint32_t)mp_obj_get_int_truncated(args[4]);
    int x = r;
    for (int y = 0; y <= r; y++) {
        while (x > 0 && x * x + y * y > r * r) {
            x--;
        }
        fill_rect(self, cx - x, cy + y, 2 * x + 1, 1, c);
        if (y) {
            fill_rect(self, cx - x, cy - y, 2 * x + 1, 1, c);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_disc_obj, 5, 5, win_disc);

// text(x, y, s, color=0, font=FONT_REG) -> pen x
static mp_obj_t win_text(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    if (!self->pix) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    sm_text_t t;
    t.buf = (uint32_t)(uintptr_t)self->pix;
    t.bw = self->w;
    t.bh = self->h;
    t.x = mp_obj_get_int(args[1]);
    t.y = mp_obj_get_int(args[2]);
    t.str = (uint32_t)(uintptr_t)mp_obj_str_get_str(args[3]);
    t.color = n_args > 4 ? (uint32_t)mp_obj_get_int_truncated(args[4]) : 0;
    t.font = n_args > 5 ? mp_obj_get_int(args[5]) : 0;
    return MP_OBJ_NEW_SMALL_INT(sm_call(OP_TEXT, (long)&t, 0, 0, true));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_text_obj, 4, 6, win_text);

// ---- screen / input ----

static mp_obj_t win_present(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    if (self->closed || !self->pix) {
        mp_raise_OSError(EPIPE);
    }
    long r = sm_call(OP_PRESENT, self->handle, (long)self->pix, 0, true);
    if (r == -EPIPE || r == -EBADF) {
        mp_raise_OSError(EPIPE);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_present_obj, win_present);

// ev(block=False): False -> poll, True -> wait, int -> timeout in ms.
static mp_obj_t win_ev(size_t n_args, const mp_obj_t *args) {
    sm_win_obj_t *self = win_of(args[0]);
    if (self->closed) {
        return mp_const_none;
    }
    long timeout = 0;
    if (n_args > 1) {
        if (args[1] == mp_const_true) {
            timeout = -1;
        } else if (args[1] == mp_const_false || args[1] == mp_const_none) {
            timeout = 0;
        } else {
            timeout = mp_obj_get_int(args[1]);
        }
    }
    sm_event_t e;
    long r = sm_call(OP_EVENT, self->handle, (long)&e, timeout, true);
    if (r <= 0) {
        return mp_const_none;
    }
    mp_obj_t t[4] = {
        MP_OBJ_NEW_SMALL_INT(e.type), MP_OBJ_NEW_SMALL_INT(e.a),
        MP_OBJ_NEW_SMALL_INT(e.b), MP_OBJ_NEW_SMALL_INT(e.c),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_ev_obj, 1, 2, win_ev);

static mp_obj_t win_keys(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    uint8_t bits[32];
    memset(bits, 0, sizeof bits);
    if (!self->closed) {
        sm_call(OP_KEYS, self->handle, (long)bits, 0, true);
    }
    return mp_obj_new_bytes(bits, sizeof bits);
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_keys_obj, win_keys);

// key(code) -> True while the key (Linux code, SC_*) is held
static mp_obj_t win_key(mp_obj_t self_in, mp_obj_t code_in) {
    sm_win_obj_t *self = win_of(self_in);
    int code = mp_obj_get_int(code_in);
    uint8_t bits[32];
    memset(bits, 0, sizeof bits);
    if (!self->closed) {
        sm_call(OP_KEYS, self->handle, (long)bits, 0, true);
    }
    return mp_obj_new_bool(code > 0 && code < 256 && ((bits[code >> 3] >> (code & 7)) & 1));
}
static MP_DEFINE_CONST_FUN_OBJ_2(win_key_obj, win_key);

static mp_obj_t win_mouse(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    int32_t m[3] = { -1, -1, 0 };
    if (!self->closed) {
        sm_call(OP_MOUSE, self->handle, (long)m, 0, true);
    }
    mp_obj_t t[3] = { MP_OBJ_NEW_SMALL_INT(m[0]), MP_OBJ_NEW_SMALL_INT(m[1]), MP_OBJ_NEW_SMALL_INT(m[2]) };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_mouse_obj, win_mouse);

static mp_obj_t win_title(mp_obj_t self_in, mp_obj_t s) {
    sm_win_obj_t *self = win_of(self_in);
    if (!self->closed) {
        sm_call(OP_TITLE, self->handle, (long)mp_obj_str_get_str(s), 0, true);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(win_title_obj, win_title);

// buffer() -> bytearray aliasing the XRGB pixels (little-endian B,G,R,X)
static mp_obj_t win_buffer(mp_obj_t self_in) {
    sm_win_obj_t *self = win_of(self_in);
    return mp_obj_new_bytearray_by_ref((size_t)self->w * self->h * 4, self->pix);
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_buffer_obj, win_buffer);

static void win_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    sm_win_obj_t *self = win_of(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_width) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(self->w);
            return;
        }
        if (attr == MP_QSTR_height) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(self->h);
            return;
        }
        if (attr == MP_QSTR_scale) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(self->scale);
            return;
        }
        if (attr == MP_QSTR_closed) {
            dest[0] = mp_obj_new_bool(self->closed);
            return;
        }
        dest[1] = MP_OBJ_SENTINEL;       // fall back to locals_dict
    }
}

static const mp_rom_map_elem_t win_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&win_del_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&win_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&win_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&win_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&win_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&win_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&win_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&win_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&win_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&win_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&win_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_disc), MP_ROM_PTR(&win_disc_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&win_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&win_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_ev), MP_ROM_PTR(&win_ev_obj) },
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&win_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&win_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse), MP_ROM_PTR(&win_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_title), MP_ROM_PTR(&win_title_obj) },
    { MP_ROM_QSTR(MP_QSTR_buffer), MP_ROM_PTR(&win_buffer_obj) },
};
static MP_DEFINE_CONST_DICT(win_locals_dict, win_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    samara_window_type,
    MP_QSTR_Window,
    MP_TYPE_FLAG_NONE,
    make_new, win_make_new,
    attr, win_attr,
    locals_dict, &win_locals_dict
    );

// ---- module functions ----

static mp_obj_t mod_rgb(mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    uint32_t c = ((mp_obj_get_int(r) & 0xFF) << 16) | ((mp_obj_get_int(g) & 0xFF) << 8) | (mp_obj_get_int(b) & 0xFF);
    return MP_OBJ_NEW_SMALL_INT(c);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_rgb_obj, mod_rgb);

static mp_obj_t mod_ticks_ms(void) {
    return mp_obj_new_int_from_uint(mp_hal_ticks_ms());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ticks_ms_obj, mod_ticks_ms);

static mp_obj_t mod_sleep_ms(mp_obj_t ms) {
    mp_int_t v = mp_obj_get_int(ms);
    if (v > 0) {
        mp_hal_delay_ms(v);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_sleep_ms_obj, mod_sleep_ms);

static mp_obj_t mod_text_width(size_t n_args, const mp_obj_t *args) {
    sm_text_t t;
    memset(&t, 0, sizeof t);
    t.str = (uint32_t)(uintptr_t)mp_obj_str_get_str(args[0]);
    t.font = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    return MP_OBJ_NEW_SMALL_INT(sm_call(OP_TEXT, (long)&t, 0, 0, true));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_text_width_obj, 1, 2, mod_text_width);

static mp_obj_t mod_font_height(size_t n_args, const mp_obj_t *args) {
    int font = n_args ? mp_obj_get_int(args[0]) : 0;
    return MP_OBJ_NEW_SMALL_INT(sm_call(OP_FONT_H, font, 0, 0, true));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_font_height_obj, 0, 1, mod_font_height);

#define RGB(r, g, b) MP_ROM_INT(((r) << 16) | ((g) << 8) | (b))

static const mp_rom_map_elem_t samara_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_samara) },
    { MP_ROM_QSTR(MP_QSTR_Window), MP_ROM_PTR(&samara_window_type) },
    { MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&mod_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&mod_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&mod_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_text_width), MP_ROM_PTR(&mod_text_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_font_height), MP_ROM_PTR(&mod_font_height_obj) },

    // events: (type, a, b, c)
    { MP_ROM_QSTR(MP_QSTR_EV_NONE), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_EV_KEY), MP_ROM_INT(1) },          // a = char (CP866 / KEY_*)
    { MP_ROM_QSTR(MP_QSTR_EV_MOUSE_DOWN), MP_ROM_INT(2) },   // a, b = x, y; c = button
    { MP_ROM_QSTR(MP_QSTR_EV_MOUSE_UP), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_EV_MOUSE_MOVE), MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_EV_CLOSE), MP_ROM_INT(5) },

    // characters in EV_KEY
    { MP_ROM_QSTR(MP_QSTR_KEY_UP), MP_ROM_INT(0x81) },
    { MP_ROM_QSTR(MP_QSTR_KEY_DOWN), MP_ROM_INT(0x82) },
    { MP_ROM_QSTR(MP_QSTR_KEY_LEFT), MP_ROM_INT(0x83) },
    { MP_ROM_QSTR(MP_QSTR_KEY_RIGHT), MP_ROM_INT(0x84) },
    { MP_ROM_QSTR(MP_QSTR_KEY_HOME), MP_ROM_INT(0x85) },
    { MP_ROM_QSTR(MP_QSTR_KEY_END), MP_ROM_INT(0x86) },
    { MP_ROM_QSTR(MP_QSTR_KEY_DEL), MP_ROM_INT(0x87) },
    { MP_ROM_QSTR(MP_QSTR_KEY_PGUP), MP_ROM_INT(0x88) },
    { MP_ROM_QSTR(MP_QSTR_KEY_PGDN), MP_ROM_INT(0x89) },
    { MP_ROM_QSTR(MP_QSTR_KEY_F1), MP_ROM_INT(0x90) },
    { MP_ROM_QSTR(MP_QSTR_KEY_ESC), MP_ROM_INT(0x1B) },
    { MP_ROM_QSTR(MP_QSTR_KEY_ENTER), MP_ROM_INT(0x0A) },
    { MP_ROM_QSTR(MP_QSTR_KEY_BACKSPACE), MP_ROM_INT(0x08) },
    { MP_ROM_QSTR(MP_QSTR_KEY_TAB), MP_ROM_INT(0x09) },
    { MP_ROM_QSTR(MP_QSTR_KEY_SPACE), MP_ROM_INT(0x20) },

    // held-key codes for key()/keys() (Linux key codes)
    { MP_ROM_QSTR(MP_QSTR_SC_ESC), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_SC_ENTER), MP_ROM_INT(28) },
    { MP_ROM_QSTR(MP_QSTR_SC_SPACE), MP_ROM_INT(57) },
    { MP_ROM_QSTR(MP_QSTR_SC_LSHIFT), MP_ROM_INT(42) },
    { MP_ROM_QSTR(MP_QSTR_SC_LCTRL), MP_ROM_INT(29) },
    { MP_ROM_QSTR(MP_QSTR_SC_UP), MP_ROM_INT(103) },
    { MP_ROM_QSTR(MP_QSTR_SC_LEFT), MP_ROM_INT(105) },
    { MP_ROM_QSTR(MP_QSTR_SC_RIGHT), MP_ROM_INT(106) },
    { MP_ROM_QSTR(MP_QSTR_SC_DOWN), MP_ROM_INT(108) },
    { MP_ROM_QSTR(MP_QSTR_SC_W), MP_ROM_INT(17) },
    { MP_ROM_QSTR(MP_QSTR_SC_A), MP_ROM_INT(30) },
    { MP_ROM_QSTR(MP_QSTR_SC_S), MP_ROM_INT(31) },
    { MP_ROM_QSTR(MP_QSTR_SC_D), MP_ROM_INT(32) },
    { MP_ROM_QSTR(MP_QSTR_SC_Q), MP_ROM_INT(16) },
    { MP_ROM_QSTR(MP_QSTR_SC_P), MP_ROM_INT(25) },
    { MP_ROM_QSTR(MP_QSTR_SC_R), MP_ROM_INT(19) },

    // fonts for text()
    { MP_ROM_QSTR(MP_QSTR_FONT_REG), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_FONT_MED), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_FONT_SMALL), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_FONT_BIG), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_FONT_HUGE), MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_FONT_MONO), MP_ROM_INT(16) },

    // colours (XRGB)
    { MP_ROM_QSTR(MP_QSTR_BLACK), RGB(0, 0, 0) },
    { MP_ROM_QSTR(MP_QSTR_WHITE), RGB(0xFF, 0xFF, 0xFF) },
    { MP_ROM_QSTR(MP_QSTR_GRAY), RGB(0x80, 0x80, 0x80) },
    { MP_ROM_QSTR(MP_QSTR_DARK), RGB(0x1D, 0x1E, 0x21) },
    { MP_ROM_QSTR(MP_QSTR_PAPER), RGB(0xEE, 0xEB, 0xE5) },
    { MP_ROM_QSTR(MP_QSTR_RED), RGB(0xCF, 0x4A, 0x3E) },
    { MP_ROM_QSTR(MP_QSTR_GREEN), RGB(0x74, 0xB0, 0x5E) },
    { MP_ROM_QSTR(MP_QSTR_BLUE), RGB(0x3E, 0x7C, 0xCF) },
    { MP_ROM_QSTR(MP_QSTR_YELLOW), RGB(0xF2, 0xC9, 0x4C) },
    { MP_ROM_QSTR(MP_QSTR_ORANGE), RGB(0xE3, 0x9B, 0x32) },
    { MP_ROM_QSTR(MP_QSTR_CYAN), RGB(0x4C, 0xC2, 0xD2) },
    { MP_ROM_QSTR(MP_QSTR_MAGENTA), RGB(0xB0, 0x5E, 0xC8) },
};
static MP_DEFINE_CONST_DICT(samara_globals, samara_globals_table);

const mp_obj_module_t mp_module_samara = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&samara_globals,
};

MP_REGISTER_MODULE(MP_QSTR_samara, mp_module_samara);
