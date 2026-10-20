/* Host test for the mutter audio-out gate (meta-anland-audio-gate.h): desktop audio
 * reaches the tablet only while it makes sound, plus a 2 s tail, so Android's audio
 * output can sleep. Run with: sh producers/gnome/anland_backend/tests/run_audio_gate_tests.sh */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mutter/src/backends/anland/meta-anland-audio-gate.h"

#define S 1000000   /* one second in microseconds */

static void test_chunk_has_sound (void)
{
  int16_t buf[1024 * 2];

  memset (buf, 0, sizeof (buf));
  assert (!anland_audio_chunk_has_sound (buf, sizeof (buf)));   /* digital silence */
  assert (!anland_audio_chunk_has_sound (buf, 0));              /* empty chunk */
  assert (!anland_audio_chunk_has_sound (NULL, 4096));          /* no data */

  buf[2047] = 1;                                                /* last sample, odd tail */
  assert (anland_audio_chunk_has_sound (buf, sizeof (buf)));
  buf[2047] = 0;
  buf[0] = -1;                                                  /* first sample, negative */
  assert (anland_audio_chunk_has_sound (buf, sizeof (buf)));
  buf[0] = 0;

  /* A non-zero byte in a size that is not a multiple of 8 (tail loop). */
  uint8_t odd[13] = { 0 };
  odd[12] = 0x80;
  assert (anland_audio_chunk_has_sound (odd, sizeof (odd)));
  assert (!anland_audio_chunk_has_sound (odd, 12));
}

static void test_gate (void)
{
  AnlandAudioGate gate = { 0 };
  bool changed;

  /* Silence from the start (nothing ever played): never forwarded. */
  assert (!anland_audio_gate_forward (&gate, false, 0, &changed) && !changed);
  assert (!anland_audio_gate_forward (&gate, false, 10 * S, &changed) && !changed);

  /* First sound: forwarded at once, state flips. */
  assert (anland_audio_gate_forward (&gate, true, 20 * S, &changed) && changed);

  /* Silence right after, and just inside the 2 s tail: still forwarded. */
  assert (anland_audio_gate_forward (&gate, false, 20 * S + 1, &changed) && !changed);
  assert (anland_audio_gate_forward (&gate, false, 22 * S - 1, &changed) && !changed);

  /* At exactly 2 s after the last sound: dropped, state flips once. */
  assert (!anland_audio_gate_forward (&gate, false, 22 * S, &changed) && changed);
  assert (!anland_audio_gate_forward (&gate, false, 30 * S, &changed) && !changed);

  /* A new sound after a long pause is forwarded immediately (first sound kept). */
  assert (anland_audio_gate_forward (&gate, true, 60 * S, &changed) && changed);

  /* A short pause inside the tail followed by sound: no gap, no flip. */
  assert (anland_audio_gate_forward (&gate, false, 61 * S, &changed) && !changed);
  assert (anland_audio_gate_forward (&gate, true, 61 * S + 500000, &changed) && !changed);
  assert (anland_audio_gate_forward (&gate, false, 63 * S, &changed) && !changed);
  assert (!anland_audio_gate_forward (&gate, false, 63 * S + 500000, &changed) && changed);

  /* changed is optional. */
  assert (!anland_audio_gate_forward (&gate, false, 70 * S, NULL));
}

int main (void)
{
  test_chunk_has_sound ();
  test_gate ();
  puts ("audio gate tests passed");
  return 0;
}
