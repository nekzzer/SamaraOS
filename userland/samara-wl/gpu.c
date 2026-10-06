#define _GNU_SOURCE
#include "swl.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "linux-dmabuf-v1-server-protocol.h"

bool gpu;
static EGLDisplay edpy;
static struct gbm_device *gbmd;
static EGLContext ectx;
static int has_mod, has_unpack;
static GLuint prog;
static GLint u_r, u_sz, u_swz, u_opq, u_flp;
static dev_t rdev;
static int tfd;
static PFNEGLCREATEIMAGEKHRPROC create_img;
static PFNEGLDESTROYIMAGEKHRPROC destroy_img;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC img2tex;

#define FMT_ARGB 0x34325241
#define FMT_XRGB 0x34325258
#define MOD_INVALID 0x00ffffffffffffffULL

static const char *vs_src =
    "attribute vec2 p; uniform vec4 r; uniform vec2 sz; uniform float flp; varying vec2 uv;\n"
    "void main() { uv = p; vec2 q = r.xy + p * r.zw; gl_Position = vec4(q.x / sz.x * 2.0 - 1.0, (q.y / sz.y * 2.0 - 1.0) * flp, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float; uniform sampler2D tx; uniform float swz, opq; varying vec2 uv;\n"
    "void main() { vec4 c = texture2D(tx, uv); if (swz > 0.5) c = c.bgra; if (opq > 0.5) c.a = 1.0; gl_FragColor = c; }\n";

static GLuint shader(int type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static void tex_setup(GLuint t) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* dmabuf */

struct par { int fd[4], off[4], stride[4]; uint64_t mod; int n; };

static void par_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void par_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t idx, uint32_t off,
                    uint32_t stride, uint32_t mhi, uint32_t mlo) {
    struct par *p = wl_resource_get_user_data(r);
    if (idx > 3 || p->fd[idx] > 0) { close(fd); return; }
    p->fd[idx] = fd; p->off[idx] = off; p->stride[idx] = stride;
    p->mod = (uint64_t)mhi << 32 | mlo;
}

static struct buf *dma_import(struct par *p, int w, int h, uint32_t fmt) {
    EGLint a[32]; int i = 0;
    a[i++] = EGL_WIDTH; a[i++] = w;
    a[i++] = EGL_HEIGHT; a[i++] = h;
    a[i++] = EGL_LINUX_DRM_FOURCC_EXT; a[i++] = fmt;
    a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT; a[i++] = p->fd[0];
    a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[i++] = p->off[0];
    a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; a[i++] = p->stride[0];
    if (has_mod && p->mod != MOD_INVALID) {
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; a[i++] = (uint32_t)p->mod;
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; a[i++] = p->mod >> 32;
    }
    a[i] = EGL_NONE;
    EGLImageKHR img = create_img(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "samara-wl: dmabuf import failed %x\n", eglGetError()); return NULL; }
    struct buf *b = calloc(1, sizeof(*b));
    b->img = img; b->w = w; b->h = h; b->fmt = fmt;
    glGenTextures(1, &b->tex);
    tex_setup(b->tex);
    img2tex(GL_TEXTURE_2D, img);
    return b;
}

static void par_make(struct wl_client *c, struct wl_resource *r, uint32_t id, int w, int h, uint32_t fmt, bool immed) {
    struct par *p = wl_resource_get_user_data(r);
    struct buf *b = NULL;
    if (p->fd[0] > 0 && (fmt == FMT_ARGB || fmt == FMT_XRGB)) b = dma_import(p, w, h, fmt);
    for (int i = 0; i < 4; i++) if (p->fd[i] > 0) { close(p->fd[i]); p->fd[i] = 0; }
    if (!b) {
        if (immed) wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "import failed");
        else zwp_linux_buffer_params_v1_send_failed(r);
        return;
    }
    b->res = wl_resource_create(c, &wl_buffer_interface, 1, immed ? id : 0);
    wl_resource_set_implementation(b->res, &buf_impl, b, buf_free);
    if (!immed) zwp_linux_buffer_params_v1_send_created(r, b->res);
}

static void par_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t fmt, uint32_t fl) {
    par_make(c, r, 0, w, h, fmt, false);
}

static void par_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w, int32_t h, uint32_t fmt, uint32_t fl) {
    par_make(c, r, id, w, h, fmt, true);
}
static const struct zwp_linux_buffer_params_v1_interface par_impl = { par_destroy, par_add, par_create, par_immed };

static void par_free(struct wl_resource *r) {
    struct par *p = wl_resource_get_user_data(r);
    for (int i = 0; i < 4; i++) if (p->fd[i] > 0) close(p->fd[i]);
    free(p);
}

static void fb_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface fb_impl = { fb_destroy };

