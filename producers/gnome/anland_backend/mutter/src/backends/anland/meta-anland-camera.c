#include "config.h"

#include "backends/anland/meta-anland-camera.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <pipewire/pipewire.h>
#include <glib.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

/*
 * Control-channel protocol + stream/shm protocol. MUST stay in sync with the
 * consumer's camera_service.h.
 */
struct camera_ctrl_msg {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t len;
    uint8_t  payload[];
} __attribute__((packed));

#define CAMERA_CTRL_GET_INFO     0x01
#define CAMERA_CTRL_START_RECORD 0x02   /* payload: id(1B) w(2B,LE) h(2B,LE) */
#define CAMERA_CTRL_STOP_RECORD  0x03   /* payload: id(1B) */
#define CAMERA_CTRL_INFO_REPLY   0x81

/* stream_fd control protocol (SEQPACKET). Frame pixels travel through shared memory;
 * this only carries the shm hand-off and the READY/DONE pacing. */
#define CAMERA_SLOTS 2
struct cam_stream_msg {
    uint8_t  type;
    uint8_t  slot;
    uint16_t fmt;   /* READY: CAM_FMT_*; else 0 */
    uint32_t a;     /* SHM_OFFER: slot_bytes; READY: width  */
    uint32_t b;     /* READY: height                        */
} __attribute__((packed));

_Static_assert(sizeof(struct camera_ctrl_msg) == 4,
               "camera control header must match Anland 5.13");
_Static_assert(sizeof(struct cam_stream_msg) == 12,
               "camera stream message must match Anland 5.13");

#define CAM_STREAM_GET_SHM   1
#define CAM_STREAM_SHM_OFFER 2
#define CAM_STREAM_READY     3
#define CAM_STREAM_DONE      4

/* Pixel layout in a slot (matches consumer camera_service.h). All are w*h*3/2 bytes,
 * single block, Y-stride = w, so only the announced SPA format differs. */
#define CAM_FMT_I420 0
#define CAM_FMT_NV12 1
#define CAM_FMT_NV21 2

/* Default resolution we advertise + request before anything is negotiated. The node
 * format then FOLLOWS whatever the consumer actually delivers (every READY carries the
 * real w/h): if the device's CameraX picks a different size, the first frame triggers a
 * re-negotiation so the node always matches the real frames -- no garbage, no truncation. */
#define CAM_DEF_W   1280
#define CAM_DEF_H   720
#define CAM_FPS     30

/* Largest size ever requested from the consumer (same rule as the consumer's
 * camera_size.h), and the standard size requested when it reports more. By default
 * the cap is the standard size itself: calls send at most 720p, and every 1080p frame
 * costs a core-sized share of this 4-CPU container across the bridge copy and the
 * app's conversion and encoders (a call with camera and screen share lagged the
 * desktop). MUTTER_ANLAND_CAMERA_1080P=1 allows up to 1080p (e.g. for OBS). */
#define CAM_CAP_W   1920
#define CAM_CAP_H   1080
#define CAM_CLAMP_W 1280
#define CAM_CLAMP_H 720

static bool camera_allow_1080p(void)
{
    return g_strcmp0(g_getenv("MUTTER_ANLAND_CAMERA_1080P"), "1") == 0;
}

#define MAX_CAMERAS 8
#define RECONNECT_SECS 1
#define MAX_CAMERA_FRAME_BYTES (256U * 1024U * 1024U)

struct cam {
    struct anland_camera *owner;
    int                   index;        /* camera id, for ctrl messages */

    /* Persistent virtual webcam node -- created when a camera first appears, kept
     * alive across consumer disconnects (blank frames keep flowing), destroyed only
     * when the camera count shrinks or the engine stops. */
    struct pw_stream     *node;
    struct spa_hook       listener;
    bool                  streaming;    /* an app is consuming this node */
    bool                  recording;    /* we've told the consumer to START on ctrl */
    bool                  process_seen; /* one-shot debug: on_process was called */

    /* An app is linked: our port has a negotiated format. PipeWire clears it when the
     * last consumer unlinks. Stream state alone can't tell: the node follows the
     * always-running speaker driver, so it stays STREAMING after the app leaves. */
    bool                  has_format;
    /* Grace period before STOP after the format is cleared, so a renegotiation
     * (clear then set) never stops and restarts the Android camera. */
    struct spa_source    *stop_timer;
    bool                  stop_pending;

    /* Current negotiated node format. The node always advertises exactly this, and we
     * re-negotiate it to match the resolution + pixel layout the consumer actually sends. */
    uint32_t              fmt_w, fmt_h;
    int                   fmt_code;   /* CAM_FMT_* the node renegotiates with (NV12) */
    /* Pixel format the linked app negotiated (SPA_VIDEO_FORMAT_YUY2 or _NV12). Each
     * camera frame is converted into it once, when it arrives. */
    uint32_t              out_fmt;

    /* Hot-swapped per (re)connect. -1 while detached -> the node emits blank frames. */
    int                   stream_fd;
    struct spa_source    *io;

    /* Per-camera shared-memory double buffer, offered by the consumer over stream_fd.
     * On READY we copy the named slot out into our own `frame` (so the consumer can
     * reuse the slot immediately) and DONE it; on_process emits `frame` at app pace. */
    int                   shm_fd;       /* owned dup of the consumer's ashmem, -1 none */
    uint8_t              *shm;          /* mmap (PROT_READ), CAMERA_SLOTS*slot_bytes    */
    size_t                shm_bytes;
    size_t                slot_bytes;
    bool                  shm_requested;

