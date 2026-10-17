package com.anland.consumer;

/**
 * Pure maths for the tablet-tool axes. Kept out of MainActivity so the unit
 * tests can reach it; the constants below come from measurements on real
 * hardware and are the only place where the consumer's units are decided.
 */
public final class ToolAxes {
    private ToolAxes() {}

    /**
     * Android's getPressure() is NOT normalised on this device: a probe measured it
     * reaching ~9.77 rather than 0..1, because the kernel ABS_PRESSURE range is wider
     * than Android assumes. Clamp so the later 0..65535 mapping is sane.
     */
    public static float normPressure(float p) {
        if (!Float.isFinite(p) || p <= 0f) return 0f;
        return p > 1f ? 1f : p;
    }

    /**
     * Android reports AXIS_TILT as an unsigned angle away from the panel normal
     * (0..pi/2) and the direction separately in AXIS_ORIENTATION, which on this
     * device was measured reaching ~4.37 rad - past +/-pi. Wrap it, then derive
     * tilt_x/tilt_y from the pair (they are not independent axes). Both are
     * returned in radians; the producer converts to the protocol's degrees.
     *
     * @param out receives tilt_x in out[0] and tilt_y in out[1].
     */
    public static void tiltXY(float tilt, float orient, float[] out) {
        float t = tilt;
        if (!Float.isFinite(t) || t < 0f) t = 0f;
        float halfPi = (float) (Math.PI / 2.0);
        if (t > halfPi) t = halfPi;
        float o = Float.isFinite(orient)
                ? (float) Math.IEEEremainder(orient, 2.0 * Math.PI) : 0f;
        out[0] = t * (float) Math.cos(o);
        out[1] = t * (float) Math.sin(o);
    }
}
