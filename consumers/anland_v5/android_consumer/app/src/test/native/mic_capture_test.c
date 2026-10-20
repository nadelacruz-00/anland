/* Exercise the production capture thread (native_audio.c) against a fake AAudio and a
 * fake producer on a socketpair; no real audio device is opened.
 * Run with: sh app/src/test/native/run_mic_capture_tests.sh
 *
 * Protects: the Android mic (and a Bluetooth headset's call mode) is held only while a
 * desktop app records, the headset router is always released again, and a lost or
 * unusable headset falls back to the default mic. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../main/jni/native_audio.c"

/* ---- fake display library ---- */

static int g_producer_fd = -1;   /* the test's end: plays the mutter bridge */
static int g_consumer_fd = -1;   /* what get_audio_fd() hands the bridge */

int get_audio_fd(display_ctx *ctx)
{
    return ctx ? g_consumer_fd : -1;
}

int __android_log_print(int priority, const char *tag, const char *format, ...)
{
    (void)priority;
    if (!getenv("MIC_TEST_VERBOSE"))
        return 0;
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

/* ---- fake AAudio ---- */

#define DEFAULT_INPUT_ID 1
#define HEADSET_INPUT_ID 42

struct AAudioStreamBuilderStruct {
    int dir, rate, channels, preset, device;
};

struct AAudioStreamStruct {
    int dir, rate, channels, preset, device;
    volatile int disconnected;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_open_inputs;          /* input streams currently open (the "mic indicator") */
static int g_input_opens;          /* input streams opened so far */
static int g_failing_device;       /* openStream fails for this input device id */
static AAudioStream *g_last_input; /* most recently opened input stream */

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder)
{
    *builder = calloc(1, sizeof(**builder));
    return *builder ? AAUDIO_OK : AAUDIO_ERROR_UNAVAILABLE;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d) { b->dir = d; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t rate) { b->rate = rate; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t c) { b->channels = c; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int32_t f) { (void)b; (void)f; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, int32_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int32_t u) { (void)b; (void)u; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int32_t t) { (void)b; (void)t; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int32_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder *b, int32_t p) { b->preset = p; }
void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t d) { b->device = d; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb,
                                         void *userdata) { (void)b; (void)cb; (void)userdata; }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb,
                                          void *userdata) { (void)b; (void)cb; (void)userdata; }
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { free(b); return AAUDIO_OK; }

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream)
{
    pthread_mutex_lock(&g_lock);
    if (b->dir == AAUDIO_DIRECTION_INPUT && b->device != 0 && b->device == g_failing_device) {
        pthread_mutex_unlock(&g_lock);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    AAudioStream *s = calloc(1, sizeof(*s));
    s->dir = b->dir;
    s->rate = b->rate ? b->rate : 48000;
    s->channels = b->channels;
    s->preset = b->preset;
    s->device = b->device ? b->device : DEFAULT_INPUT_ID;
    if (s->dir == AAUDIO_DIRECTION_INPUT) {
        g_open_inputs++;
        g_input_opens++;
        g_last_input = s;
    }
    pthread_mutex_unlock(&g_lock);
    *stream = s;
    return AAUDIO_OK;
}

const char *AAudio_convertResultToText(aaudio_result_t r) { (void)r; return "fake"; }
const char *AAudio_convertStreamStateToText(aaudio_stream_state_t s) { (void)s; return "fake"; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s) { (void)s; return AAUDIO_STREAM_STATE_STARTED; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s->rate; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s->channels; }
int32_t AAudioStream_getDeviceId(AAudioStream *s) { return s->device; }

aaudio_result_t AAudioStream_close(AAudioStream *s)
{
    pthread_mutex_lock(&g_lock);
    if (s->dir == AAUDIO_DIRECTION_INPUT)
        g_open_inputs--;
    if (g_last_input == s)
        g_last_input = NULL;
    pthread_mutex_unlock(&g_lock);
    free(s);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_read(AAudioStream *s, void *buffer, int32_t frames, int64_t timeout_ns)
{
    (void)timeout_ns;
    if (s->disconnected)
        return AAUDIO_ERROR_DISCONNECTED;
    usleep(2000);
    memset(buffer, 0, (size_t)frames * (size_t)s->channels * sizeof(int16_t));
    return frames;
}

/* ---- fake Java MicRouter ---- */

static int g_route_calls, g_release_calls;
static int g_routed;            /* 1 between a route and its release */
static int g_headset_id;        /* what the router hands back: 0 = no headset */

static int fake_route(void *userdata, int active)
{
    assert(userdata == &g_routed);
    pthread_mutex_lock(&g_lock);
    if (active) {
        g_route_calls++;
        g_routed = 1;
    } else {
        g_release_calls++;
        g_routed = 0;
    }
    int id = active ? g_headset_id : 0;
    pthread_mutex_unlock(&g_lock);
    return id;
}

/* ---- helpers ---- */

static int locked(const int *v)
{
    pthread_mutex_lock(&g_lock);
    int r = *v;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Drain what the bridge sends (formats and mic PCM); count the PCM packets. */
static int drain_pcm(void)
{
    int pcm = 0;
    uint8_t buf[MAX_DGRAM];
    for (;;) {
        ssize_t n = recv(g_producer_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct audio_msg))
            return pcm;
        struct audio_msg h;
        memcpy(&h, buf, sizeof(h));
        if (h.type == AUDIO_MSG_PCM)
            pcm++;
    }
}

#define WAIT_FOR(cond)                                                            \
    do {                                                                          \
        uint64_t until_ = ms_now() + 2000;                                        \
        while (!(cond) && ms_now() < until_) { drain_pcm(); usleep(5000); }       \
        if (!(cond)) {                                                            \
            fprintf(stderr, "%s:%d: timed out waiting for %s\n", __FILE__,       \
                    __LINE__, #cond);                                             \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

/* Recording keeps producing PCM for the producer. */
static void expect_pcm_flowing(void)
{
    drain_pcm();
    usleep(60000);
    assert(drain_pcm() > 0);
}

static void send_capture_state(uint32_t active)
{
    struct audio_msg h = { .type = AUDIO_MSG_CAPTURE_STATE, .size = sizeof(active) };
    uint8_t buf[sizeof(h) + sizeof(active)];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &active, sizeof(active));
    assert(send(g_producer_fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
}

static int input_preset(void)
{
    pthread_mutex_lock(&g_lock);
    int p = g_last_input ? g_last_input->preset : -1;
    pthread_mutex_unlock(&g_lock);
    return p;
}

static int input_device(void)
{
    pthread_mutex_lock(&g_lock);
    int d = g_last_input ? g_last_input->device : -1;
    pthread_mutex_unlock(&g_lock);
    return d;
}

/* ---- tests ---- */

int main(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    g_producer_fd = sv[0];
    g_consumer_fd = sv[1];

    audio_bridge *b = audio_create();
    assert(b);
    audio_set_mic_router(b, fake_route, &g_routed);
    audio_start(b);
    audio_set_mic_enabled(b, 1);

    /* Nothing opens the mic before a connection exists. */
    usleep(80000);
    assert(locked(&g_open_inputs) == 0);

    /* An older producer never reports capture state: record whenever the mic is
     * enabled, as before, without asking the router (no headset call mode). */
    audio_set_ctx(b, (display_ctx *)1);
    WAIT_FOR(locked(&g_open_inputs) == 1);
    assert(input_device() == DEFAULT_INPUT_ID);
    assert(input_preset() == AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    expect_pcm_flowing();
    assert(locked(&g_route_calls) == 0);

    /* A producer that reports "no app records": the mic closes. */
    send_capture_state(0);
    WAIT_FOR(locked(&g_open_inputs) == 0);
    usleep(80000);
    assert(locked(&g_open_inputs) == 0);
    assert(locked(&g_route_calls) == 0);

    /* An app starts recording and a headset is connected: route to it. */
    g_headset_id = HEADSET_INPUT_ID;
    send_capture_state(1);
    WAIT_FOR(locked(&g_open_inputs) == 1);
    assert(locked(&g_route_calls) == 1 && locked(&g_routed) == 1);
    assert(input_device() == HEADSET_INPUT_ID);
    assert(input_preset() == AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    expect_pcm_flowing();

    /* The app stops: the mic closes and the headset is released (A2DP returns). */
    send_capture_state(0);
    WAIT_FOR(locked(&g_open_inputs) == 0 && locked(&g_routed) == 0);
    assert(locked(&g_release_calls) == 1);

    /* The headset input cannot be opened: release it, use the default mic. */
    g_failing_device = HEADSET_INPUT_ID;
    send_capture_state(1);
    WAIT_FOR(locked(&g_open_inputs) == 1);
    assert(input_device() == DEFAULT_INPUT_ID);
    assert(input_preset() == AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    assert(locked(&g_routed) == 0);
    expect_pcm_flowing();
    g_failing_device = 0;

    /* A headset connects mid-recording: Java asks for a reroute. */
    int opens = locked(&g_input_opens);
    audio_request_mic_reroute(b);
    WAIT_FOR(locked(&g_input_opens) > opens && input_device() == HEADSET_INPUT_ID);
    assert(locked(&g_open_inputs) == 1 && locked(&g_routed) == 1);
    expect_pcm_flowing();

    /* The headset goes away mid-recording: the stream disconnects, the bridge asks
     * the router again and keeps recording from the default mic. */
    g_headset_id = 0;
    pthread_mutex_lock(&g_lock);
    g_last_input->disconnected = 1;
    pthread_mutex_unlock(&g_lock);
    WAIT_FOR(input_device() == DEFAULT_INPUT_ID);
    assert(locked(&g_open_inputs) == 1);
    expect_pcm_flowing();

    /* The user turns the mic setting off while an app records: close and release. */
    audio_set_mic_enabled(b, 0);
    WAIT_FOR(locked(&g_open_inputs) == 0 && locked(&g_routed) == 0);
    audio_set_mic_enabled(b, 1);
    WAIT_FOR(locked(&g_open_inputs) == 1);

    /* A reconnect to a producer that has not reported yet falls back to the old
     * behaviour until it does; the routed headset is released on the way. */
    g_headset_id = HEADSET_INPUT_ID;
    audio_request_mic_reroute(b);
    WAIT_FOR(input_device() == HEADSET_INPUT_ID);
    audio_set_ctx(b, NULL);
    WAIT_FOR(locked(&g_open_inputs) == 0 && locked(&g_routed) == 0);

    /* Connecting to a current producer, which reports "idle" at once, never opens
     * the mic, not even briefly (that would flick a headset into call mode). */
    opens = locked(&g_input_opens);
    audio_set_ctx(b, (display_ctx *)1);
    send_capture_state(0);
    usleep(CAP_GATE_WAIT_MS * 1000 + 300000);
    assert(locked(&g_input_opens) == opens && locked(&g_open_inputs) == 0);

    /* Stopping the bridge mid-recording releases everything. */
    send_capture_state(1);
    WAIT_FOR(locked(&g_open_inputs) == 1 && locked(&g_routed) == 1);
    audio_stop(b);
    assert(locked(&g_open_inputs) == 0);
    assert(locked(&g_routed) == 0);

    audio_destroy(b);
    close(sv[0]);
    close(sv[1]);
    puts("mic capture tests passed");
    return 0;
}
