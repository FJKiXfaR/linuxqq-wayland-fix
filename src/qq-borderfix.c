/*
 * qq-borderfix：共享屏幕时把 QQ 的「屏幕共享」全屏边框窗口藏起来。
 *
 * ── 现象与结论 ──
 * Wayland 下发起屏幕共享后，QQ 会显示一个标题为「屏幕共享」的全屏边框窗口。
 * 它是 QQ 原生代码（标题串在 wrapper.node 里）通过 Chromium/Ozone 建的
 * Wayland toplevel：
 *   - Electron 的 BrowserWindow / BaseWindow API 看不到它；
 *   - QQ 的 Chromium 静态链接了自带 Wayland client（qq 二进制既没有对系统
 *     libwayland 的符号引用，也没有 NEEDED），符号级 LD_PRELOAD 拦不到。
 * 所以本库退到 socket 层：拦 libc 的 sendmsg，解析出站的 Wayland 线协议，
 * 把这条 surface 的 wl_surface.attach(buffer) 原地改写成 attach(NULL)，
 * 让它永远不映射；其它窗口一律不碰。
 *
 * ── 实现要点 ──
 * - connect 时识别 Wayland socket 并记下 fd；一个进程可能有多条 Wayland
 *   连接（不同组件的连接对象 id 空间独立），所有状态按连接隔离；
 * - 按创建关系重建对象链：
 *     wl_display.get_registry → wl_registry.bind（载荷里带接口名字符串）
 *     → wl_compositor.create_surface → xdg_wm_base.get_xdg_surface
 *     → xdg_surface.get_toplevel → set_title / set_min_size / set_max_size
 *   → 找到每个 xdg_toplevel 对应的 wl_surface；
 * - 判定：标题 ==「屏幕共享」，min < 600 且 max > 10000（不设限）→ 全屏边框；
 *   同标题但 min == max 的固定小窗（共享预览、87x40 工具条）排除；
 * - 隐藏：把该 wl_surface 后续 attach 消息里的 buffer id 改成 0。
 *
 * ── 维护注意（踩过的坑）──
 * - 绝不向连接里注入额外消息：曾用「补发 attach(NULL)+commit」，在 sendmsg
 *   部分发送 / 携带 fd 分两次发时，会插进一条消息中间，合成器报
 *   `invalid object` 并杀死 QQ；只做原地改写；
 * - sendmsg 可能只发出一部分，被切断的消息要在下次跳过，保持消息边界对齐
 *   （见 wl_skip）；
 * - libwayland 环形缓冲的 iovec 可能是 2 段，需要拼起来解析。
 *
 * ── 环境变量 ──
 *   QQ_BORDER_FIX_DISABLE=1  关掉
 *   QQ_BORDER_FIX_DEBUG=1    输出跟踪细节
 *
 * ── 如何整体删除本功能 ──
 * 1. 删除本文件 src/qq-borderfix.c；
 * 2. Makefile：删 BF_LIB 定义、$(BF_LIB) 构建规则、all / install / clean 里
 *    的 $(BF_LIB)；
 * 3. linuxqq-wayland-fix.in：删 BORDERFIX_LIB 定义、preload 循环里的
 *    "$BORDERFIX_LIB"、--doctor 的「共享边框隐藏」一行、帮助头部
 *    QQ_BORDER_FIX_DISABLE 注释；
 * 4. 删 README / docs 里相关段落。
 * 以上删掉后其余修复不受影响，本库也不依赖仓库内其它代码。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, "[qq-borderfix] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define MAX_WL_OBJECTS 128
#define MAX_WL_FDS 8

enum wl_iface {
    IF_NONE = 0,
    IF_REGISTRY,
    IF_COMPOSITOR,
    IF_SURFACE,
    IF_XDG_WM_BASE,
    IF_XDG_SURFACE,
    IF_XDG_TOPLEVEL,
};

struct wl_object_state {
    uint32_t id;
    int conn; /* 属于哪条 Wayland 连接：不同连接的对象 id 空间互相独立 */
    uint8_t iface;
    uint8_t border;
    uint8_t has_title;
    uint32_t surface; /* xdg_surface / xdg_toplevel 对应的 wl_surface */
    int32_t min_w, min_h, max_w, max_h;
};