    /* Producer-owned copy of the latest frame, already in frame_fmt (loop thread
     * only). on_process hands it to the app once (frame_new), not on every graph
     * cycle: the node follows the always-running audio driver (~47 Hz), and copying
     * a 1080p frame on each cycle kept a core busy. */
    uint8_t              *frame;
    size_t                frame_cap;
    size_t                frame_size;
    uint32_t              frame_fmt, frame_w, frame_h;
    bool                  have_frame;
    bool                  frame_new;
    int64_t               last_blank_us;   /* last gray frame, while not live */
};

struct anland_camera {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *reconnect_timer;
    bool                   pw_connected;
    bool                   loop_started;

    int                    ctrl_fd;     /* owned; shared control socket, -1 when none */
    struct spa_source     *ctrl_io;
    uint8_t                ctrl_buffer[sizeof(struct camera_ctrl_msg) +
                                       1 + MAX_CAMERAS * 4];
    size_t                 ctrl_buffer_len;
    bool                   info_pending;
    int                    num_cameras; /* number of live nodes */
    struct cam             cams[MAX_CAMERAS];
};

static int  build_pw(struct anland_camera *c);
static void teardown_pw(struct anland_camera *c);
static void create_nodes(struct anland_camera *c);
static void destroy_nodes(struct anland_camera *c);
static int  create_node(struct cam *cam);
static void destroy_node(struct cam *cam);
static void on_stream_readable(void *data, int fd, uint32_t mask);
static void attach_fd(struct cam *cam, int fd);
static void detach_fd(struct cam *cam);
static void clear_resources_locked(struct anland_camera *c);
static void renegotiate_format(struct cam *cam, uint32_t w, uint32_t h,
                               int fmt_code);
#define CAM_N_OUT_FMTS 2
static size_t yuy2_frame_size(uint32_t w, uint32_t h);
static uint32_t build_video_formats(struct spa_pod_builder *b, uint32_t w, uint32_t h,
                                    const struct spa_pod **params);

static bool camera_frame_size(uint32_t width, uint32_t height, size_t *size)
{
    size_t pixels;

    if (width == 0 || height == 0 || width > UINT16_MAX ||
        height > UINT16_MAX || width > SIZE_MAX / height)
        return false;
    pixels = (size_t)width * height;
    if (pixels > SIZE_MAX - pixels / 2 ||
        pixels + pixels / 2 > MAX_CAMERA_FRAME_BYTES)
        return false;
    *size = pixels + pixels / 2;
    return true;
}

static bool valid_camera_format(int format)
{
    return format == CAM_FMT_I420 || format == CAM_FMT_NV12 ||
           format == CAM_FMT_NV21;
}

/* ---- control channel ---- */