static void fb_send(struct wl_client *c, struct wl_resource *dm, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(dm), id);
    wl_resource_set_implementation(r, &fb_impl, NULL, NULL);
    struct wl_array dev, idx;
    wl_array_init(&dev);
    memcpy(wl_array_add(&dev, sizeof rdev), &rdev, sizeof rdev);
    zwp_linux_dmabuf_feedback_v1_send_format_table(r, tfd, 32);
    zwp_linux_dmabuf_feedback_v1_send_main_device(r, &dev);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &dev);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, 0);
    wl_array_init(&idx);
    *(uint16_t *)wl_array_add(&idx, 2) = 0;
    *(uint16_t *)wl_array_add(&idx, 2) = 1;
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &idx);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
    zwp_linux_dmabuf_feedback_v1_send_done(r);
    wl_array_release(&dev);
    wl_array_release(&idx);
}

static void dm_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void dm_params(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *pr = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(pr, &par_impl, calloc(1, sizeof(struct par)), par_free);
}

static void dm_defb(struct wl_client *c, struct wl_resource *r, uint32_t id) { fb_send(c, r, id); }
static void dm_surfb(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *s) { fb_send(c, r, id); }
static const struct zwp_linux_dmabuf_v1_interface dm_impl = { dm_destroy, dm_params, dm_defb, dm_surfb };

static void dm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, ver > 4 ? 4 : ver, id);
    wl_resource_set_implementation(r, &dm_impl, NULL, NULL);
    if (ver >= 4) return;
    if (ver == 3) {
        zwp_linux_dmabuf_v1_send_modifier(r, FMT_ARGB, MOD_INVALID >> 32, (uint32_t)MOD_INVALID);
        zwp_linux_dmabuf_v1_send_modifier(r, FMT_XRGB, MOD_INVALID >> 32, (uint32_t)MOD_INVALID);
    } else {
        zwp_linux_dmabuf_v1_send_format(r, FMT_ARGB);
        zwp_linux_dmabuf_v1_send_format(r, FMT_XRGB);
    }
}

void gpu_init(void) {
    // opt in only, glamor xwayland stalls on virgl right now
    if (!getenv("SAMARA_GPU") && access("/etc/samara-gpu", F_OK)) return;
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return;
    struct stat st;
    fstat(fd, &st);
    rdev = st.st_rdev;
    struct gbm_device *g = gbmd = gbm_create_device(fd);
    if (!g) goto bad;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!gpd) goto bad;
    edpy = gpd(EGL_PLATFORM_GBM_KHR, g, NULL);
    EGLint maj, min;
    if (!eglInitialize(edpy, &maj, &min)) goto bad;
    const char *ex = eglQueryString(edpy, EGL_EXTENSIONS);
    if (!strstr(ex, "EGL_EXT_image_dma_buf_import")) goto bad;
    has_mod = strstr(ex, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ectx = eglCreateContext(edpy, NULL, EGL_NO_CONTEXT, ca);
    if (ectx == EGL_NO_CONTEXT) goto bad;
    if (!eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx)) goto bad;
    create_img = (void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_img = (void *)eglGetProcAddress("eglDestroyImageKHR");
    img2tex = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_img || !img2tex) goto bad;

    prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) goto bad;
    glUseProgram(prog);
    u_r = glGetUniformLocation(prog, "r");
    u_sz = glGetUniformLocation(prog, "sz");
    u_swz = glGetUniformLocation(prog, "swz");
    u_opq = glGetUniformLocation(prog, "opq");
    u_flp = glGetUniformLocation(prog, "flp");
    static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    has_unpack = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_unpack_subimage") != NULL;

    tfd = memfd_create("fmts", 0);
    uint32_t tb[8] = { FMT_ARGB, 0, (uint32_t)MOD_INVALID, MOD_INVALID >> 32, FMT_XRGB, 0, (uint32_t)MOD_INVALID, MOD_INVALID >> 32 };
    write(tfd, tb, sizeof tb);
    wl_global_create(dpy, &zwp_linux_dmabuf_v1_interface, 4, NULL, dm_bind);
    gpu = true;
    fprintf(stderr, "samara-wl: gl %s\n", glGetString(GL_RENDERER));
    return;
bad:
    fprintf(stderr, "samara-wl: no gl, cpu path\n");
    close(fd);
}

// fullscreen window: render into a bo and let the kernel put it on the screen as is
static bool scan_bo(struct tl *t) {
    if (!access("/etc/samara-noscan", F_OK)) return false;     // to compare with the readback path
    struct gbm_bo *bo = gbm_bo_create(gbmd, t->cw, t->ch, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!bo) return false;
    int fd = gbm_bo_get_fd(bo);
    EGLint a[] = { EGL_WIDTH, t->cw, EGL_HEIGHT, t->ch, EGL_LINUX_DRM_FOURCC_EXT, FMT_XRGB,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo), EGL_NONE };
    EGLImageKHR img = create_img(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (img == EGL_NO_IMAGE_KHR) { close(fd); gbm_bo_destroy(bo); return false; }
    img2tex(GL_TEXTURE_2D, img);
    t->sbo = bo; t->simg = img; t->sfd = fd; t->scan = true;
    return true;
}

