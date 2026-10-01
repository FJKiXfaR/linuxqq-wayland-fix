/*
 * qq-wl-portal：给 QQ 补上缺失的 Wayland 共享源选择器。
 *
 * 背景（QQ 3.2.34，avsdk/broadcast-core.so）：
 *   - IsWayland() 只看 getenv("XDG_SESSION_TYPE") == "wayland"；
 *   - Wayland 分支 MonitorCapture_WaylandProc 把上层传来的 64 位「显示器句柄」
 *     拆成 fd（低 32 位）和 node id（高 32 位），
 *     分别交给 pw_context_connect_fd / pw_stream_connect；
 *   - 本该由 CaptureSelector 通过 portal 填好这两个值，但上层从不调用它，
 *     传进来的仍是 X11 显示器编号，于是连不上。
 *
 * 另外修正「共享设备音频」：broadcast-core 的 AudioCapture_Init 只接受采样格式为
 * S16LE / FLOAT32LE 的输出设备，否则返回 E_NOTIMPL（0x80004001），声卡是 s32le 时
 * 系统声音就共享不了。见文件末尾的 pa_context_get_*_info_by_name 包装。
 *
 * 还修正了 QQ 采集代码的一个 bug：共享内存帧它按 width*4 整块 memcpy，
 * 忽略了 chunk->stride / offset，合成器给的行有填充时（常见于共享单个窗口）画面会斜切成条纹。
 * 见「5. 修正共享内存帧的行跨度」。
 *
 * 本库做三件事（只影响 broadcast-core.so，QQ 其余部分看到的一切照旧）：
 *   1. broadcast-core 读取 XDG_SESSION_TYPE 时返回 "wayland"，让它走 Wayland 分支；
 *   2. broadcast-core 通过 dlsym 取 pw_context_connect_fd / pw_stream_connect /
 *      pw_core_disconnect 时，换成本库的包装函数；
 *   3. 包装函数里自己走一遍 xdg-desktop-portal ScreenCast 流程（会弹出合成器的
 *      选择框），把真正的 PipeWire fd 和 node id 换进去；断开时关闭 portal 会话。
 *
 * 用法：LD_PRELOAD=libqq-wl-portal.so XDG_SESSION_TYPE=x11 linuxqq ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pthread.h>
#include <pipewire/stream.h>
#include <pulse/introspect.h>
#include <spa/param/video/format-utils.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG(...) fprintf(stderr, "[qq-wl-portal pid=%ld] " __VA_ARGS__), \
                 fputc('\n', stderr)

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"

extern char **environ;

/* ---------- 调用方判断 ---------- */

static int from_broadcast_core(const void *caller)
{
    Dl_info info;

    if (!dladdr(caller, &info) || !info.dli_fname)
        return 0;

    const char *base = strrchr(info.dli_fname, '/');
    return !strcmp(base ? base + 1 : info.dli_fname, "broadcast-core.so");
}

/* ---------- 总开关 ---------- */

#ifndef QQWL_VERSION
#define QQWL_VERSION "dev"
#endif

static char *lookup_env(const char *name);

/* QQ_WL_NATIVE_DISABLE=1 时所有拦截都不生效，用于排查问题。 */
static int enabled(void)
{
    static int state = -1;

    if (state < 0) {
        const char *v = lookup_env("QQ_WL_NATIVE_DISABLE");
        state = !(v && *v && strcmp(v, "0"));
    }
    return state;
}

/* ---------- 1. getenv ---------- */

static char *lookup_env(const char *name)
{
    size_t n = strlen(name);

    for (char **e = environ; e && *e; ++e)
        if (!strncmp(*e, name, n) && (*e)[n] == '=')
            return *e + n + 1;
    return NULL;
}

char *getenv(const char *name)
{
    if (name && !strcmp(name, "XDG_SESSION_TYPE") && enabled() &&
        from_broadcast_core(__builtin_return_address(0)))
        return (char *)"wayland";

    return name ? lookup_env(name) : NULL;
}