static void send_ctrl(struct anland_camera *c, uint8_t type,
                      const uint8_t *payload, uint16_t len)
{
    if (c->ctrl_fd < 0)
        return;
    uint8_t buf[sizeof(struct camera_ctrl_msg) + 8];
    if (len > sizeof(buf) - sizeof(struct camera_ctrl_msg))
        return;
    struct camera_ctrl_msg *h = (struct camera_ctrl_msg *)buf;
    h->type = type;
    h->reserved = 0;
    h->len = GUINT16_TO_LE(len);
    if (len)
        memcpy(buf + sizeof(*h), payload, len);
    send(c->ctrl_fd, buf, sizeof(*h) + len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static bool
handle_camera_info(struct anland_camera *c,
                   const uint8_t       *payload,
                   size_t               payload_size)
{
    uint8_t n_cameras;

    if (payload_size < 1)
        return false;

    n_cameras = payload[0];
    if (n_cameras != c->num_cameras ||
        payload_size != 1 + (size_t) n_cameras * 4)
        return false;

    for (int i = 0; i < n_cameras; i++) {
        struct cam *cam = &c->cams[i];
        uint16_t wire_width;
        uint16_t wire_height;
        uint32_t width;
        uint32_t height;
        size_t frame_size;

        memcpy(&wire_width, payload + 1 + (size_t) i * 4,
               sizeof(wire_width));
        memcpy(&wire_height, payload + 1 + (size_t) i * 4 + 2,
               sizeof(wire_height));
        width = GUINT16_FROM_LE(wire_width);
        height = GUINT16_FROM_LE(wire_height);
        if (!camera_frame_size(width, height, &frame_size))
            return false;

        /* Never ask the consumer for more than the cap (720p by default, 1080p at
         * most), whatever it reports: an older consumer reports the full sensor size,
         * and streaming it once OOM'd the whole Android device. 1280x720 is listed by
         * practically every camera; the consumer caps at 1080p by the same rule
         * (camera_size.h). */
        const uint32_t cap_w = camera_allow_1080p() ? CAM_CAP_W : CAM_CLAMP_W;
        const uint32_t cap_h = camera_allow_1080p() ? CAM_CAP_H : CAM_CLAMP_H;
        if (width > cap_w || height > cap_h) {
            g_message("anland-camera: camera %d reports %ux%u, requesting %ux%u",
                      i, width, height, CAM_CLAMP_W, CAM_CLAMP_H);
            width = CAM_CLAMP_W;
            height = CAM_CLAMP_H;
        }

        if (!cam->node) {
            cam->fmt_w = width;
            cam->fmt_h = height;
        } else if (!cam->have_frame &&
                   (cam->fmt_w != width || cam->fmt_h != height)) {
            renegotiate_format(cam, width, height, cam->fmt_code);
        }
    }

    return true;
}

static bool
consume_control_messages(struct anland_camera *c)
{
    while (c->ctrl_buffer_len >= sizeof(struct camera_ctrl_msg)) {
        struct camera_ctrl_msg header;
        size_t message_size;

        memcpy(&header, c->ctrl_buffer, sizeof(header));
        header.len = GUINT16_FROM_LE(header.len);
        if (header.len > sizeof(c->ctrl_buffer) - sizeof(header))
            return false;

        message_size = sizeof(header) + header.len;
        if (c->ctrl_buffer_len < message_size)
            return true;

        /* Accept every well-formed INFO_REPLY, not only the one we are waiting for.
         * The consumer keeps one control socket per window and hands it over again
         * on each session activation, so a reply to an earlier GET_INFO can arrive
         * after a newer request was sent. Rejecting that duplicate used to tear down
         * the whole camera channel at login (no START ever sent, grey frames
         * forever). The reply only carries sizes, so handling it twice is harmless. */
        if (header.type != CAMERA_CTRL_INFO_REPLY || header.reserved != 0 ||
            !handle_camera_info(c, c->ctrl_buffer + sizeof(header), header.len)) {
            g_warning("anland-camera: rejected control message type 0x%02x "
                      "(reserved %u, len %u, %d camera(s) attached, %s)",
                      header.type, header.reserved, header.len, c->num_cameras,
                      c->info_pending ? "reply pending" : "no reply pending");
            return false;
        }

        c->info_pending = false;
        c->ctrl_buffer_len -= message_size;
        memmove(c->ctrl_buffer, c->ctrl_buffer + message_size,
                c->ctrl_buffer_len);
    }

    return true;
}

static void
on_control_readable(void *data,
                    int   fd,
                    uint32_t mask)
{
    struct anland_camera *c = data;

    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        clear_resources_locked(c);
        return;
    }

    while (mask & SPA_IO_IN) {
        ssize_t n;

        if (c->ctrl_buffer_len == sizeof(c->ctrl_buffer) || fd != c->ctrl_fd) {
            clear_resources_locked(c);
            return;
        }

        n = recv(fd, c->ctrl_buffer + c->ctrl_buffer_len,
                 sizeof(c->ctrl_buffer) - c->ctrl_buffer_len, MSG_DONTWAIT);
        if (n > 0) {
            c->ctrl_buffer_len += (size_t) n;
            if (!consume_control_messages(c)) {
                g_warning("Anland camera control channel sent an invalid reply");
                clear_resources_locked(c);
                return;
            }
            continue;
        }
        if (n == 0) {
            clear_resources_locked(c);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)
            return;
#if EWOULDBLOCK != EAGAIN
        if (errno == EWOULDBLOCK)
            return;
#endif
        clear_resources_locked(c);
        return;
    }
}

static void
request_camera_info(struct anland_camera *c)
{
    struct camera_ctrl_msg request = {
        .type = CAMERA_CTRL_GET_INFO,
        .reserved = 0,
        .len = 0,
    };

    if (c->ctrl_fd < 0)
        return;

    if (send(c->ctrl_fd, &request, sizeof(request),
             MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t) sizeof(request))
        c->info_pending = true;
}

/* Send a fixed stream-control message (GET_SHM / DONE) on a camera's stream socket. */
static void send_stream(struct cam *cam, uint8_t type, uint8_t slot)
{
    if (cam->stream_fd < 0)
        return;
    struct cam_stream_msg m = { .type = type, .slot = slot };
    send(cam->stream_fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* Ask the consumer to hand over the shm fd (once per (re)attach, while streaming). */
static void request_shm(struct cam *cam)
{
    if (!cam->streaming || cam->stream_fd < 0 || cam->shm || cam->shm_requested)
        return;
    cam->shm_requested = true;
    send_stream(cam, CAM_STREAM_GET_SHM, 0);
}

/* Reconcile the "should the consumer be recording?" state with what we've told it.
 * Recording is wanted when an app is consuming the node AND we have a live control
 * socket + stream fd. Requests the consumer at the node's current format. */
static void update_recording(struct cam *cam)
{
    const bool want = cam->streaming && cam->has_format &&
                      cam->stream_fd >= 0 && cam->owner->ctrl_fd >= 0;
    if (want == cam->recording)
        return;
    /* Viewer just left: the grace timer decides (on_stop_timer), not other paths. */
    if (!want && cam->stop_pending)
        return;
    if (want) {
        uint8_t p[5];
        uint16_t w = GUINT16_TO_LE((uint16_t)cam->fmt_w);
        uint16_t h = GUINT16_TO_LE((uint16_t)cam->fmt_h);
        p[0] = (uint8_t)cam->index;
        memcpy(p + 1, &w, sizeof(w));
        memcpy(p + 3, &h, sizeof(h));
        send_ctrl(cam->owner, CAMERA_CTRL_START_RECORD, p, sizeof(p));
        g_debug("anland-camera: start camera %d at %ux%u\n",
                   cam->index, cam->fmt_w, cam->fmt_h);
        request_shm(cam);   /* pull the shm fd so frames can flow */
    } else if (cam->owner->ctrl_fd >= 0) {
        uint8_t id = (uint8_t)cam->index;
        send_ctrl(cam->owner, CAMERA_CTRL_STOP_RECORD, &id, 1);
        g_debug("anland-camera: stop camera %d\n", cam->index);
    }
    cam->recording = want;
}

/* ---- frame emission ---- */

/* Gray frames while detached / before the first frame: enough to keep an app's
 * pipeline alive and show something, at a fraction of the graph rate. */
#define CAM_BLANK_INTERVAL_US (100 * 1000)

/* The graph calls this on every cycle of the driver it follows. Emit each received
 * frame once, or neutral gray (rate-limited) while detached / before the first frame
 * arrives, so a consuming app always gets a feed. Runs on the loop thread, same as the
 * stream-socket reader, so cam->frame needs no lock. */
static void on_process(void *data)
{
    struct cam *cam = data;
    if (!cam->node)
        return;

    const bool yuy2 = cam->out_fmt == SPA_VIDEO_FORMAT_YUY2;
    const bool live = cam->have_frame && cam->stream_fd >= 0 &&
                      cam->frame_fmt == cam->out_fmt &&
                      cam->frame_w == cam->fmt_w && cam->frame_h == cam->fmt_h;
    const int64_t now = g_get_monotonic_time();

    if (live && !cam->frame_new)
        return;   /* no new camera frame since the last one: the app keeps it */
    if (!live && now - cam->last_blank_us < CAM_BLANK_INTERVAL_US)
        return;

    struct pw_buffer *pwb = pw_stream_dequeue_buffer(cam->node);
    if (!cam->process_seen) {
        cam->process_seen = true;
    }
    if (!pwb)
        return;
    struct spa_data *d = &pwb->buffer->datas[0];
    if (d->data && d->chunk) {
        size_t out_bytes = 0;

        if (yuy2)
            out_bytes = yuy2_frame_size(cam->fmt_w, cam->fmt_h);
        else if (!camera_frame_size(cam->fmt_w, cam->fmt_h, &out_bytes))
            out_bytes = SIZE_MAX;
        if (out_bytes > d->maxsize) {
            pw_stream_queue_buffer(cam->node, pwb);
            return;         /* buffers of the previous format: wait for the new pool */
        }
        if (live) {
            memcpy(d->data, cam->frame, out_bytes);
            cam->frame_new = false;
        } else {
            memset(d->data, 128, out_bytes);   /* neutral gray, in NV12 and YUY2 alike */
            cam->last_blank_us = now;
        }
        d->chunk->offset = 0;
        d->chunk->stride = yuy2 ? (int32_t)cam->fmt_w * 2 : (int32_t)cam->fmt_w;
        d->chunk->size = (uint32_t)out_bytes;
    }
    pw_stream_queue_buffer(cam->node, pwb);
}

/* Re-announce the node's format to match what the consumer is actually sending (size
 * AND pixel layout), so the advertised caps always equal the real frames. The app
 * renegotiates and on_param_changed resizes the buffer pool. fmt_* are updated
 * optimistically so this doesn't re-fire on every subsequent frame. */
static void renegotiate_format(struct cam *cam, uint32_t w, uint32_t h, int fmt_code)
{
    size_t frame_size;

    if (!cam->node || !valid_camera_format(fmt_code) ||
        !camera_frame_size(w, h, &frame_size))
        return;
    g_debug("anland-camera: camera %d format %ux%u/%d -> %ux%u/%d\n",
               cam->index, cam->fmt_w, cam->fmt_h, cam->fmt_code,
               w, h, fmt_code);
    cam->fmt_w = w;
    cam->fmt_h = h;
    cam->fmt_code = fmt_code;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[CAM_N_OUT_FMTS];
    pw_stream_update_params(cam->node, params, build_video_formats(&b, w, h, params));
}

/* ---- stream socket: shm hand-off + READY/DONE pacing ---- */

static void unmap_shm(struct cam *cam)
{
    if (cam->shm) {
        munmap(cam->shm, cam->shm_bytes);
        cam->shm = NULL;
    }
    if (cam->shm_fd >= 0) {
        close(cam->shm_fd);
        cam->shm_fd = -1;
    }
    cam->shm_bytes = cam->slot_bytes = 0;
    cam->shm_requested = false;
}

/* Copy one consumer frame (w*h*3/2, Y stride w) into dst as NV12, the format the node
 * advertises. libwebrtc (Firefox/Zen, Chromium) has no NV21 mapping: offered only NV21
 * the camera had zero usable formats and Zen crashed on the empty list. NV21 -> NV12
 * is a swap of each chroma byte pair; I420 -> NV12 interleaves the U and V planes. */
static void copy_frame_as_nv12(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h,
                               int fmt)
{
    const size_t y_size = (size_t)w * h;
    const size_t c_size = y_size / 2;

    memcpy(dst, src, y_size);
    dst += y_size;
    src += y_size;
    switch (fmt) {
    case CAM_FMT_NV21:
        for (size_t i = 0; i + 1 < c_size; i += 2) {
            dst[i] = src[i + 1];      /* U */
            dst[i + 1] = src[i];      /* V */
        }
        break;
    case CAM_FMT_I420: {
        const uint8_t *u = src, *v = src + c_size / 2;
        for (size_t i = 0; i < c_size / 2; i++) {
            dst[2 * i] = u[i];
            dst[2 * i + 1] = v[i];
        }
        break;
    }
    default:   /* CAM_FMT_NV12 */
        memcpy(dst, src, c_size);
        break;
    }
}

static size_t yuy2_frame_size(uint32_t w, uint32_t h)
{
    return (size_t)w * h * 2;
}

/* Copy one consumer frame (NV21, NV12 or I420; Y stride w) into dst as YUY2 (Y0 U Y1 V
 * per pixel pair), each chroma row serving two output rows. OBS reads NV12 from shared
 * memory with a wrong row size (its format table gives NV12 2 bytes per pixel) and
 * crashes copying the first frame; YUY2 really is 2 bytes per pixel, so OBS handles it. */
static void copy_frame_as_yuy2(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h,
                               int fmt)
{
    const uint8_t *chroma = src + (size_t)w * h;
    const uint8_t *u, *v;
    size_t c_stride, c_step;

    switch (fmt) {
    case CAM_FMT_NV21:
        v = chroma;
        u = chroma + 1;
        c_stride = w;
        c_step = 2;
        break;
    case CAM_FMT_I420:
        u = chroma;
        v = chroma + (size_t)(w / 2) * (h / 2);
        c_stride = w / 2;
        c_step = 1;
        break;
    default:   /* CAM_FMT_NV12 */
        u = chroma;
        v = chroma + 1;
        c_stride = w;
        c_step = 2;
        break;
    }

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * w;
        const uint8_t *ur = u + (size_t)(y / 2) * c_stride;
        const uint8_t *vr = v + (size_t)(y / 2) * c_stride;
        uint8_t *out = dst + (size_t)y * w * 2;

        for (uint32_t x = 0, i = 0; x + 1 < w; x += 2, i += c_step) {
            const uint32_t px = (uint32_t)row[x] | (uint32_t)ur[i] << 8 |
                                (uint32_t)row[x + 1] << 16 | (uint32_t)vr[i] << 24;
            memcpy(out, &px, sizeof(px));   /* little-endian: Y0 U Y1 V */
            out += 4;
        }
    }
}

static void handle_stream_msg(struct cam *cam, const struct cam_stream_msg *m, int rfd)
{
    switch (m->type) {
    case CAM_STREAM_SHM_OFFER: {
        size_t slot_bytes = m->a;
        size_t total;

        if (rfd < 0 || slot_bytes == 0 ||
            slot_bytes > MAX_CAMERA_FRAME_BYTES ||
            slot_bytes > SIZE_MAX / CAMERA_SLOTS) {
            if (rfd >= 0)
                close(rfd);
            return;
        }
        total = (size_t)CAMERA_SLOTS * slot_bytes;
        if (cam->shm)
            munmap(cam->shm, cam->shm_bytes);
        if (cam->shm_fd >= 0)
            close(cam->shm_fd);
        cam->shm_fd = rfd;
        cam->slot_bytes = slot_bytes;
        cam->shm_bytes = total;
        cam->shm = mmap(NULL, total, PROT_READ, MAP_SHARED, rfd, 0);
        if (cam->shm == MAP_FAILED) {
            cam->shm = NULL;
            close(rfd);
            cam->shm_fd = -1;
            cam->shm_bytes = cam->slot_bytes = 0;
            return;
        }
        g_debug("anland-camera: camera %d mapped %zu bytes per slot\n",
                   cam->index, slot_bytes);
        break;
    }
    case CAM_STREAM_READY: {
        if (rfd >= 0)
            close(rfd);   /* READY carries no fd */
        uint8_t slot = m->slot;
        uint32_t w = m->a, h = m->b;
        int fmt = m->fmt;
        size_t bytes;
        bool valid = valid_camera_format(fmt) &&
                     camera_frame_size(w, h, &bytes);
        if (cam->shm && slot < CAMERA_SLOTS && valid &&
            bytes <= cam->slot_bytes) {
            const bool yuy2 = cam->out_fmt == SPA_VIDEO_FORMAT_YUY2;
            const size_t out_bytes = yuy2 ? yuy2_frame_size(w, h) : bytes;
            const uint8_t *slot_data = cam->shm + (size_t)slot * cam->slot_bytes;

            if (cam->frame_cap < out_bytes) {
                uint8_t *nb = realloc(cam->frame, out_bytes);
                if (nb) {
                    cam->frame = nb;
                    cam->frame_cap = out_bytes;
                }
            }
            if (cam->frame_cap >= out_bytes && out_bytes > 0) {
                /* Copy out of the slot promptly so the consumer can reuse it, converting
                 * once into the format the app negotiated. */
                if (yuy2)
                    copy_frame_as_yuy2(cam->frame, slot_data, w, h, fmt);
                else
                    copy_frame_as_nv12(cam->frame, slot_data, w, h, fmt);
                cam->frame_size = out_bytes;
                cam->frame_fmt = cam->out_fmt;
                cam->frame_w = w;
                cam->frame_h = h;
                cam->have_frame = true;
                cam->frame_new = true;
            }
        }
        /* Release the slot (even if we couldn't read, so the consumer never wedges). */
        send_stream(cam, CAM_STREAM_DONE, slot);
        /* The pixel layout is always converted to NV12 above; only a size change
         * needs a renegotiation. */
        if (valid && (w != cam->fmt_w || h != cam->fmt_h))
            renegotiate_format(cam, w, h, CAM_FMT_NV12);
        break;
    }
    default:
        if (rfd >= 0)
            close(rfd);
        break;
    }
}

static void on_stream_readable(void *data, int fd, uint32_t mask)
{
    struct cam *cam = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP))
        return;
    if (!(mask & SPA_IO_IN))
        return;

    for (;;) {
        struct cam_stream_msg m;
        int rfd = -1;
        struct iovec iov = { .iov_base = &m, .iov_len = sizeof(m) };
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsg;
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                              .msg_control = cmsg.buf, .msg_controllen = sizeof(cmsg.buf) };
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (n <= 0)
            break;
        if (msg.msg_flags & MSG_CTRUNC)
            continue;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
                c->cmsg_len < CMSG_LEN(sizeof(int)))
                continue;
            memcpy(&rfd, CMSG_DATA(c), sizeof(rfd));
            break;
        }
        if (n == (ssize_t)sizeof(m))
            handle_stream_msg(cam, &m, rfd);
        else if (rfd >= 0)
            close(rfd);
    }
}

