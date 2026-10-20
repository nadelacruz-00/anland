package com.anland.consumer;

import static org.junit.Assert.assertEquals;

import android.media.AudioDeviceInfo;

import org.junit.Test;

/**
 * Which device the desktop mic is routed to. A wrong pick either records from the
 * wrong mic or puts a device into call mode that cannot record.
 */
public class MicRouterTest {
    private static final int BLE = AudioDeviceInfo.TYPE_BLE_HEADSET;
    private static final int SCO = AudioDeviceInfo.TYPE_BLUETOOTH_SCO;
    private static final int SPEAKER = AudioDeviceInfo.TYPE_BUILTIN_SPEAKER;
    private static final int EARPIECE = AudioDeviceInfo.TYPE_BUILTIN_EARPIECE;
    private static final int BLE_SPEAKER = AudioDeviceInfo.TYPE_BLE_SPEAKER;

    @Test
    public void prefersLeAudioOverClassicBluetooth() {
        assertEquals(2, MicRouter.pickHeadset(new int[] {SPEAKER, SCO, BLE}));
        assertEquals(0, MicRouter.pickHeadset(new int[] {BLE, SCO}));
    }

    @Test
    public void fallsBackToClassicBluetooth() {
        assertEquals(1, MicRouter.pickHeadset(new int[] {EARPIECE, SCO, SPEAKER}));
    }

    @Test
    public void takesTheFirstOfSeveralHeadsetsOfOneKind() {
        assertEquals(0, MicRouter.pickHeadset(new int[] {SCO, SCO}));
    }

    @Test
    public void builtInDevicesAndSpeakersAreNotHeadsets() {
        // A Bluetooth LE speaker has no mic: routing to it would leave nothing to record.
        assertEquals(-1, MicRouter.pickHeadset(new int[] {SPEAKER, EARPIECE, BLE_SPEAKER}));
    }

    @Test
    public void noDevicesMeansBuiltInMic() {
        assertEquals(-1, MicRouter.pickHeadset(new int[0]));
        assertEquals(-1, MicRouter.pickHeadset(null));
    }
}