/* ---------- 3. portal 流程 ---------- */

struct pw_core;
struct pw_context;
struct pw_properties;
struct pw_stream;
struct spa_pod;

typedef struct pw_core *(*connect_fd_fn)(struct pw_context *, int,
                                         struct pw_properties *, size_t);
typedef int (*stream_connect_fn)(struct pw_stream *, int, uint32_t, int,
                                 const struct spa_pod **, uint32_t);
typedef int (*core_disconnect_fn)(struct pw_core *);

static connect_fd_fn real_connect_fd;
static stream_connect_fn real_stream_connect;
static core_disconnect_fn real_core_disconnect;

/* 当前（唯一）一次共享的状态。QQ 同一时间只有一路屏幕共享。 */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pw_core *active_core;
static char *active_session;
static uint32_t active_node = UINT32_MAX;

/*
 * portal 会话绑定在发起请求的 D-Bus 连接上，连接一断会话就被收回。
 * g_bus_get_sync 返回的共享连接在最后一个引用释放时会被关闭，
 * 而 ppapi 进程里没有别人持有它，所以这里常驻持有一份。
 */
static GDBusConnection *bus(void)
{
    static GDBusConnection *conn;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    if (!conn || g_dbus_connection_is_closed(conn)) {
        GError *error = NULL;
        if (conn)
            g_object_unref(conn);
        conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
        if (!conn) {
            LOG("session bus: %s", (long)getpid(), error->message);
            g_error_free(error);
        }
    }
    pthread_mutex_unlock(&lock);
    return conn;
}

struct waiter {
    GMainLoop *loop;
    guint32 response;
    GVariant *results;
    int done;
};

static void on_response(GDBusConnection *conn, const char *sender,
                        const char *path, const char *iface,
                        const char *signal, GVariant *params, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal;
    struct waiter *w = data;

    g_variant_get(params, "(u@a{sv})", &w->response, &w->results);
    w->done = 1;
    g_main_loop_quit(w->loop);
}

static gboolean on_timeout(gpointer data)
{
    g_main_loop_quit(((struct waiter *)data)->loop);
    return G_SOURCE_REMOVE;
}

/*
 * 调用一个返回 Request 的 portal 方法并等待其 Response 信号。
 * options 必须是尚未 end 的 a{sv} builder，本函数会往里加 handle_token。
 * 成功返回 results（调用方 unref），失败/取消返回 NULL。
 */
static GVariant *portal_request(GDBusConnection *conn, GMainContext *ctx,
                                const char *method, const char *session,
                                GVariantBuilder *options, int timeout_s)
{
    static int counter;
    char token[64];
    snprintf(token, sizeof(token), "qqwlportal%ld_%d",
             (long)getpid(), ++counter);
    g_variant_builder_add(options, "{sv}", "handle_token",
                          g_variant_new_string(token));

    /* 预测 request 路径，先订阅再调用，避免错过信号。 */
    char *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
    for (char *p = sender; *p; ++p)
        if (*p == '.')
            *p = '_';
    char *request_path = g_strdup_printf(
        PORTAL_PATH "/request/%s/%s", sender, token);
    g_free(sender);

    struct waiter w = { .loop = g_main_loop_new(ctx, FALSE) };
    guint sub = g_dbus_connection_signal_subscribe(
        conn, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
        request_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response,
        &w, NULL);

    GVariant *params;
    if (!strcmp(method, "CreateSession"))
        params = g_variant_new("(a{sv})", options);
    else if (!strcmp(method, "Start"))
        params = g_variant_new("(osa{sv})", session, "", options);
    else
        params = g_variant_new("(oa{sv})", session, options);

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        conn, PORTAL_BUS, PORTAL_PATH, SCREENCAST_IFACE, method, params,
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (!reply) {
        LOG("%s failed: %s", (long)getpid(), method, error->message);
        g_error_free(error);
    } else {
        g_variant_unref(reply);
        GSource *timer = g_timeout_source_new_seconds(timeout_s);
        g_source_set_callback(timer, on_timeout, &w, NULL);
        g_source_attach(timer, ctx);
        if (!w.done)
            g_main_loop_run(w.loop);
        g_source_destroy(timer);
        g_source_unref(timer);
        if (!w.done)
            LOG("%s timed out", (long)getpid(), method);
        else if (w.response != 0)
            LOG("%s response=%u (1=用户取消, 2=失败)",
                (long)getpid(), method, w.response);
    }

    g_dbus_connection_signal_unsubscribe(conn, sub);
    g_main_loop_unref(w.loop);
    g_free(request_path);

    if (w.done && w.response == 0)
        return w.results;
    if (w.results)
        g_variant_unref(w.results);
    return NULL;
}