/* Point a camera at a fresh stream socket (or -1 to detach). Drops the old shm mapping
 * (the new connection re-offers it) so a stale region never bleeds into the new one. */
static void attach_fd(struct cam *cam, int fd)
{
    struct pw_loop *loop = pw_thread_loop_get_loop(cam->owner->loop);
    if (cam->io) {
        pw_loop_destroy_source(loop, cam->io);
        cam->io = NULL;
    }
    if (cam->stream_fd >= 0)
        close(cam->stream_fd);
    cam->stream_fd = fd;
    unmap_shm(cam);                   /* re-requested on the new connection */
    cam->have_frame = false;          /* show blank until a fresh frame arrives */
    if (fd >= 0)
        cam->io = pw_loop_add_io(loop, fd, SPA_IO_IN, false, on_stream_readable, cam);
    if (fd >= 0 && !cam->io) {
        close(cam->stream_fd);
        cam->stream_fd = -1;
    }
    update_recording(cam);            /* START + GET_SHM if an app is consuming */
    request_shm(cam);                 /* also (re)request if already streaming */
}

static void detach_fd(struct cam *cam)
{
    attach_fd(cam, -1);
}

/* ---- Video/Source node ---- */

/* The formats an app may pick, as separate EnumFormat params (OBS reads one plain
 * format per param). YUY2 first: an app that leaves the choice to PipeWire (OBS by
 * default) gets the first, and OBS crashes on NV12 (see nv12_to_yuy2). libwebrtc
 * (Zen, Chromium) maps both. Returns the number of params written. */
