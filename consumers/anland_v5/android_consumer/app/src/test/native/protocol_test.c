/*
 * Host-side checks for the tablet-tool wire contract (common/protocol.h).
 * Built and run by run_input_grab_tests.sh; no Android, no device.
 *
 * The tool events exist so that a consumer and a producer of different
 * vintages keep talking to each other: every tool struct must fit the existing
 * 16-byte payload union, which is what keeps sizeof (struct InputEvent) and the
 * "skip the types you do not know" rule working. Nothing enforces that at
 * runtime - a struct that overflows the union does not fail, it makes the next
 * event be read from the wrong offset - so it is asserted here.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"
#include "anland_device.h"

/* type + payload, unchanged by the tool events (touch/pointer_motion are 16). */
#define INPUT_EVENT_SIZE    20
#define INPUT_EVENT_PAYLOAD 16

_Static_assert (sizeof (struct InputEvent) == INPUT_EVENT_SIZE,
                "struct InputEvent changed size: a consumer and a producer of "
                "different vintages would desynchronise");
_Static_assert (sizeof (anland_device_input_t) == INPUT_EVENT_SIZE,
                "the producer's input struct no longer matches the wire struct");

#define CHECK_FITS(member) \
  _Static_assert (sizeof (((struct InputEvent *) 0)->member) <= INPUT_EVENT_PAYLOAD, \
                  #member " does not fit the 16-byte payload union")

CHECK_FITS (tool);
CHECK_FITS (tool_position);
CHECK_FITS (tool_axes);
CHECK_FITS (tool_button);
CHECK_FITS (tool_frame);

/* The producer transport passes the wire type through verbatim, so these two
 * enums are the same numbers by construction; keep them that way. */
#define CHECK_SAME(producer_value, wire_value) \
  _Static_assert ((producer_value) == (wire_value), \
                  #producer_value " and " #wire_value " disagree")

CHECK_SAME (ANLAND_DEVICE_IN_TOOL,          INPUT_TYPE_TOOL);
CHECK_SAME (ANLAND_DEVICE_IN_TOOL_POSITION, INPUT_TYPE_TOOL_POSITION);
CHECK_SAME (ANLAND_DEVICE_IN_TOOL_AXES,     INPUT_TYPE_TOOL_AXES);
CHECK_SAME (ANLAND_DEVICE_IN_TOOL_BUTTON,   INPUT_TYPE_TOOL_BUTTON);
CHECK_SAME (ANLAND_DEVICE_IN_TOOL_FRAME,    INPUT_TYPE_TOOL_FRAME);

CHECK_SAME (ANLAND_DEVICE_TOOL_ADDED,        TOOL_ACTION_ADDED);
CHECK_SAME (ANLAND_DEVICE_TOOL_REMOVED,      TOOL_ACTION_REMOVED);
CHECK_SAME (ANLAND_DEVICE_TOOL_PROXIMITY_IN, TOOL_POSITION_PROXIMITY_IN);
CHECK_SAME (ANLAND_DEVICE_TOOL_MOTION,       TOOL_POSITION_MOTION);
CHECK_SAME (ANLAND_DEVICE_TOOL_PROXIMITY_OUT, TOOL_POSITION_PROXIMITY_OUT);
CHECK_SAME (ANLAND_DEVICE_TOOL_TYPE_PEN,     TOOL_TYPE_PEN);
CHECK_SAME (ANLAND_DEVICE_TOOL_TYPE_ERASER,  TOOL_TYPE_ERASER);
CHECK_SAME (ANLAND_DEVICE_TOOL_CAP_PRESSURE, TOOL_CAP_PRESSURE);
CHECK_SAME (ANLAND_DEVICE_TOOL_CAP_TILT,     TOOL_CAP_TILT);
CHECK_SAME (ANLAND_DEVICE_TOOL_CAP_DISTANCE, TOOL_CAP_DISTANCE);
CHECK_SAME (ANLAND_DEVICE_TOOL_CAP_ROTATION, TOOL_CAP_ROTATION);

/* One event as the transport copies it: type + payload, nothing more. */
static void
tool_axes_round_trip (void)
{
  const struct InputEvent out = {
    .type = INPUT_TYPE_TOOL_AXES,
    .tool_axes = { .tool_id = 7, .pressure = 0.25f,
                   .tilt_x = 12.5f, .tilt_y = -7.5f },
  };
  struct InputEvent in;
  uint8_t wire[sizeof (struct InputEvent)];

  memcpy (wire, &out, sizeof (out));
  memcpy (&in, wire, sizeof (in));

  assert (in.type == INPUT_TYPE_TOOL_AXES);
  assert (in.tool_axes.tool_id == 7);
  assert (in.tool_axes.pressure == 0.25f);
  assert (in.tool_axes.tilt_x == 12.5f);
  assert (in.tool_axes.tilt_y == -7.5f);
}

int
main (void)
{
  tool_axes_round_trip ();
  return 0;
}