static void close_session(GDBusConnection *conn, const char *session)
{
    GVariant *r = g_dbus_connection_call_sync(
        conn, PORTAL_BUS, session, "org.freedesktop.portal.Session", "Close",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (r)
        g_variant_unref(r);
}

static guint32 cursor_mode(GDBusConnection *conn)
{
    GVariant *r = g_dbus_connection_call_sync(
        conn, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", SCREENCAST_IFACE, "AvailableCursorModes"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    guint32 modes = 0;

    if (r) {
        GVariant *v;
        g_variant_get(r, "(v)", &v);
        modes = g_variant_get_uint32(v);
        g_variant_unref(v);
        g_variant_unref(r);
    }
    /* 2 = 光标画进画面；不支持就 1 = 隐藏。 */
    return (modes & 2) ? 2 : 1;
}

/*
 * 完整走一遍 CreateSession → SelectSources → Start → OpenPipeWireRemote。
 * 成功返回 PipeWire fd 并填 node/session，失败返回 -1。
 */
static int run_portal(uint32_t *node, char **session_out)
{
    GError *error = NULL;
    GDBusConnection *conn = bus();
    if (!conn)
        return -1;

    /* 在本线程私有的 main context 里等信号，不干扰 QQ 自己的事件循环。 */
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    int fd = -1;
    char *session = NULL;
    GVariant *results;
    GVariantBuilder opts;

    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                          g_variant_new_string("qqwlportal"));
    results = portal_request(conn, ctx, "CreateSession", NULL, &opts, 30);
    if (!results)
        goto out;
    g_variant_lookup(results, "session_handle", "s", &session);
    g_variant_unref(results);
    if (!session)
        goto out;

    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(1 | 2));
    g_variant_builder_add(&opts, "{sv}", "multiple",
                          g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "cursor_mode",
                          g_variant_new_uint32(cursor_mode(conn)));
    results = portal_request(conn, ctx, "SelectSources", session, &opts, 30);
    if (!results)
        goto fail;
    g_variant_unref(results);

    /* 这一步合成器弹出选择框，给用户足够时间。 */
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    results = portal_request(conn, ctx, "Start", session, &opts, 300);
    if (!results)
        goto fail;

    GVariant *streams = g_variant_lookup_value(
        results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (streams && g_variant_n_children(streams) > 0)
        g_variant_get_child(streams, 0, "(u@a{sv})", node, NULL);
    else
        *node = UINT32_MAX;
    if (streams)
        g_variant_unref(streams);
    g_variant_unref(results);
    if (*node == UINT32_MAX) {
        LOG("Start returned no stream", (long)getpid());
        goto fail;
    }

    GUnixFDList *fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        conn, PORTAL_BUS, PORTAL_PATH, SCREENCAST_IFACE, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session, NULL), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL, &error);
    if (!reply) {
        LOG("OpenPipeWireRemote: %s", (long)getpid(), error->message);
        g_clear_error(&error);
        goto fail;
    }
    gint32 index;
    g_variant_get(reply, "(h)", &index);
    fd = g_unix_fd_list_get(fds, index, NULL);
    g_variant_unref(reply);
    g_object_unref(fds);
    if (fd < 0)
        goto fail;

    *session_out = session;
    session = NULL;
    goto out;