static uint32_t build_video_formats(struct spa_pod_builder *b, uint32_t w, uint32_t h,
                                    const struct spa_pod **params)
{
    static const uint32_t formats[CAM_N_OUT_FMTS] = {
        SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_NV12,
    };

    for (uint32_t i = 0; i < CAM_N_OUT_FMTS; i++) {
        struct spa_video_info_raw info;
        spa_zero(info);
        info.format = formats[i];
        info.size = SPA_RECTANGLE(w, h);
        info.framerate = SPA_FRACTION(CAM_FPS, 1);
        params[i] = spa_format_video_raw_build(b, SPA_PARAM_EnumFormat, &info);
    }
    return CAM_N_OUT_FMTS;
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    struct cam *cam = data;
    (void)error;
    (void)old;
    const bool streaming = (state == PW_STREAM_STATE_STREAMING);
    if (streaming == cam->streaming)
        return;
    cam->streaming = streaming;
    update_recording(cam);
}

/* The format was negotiated: parse it and declare a buffer pool of the matching size.
 * Without this PipeWire never allocates correctly-sized buffers and
 * pw_stream_dequeue_buffer() returns NULL forever, so on_process can never emit. */
#define CAM_STOP_GRACE_SECS 2

static void arm_stop_timer(struct cam *cam, bool arm)
{
    struct timespec value = { .tv_sec = arm ? CAM_STOP_GRACE_SECS : 0, .tv_nsec = 0 };

    if (!cam->stop_timer)
        return;
    pw_loop_update_timer(pw_thread_loop_get_loop(cam->owner->loop), cam->stop_timer,
                         &value, NULL, false);
    cam->stop_pending = arm;
}

