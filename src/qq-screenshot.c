/*
 * qq-screenshot：按截图键（Ctrl+Alt+A）时 QQ 不再闪退，并截到真实画面。
 *
 * QQ 截图前先执行 `echo $XDG_SESSION_TYPE`：是 wayland 就走 GNOME 专用的方式，
 * 否则用主程序里的 X11 代码对根窗口 XGetImage / XShmGetImage 截全屏。
 * 启动器为了让屏幕共享可用，给 QQ 的是 XDG_SESSION_TYPE=x11，于是走 X11 这条；
 * 而 Wayland 桌面下的 XWayland 是 rootless 的，根窗口没有内容，GetImage 必然 BadMatch。
 * Xlib 打印错误后返回 NULL，QQ 不检查就去读像素，段错误。
 *
 * 这里只处理对根窗口的截取（别的窗口照常交给 Xlib）：
 *   1. 在 Wayland 会话里（有 WAYLAND_DISPLAY），通过 wlr-screencopy 逐个截取 Wayland 输出，
 *      按 X 的显示器布局（XRandR monitors，名字与 wl_output 名字对应）拼成根窗口坐标系下的画面。
 *      XShmGetImage 在 rootless XWayland 上不报错、只给全黑，所以不能「X 失败了再截」；
 *   2. 不在 Wayland 会话里，或合成器不支持 wlr-screencopy（KDE、GNOME）时，照常调用 Xlib，
 *      但临时接管 X 错误（默认处理会直接退出进程）；仍然失败就给一张黑图，至少不闪退。
 *      真正的 X11 会话里截取会成功，行为不变。
 *
 * QQ_SCREENSHOT_FIX_DISABLE=1 可以关掉。
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define LOG(...) do { fprintf(stderr, "[qq-screenshot] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---------------- 截取 Wayland 输出 ---------------- */

struct output {
    struct wl_output *wl;
    char name[64];
    uint32_t *pix; /* 截到的画面，0x00RRGGBB */
    int w, h;
    struct output *next;
};

struct capture {
    struct wl_display *dpy;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *mgr;
    struct output *outputs;
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *make, const char *model, int32_t tr)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static void out_name(void *d, struct wl_output *o, const char *name)
{
    struct output *out = d;
    (void)o;
    snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done,
    .scale = out_scale, .name = out_name, .description = out_desc,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
    struct capture *c = data;

    if (!strcmp(iface, wl_shm_interface.name)) {
        c->shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        c->mgr = wl_registry_bind(r, id, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct output *o = calloc(1, sizeof *o);
        if (!o)
            return;
        o->wl = wl_registry_bind(r, id, &wl_output_interface, ver < 4 ? ver : 4);
        wl_output_add_listener(o->wl, &output_listener, o);
        o->next = c->outputs;
        c->outputs = o;
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

struct frame {
    uint32_t format, w, h, stride;
    int have_buffer, buffer_done, ready, failed, y_invert;
};

static int supported_format(uint32_t f)
{
    return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 ||
           f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888;
}

static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                      uint32_t w, uint32_t h, uint32_t stride)
{
    struct frame *fr = d;
    (void)f;
    /* 可能给出多个格式，取第一个支持的 */
    if (!fr->have_buffer && supported_format(format)) {
        fr->format = format; fr->w = w; fr->h = h; fr->stride = stride;
        fr->have_buffer = 1;
    }
}
static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    struct frame *fr = d;
    (void)f;
    fr->y_invert = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}
static void fr_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{
    (void)f; (void)a; (void)b; (void)c;
    ((struct frame *)d)->ready = 1;
}
static void fr_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct frame *)d)->failed = 1; }
static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void fr_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)fmt; (void)w; (void)h;
}
static void fr_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct frame *)d)->buffer_done = 1; }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = fr_buffer, .flags = fr_flags, .ready = fr_ready, .failed = fr_failed,
    .damage = fr_damage, .linux_dmabuf = fr_dmabuf, .buffer_done = fr_buffer_done,
};