fail:
    close_session(conn, session);
out:
    g_free(session);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return fd;
}

/* ---------- 包装函数 ---------- */

static struct pw_core *wrap_connect_fd(struct pw_context *context, int fd,
                                       struct pw_properties *props,
                                       size_t user_data_size)
{
    /* fd 其实是 QQ 传来的 X11 显示器编号的低 32 位，不能交给 PipeWire（会被关掉）。 */
    LOG("v" QQWL_VERSION ": broadcast-core asked to connect fd=%d, opening portal",
        (long)getpid(), fd);

    uint32_t node = UINT32_MAX;
    char *session = NULL;
    int real_fd = run_portal(&node, &session);

    if (real_fd < 0) {
        /* 取消或失败：给一个必然连不上的 socket，让 QQ 走它自己的失败路径。 */
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
            return NULL;
        close(pair[1]);
        real_fd = pair[0];
        LOG("portal failed or cancelled; capture will show nothing",
            (long)getpid());
    } else {
        LOG("portal ok: pipewire fd=%d node=%u", (long)getpid(), real_fd,
            node);
    }

    struct pw_core *core = real_connect_fd(context, real_fd, props,
                                           user_data_size);

    pthread_mutex_lock(&state_lock);
    if (active_session) {
        /* 上一轮没走到 disconnect（理论上不会发生），这里兜底关掉。 */
        if (bus())
            close_session(bus(), active_session);
        g_free(active_session);
    }
    active_core = core;
    active_session = session;
    active_node = node;
    pthread_mutex_unlock(&state_lock);
    return core;
}

static int wrap_stream_connect(struct pw_stream *stream, int direction,
                               uint32_t target_id, int flags,
                               const struct spa_pod **params,
                               uint32_t n_params)
{
    pthread_mutex_lock(&state_lock);
    uint32_t node = active_node;
    pthread_mutex_unlock(&state_lock);

    if (node != UINT32_MAX) {
        LOG("stream connect target %u -> %u", (long)getpid(), target_id, node);
        target_id = node;
    }
    return real_stream_connect(stream, direction, target_id, flags, params,
                               n_params);
}

static int wrap_core_disconnect(struct pw_core *core)
{
    char *session = NULL;

    pthread_mutex_lock(&state_lock);
    if (core && core == active_core) {
        session = active_session;
        active_session = NULL;
        active_core = NULL;
        active_node = UINT32_MAX;
    }
    pthread_mutex_unlock(&state_lock);

    int result = real_core_disconnect(core);

    if (session) {
        if (bus())
            close_session(bus(), session);
        LOG("share ended, portal session closed", (long)getpid());
        g_free(session);
    }
    return result;
}

/* ---------- 5. 修正共享内存帧的行跨度 ---------- */

/*
 * QQ 的 OnStreamProcess 对共享内存帧（MemPtr/MemFd）这样处理：
 *     memcpy(pbo, datas[0].data, width * height * 4);
 * 完全忽略 chunk->offset 和 chunk->stride。合成器的每行带填充时
 * （stride > width*4，共享宽度不规整的窗口时很常见），画面会逐行错位成斜条纹。
 *
 * 修法：额外挂一个监听器记录协商出的宽高；取帧时如果行有填充或有偏移，
 * 就把帧重新排成紧凑的一块，临时把 datas[0].data 指过去，还帧时再改回来。
 * DMA-BUF 帧 QQ 会正确使用 stride，不处理。
 */
typedef int (*add_listener_fn)(struct pw_stream *, struct spa_hook *,
                               const struct pw_stream_events *, void *);
typedef struct pw_buffer *(*dequeue_fn)(struct pw_stream *);
typedef int (*queue_fn)(struct pw_stream *, struct pw_buffer *);
typedef void (*stream_destroy_fn)(struct pw_stream *);

static add_listener_fn real_add_listener;
static dequeue_fn real_dequeue;
static queue_fn real_queue;
static stream_destroy_fn real_stream_destroy;