/* Grace period over: if no app re-linked, stop the Android camera. */
static void on_stop_timer(void *data, uint64_t expirations)
{
    struct cam *cam = data;
    (void)expirations;

    cam->stop_pending = false;
    if (!cam->has_format && cam->recording)
        g_message("anland-camera: camera %d has no viewer, stopping", cam->index);
    update_recording(cam);
    /* Rebuild the node. It follows the always-running anland-speaker driver, so it
     * stays RUNNING after the last viewer and GNOME Shell keeps its "camera in use"
     * indicator lit although the Android camera is off. A fresh node is suspended
     * and links normally (pw_stream_set_active(false) instead left it unable to
     * negotiate with the next viewer). The stream fd, its shm and reader survive. */
    if (!cam->has_format && cam->node && cam->owner->pw_connected) {
        struct pw_loop *loop = pw_thread_loop_get_loop(cam->owner->loop);
        destroy_node(cam);
        create_node(cam);
        if (cam->node && cam->stream_fd >= 0 && !cam->io)
            cam->io = pw_loop_add_io(loop, cam->stream_fd, SPA_IO_IN, false,
                                     on_stream_readable, cam);
    }
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct cam *cam = data;
    if (id != SPA_PARAM_Format)
        return;

    if (!param) {
        /* Last consumer unlinked. Stop after the grace period unless one returns. */
        cam->has_format = false;
        if (cam->recording)
            arm_stop_timer(cam, true);
        return;
    }
    if (!cam->has_format)
        g_message("anland-camera: camera %d has a viewer", cam->index);
    cam->has_format = true;
    if (cam->stop_pending)
        arm_stop_timer(cam, false);

    uint32_t mtype, msubtype;
    if (spa_format_parse(param, &mtype, &msubtype) < 0 ||
        mtype != SPA_MEDIA_TYPE_video || msubtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    struct spa_video_info_raw raw;
    spa_zero(raw);
    if (spa_format_video_raw_parse(param, &raw) < 0)
        return;
    if (raw.size.width)
        cam->fmt_w = raw.size.width;
    if (raw.size.height)
        cam->fmt_h = raw.size.height;
    cam->out_fmt = raw.format == SPA_VIDEO_FORMAT_YUY2 ? SPA_VIDEO_FORMAT_YUY2
                                                       : SPA_VIDEO_FORMAT_NV12;
    g_message("anland-camera: camera %d sends %s %ux%u", cam->index,
              cam->out_fmt == SPA_VIDEO_FORMAT_YUY2 ? "YUY2" : "NV12",
              cam->fmt_w, cam->fmt_h);

    size_t frame_size;
    if (!camera_frame_size(cam->fmt_w, cam->fmt_h, &frame_size))
        return;
    uint32_t stride = cam->fmt_w;
    if (cam->out_fmt == SPA_VIDEO_FORMAT_YUY2) {
        frame_size = yuy2_frame_size(cam->fmt_w, cam->fmt_h);
        stride = cam->fmt_w * 2;
    }
    if (frame_size > INT32_MAX)
        return;

    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)frame_size),
        SPA_PARAM_BUFFERS_stride,   SPA_POD_Int((int)stride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
            (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    pw_stream_update_params(cam->node, params, 1);
    update_recording(cam);   /* a viewer is linked: START if not already recording */
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static int connect_node(struct cam *cam)
{
    char name[32], desc[48];
    snprintf(name, sizeof(name), "anland-camera-%d", cam->index);
    snprintf(desc, sizeof(desc), "Anland remote camera %d", cam->index);

    cam->node = pw_stream_new(cam->owner->core, name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_NODE_NAME, name,
            PW_KEY_NODE_DESCRIPTION, desc,
            NULL));
    if (!cam->node)
        return -1;
    pw_stream_add_listener(cam->node, &cam->listener, &stream_events, cam);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[CAM_N_OUT_FMTS];
    uint32_t n_params = build_video_formats(&b, cam->fmt_w, cam->fmt_h, params);

    /* No DRIVER flag: the consuming app (graph) drives, so PipeWire calls on_process
     * on demand. No RT_PROCESS either, so on_process runs on the same loop thread as
     * the stream-socket reader and shares cam->frame without a lock. */
    int ret = pw_stream_connect(cam->node, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                PW_STREAM_FLAG_MAP_BUFFERS, params, n_params);
    if (ret < 0) {
        spa_hook_remove(&cam->listener);
        pw_stream_destroy(cam->node);
        cam->node = NULL;
    }
    return ret;
}

/* Create the node for one camera slot (no fd attached yet). PipeWire drives its
 * .process whenever an app consumes it. */
static int create_node(struct cam *cam)
{
    if (cam->node)
        return 0;
    cam->streaming = false;
    cam->recording = false;
    cam->process_seen = false;
    cam->has_format = false;
    if (cam->fmt_w == 0 || cam->fmt_h == 0) {
        cam->fmt_w = CAM_DEF_W;
        cam->fmt_h = CAM_DEF_H;
    }
    /* Frames are kept as NV12 (converted on copy) and offered as YUY2 or NV12 -- no
     * live format switch (which crashes downstream gst). Only resolution ever
     * renegotiates. */
    cam->fmt_code = CAM_FMT_NV12;
    cam->out_fmt = SPA_VIDEO_FORMAT_NV12;
    return connect_node(cam);
}

/* (Re)create nodes for the current camera count, e.g. after a PipeWire reconnect.
 * The stream fds in cam->stream_fd are preserved, so readers are re-armed. */
static void create_nodes(struct anland_camera *c)
{
    if (!c->pw_connected || !c->core)
        return;
    struct pw_loop *loop = pw_thread_loop_get_loop(c->loop);
    for (int i = 0; i < c->num_cameras; i++) {
        struct cam *cam = &c->cams[i];
        if (cam->node)
            continue;
        create_node(cam);
        if (cam->stream_fd >= 0 && !cam->io)
            cam->io = pw_loop_add_io(loop, cam->stream_fd, SPA_IO_IN, false,
                                     on_stream_readable, cam);
    }
}

/* Destroy one camera's node and reader. Keeps stream_fd (caller decides). */
static void destroy_node(struct cam *cam)
{
    struct pw_loop *loop = cam->owner && cam->owner->loop
                               ? pw_thread_loop_get_loop(cam->owner->loop) : NULL;
    if (cam->node) {
        spa_hook_remove(&cam->listener);
        pw_stream_destroy(cam->node);
        cam->node = NULL;
    }
    if (cam->io && loop) {
        pw_loop_destroy_source(loop, cam->io);
        cam->io = NULL;
    }
    cam->streaming = false;
    cam->recording = false;
    cam->has_format = false;
    if (cam->stop_pending)
        arm_stop_timer(cam, false);
}

static void destroy_nodes(struct anland_camera *c)
{
    for (int i = 0; i < MAX_CAMERAS; i++)
        destroy_node(&c->cams[i]);
}

/* ---- PipeWire connection lifecycle (mirrors the audio engine) ---- */

static void arm_reconnect(struct anland_camera *c)
{
    struct timespec val = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(c->loop), c->reconnect_timer,
                         &val, NULL, false);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_camera *c = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        c->pw_connected = false;
        arm_reconnect(c);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static int build_pw(struct anland_camera *c)
{
    c->core = pw_context_connect(c->context, NULL, 0);
    if (!c->core)
        return -1;
    pw_core_add_listener(c->core, &c->core_listener, &core_events, c);
    return 0;
}

static void teardown_pw(struct anland_camera *c)
{
    destroy_nodes(c);
    if (c->core) {
        spa_hook_remove(&c->core_listener);
        pw_core_disconnect(c->core);
        c->core = NULL;
    }
}

static void on_reconnect_timer(void *data, uint64_t expirations)
{
    struct anland_camera *c = data;
    (void)expirations;
    if (c->pw_connected)
        return;

    teardown_pw(c);
    if (build_pw(c) == 0) {
        c->pw_connected = true;
        create_nodes(c);   /* rebuild nodes from the still-valid stream fds */
    } else {
        teardown_pw(c);
        arm_reconnect(c);
    }
}

/* ---- public API ---- */

static void close_resource_set(int ctrl_fd, const int *stream_fds, int num_cameras)
{
    if (ctrl_fd >= 0)
        close(ctrl_fd);
    if (!stream_fds)
        return;
    for (int i = 0; i < num_cameras; i++)
        if (stream_fds[i] >= 0)
            close(stream_fds[i]);
}

void meta_anland_camera_set_resources(struct anland_camera *c, int ctrl_fd,
                                 const int *stream_fds, int num_cameras)
{
    if (!c || !stream_fds || num_cameras < 0) {
        close_resource_set(ctrl_fd, stream_fds,
                           num_cameras > 0 ? num_cameras : 0);
        return;
    }

    int accepted = num_cameras < MAX_CAMERAS ? num_cameras : MAX_CAMERAS;
    for (int i = accepted; i < num_cameras; i++) {
        if (stream_fds[i] >= 0)
            close(stream_fds[i]);
    }

    pw_thread_loop_lock(c->loop);

    /* Swap in the new control socket. */
    if (c->ctrl_io) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->ctrl_io);
        c->ctrl_io = NULL;
    }
    if (c->ctrl_fd >= 0)
        close(c->ctrl_fd);
    c->ctrl_fd = ctrl_fd;
    c->ctrl_buffer_len = 0;
    c->info_pending = false;
    if (c->ctrl_fd >= 0) {
        c->ctrl_io = pw_loop_add_io(pw_thread_loop_get_loop(c->loop), c->ctrl_fd,
                                    SPA_IO_IN, false, on_control_readable, c);
        if (!c->ctrl_io) {
            g_warning("Failed to monitor the Anland camera control channel");
            clear_resources_locked(c);
            pw_thread_loop_unlock(c->loop);
            return;
        }
    }

    /* Pop excess cameras if the new connection exposes fewer than before. */
    for (int i = accepted; i < c->num_cameras; i++) {
        destroy_node(&c->cams[i]);
        detach_fd(&c->cams[i]);   /* closes the now-stale stream fd */
    }

    /* Create any newly-appeared cameras, then (re)attach every live one's stream fd.
     * Existing nodes are kept (apply-in-place, like the audio engine hot-swaps its
     * socket) so consuming apps never see the webcam disappear across a reconnect. */
    c->num_cameras = accepted;
    for (int i = 0; i < accepted; i++) {
        struct cam *cam = &c->cams[i];
        cam->owner = c;
        cam->index = i;
        if (c->pw_connected && c->core)
            create_node(cam);
        attach_fd(cam, stream_fds[i]);
    }

    /* The reply is parsed by the PipeWire loop, so a missing consumer response cannot
     * stall Mutter's main context. It also confirms that the resource fd count and
     * advertised physical cameras agree. */
    request_camera_info(c);

    pw_thread_loop_unlock(c->loop);
}

