/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

/*
 * Decides whether a chunk of desktop audio is forwarded to the consumer.
 *
 * The consumer stops its Android output stream after a short time without PCM, so
 * Android's audio hardware can sleep. Forwarding silence (the speaker node runs on
 * an always-processing graph) would keep it awake forever. A chunk is forwarded
 * when it contains any non-zero sample, or within ANLAND_AUDIO_GRACE_US of the last
 * one, so short pauses and the tail of a sound still go through. Only exact
 * digital silence counts as silence.
 *
 * Dependency-free so the realtime callback can use it without locks and a host
 * test can exercise it (producers/gnome/anland_backend/tests).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ANLAND_AUDIO_GRACE_US (2 * 1000 * 1000)

typedef struct
{
  int64_t last_sound_us;
  bool heard;    /* a non-zero sample was seen at least once */
  bool active;   /* the last decision forwarded */
} AnlandAudioGate;

/* Whether any byte, and so any sample, of the chunk is non-zero. */
static inline bool
anland_audio_chunk_has_sound (const void *data,
                              size_t      bytes)
{
  const uint8_t *p = data;
  size_t i = 0;

  if (!p)
    return false;

  for (; i + sizeof (uint64_t) <= bytes; i += sizeof (uint64_t))
    {
      uint64_t word;

      memcpy (&word, p + i, sizeof (word));
      if (word)
        return true;
    }
  for (; i < bytes; i++)
    {
      if (p[i])
        return true;
    }
  return false;
}

/* Record one chunk at now_us (monotonic) and return whether to forward it.
 * *changed (optional) is set when the forward/drop state flips. */
static inline bool
anland_audio_gate_forward (AnlandAudioGate *gate,
                           bool             has_sound,
                           int64_t          now_us,
                           bool            *changed)
{
  bool was_active = gate->active;

  if (has_sound)
    {
      gate->last_sound_us = now_us;
      gate->heard = true;
    }
  gate->active = gate->heard &&
                 now_us - gate->last_sound_us < ANLAND_AUDIO_GRACE_US;
  if (changed)
    *changed = gate->active != was_active;
  return gate->active;
}