struct stream_info {
    struct stream_info *next;
    struct pw_stream *stream;
    struct spa_hook hook;
    uint32_t width, height;
    uint8_t *packed;
    size_t packed_size;
    struct pw_buffer *patched;
    void *orig_data;
    int32_t logged_stride;
};

static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stream_info *streams;

static struct stream_info *find_stream(struct pw_stream *stream)
{
    pthread_mutex_lock(&streams_lock);
    struct stream_info *s = streams;
    while (s && s->stream != stream)
        s = s->next;
    pthread_mutex_unlock(&streams_lock);
    return s;
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct stream_info *s = data;
    uint32_t media_type, media_subtype;
    struct spa_video_info_raw raw;

    if (id != SPA_PARAM_Format || !param)
        return;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
        media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_zero(raw);
    if (spa_format_video_raw_parse(param, &raw) < 0)
        return;
    s->width = raw.size.width;
    s->height = raw.size.height;
    s->logged_stride = 0;
    LOG("stream format %ux%u (video format %u)", (long)getpid(),
        s->width, s->height, raw.format);
}

static const struct pw_stream_events stride_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
};

static int wrap_add_listener(struct pw_stream *stream, struct spa_hook *hook,
                             const struct pw_stream_events *events, void *data)
{
    int r = real_add_listener(stream, hook, events, data);

    if (!find_stream(stream)) {
        struct stream_info *s = calloc(1, sizeof(*s));
        if (s) {
            s->stream = stream;
            /* 在 QQ 自己的监听器之后注册，param_changed 时两者都会收到。 */
            real_add_listener(stream, &s->hook, &stride_events, s);
            pthread_mutex_lock(&streams_lock);
            s->next = streams;
            streams = s;
            pthread_mutex_unlock(&streams_lock);
        }
    }
    return r;
}

static void wrap_stream_destroy(struct pw_stream *stream)
{
    struct stream_info *s = NULL;

    pthread_mutex_lock(&streams_lock);
    for (struct stream_info **p = &streams; *p; p = &(*p)->next) {
        if ((*p)->stream == stream) {
            s = *p;
            *p = s->next;
            break;
        }
    }
    pthread_mutex_unlock(&streams_lock);

    /* pw_stream_destroy 会清掉流上所有监听器，包括我们的 hook。 */
    real_stream_destroy(stream);
    if (s) {
        free(s->packed);
        free(s);
    }
}

static struct pw_buffer *wrap_dequeue(struct pw_stream *stream)
{
    struct pw_buffer *b = real_dequeue(stream);
    struct stream_info *s;

    if (!b || !b->buffer || b->buffer->n_datas < 1 || !enabled())
        return b;
    s = find_stream(stream);
    if (!s || !s->width || !s->height)
        return b;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->type == SPA_DATA_DmaBuf || !d->data || !d->chunk)
        return b;

    size_t row = (size_t)s->width * 4;
    size_t stride = d->chunk->stride > 0 ? (size_t)d->chunk->stride : row;
    size_t offset = d->chunk->offset;
    if ((stride == row && offset == 0) || stride < row || offset >= d->maxsize)
        return b;

    /* QQ 会从 data 读 width*height*4 字节，所以缓冲区至少这么大。 */
    size_t need = row * s->height;
    if (s->packed_size < need) {
        uint8_t *p = realloc(s->packed, need);
        if (!p)
            return b;
        s->packed = p;
        s->packed_size = need;
    }

    size_t avail = d->maxsize - offset;
    size_t rows = avail >= row ? (avail - row) / stride + 1 : 0;
    if (rows > s->height)
        rows = s->height;

    const uint8_t *src = (const uint8_t *)d->data + offset;
    for (size_t y = 0; y < rows; ++y)
        memcpy(s->packed + y * row, src + y * stride, row);
    if (rows < s->height)
        memset(s->packed + rows * row, 0, (s->height - rows) * row);

    if (s->logged_stride != (int32_t)stride) {
        s->logged_stride = (int32_t)stride;
        LOG("repacking frames: %ux%u stride %zu -> %zu offset %zu", (long)getpid(),
            s->width, s->height, stride, row, offset);
    }

    s->orig_data = d->data;
    s->patched = b;
    d->data = s->packed;
    return b;
}