/* 带超时地分发事件，直到 *flag 或 *fail 置位。 */
static int dispatch_until(struct wl_display *dpy, int *flag, int *fail, int timeout_ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!*flag && !*fail) {
        while (wl_display_prepare_read(dpy) != 0)
            if (wl_display_dispatch_pending(dpy) < 0)
                return -1;
        if (*flag || *fail) {
            wl_display_cancel_read(dpy);
            break;
        }
        wl_display_flush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) {
            wl_display_cancel_read(dpy);
            return -1;
        }
        if (wl_display_read_events(dpy) < 0 || wl_display_dispatch_pending(dpy) < 0)
            return -1;
    }
    return *flag ? 0 : -1;
}

static int capture_output(struct capture *c, struct output *o)
{
    struct frame fr = { 0 };
    struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(c->mgr, 0, o->wl);
    int ret = -1;

    zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, &fr);
    /* v3 用 buffer_done 表示格式列完了；更早的版本一次往返后 buffer 事件就到了 */
    if (zwlr_screencopy_manager_v1_get_version(c->mgr) >= 3)
        dispatch_until(c->dpy, &fr.buffer_done, &fr.failed, 2000);
    else
        wl_display_roundtrip(c->dpy);
    if (!fr.have_buffer || fr.failed) {
        LOG("output %s: no usable shm format", o->name);
        goto out;
    }

    size_t size = (size_t)fr.stride * fr.h;
    int fd = memfd_create("qq-screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0)
            close(fd);
        goto out;
    }
    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        goto out;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, fr.w, fr.h, fr.stride, fr.format);
    wl_shm_pool_destroy(pool);
    close(fd);

    zwlr_screencopy_frame_v1_copy(f, buf);
    if (dispatch_until(c->dpy, &fr.ready, &fr.failed, 3000) == 0 &&
        (o->pix = malloc((size_t)fr.w * fr.h * 4))) {
        int swap = fr.format == WL_SHM_FORMAT_XBGR8888 || fr.format == WL_SHM_FORMAT_ABGR8888;
        for (uint32_t y = 0; y < fr.h; y++) {
            const uint32_t *src = (const uint32_t *)(map + (size_t)(fr.y_invert ? fr.h - 1 - y : y) * fr.stride);
            uint32_t *dst = o->pix + (size_t)y * fr.w;
            for (uint32_t x = 0; x < fr.w; x++) {
                uint32_t p = src[x];
                if (swap)
                    p = (p & 0x0000ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
                dst[x] = p & 0x00ffffff;
            }
        }
        o->w = fr.w;
        o->h = fr.h;
        ret = 0;
    } else {
        LOG("output %s: capture failed", o->name);
    }
    wl_buffer_destroy(buf);
    munmap(map, size);
out:
    zwlr_screencopy_frame_v1_destroy(f);
    return ret;
}

static void capture_free(struct capture *c)
{
    for (struct output *o = c->outputs, *n; o; o = n) {
        n = o->next;
        if (o->wl)
            wl_output_destroy(o->wl);
        free(o->pix);
        free(o);
    }
    if (c->mgr)
        zwlr_screencopy_manager_v1_destroy(c->mgr);
    if (c->shm)
        wl_shm_destroy(c->shm);
    if (c->dpy)
        wl_display_disconnect(c->dpy);
    memset(c, 0, sizeof *c);
}

/* 截取所有输出。至少截到一个时返回 0。 */
static int capture_all(struct capture *c)
{
    memset(c, 0, sizeof *c);
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy)
        return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &registry_listener, c);
    wl_display_roundtrip(c->dpy);
    wl_display_roundtrip(c->dpy); /* wl_output.name */
    wl_registry_destroy(reg);
    if (!c->mgr || !c->shm) {
        LOG("compositor has no wlr-screencopy");
        return -1;
    }
    int got = 0;
    for (struct output *o = c->outputs; o; o = o->next)
        got += capture_output(c, o) == 0;
    return got ? 0 : -1;
}

