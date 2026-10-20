#ifndef DISPLAY_PROTOCOL_H
#define DISPLAY_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

#define CTRL_MSG_CONSUMER_HELLO  1
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_REJECT          8
#define CTRL_MSG_PICKUP_FDS      9
#define CTRL_MSG_FDS_READY      10

#define DATA_MSG_BUF_READY       100
#define DATA_MSG_REFRESH_DONE    101
#define DATA_MSG_INPUT_EVENT     102
#define DATA_MSG_OUTPUT_EVENT    103
#define DATA_MSG_INPUT_EXTEND_FDS  104
#define DATA_MSG_BUFS_READY      200

#define MAX_BUFS 8

struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct data_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct screen_info {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t refresh;
} __attribute__((packed));

struct buf_info {
    uint32_t stride;
    uint32_t width;      /* buffer logical width  (consumer-side native resolution) */
    uint32_t height;     /* buffer logical height (consumer-side native resolution) */
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
} __attribute__((packed));

#define INPUT_TYPE_TOUCH          1
#define INPUT_TYPE_KEY            2
#define INPUT_TYPE_POINTER_MOTION 3
#define INPUT_TYPE_POINTER_BUTTON 4
#define INPUT_TYPE_POINTER_AXIS   5
#define INPUT_TYPE_TOUCH_FRAME    6
/* Not really input: the consumer reports its current display refresh rate over
 * the same data channel so the producer can repace its RenderLoop at runtime.
 * Deliberately reuses the InputEvent framing (DATA_MSG_INPUT_EVENT) so the
 * producer's poll_input_event() drains it like any other event instead of
 * stalling the stream on an unknown DATA_MSG_* header. */
#define INPUT_TYPE_DISPLAY_REFRESH 7
#define INPUT_TYPE_CLIPBOARD      8
#define INPUT_TYPE_TEXT_INPUT      9
#define INPUT_TYPE_ACTION         10
/* Consumer -> producer: hands back the fds for a requested service (e.g. camera).
 * The InputEvent carries { service type, fdnum }; the fdnum fds follow as a
 * separate DATA_MSG_INPUT_EXTEND_FDS message (SCM_RIGHTS). */
#define INPUT_TYPE_RESOURCE       11
#define INPUT_TYPE_RESOURCE_INVALID 12

/* ---------------------------------------------------------------------------
 * Stylus / tablet tool. Consumer -> producer, over the same data channel.
 *
 * Every tool struct below is deliberately kept within the existing 16-byte
 * union (sizeof(touch) == sizeof(pointer_motion) == 16), so sizeof(struct
 * InputEvent) does not change. That gives compatibility for free in both
 * directions: an older producer paired with a newer consumer skips unknown
 * tool events instead of misaligning the stream, and a newer producer paired
 * with an older consumer simply never sees them and keeps using the touch
 * path. No explicit version handshake is required for that reason.
 *
 * Coordinates and axis values use float, matching the existing touch and
 * pointer events. Both sides are the same architecture in practice.
 *
 * MEASURED ON HARDWARE (OnePlus Pad 3 + Stylo 2), do not "simplify" these away:
 *  - Android's getPressure() is NOT normalised on this device and reached
 *    ~9.77 instead of 0..1. The consumer MUST normalise before sending, and
 *    the producer MUST clamp defensively.
 *  - AXIS_ORIENTATION reached ~4.37 rad, past +/-pi. The consumer MUST wrap it.
 *  - AXIS_TILT is unsigned 0..pi/2; tilt direction comes from orientation, so
 *    tilt_x/tilt_y are derived from the pair rather than read directly.
 *  - The barrel button was NOT observed on the event path at all (the Stylo 2
 *    pairs over Bluetooth HID), so tool_button is emitted even when unused.
 * ------------------------------------------------------------------------- */
#define INPUT_TYPE_TOOL           13 /* tool added / removed, with capabilities */
#define INPUT_TYPE_TOOL_POSITION  14 /* proximity in/out and motion             */
#define INPUT_TYPE_TOOL_AXES      15 /* pressure and tilt                        */
#define INPUT_TYPE_TOOL_BUTTON    16 /* tip contact and barrel buttons           */
#define INPUT_TYPE_TOOL_FRAME     17 /* end of one hardware sample group         */

/* INPUT_TYPE_TOOL */
#define TOOL_ACTION_ADDED         0
#define TOOL_ACTION_REMOVED       1