static struct wl_object_state wl_objects[MAX_WL_OBJECTS];
static int wl_fds[MAX_WL_FDS];
static int wl_fd_count;
static size_t wl_skip[MAX_WL_FDS]; /* 部分发送后，需要跳过的消息余量 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t io_once = PTHREAD_ONCE_INIT;
static int (*real_connect)(int, const struct sockaddr *, socklen_t);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);

static int disabled, debug;

__attribute__((constructor))
static void qq_borderfix_init(void)
{
    const char *v = getenv("QQ_BORDER_FIX_DISABLE");
    if (v && *v && strcmp(v, "0") != 0)
        disabled = 1;
    v = getenv("QQ_BORDER_FIX_DEBUG");
    debug = v && *v && strcmp(v, "0") != 0;
    if (debug)
        LOG("watching wayland traffic (pid=%ld)%s", (long)getpid(),
            disabled ? " (disabled)" : "");
}

static void resolve_io(void)
{
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
}

/* ---------------- 小工具 ---------------- */

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* ---------------- 连接表 ---------------- */

static int tracked_fd(int fd)
{
    for (int i = 0; i < wl_fd_count; i++)
        if (wl_fds[i] == fd)
            return i;
    return -1;
}

/* 记录新连接；同一 fd 被新连接复用时清掉旧连接留下的状态。 */
static void track_conn(int conn, int fd)
{
    wl_fds[conn] = fd;
    wl_skip[conn] = 0;
    for (size_t i = 0; i < MAX_WL_OBJECTS; i++)
        if (wl_objects[i].conn == conn)
            memset(&wl_objects[i], 0, sizeof wl_objects[i]);
}

/* ---------------- 对象表（按连接隔离） ---------------- */

static struct wl_object_state *object_state(int conn, uint32_t id, int create)
{
    struct wl_object_state *free_slot = NULL;
    for (size_t i = 0; i < MAX_WL_OBJECTS; i++) {
        if (wl_objects[i].iface != IF_NONE && wl_objects[i].conn == conn &&
            wl_objects[i].id == id)
            return &wl_objects[i];
        if (create && !free_slot && wl_objects[i].iface == IF_NONE)
            free_slot = &wl_objects[i];
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->id = id;
        free_slot->conn = conn;
        return free_slot;
    }
    return NULL;
}

static void object_forget(int conn, uint32_t id)
{
    struct wl_object_state *o = object_state(conn, id, 0);
    if (o)
        memset(o, 0, sizeof *o);
}

static int iface_from_name(const uint8_t *name, size_t len)
{
    if (len == sizeof("wl_registry") && !memcmp(name, "wl_registry", len))
        return IF_REGISTRY;
    if (len == sizeof("wl_compositor") && !memcmp(name, "wl_compositor", len))
        return IF_COMPOSITOR;
    if (len == sizeof("xdg_wm_base") && !memcmp(name, "xdg_wm_base", len))
        return IF_XDG_WM_BASE;
    return IF_NONE;
}

/* ---------------- 判定与改写 ---------------- */

static void border_check(struct wl_object_state *o)
{
    if (debug)
        LOG("candidate conn=%d id=%u title=%u surface=%u min=%dx%d max=%dx%d",
            o->conn, o->id, o->has_title, o->surface,
            o->min_w, o->min_h, o->max_w, o->max_h);
    if (o->border || !o->has_title || !o->surface)
        return;
    if (!(o->min_w < 600 && o->min_h < 600 && o->max_w > 10000 && o->max_h > 10000))
        return;

    o->border = 1;
    struct wl_object_state *surf = object_state(o->conn, o->surface, 0);
    if (surf)
        surf->border = 1;
    LOG("hiding 屏幕共享 border window (min %dx%d max %dx%d)",
        o->min_w, o->min_h, o->max_w, o->max_h);
}

/* ---------------- Wayland 线协议 ---------------- */