static void clear_resources_locked(struct anland_camera *c)
{
    /* Consumer is gone: drop the control socket and detach every camera's stream fd.
     * The nodes stay alive and keep emitting blank frames, so PipeWire and any
     * capturing app never see the cameras disappear -- the audio engine's detach
     * behaviour. */
    for (int i = 0; i < c->num_cameras; i++) {
        struct cam *cam = &c->cams[i];
        if (cam->recording && c->ctrl_fd >= 0) {
            uint8_t id = (uint8_t)cam->index;
            send_ctrl(c, CAMERA_CTRL_STOP_RECORD, &id, 1);
        }
        cam->recording = false;
    }
    if (c->ctrl_io) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->ctrl_io);
        c->ctrl_io = NULL;
    }
    if (c->ctrl_fd >= 0) {
        close(c->ctrl_fd);
        c->ctrl_fd = -1;
    }
    c->ctrl_buffer_len = 0;
    c->info_pending = false;
    for (int i = 0; i < c->num_cameras; i++)
        detach_fd(&c->cams[i]);
}

void meta_anland_camera_clear(struct anland_camera *c)
{
    if (!c)
        return;
    pw_thread_loop_lock(c->loop);
    clear_resources_locked(c);
    pw_thread_loop_unlock(c->loop);
}

