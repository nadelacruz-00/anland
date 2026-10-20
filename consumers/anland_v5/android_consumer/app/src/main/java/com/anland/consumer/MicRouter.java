package com.anland.consumer;

import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.List;

/**
 * Routes the mic forwarded to the desktop to a Bluetooth headset while a desktop app
 * records, and gives the headset back afterwards.
 *
 * <p>A headset can only record in its call mode: classic Bluetooth (SCO) then plays
 * narrow-band "call quality" audio and LE Audio switches to a conversational stream.
 * So the headset is made the communication device only while the producer reports
 * that a desktop app records; the rest of the time it keeps its high-quality media
 * playback. The native capture thread calls {@link #onMicCapture} on those changes.
 */
final class MicRouter {
    private static final String TAG = "AnlandMic";

    private final AudioManager mAudioManager;
    private final Native mNative;
    /* Guarded by this: onMicCapture runs on the native capture thread, the device
     * callback on the main thread. */
    private boolean mActive;
    private AudioDeviceInfo mRouted;

    private final AudioDeviceCallback mDeviceCallback = new AudioDeviceCallback() {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
            onDevicesChanged();
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            onDevicesChanged();
        }
    };

    MicRouter(AudioManager audioManager, Native nativeBridge) {
        mAudioManager = audioManager;
        mNative = nativeBridge;
        if (mAudioManager != null)
            mAudioManager.registerAudioDeviceCallback(mDeviceCallback,
                                                      new Handler(Looper.getMainLooper()));
    }

    /** Stop watching devices and give back any headset. Call after the native bridge stopped. */
    void release() {
        if (mAudioManager != null)
            mAudioManager.unregisterAudioDeviceCallback(mDeviceCallback);
        synchronized (this) {
            mActive = false;
            clearRoute();
        }
    }

    /**
     * Called from the native capture thread. {@code active}: a desktop app started
     * recording; returns the AAudio input device id of the headset to record from, or
     * 0 for the default (built-in) mic. {@code !active}: recording stopped; the headset
     * goes back to media playback.
     */
    synchronized int onMicCapture(boolean active) {
        mActive = active;
        if (!active) {
            clearRoute();
            return 0;
        }
        return route();
    }

    /**
     * Index of the Bluetooth headset to record from among devices of these
     * {@link AudioDeviceInfo} types, or -1 if there is none. LE Audio is preferred: it
     * records at a better quality and can keep playing alongside.
     */
    static int pickHeadset(int[] types) {
        if (types == null)
            return -1;
        int sco = -1;
        for (int i = 0; i < types.length; i++) {
            if (types[i] == AudioDeviceInfo.TYPE_BLE_HEADSET)
                return i;
            if (types[i] == AudioDeviceInfo.TYPE_BLUETOOTH_SCO && sco < 0)
                sco = i;
        }
        return sco;
    }

    private AudioDeviceInfo preferredHeadset() {
        if (mAudioManager == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.S)
            return null;
        List<AudioDeviceInfo> devices = mAudioManager.getAvailableCommunicationDevices();
        int[] types = new int[devices.size()];
        for (int i = 0; i < types.length; i++)
            types[i] = devices.get(i).getType();
        int pick = pickHeadset(types);
        return pick < 0 ? null : devices.get(pick);
    }

    /* The communication devices are outputs; record from the same headset's input. */
    private AudioDeviceInfo inputOf(AudioDeviceInfo headset) {
        for (AudioDeviceInfo in : mAudioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            if (in.getType() == headset.getType()
                    && in.getAddress().equals(headset.getAddress()))
                return in;
        }
        return null;
    }

    private int route() {
        AudioDeviceInfo headset = preferredHeadset();
        if (headset == null) {
            clearRoute();
            Log.i(TAG, "no Bluetooth headset, recording from the built-in mic");
            return 0;
        }
        AudioDeviceInfo input = inputOf(headset);
        if (input == null) {
            clearRoute();
            Log.e(TAG, "mic: headset routing failed: " + headset.getProductName()
                    + " has no mic input");
            return 0;
        }
        if (mRouted == null || mRouted.getId() != headset.getId()) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S
                    || !mAudioManager.setCommunicationDevice(headset)) {
                clearRoute();
                Log.e(TAG, "mic: headset routing failed: " + headset.getProductName()
                        + " refused as communication device");
                return 0;
            }
            mRouted = headset;
        }
        Log.i(TAG, "mic routed to " + headset.getProductName() + " (input " + input.getId() + ")");
        return input.getId();
    }

    private void clearRoute() {
        if (mRouted == null)
            return;
        mRouted = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            mAudioManager.clearCommunicationDevice();
        Log.i(TAG, "headset released");
    }

    /* A headset came or went: if recording and the preferred mic changed, have the
     * capture thread reopen through onMicCapture. Its stream also reports a disconnect
     * by itself when the routed headset goes away. */
    private void onDevicesChanged() {
        synchronized (this) {
            if (!mActive)
                return;
            AudioDeviceInfo want = preferredHeadset();
            if (want == null ? mRouted == null
                             : mRouted != null && mRouted.getId() == want.getId())
                return;
        }
        mNative.requestMicReroute();
    }
}