static void handle_wire_request(int conn, uint32_t id, uint32_t opcode,
                                uint8_t *args, size_t len)
{
    if (id == 1) { /* wl_display：get_registry */
        if (opcode == 1 && len >= 4) {
            struct wl_object_state *o = object_state(conn, get_u32(args), 1);
            if (o)
                o->iface = IF_REGISTRY;
        }
        return;
    }

    struct wl_object_state *o = object_state(conn, id, 0);
    if (!o)
        return;

    switch (o->iface) {
    case IF_REGISTRY:
        if (opcode == 0 && len >= 16) { /* bind(name, interface, version, new_id) */
            size_t slen = get_u32(args + 4);
            size_t padded = (slen + 3) & ~(size_t)3;
            if (slen < 1 || slen > 128 || len < 16 + padded)
                return;
            uint32_t new_id = get_u32(args + 12 + padded);
            int iface = iface_from_name(args + 8, slen);
            struct wl_object_state *n = object_state(conn, new_id, 1);
            if (n)
                n->iface = (uint8_t)iface;
        }
        break;
    case IF_COMPOSITOR:
        if (opcode == 0 && len >= 4) { /* create_surface(new_id) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n)
                n->iface = IF_SURFACE;
        }
        break;
    case IF_XDG_WM_BASE:
        if (opcode == 2 && len >= 8) { /* get_xdg_surface(new_id, wl_surface) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n) {
                n->iface = IF_XDG_SURFACE;
                n->surface = get_u32(args + 4);
            }
        }
        break;
    case IF_XDG_SURFACE:
        if (opcode == 1 && len >= 4) { /* get_toplevel(new_id) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n) {
                n->iface = IF_XDG_TOPLEVEL;
                n->surface = o->surface;
            }
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    case IF_XDG_TOPLEVEL:
        if (opcode == 2 && len >= 4) { /* set_title(string) */
            size_t slen = get_u32(args);
            size_t padded = (slen + 3) & ~(size_t)3;
            if (slen == sizeof("屏幕共享") && len >= 4 + padded &&
                !memcmp(args + 4, "屏幕共享", sizeof("屏幕共享")))
                o->has_title = 1;
            border_check(o);
        } else if (opcode == 7 && len >= 8) { /* set_max_size */
            o->max_w = (int32_t)get_u32(args);
            o->max_h = (int32_t)get_u32(args + 4);
            border_check(o);
        } else if (opcode == 8 && len >= 8) { /* set_min_size */
            o->min_w = (int32_t)get_u32(args);
            o->min_h = (int32_t)get_u32(args + 4);
            border_check(o);
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    case IF_SURFACE:
        if (opcode == 1 && o->border) { /* attach(buffer, x, y) -> attach(NULL) */
            if (len >= 12 && get_u32(args) != 0)
                put_u32(args, 0);
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    }
}

/* 从缓冲区开头按消息头推进；size 不合法就整块停止。 */
static void rewrite_chunk(int conn, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t id = get_u32(buf + off);
        uint32_t word = get_u32(buf + off + 4);
        size_t size = word >> 16;
        uint32_t opcode = word & 0xffff;
        if (size < 8 || (size & 3) || size > len - off)
            break;
        handle_wire_request(conn, id, opcode, buf + off + 8, size - 8);
        off += size;
    }
}

/* 找到包含第 sent 个字节的那条消息的结束偏移（部分发送后的重同步用）。 */
static size_t message_end_at(const uint8_t *buf, size_t len, size_t start, size_t sent)
{
    size_t off = start;
    while (off + 8 <= len) {
        uint32_t word = get_u32(buf + off + 4);
        size_t size = word >> 16;
        if (size < 8 || (size & 3) || size > len - off)
            break;
        if (off + size > sent)
            return off + size;
        off += size;
    }
    return len;
}

/* ---------------- 拦截 ---------------- */

/* 地址是不是 Wayland socket：常规名字含 "wayland"，另外兼容自定义
 * WAYLAND_DISPLAY（可能是绝对路径或抽象名）。 */
static int wayland_addr_matches(const struct sockaddr_un *un, socklen_t len)
{
    const uint8_t *p = (const uint8_t *)un->sun_path;
    size_t base = offsetof(struct sockaddr_un, sun_path);
    size_t n = len > base ? (size_t)len - base : 0;

    for (size_t i = 0; i + 7 <= n; i++)
        if (!memcmp(p + i, "wayland", 7))
            return 1;

    const char *disp = getenv("WAYLAND_DISPLAY");
    if (!disp || !*disp)
        return 0;
    const char *name = strrchr(disp, '/');
    name = name ? name + 1 : disp;
    size_t dl = strlen(name);
    for (size_t i = 0; dl && i + dl <= n; i++)
        if (!memcmp(p + i, name, dl))
            return 1;
    return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    pthread_once(&io_once, resolve_io);
    int rc = real_connect(fd, addr, len);

    if (rc == 0 && !disabled && addr && addr->sa_family == AF_UNIX &&
        wayland_addr_matches((const struct sockaddr_un *)addr, len)) {
        pthread_mutex_lock(&lock);
        int conn = tracked_fd(fd);
        if (conn < 0 && wl_fd_count < MAX_WL_FDS)
            conn = wl_fd_count++;
        if (conn >= 0)
            track_conn(conn, fd);
        pthread_mutex_unlock(&lock);
        if (debug)
            LOG("tracking wayland fd=%d", fd);
    }
    return rc;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    pthread_once(&io_once, resolve_io);

    int conn = (!disabled && msg) ? tracked_fd(fd) : -1;

    if (debug && conn >= 0) {
        static int calls;
        if (calls < 12) {
            calls++;
            LOG("sendmsg fd=%d iovlen=%zu len0=%zu len1=%zu",
                fd, msg->msg_iovlen,
                msg->msg_iov ? msg->msg_iov[0].iov_len : 0,
                (msg->msg_iov && msg->msg_iovlen > 1) ? msg->msg_iov[1].iov_len : 0);
        }
    }

    struct iovec *iov = msg ? msg->msg_iov : NULL;
    size_t total = 0;
    uint8_t *tmp = NULL;
    uint8_t *buf = NULL;
    size_t parse_start = 0;

    if (conn >= 0 && msg && msg->msg_iovlen >= 1 && iov && iov[0].iov_base) {
        total = iov[0].iov_len;
        if (msg->msg_iovlen >= 2 && iov[1].iov_base && iov[1].iov_len > 0) {
            /* 环形缓冲可能拆成两段，拼起来一起解析/改写 */
            tmp = malloc(total + iov[1].iov_len);
            if (tmp) {
                memcpy(tmp, iov[0].iov_base, iov[0].iov_len);
                memcpy(tmp + iov[0].iov_len, iov[1].iov_base, iov[1].iov_len);
                total += iov[1].iov_len;
                buf = tmp;
            }
        } else {
            buf = iov[0].iov_base;
        }
    }

    if (buf && total >= 8) {
        pthread_mutex_lock(&lock);
        if (wl_skip[conn] >= total) {
            wl_skip[conn] -= total;
        } else {
            if (wl_skip[conn] > 0) {
                parse_start = wl_skip[conn];
                wl_skip[conn] = 0;
            }
            rewrite_chunk(conn, buf + parse_start, total - parse_start);
        }
        pthread_mutex_unlock(&lock);
    }

    if (tmp) {
        memcpy(iov[0].iov_base, tmp, iov[0].iov_len);
        if (iov[1].iov_base)
            memcpy(iov[1].iov_base, tmp + iov[0].iov_len, total - iov[0].iov_len);
    }

    ssize_t rc = real_sendmsg(fd, msg, flags);

    /* 部分发送把一条消息切成两半时，下次跳过它的余量，保持边界对齐。 */
    if (buf && rc > 0 && (size_t)rc < total) {
        size_t skip;
        if ((size_t)rc < parse_start)
            skip = parse_start - (size_t)rc;
        else
            skip = message_end_at(buf, total, parse_start, (size_t)rc) - (size_t)rc;
        pthread_mutex_lock(&lock);
        wl_skip[conn] = skip;
        pthread_mutex_unlock(&lock);
        if (debug)
            LOG("partial send (%zd/%zu), skip %zu next", rc, total, skip);
    }

    free(tmp);
    return rc;
}