static int wrap_queue(struct pw_stream *stream, struct pw_buffer *b)
{
    struct stream_info *s = find_stream(stream);

    if (s && b && s->patched == b) {
        b->buffer->datas[0].data = s->orig_data;
        s->patched = NULL;
    }
    return real_queue(stream, b);
}

/* ---------- 2. dlsym ---------- */

typedef void *(*dlsym_fn)(void *, const char *);

/* 汇编蹦床（dlsym_trampoline.S）直接跳到这里，所以不能是 static。 */
__attribute__((visibility("hidden"))) dlsym_fn qqwl_real_dlsym;

static void resolve_dlsym(void)
{
    qqwl_real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!qqwl_real_dlsym)
        qqwl_real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
}

__attribute__((constructor))
static void init_dlsym(void)
{
    if (!qqwl_real_dlsym)
        resolve_dlsym();
}

static void *intercept(void *handle, const char *name)
{
    void *real = qqwl_real_dlsym(handle, name);

    if (!real)
        return NULL;
    if (!strcmp(name, "pw_context_connect_fd")) {
        real_connect_fd = (connect_fd_fn)real;
        return (void *)wrap_connect_fd;
    }
    if (!strcmp(name, "pw_stream_connect")) {
        real_stream_connect = (stream_connect_fn)real;
        return (void *)wrap_stream_connect;
    }
    if (!strcmp(name, "pw_core_disconnect")) {
        real_core_disconnect = (core_disconnect_fn)real;
        return (void *)wrap_core_disconnect;
    }
    if (!strcmp(name, "pw_stream_add_listener")) {
        real_add_listener = (add_listener_fn)real;
        return (void *)wrap_add_listener;
    }
    if (!strcmp(name, "pw_stream_dequeue_buffer")) {
        real_dequeue = (dequeue_fn)real;
        return (void *)wrap_dequeue;
    }
    if (!strcmp(name, "pw_stream_queue_buffer")) {
        real_queue = (queue_fn)real;
        return (void *)wrap_queue;
    }
    if (!strcmp(name, "pw_stream_destroy")) {
        real_stream_destroy = (stream_destroy_fn)real;
        return (void *)wrap_stream_destroy;
    }
    return real;
}

/*
 * 由 dlsym 蹦床调用。返回非 NULL：直接把它作为 dlsym 的结果；
 * 返回 NULL：蹦床用尾跳转把原调用转给真 dlsym。
 *
 * 为什么要蹦床：glibc 的 dlsym 用返回地址判断调用者（RTLD_NEXT 依赖它），
 * 转发时必须保持返回地址不变，即尾跳转而不是普通调用。
 * GCC 15 之前没有 musttail，Debian 12 / Ubuntu 24.04 的编译器做不到，所以用汇编。
 */
__attribute__((visibility("hidden")))
void *qqwl_dlsym_decide(void *handle, const char *name, const void *caller)
{
    if (!qqwl_real_dlsym)
        resolve_dlsym();

    if (name && !strncmp(name, "pw_", 3) && enabled() &&
        from_broadcast_core(caller))
        return intercept(handle, name);
    return NULL;
}

#if !defined(__x86_64__) && !defined(__aarch64__)
/* 其它架构没有汇编蹦床，要求编译器支持 musttail（GCC 15+ / clang 13+）。 */
void *dlsym(void *handle, const char *name)
{
    void *r = qqwl_dlsym_decide(handle, name, __builtin_return_address(0));
    if (r)
        return r;
    __attribute__((musttail)) return qqwl_real_dlsym(handle, name);
}
#endif

/* ---------- 4. 共享设备音频的采样格式 ---------- */