/* ---------------- 按 X 的布局拼成根窗口画面 ---------------- */

typedef struct {
    Atom name;
    Bool primary, automatic;
    int noutput, x, y, width, height, mwidth, mheight;
    void *outputs;
} MonitorInfo; /* 与 XRRMonitorInfo 布局一致；运行时 dlopen libXrandr，不增加链接依赖 */

typedef MonitorInfo *(*get_monitors_fn)(Display *, Window, Bool, int *);
typedef void (*free_monitors_fn)(MonitorInfo *);

/* 根窗口的整张画面。缓存一小会儿：QQ 可能对每块屏各取一次 */
static struct {
    Display *dpy;
    uint32_t *pix;
    int w, h;
    struct timespec at;
} root_cache;

static void blit(uint32_t *dst, int dw, int dh, int mx, int my, int mw, int mh, const struct output *o)
{
    for (int y = 0; y < mh; y++) {
        int ry = my + y;
        if (ry < 0 || ry >= dh)
            continue;
        const uint32_t *src = o->pix + (size_t)((long)y * o->h / mh) * o->w;
        uint32_t *row = dst + (size_t)ry * dw;
        for (int x = 0; x < mw; x++) {
            int rx = mx + x;
            if (rx >= 0 && rx < dw)
                row[rx] = src[(long)x * o->w / mw];
        }
    }
}

static uint32_t *root_image(Display *dpy, Window root, int *w, int *h)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (root_cache.pix && root_cache.dpy == dpy &&
        (now.tv_sec - root_cache.at.tv_sec) * 1000 + (now.tv_nsec - root_cache.at.tv_nsec) / 1000000 < 1500) {
        *w = root_cache.w;
        *h = root_cache.h;
        return root_cache.pix;
    }
    free(root_cache.pix);
    root_cache.pix = NULL;

    Window r;
    int rx, ry;
    unsigned rw, rh, bw, depth;
    if (!XGetGeometry(dpy, root, &r, &rx, &ry, &rw, &rh, &bw, &depth))
        return NULL;

    struct capture c;
    if (capture_all(&c)) {
        capture_free(&c);
        return NULL;
    }
    uint32_t *pix = calloc((size_t)rw * rh, 4);
    if (!pix) {
        capture_free(&c);
        return NULL;
    }

    void *xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    get_monitors_fn get = xrandr ? (get_monitors_fn)dlsym(xrandr, "XRRGetMonitors") : NULL;
    free_monitors_fn freem = xrandr ? (free_monitors_fn)dlsym(xrandr, "XRRFreeMonitors") : NULL;
    int n = 0, placed = 0;
    MonitorInfo *m = get ? get(dpy, root, True, &n) : NULL;
    for (int i = 0; i < n; i++) {
        char *name = XGetAtomName(dpy, m[i].name);
        for (struct output *o = c.outputs; o && name; o = o->next)
            if (o->pix && !strcmp(o->name, name)) {
                blit(pix, rw, rh, m[i].x, m[i].y, m[i].width, m[i].height, o);
                placed++;
                break;
            }
        XFree(name);
    }
    if (m && freem)
        freem(m);
    if (xrandr)
        dlclose(xrandr);
    if (!placed) /* 对不上名字时：铺满第一块屏 */
        for (struct output *o = c.outputs; o; o = o->next)
            if (o->pix) {
                blit(pix, rw, rh, 0, 0, rw, rh, o);
                break;
            }
    LOG("captured %ux%u root from Wayland (%d monitor(s) matched)", rw, rh, placed);
    capture_free(&c);

    root_cache.dpy = dpy;
    root_cache.pix = pix;
    root_cache.w = rw;
    root_cache.h = rh;
    root_cache.at = now;
    *w = rw;
    *h = rh;
    return pix;
}

