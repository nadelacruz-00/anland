package com.anland.consumer;

import static org.junit.Assert.*;

import org.junit.Test;

public class ToolAxesTest {
    private static final float HALF_PI = (float) (Math.PI / 2.0);

    @Test
    public void pressureIsClampedToTheUnitRange() {
        assertEquals(0.5f, ToolAxes.normPressure(0.5f), 1e-6f);
        assertEquals(0f, ToolAxes.normPressure(0f), 1e-6f);
        assertEquals(0f, ToolAxes.normPressure(-1f), 1e-6f);
        assertEquals(0f, ToolAxes.normPressure(Float.NaN), 1e-6f);
        // Measured on the Pad 3: the raw value reached ~9.77, not 0..1.
        assertEquals(1f, ToolAxes.normPressure(9.77f), 1e-6f);
        // A reading that is not finite counts as "no pressure" rather than as full
        // pressure, so a broken axis value cannot look like a hard press.
        assertEquals(0f, ToolAxes.normPressure(Float.POSITIVE_INFINITY), 1e-6f);
    }

    @Test
    public void tiltIsClampedToTheHalfPiCone() {
        float[] out = new float[2];

        // Past the physical maximum, and a negative reading: both clamp to 0.
        ToolAxes.tiltXY((float) Math.PI, 0f, out);
        assertEquals(HALF_PI, out[0], 1e-5f);
        assertEquals(0f, out[1], 1e-5f);

        ToolAxes.tiltXY(-1f, 0f, out);
        assertEquals(0f, out[0], 1e-5f);
        assertEquals(0f, out[1], 1e-5f);

        // tilt_x/tilt_y are one vector rotated by the orientation, so the pair
        // keeps the tilt magnitude whatever the direction is.
        ToolAxes.tiltXY(0.75f, 1.1f, out);
        assertEquals(0.75, (double) Math.hypot(out[0], out[1]), 1e-4);
    }

    @Test
    public void orientationIsWrappedIntoTheSignedPiRange() {
        float[] measured = new float[2];
        float[] wrapped = new float[2];

        // Measured on the Pad 3: AXIS_ORIENTATION reached ~4.37 rad, past +/-pi.
        ToolAxes.tiltXY(0.75f, 4.37f, measured);
        ToolAxes.tiltXY(0.75f, (float) (4.37 - 2.0 * Math.PI), wrapped);

        assertEquals(wrapped[0], measured[0], 1e-5f);
        assertEquals(wrapped[1], measured[1], 1e-5f);
        assertTrue(measured[0] < 0f);
        assertTrue(measured[1] < 0f);
    }
}