/* True while a consumer's control channel is attached. The backend uses this to keep
 * the camera across display-only changes instead of re-requesting it: the consumer
 * treats every re-allocation as "previous producer gone" and stops recording. */
gboolean meta_anland_camera_has_resources(struct anland_camera *c)
{
    gboolean attached;

    if (!c)
        return FALSE;
    pw_thread_loop_lock(c->loop);
    attached = c->ctrl_fd >= 0;
    pw_thread_loop_unlock(c->loop);
    return attached;
}

struct anland_camera *meta_anland_camera_new(void)
{
    pw_init(NULL, NULL);

    struct anland_camera *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ctrl_fd = -1;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        c->cams[i].owner = c;
        c->cams[i].index = i;
        c->cams[i].stream_fd = -1;
        c->cams[i].shm_fd = -1;
    }

    c->loop = pw_thread_loop_new("anland-camera", NULL);
    if (!c->loop)
        goto fail;

    c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->context)
        goto fail;

    c->reconnect_timer = pw_loop_add_timer(pw_thread_loop_get_loop(c->loop),
                                           on_reconnect_timer, c);
    if (!c->reconnect_timer)
        goto fail;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        c->cams[i].stop_timer = pw_loop_add_timer(pw_thread_loop_get_loop(c->loop),
                                                  on_stop_timer, &c->cams[i]);
        if (!c->cams[i].stop_timer)
            goto fail;
    }

    if (pw_thread_loop_start(c->loop) < 0)
        goto fail;
    c->loop_started = true;

    pw_thread_loop_lock(c->loop);
    if (build_pw(c) == 0) {
        c->pw_connected = true;
    } else {
        teardown_pw(c);
        arm_reconnect(c);
    }
    pw_thread_loop_unlock(c->loop);

    return c;

fail:
    if (c->loop_started)
        pw_thread_loop_stop(c->loop);
    for (int i = 0; i < MAX_CAMERAS; i++)
        if (c->cams[i].stop_timer)
            pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop),
                                   c->cams[i].stop_timer);
    if (c->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop),
                               c->reconnect_timer);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);
    free(c);
    return NULL;
}

void meta_anland_camera_free(struct anland_camera *c)
{
    if (!c)
        return;

    if (c->loop_started)
        pw_thread_loop_lock(c->loop);
    clear_resources_locked(c);
    teardown_pw(c);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        struct cam *cam = &c->cams[i];
        unmap_shm(cam);
        free(cam->frame);
        cam->frame = NULL;
        if (cam->stop_timer)
            pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), cam->stop_timer);
        cam->stop_timer = NULL;
    }
    if (c->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->reconnect_timer);
    if (c->loop_started)
        pw_thread_loop_unlock(c->loop);

    if (c->loop_started)
        pw_thread_loop_stop(c->loop);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);
    free(c);
}