/* 把根窗口画面里 (x, y, w, h) 这一块写进 32 位 ZPixmap 图像。截不到时返回 -1，图像不动。 */
static int fill_from_wayland(Display *dpy, Window root, XImage *img, int x, int y)
{
    int rw = 0, rh = 0;
    uint32_t *pix = NULL;

    if (getenv("WAYLAND_DISPLAY") && img->format == ZPixmap && img->bits_per_pixel == 32)
        pix = root_image(dpy, root, &rw, &rh);
    if (!pix)
        return -1;
    for (int j = 0; j < img->height; j++) {
        uint32_t *row = (uint32_t *)(img->data + (size_t)j * img->bytes_per_line);
        for (int i = 0; i < img->width; i++) {
            int sx = x + i, sy = y + j;
            row[i] = (sx >= 0 && sy >= 0 && sx < rw && sy < rh) ? pix[(size_t)sy * rw + sx] : 0;
        }
    }
    return 0;
}

/* ---------------- 拦截 ---------------- */

static int enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *v = getenv("QQ_SCREENSHOT_FIX_DISABLE");
        state = !(v && *v && strcmp(v, "0"));
    }
    return state;
}

static int is_root(Display *dpy, Drawable d)
{
    for (int i = 0; i < ScreenCount(dpy); i++)
        if (d == RootWindow(dpy, i))
            return 1;
    return 0;
}

/* 截取期间临时接管 X 错误（默认处理会直接退出进程）。QQ 只在主线程截图。 */
static int x_failed;
static int trap(Display *dpy, XErrorEvent *e)
{
    (void)dpy; (void)e;
    x_failed = 1;
    return 0;
}

typedef XImage *(*get_image_fn)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int);
typedef Bool (*shm_get_image_fn)(Display *, Drawable, XImage *, int, int, unsigned long);

XImage *XGetImage(Display *dpy, Drawable d, int x, int y, unsigned w, unsigned h,
                  unsigned long planes, int format)
{
    static get_image_fn real;
    if (!real)
        real = (get_image_fn)dlsym(RTLD_NEXT, "XGetImage");

    if (!enabled() || !is_root(dpy, d) || !w || !h)
        return real(dpy, d, x, y, w, h, planes, format);

    int scr = DefaultScreen(dpy);
    int depth = DefaultDepth(dpy, scr);
    XImage *out = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, format, 0, NULL, w, h, 32, 0);
    if (!out)
        return NULL;
    out->data = calloc((size_t)out->bytes_per_line * h * (format == ZPixmap ? 1 : depth), 1);
    if (!out->data) {
        XDestroyImage(out);
        return NULL;
    }
    if (fill_from_wayland(dpy, d, out, x, y) == 0)
        return out;

    XSync(dpy, False);
    x_failed = 0;
    XErrorHandler old = XSetErrorHandler(trap);
    XImage *img = real(dpy, d, x, y, w, h, planes, format);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (img && !x_failed) {
        XDestroyImage(out);
        return img;
    }
    if (img)
        XDestroyImage(img);
    LOG("XGetImage on root %ux%u+%d+%d failed, returning a black image", w, h, x, y);
    return out;
}

Bool XShmGetImage(Display *dpy, Drawable d, XImage *img, int x, int y, unsigned long planes)
{
    static shm_get_image_fn real;
    if (!real)
        real = (shm_get_image_fn)dlsym(RTLD_NEXT, "XShmGetImage");
    if (!real)
        return False;

    if (!enabled() || !img || !is_root(dpy, d))
        return real(dpy, d, img, x, y, planes);

    if (fill_from_wayland(dpy, d, img, x, y) == 0)
        return True;

    XSync(dpy, False);
    x_failed = 0;
    XErrorHandler old = XSetErrorHandler(trap);
    Bool ok = real(dpy, d, img, x, y, planes);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (ok && !x_failed)
        return ok;
    memset(img->data, 0, (size_t)img->bytes_per_line * img->height);
    LOG("XShmGetImage on root %dx%d+%d+%d failed, returning a black image", img->width, img->height, x, y);
    return True;
}
