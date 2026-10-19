/* Host regression tests for the camera size picker: the service must never open or
 * report a size above CAM_CAP_W x CAM_CAP_H, nor one the camera does not list.
 * Run with: sh app/src/test/native/run_camera_size_tests.sh */
#include <assert.h>
#include <stdio.h>

#include "../../main/jni/camera_size.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* YUV_420_888 outputs of a 13 MP 4:3 rear sensor (4160x3120 max). */
static const struct cam_size rear[] = {
    { 4160, 3120 }, { 4000, 3000 }, { 3840, 2160 }, { 2560, 1920 },
    { 1920, 1440 }, { 1600, 1200 }, { 1920, 1080 }, { 1440, 1080 },
    { 1280,  960 }, { 1280,  720 }, {  960,  720 }, {  640,  480 },
    {  320,  240 },
};

static void expect(struct cam_size got, int32_t w, int32_t h)
{
    if (got.w != w || got.h != h) {
        fprintf(stderr, "expected %dx%d, got %dx%d\n", w, h, got.w, got.h);
        assert(0);
    }
}

int main(void)
{
    struct cam_size s;

    /* Sensor max is above the cap: report the largest size within the cap. */
    assert(cam_size_capped_max(rear, N(rear), &s));
    expect(s, 1920, 1080);

    /* The cap is per dimension, not by area: 1600x1200 is under 1920*1080 pixels but
     * taller than 1080, so it must lose to 1440x1080 (an area rule would pick it). */
    static const struct cam_size no_1080p[] = {
        { 4160, 3120 }, { 1600, 1200 }, { 1440, 1080 }, { 1280, 720 },
    };
    assert(cam_size_capped_max(no_1080p, N(no_1080p), &s));
    expect(s, 1440, 1080);

    /* No preference (0x0) opens the capped max, never the sensor max. */
    assert(cam_size_choose(rear, N(rear), 0, 0, &s));
    expect(s, 1920, 1080);

    /* A request above the cap (an old producer asking for the sensor max) is clamped. */
    assert(cam_size_choose(rear, N(rear), 4160, 3120, &s));
    expect(s, 1920, 1080);

    /* A request between listed sizes gets the largest listed size below it. */
    assert(cam_size_choose(rear, N(rear), 1300, 1000, &s));
    expect(s, 1280, 960);

    /* An exact listed request is honoured. */
    assert(cam_size_choose(rear, N(rear), 1280, 720, &s));
    expect(s, 1280, 720);

    /* A request below every listed size falls back to the smallest one within the cap. */
    static const struct cam_size big_only[] = { { 4160, 3120 }, { 1920, 1080 }, { 1280, 720 } };
    assert(cam_size_choose(big_only, N(big_only), 320, 240, &s));
    expect(s, 1280, 720);

    /* Nothing within the cap: no size, so the caller refuses to open the camera. */
    static const struct cam_size too_big[] = { { 4160, 3120 }, { 3840, 2160 } };
    assert(!cam_size_capped_max(too_big, N(too_big), &s));
    assert(!cam_size_choose(too_big, N(too_big), 0, 0, &s));

    /* Empty list (metadata unavailable): no size. */
    assert(!cam_size_capped_max(rear, 0, &s));
    assert(!cam_size_choose(rear, 0, 1280, 720, &s));

    /* Bogus entries are ignored. */
    static const struct cam_size bogus[] = { { 0, 1080 }, { -1, -1 }, { 640, 480 } };
    assert(cam_size_capped_max(bogus, N(bogus), &s));
    expect(s, 640, 480);

    puts("camera_size_test: ok");
    return 0;
}