void gpu_tl_alloc(struct tl *t) {
    gpu_tl_free(t);
    glGenTextures(1, &t->ftex);
    tex_setup(t->ftex);
    if (t->cw != scr_w || t->ch != scr_h || !scan_bo(t))
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->cw, t->ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->ftex, 0);
}

void gpu_tl_free(struct tl *t) {
    if (t->sbo) {
        sm(OP_SCANOUT, -1, 0, 0);
        destroy_img(edpy, t->simg);
        close(t->sfd);
        gbm_bo_destroy(t->sbo);
        t->sbo = NULL; t->scan = false;
    }
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->ftex) glDeleteTextures(1, &t->ftex);
    t->fbo = t->ftex = 0;
}

void gpu_begin(struct tl *t, int x0, int y0, int x1, int y1) {
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->cw, t->ch);
    glEnable(GL_SCISSOR_TEST);
    // scanout bo is top row first like the cpu buffer. if it shows upside down on some host: SAMARA_SCANFLIP=1
    int fl = t->scan && getenv("SAMARA_SCANFLIP");
    glUniform1f(u_flp, fl ? -1 : 1);
    glScissor(x0, fl ? t->ch - y1 : y0, x1 - x0, y1 - y0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glUniform2f(u_sz, t->cw, t->ch);
}

void gpu_quad(struct surf *s, int ax, int ay) {
    unsigned tx = s->cur ? s->cur->tex : s->tex;
    if (!tx) return;
    glBindTexture(GL_TEXTURE_2D, tx);
    glUniform4f(u_r, ax, ay, s->w, s->h);
    glUniform1f(u_swz, s->cur != NULL);
    glUniform1f(u_opq, !s->argb);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void gpu_end(struct tl *t, int x0, int y0, int x1, int y1) {
    static uint32_t *tmp;
    static int tsz;
    int w = x1 - x0, h = y1 - y0;
    if (t->scan) {
        glFlush();
        if (sm(OP_SCANOUT, t->sfd, scr_w << 16 | scr_h, 0) == 0) return;
        t->scan = false;       // kernel said no, plain readback from now on
    }
    if (w == t->cw) {
        glReadPixels(0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, t->pix + y0 * t->cw);
        return;
    }
    if (w * h > tsz) { tsz = w * h; tmp = realloc(tmp, tsz * 4); }
    glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < h; y++) memcpy(t->pix + (y0 + y) * t->cw + x0, tmp + y * w, w * 4);
}

void gpu_buf_free(struct buf *b) {
    if (b->tex) glDeleteTextures(1, &b->tex);
    if (b->img && b->img != (void *)1) destroy_img(edpy, b->img);
}

static void cur_drop(struct surf *s) {
    if (!s->cur) return;
    s->cur->hold = NULL;
    wl_buffer_send_release(s->cur->res);
    s->cur = NULL;
}

void gpu_surf_free(struct surf *s) {
    if (!gpu) return;
    cur_drop(s);
    if (s->tex) glDeleteTextures(1, &s->tex);
    s->tex = 0; s->tw = s->th = 0;
}

void gpu_commit(struct surf *s, struct buf *b) {
    int k = s->k;
    bool resized = s->w != b->w * k || s->h != b->h * k || !s->has_buf;
    s->w = b->w * k; s->h = b->h * k;
    if (resized || s->dx1 <= s->dx0) { s->dx0 = s->dy0 = 0; s->dx1 = s->w; s->dy1 = s->h; }
    if (s->dx0 < 0) s->dx0 = 0;
    if (s->dy0 < 0) s->dy0 = 0;
    if (s->dx1 > s->w) s->dx1 = s->w;
    if (s->dy1 > s->h) s->dy1 = s->h;
    s->has_buf = true;
    if (b->img) {
        if (s->cur != b) {
            cur_drop(s);
            s->cur = b;
            b->hold = s;
        }
        s->argb = b->fmt == FMT_ARGB;
        return;
    }
    cur_drop(s);
    if (!s->tex) { glGenTextures(1, &s->tex); tex_setup(s->tex); }
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (s->tw != b->w || s->th != b->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, b->w, b->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s->tw = b->w; s->th = b->h;
        s->dx0 = s->dy0 = 0; s->dx1 = s->w; s->dy1 = s->h;
    }
    int x0 = s->dx0 / k, x1 = (s->dx1 + k - 1) / k, y0 = s->dy0 / k, y1 = (s->dy1 + k - 1) / k;
    char *src = (char *)b->pool->data + b->off;
    // one upload per damaged rectangle, not hundreds of virgl calls per frame
    if (has_unpack && !(b->stride & 3)) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, b->stride / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, src + y0 * b->stride + x0 * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    } else if (b->stride == b->w * 4 && x0 == 0 && x1 == b->w)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, b->w, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, src + y0 * b->stride);
    else for (int y = y0; y < y1; y++)
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y, x1 - x0, 1, GL_RGBA, GL_UNSIGNED_BYTE, src + y * b->stride + x0 * 4);
    s->argb = b->fmt == WL_SHM_FORMAT_ARGB8888;
    wl_buffer_send_release(b->res);
}
