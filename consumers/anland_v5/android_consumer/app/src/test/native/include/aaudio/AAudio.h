#ifndef ANLAND_TEST_AAUDIO_H
#define ANLAND_TEST_AAUDIO_H

/* The subset of <aaudio/AAudio.h> that native_audio.c uses, for host tests. The
 * functions are implemented by the test (mic_capture_test.c). */
#include <stdint.h>

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_data_callback_result_t;
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;

enum {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_DISCONNECTED = -899,
    AAUDIO_ERROR_INVALID_STATE = -895,
    AAUDIO_ERROR_UNAVAILABLE = -884,
    AAUDIO_UNSPECIFIED = 0,
    AAUDIO_DIRECTION_OUTPUT = 0,
    AAUDIO_DIRECTION_INPUT = 1,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_SHARING_MODE_SHARED = 1,
    AAUDIO_USAGE_MEDIA = 1,
    AAUDIO_CONTENT_TYPE_MUSIC = 2,
    AAUDIO_PERFORMANCE_MODE_NONE = 10,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
    AAUDIO_INPUT_PRESET_VOICE_RECOGNITION = 6,
    AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION = 7,
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    AAUDIO_STREAM_STATE_STARTING = 3,
    AAUDIO_STREAM_STATE_STARTED = 4,
    AAUDIO_STREAM_STATE_DISCONNECTED = 13,
};

typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(
    AAudioStream *stream, void *userdata, void *audio_data, int32_t num_frames);
typedef void (*AAudioStream_errorCallback)(
    AAudioStream *stream, void *userdata, aaudio_result_t error);

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t rate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t channels);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int32_t format);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, int32_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int32_t usage);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int32_t type);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int32_t mode);
void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder *b, int32_t preset);
void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t device);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b,
                                         AAudioStream_dataCallback cb, void *userdata);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b,
                                          AAudioStream_errorCallback cb, void *userdata);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b);

const char *AAudio_convertResultToText(aaudio_result_t result);
const char *AAudio_convertStreamStateToText(aaudio_stream_state_t state);

aaudio_result_t AAudioStream_requestStart(AAudioStream *s);
aaudio_result_t AAudioStream_requestStop(AAudioStream *s);
aaudio_result_t AAudioStream_close(AAudioStream *s);
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s);
int32_t AAudioStream_getSampleRate(AAudioStream *s);
int32_t AAudioStream_getChannelCount(AAudioStream *s);
int32_t AAudioStream_getDeviceId(AAudioStream *s);
aaudio_result_t AAudioStream_read(AAudioStream *s, void *buffer, int32_t frames,
                                  int64_t timeout_ns);

#endif