/*
 * broadcast-core 先查默认输出设备的 sample_spec，再按同样的格式去录它的 .monitor。
 * 它只支持 S16LE / FLOAT32LE；其它格式（常见的 S32LE、S24LE）一律报 E_NOTIMPL。
 * 这里在回调里把格式改报成 FLOAT32LE。录音端可以要求任意格式，
 * 由 PipeWire/PulseAudio 负责转换，所以对实际声音没有影响。
 */
/*
 * 取 libpulse 里的真函数。libpulse 往往是作为 broadcast-core 的依赖被 dlopen 进来的，
 * 处在局部作用域，dlsym(RTLD_NEXT) 搜不到（会得到 NULL），所以直接问已加载的 libpulse。
 */
static void *real_pa(const char *name)
{
    void *f = dlsym(RTLD_NEXT, name);
    if (!f) {
        void *h = dlopen("libpulse.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            f = dlsym(h, name);
            dlclose(h);
        }
    }
    if (!f)
        LOG("cannot resolve %s", (long)getpid(), name);
    return f;
}

struct info_trampoline {
    void (*cb)(pa_context *, const void *, int, void *);
    void *userdata;
};

static void fix_spec(pa_sample_spec *spec)
{
    if (spec->format != PA_SAMPLE_S16LE && spec->format != PA_SAMPLE_FLOAT32LE) {
        /* 不调用 pa_sample_format_to_string：本库不链接 libpulse，避免硬依赖。 */
        LOG("device audio: report sample format %d as float32le (5) to broadcast-core",
            (long)getpid(), (int)spec->format);
        spec->format = PA_SAMPLE_FLOAT32LE;
    }
}

static void sink_trampoline(pa_context *c, const pa_sink_info *i, int eol,
                            void *data)
{
    struct info_trampoline *t = data;
    if (i) {
        pa_sink_info copy = *i;
        fix_spec(&copy.sample_spec);
        t->cb(c, &copy, eol, t->userdata);
    } else {
        t->cb(c, NULL, eol, t->userdata);
    }
    if (eol)
        free(t);
}

static void source_trampoline(pa_context *c, const pa_source_info *i, int eol,
                              void *data)
{
    struct info_trampoline *t = data;
    if (i) {
        pa_source_info copy = *i;
        fix_spec(&copy.sample_spec);
        t->cb(c, &copy, eol, t->userdata);
    } else {
        t->cb(c, NULL, eol, t->userdata);
    }
    if (eol)
        free(t);
}

pa_operation *pa_context_get_sink_info_by_name(pa_context *c, const char *name,
                                               pa_sink_info_cb_t cb,
                                               void *userdata)
{
    static pa_operation *(*real)(pa_context *, const char *,
                                 pa_sink_info_cb_t, void *);
    if (!real)
        real = real_pa("pa_context_get_sink_info_by_name");

    struct info_trampoline *t;
    if (cb && enabled() && from_broadcast_core(__builtin_return_address(0)) &&
        (t = malloc(sizeof(*t)))) {
        t->cb = (void *)cb;
        t->userdata = userdata;
        pa_operation *op = real(c, name, sink_trampoline, t);
        if (!op)
            free(t);
        return op;
    }
    return real(c, name, cb, userdata);
}

pa_operation *pa_context_get_source_info_by_name(pa_context *c,
                                                 const char *name,
                                                 pa_source_info_cb_t cb,
                                                 void *userdata)
{
    static pa_operation *(*real)(pa_context *, const char *,
                                 pa_source_info_cb_t, void *);
    if (!real)
        real = real_pa("pa_context_get_source_info_by_name");

    struct info_trampoline *t;
    if (cb && enabled() && from_broadcast_core(__builtin_return_address(0)) &&
        (t = malloc(sizeof(*t)))) {
        t->cb = (void *)cb;
        t->userdata = userdata;
        pa_operation *op = real(c, name, source_trampoline, t);
        if (!op)
            free(t);
        return op;
    }
    return real(c, name, cb, userdata);
}