/* INPUT_TYPE_TOOL_POSITION */
#define TOOL_POSITION_PROXIMITY_IN  0
#define TOOL_POSITION_MOTION        1
#define TOOL_POSITION_PROXIMITY_OUT 2

/* Tool types; same values as wayland tablet-v2 tool types. */
#define TOOL_TYPE_PEN             0x140
#define TOOL_TYPE_ERASER          0x141

/* Capability bits reported in InputEvent.tool.caps. */
#define TOOL_CAP_PRESSURE         0x01
#define TOOL_CAP_TILT             0x02
#define TOOL_CAP_DISTANCE         0x04
#define TOOL_CAP_ROTATION         0x08

/* Service identifiers used by OUTPUT_TYPE_RESOURCES_REQUEST / INPUT_TYPE_RESOURCE. */
#define SERVICE_TYPE_CAMERA 1

#define INPUT_ACTION_DOWN    0
#define INPUT_ACTION_UP      1
#define INPUT_ACTION_MOVE    2

struct InputEvent {
    uint32_t type;
    union {
        struct {
            int32_t  action;
            float    x;
            float    y;
            int32_t  pointer_id;
        } touch;
        struct {
            int32_t  action;
            int32_t  keycode;
        } key;
        struct {
            float    x;
            float    y;
            float    dx;
            float    dy;
        } pointer_motion;
        struct {
            uint32_t button;
            int32_t  pressed;
        } pointer_button;
        struct {
            uint32_t axis;
            float    value;
            int32_t  discrete;
        } pointer_axis;
        struct {
            uint32_t refresh_mhz; // current display refresh rate, milli-Hz
        } display;
        struct {
            uint32_t size; // notification header; raw clipboard bytes follow
        } clipboard;
        struct {
            uint32_t size; // notification header; raw UTF-8 text bytes follow
        } text_input;
        struct {
            uint32_t action;
            int32_t value;
        } input_action;
        struct {
            uint32_t type;
            uint32_t fdnum; // number of fds following in DATA_MSG_INPUT_EXTEND_FDS
        } resource;
        /* --- stylus / tablet tool (16 bytes max, see the note above) --- */
        struct {
            uint32_t action;    /* TOOL_ACTION_*   */
            uint32_t tool_id;   /* stable per physical tool */
            uint32_t tool_type; /* TOOL_TYPE_*     */
            uint32_t caps;      /* TOOL_CAP_* bitmask */
        } tool;                 /* 16 */
        struct {
            uint32_t action;    /* TOOL_POSITION_* */
            uint32_t tool_id;
            float    x;         /* compositor logical coordinates */
            float    y;
        } tool_position;        /* 16 */
        struct {
            uint32_t tool_id;
            float    pressure;  /* consumer-normalised 0..1; producer clamps */
            float    tilt_x;    /* radians */
            float    tilt_y;    /* radians */
        } tool_axes;            /* 16 */
        struct {
            uint32_t tool_id;
            uint32_t button;    /* BTN_STYLUS / BTN_STYLUS2; 0 when only tip */
            int32_t  pressed;
            int32_t  tip;       /* 1 = tip contact (BTN_TOUCH semantics) */
        } tool_button;          /* 16 */
        struct {
            uint32_t tool_id;
            uint32_t time_ms;   /* consumer event time; frame boundary */
        } tool_frame;           /* 8 */

        struct {
            uint32_t padding[4];
        };
    };
} __attribute__((packed));

struct OutputEvent{
    uint32_t type;
    union {
        struct {
            uint32_t size; // notification header; raw clipboard bytes follow
        } clipboard;
        struct {
            uint32_t type;
            uint32_t args[3]; //support 3 args
        } resources_request;
        struct {
            uint32_t var;   //CONSUMER_VAR_* identifier
            uint32_t value; //new UINT32 value (0 = default / release)
        } set_consumer_var;
        struct {
            pid_t pid;      // PID as seen in the producer's PID namespace
            uint8_t flags;  // SCHEDULING_FLAG_*
        } scheduling;
        struct
        {
            uint32_t padding[4];
        };

    };
} __attribute__((packed));

#define OUTPUT_TYPE_CLIPBOARD 1
#define OUTPUT_TYPE_RESOURCES_REQUEST 2
/* Producer -> consumer: set a transient Android-side runtime parameter (UINT32).
 * Fixed-size OutputEvent { var, value }, no trailing payload. A non-zero value
 * requests the Android app enable the corresponding behaviour while asserted; 0
 * withdraws the request. The consumer regresses every var to 0 on its own
 * fallback, so the producer MUST resend the current value on each reconnect. */
