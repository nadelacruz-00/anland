#ifndef CAMERA_SIZE_H
#define CAMERA_SIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Camera output-size selection. Kept free of NDK types so it runs on the host in
 * app/src/test/native/camera_size_test.c.
 *
 * Every size the service ever opens or reports is capped at CAM_CAP_W x CAM_CAP_H
 * (width AND height, not area). Opening the full sensor (e.g. 4160x3120) makes the
 * HAL stream ~19.5 MB frames, which once pushed the whole device into a global OOM.
 * The mutter producer clamps with the same rule, so the two sides never disagree.
 */
#define CAM_CAP_W 1920
#define CAM_CAP_H 1080

struct cam_size {
    int32_t w;
    int32_t h;
};

/* Largest-area size with w <= max_w && h <= max_h. False if none fits. */
static inline bool cam_size_largest_within(const struct cam_size *sizes, size_t n,
                                           int32_t max_w, int32_t max_h,
                                           struct cam_size *out)
{
    long best = 0;
    for (size_t i = 0; i < n; i++) {
        const struct cam_size *s = &sizes[i];
        if (s->w <= 0 || s->h <= 0 || s->w > max_w || s->h > max_h)
            continue;
        long area = (long)s->w * s->h;
        if (area > best) { best = area; *out = *s; }
    }
    return best > 0;
}

/* Smallest-area size with w <= max_w && h <= max_h. False if none fits. */
static inline bool cam_size_smallest_within(const struct cam_size *sizes, size_t n,
                                            int32_t max_w, int32_t max_h,
                                            struct cam_size *out)
{
    long best = 0;
    for (size_t i = 0; i < n; i++) {
        const struct cam_size *s = &sizes[i];
        if (s->w <= 0 || s->h <= 0 || s->w > max_w || s->h > max_h)
            continue;
        long area = (long)s->w * s->h;
        if (best == 0 || area < best) { best = area; *out = *s; }
    }
    return best > 0;
}

/* The camera's reported "max": the largest supported size within the cap. */
static inline bool cam_size_capped_max(const struct cam_size *sizes, size_t n,
                                       struct cam_size *out)
{
    return cam_size_largest_within(sizes, n, CAM_CAP_W, CAM_CAP_H, out);
}

/*
 * Size to actually open for a START_RECORD request of req_w x req_h (0 = no
 * preference). Picks the largest supported size within both the request and the cap;
 * if the request is smaller than every supported size, the smallest supported size
 * within the cap. Never returns a size above the cap or one the camera doesn't list.
 */
static inline bool cam_size_choose(const struct cam_size *sizes, size_t n,
                                   int32_t req_w, int32_t req_h, struct cam_size *out)
{
    int32_t bw = (req_w > 0 && req_w < CAM_CAP_W) ? req_w : CAM_CAP_W;
    int32_t bh = (req_h > 0 && req_h < CAM_CAP_H) ? req_h : CAM_CAP_H;
    if (cam_size_largest_within(sizes, n, bw, bh, out))
        return true;
    return cam_size_smallest_within(sizes, n, CAM_CAP_W, CAM_CAP_H, out);
}

#endif /* CAMERA_SIZE_H */