#define OUTPUT_TYPE_SET_CONSUMER_VAR 3
/* Producer -> consumer: foreground-scheduling switch. The producer reports
 * the currently focused subtree after every reconnect and on each activation
 * change: {previous pid, off} before {new pid, on}, so each event is
 * self-contained. The compositor itself is the default focus -- with no
 * active client it reports its own pid like any other app. pid is as seen in
 * the producer's PID namespace. The consumer forwards it to the root helper,
 * which setns()es into that namespace and writes pid into the top-app (on)
 * or root (off) cgroup files -- an O(1) operation. */
#define OUTPUT_TYPE_SCHEDULING 4

/* scheduling.flags */
#define SCHEDULING_FLAG_SETTREE 0x01 /* operate on pid's whole process subtree */
#define SCHEDULING_FLAG_ON      0x02 /* 1 = move into top-app, 0 = restore */

/* Var identifiers addressable via OUTPUT_TYPE_SET_CONSUMER_VAR. */
/* 1 = force-enable Android pointer capture (Wayland zwp_locked_pointer_v1 active,
 *   a game grabbed the mouse for relative motion); 0 = release. */
#define CONSUMER_VAR_CAPTURE_MOUSE 1

/*
 * Audio runs on its own dedicated bidirectional socketpair (hello fd slot 4),
 * deliberately kept off the data channel so a burst of PCM never head-of-line
 * blocks input/clipboard. The socket is full duplex: the producer writes desktop
 * playback PCM that the consumer reads, and the consumer writes microphone PCM
 * that the producer reads -- each side only ever reads what the other wrote.
 * Every direction sends one AUDIO_MSG_FORMAT, then a stream of AUDIO_MSG_PCM.
 *
 * PCM is interleaved per frame. For stereo (channels == 2) the sample order
 * within each frame is LEFT then RIGHT, i.e. channel positions FL, FR -- the
 * producer's SPA channel map and the consumer's AAudio stereo layout must both
 * honour this order so the left/right channels are never swapped.
 *
 * Audio quality is negotiated, not pinned: the consumer owns the real hardware,
 * so on connect it opens its AAudio streams, reads back the rate/channels the
 * device actually chose, and sends one AUDIO_MSG_FORMAT per direction (tagged with
 * role) to the producer. The producer builds its PipeWire sink/source to match,
 * so neither side resamples blindly and the PCM byte math lines up on both ends.
 */
#define AUDIO_MSG_FORMAT 1
#define AUDIO_MSG_PCM    2
#define AUDIO_MSG_SHM 3 // request shared-memory ring-buffer audio transport instead of socket sends (fewer copies, lower latency)
#define AUDIO_MSG_SHM_FD 4 // producer -> consumer: shared-memory fd, consumer mmaps it and reads audio data directly
/* producer -> consumer: payload is one uint32_t, 1 while a desktop app records from the
 * virtual mic source, 0 once none does. A consumer that has seen this message on a
 * connection captures the mic only while it is 1 (and may route it to a headset then);
 * without it the consumer captures whenever its mic setting is on. Consumers that
 * predate it ignore it (they only read AUDIO_MSG_PCM from the producer). */
#define AUDIO_MSG_CAPTURE_STATE 5

/* PCM sample format codes for struct audio_format.format. */
#define AUDIO_FORMAT_S16LE 0

/* Which direction a struct audio_format describes. */
#define AUDIO_ROLE_PLAYBACK 0   /* producer -> consumer (desktop sound -> speaker) */
#define AUDIO_ROLE_CAPTURE  1   /* consumer -> producer (mic -> virtual source)    */

struct audio_format {
    uint32_t rate;       /* frames per second the device chose, e.g. 48000 / 44100 */
    uint32_t channels;   /* interleaved; stereo is L,R (FL,FR) */
    uint32_t format;     /* AUDIO_FORMAT_* */
    uint32_t role;       /* AUDIO_ROLE_* -- which direction this describes */
    uint32_t quantum;    /* requested buffer in frames (latency preset); 0 = engine default */
} __attribute__((packed));

struct audio_msg {
    uint32_t type;       /* AUDIO_MSG_FORMAT | AUDIO_MSG_PCM | AUDIO_MSG_CAPTURE_STATE */
    uint32_t size;       /* payload bytes that follow this header */
} __attribute__((packed));


#endif
