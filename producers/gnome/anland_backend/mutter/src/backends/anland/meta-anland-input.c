/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-anland-input.h"

#include "backends/meta-backend-private.h"
#ifdef ANLAND_MUTTER_50
#include "backends/meta-logical-monitor-private.h"
#else
#include "backends/meta-logical-monitor.h"
#endif
#include "backends/meta-monitor-manager-private.h"
#include "backends/native/meta-input-thread.h"
#include "backends/native/meta-virtual-input-device-native.h"

#include <linux/input.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANLAND_MAX_TOUCH_BATCH_EVENTS 128
#define ANLAND_TOUCH_BATCH_TIMEOUT_MS 100

struct _MetaAnlandInput
{
  MetaBackend *backend;
  ClutterVirtualInputDevice *pointer;
  ClutterVirtualInputDevice *keyboard;
  ClutterVirtualInputDevice *touchscreen;
  ClutterVirtualInputDevice *tablet;
  gboolean pressed_keys[KEY_CNT];
  gboolean pressed_buttons[KEY_CNT];
  GHashTable *touch_slots;
  gboolean active_touch_slots[CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS];
  GArray *touch_batch;
  guint touch_batch_timeout_id;

  /* Stylus / tablet tool state. One stable ClutterInputDeviceTool per tool_id
   * (an eraser is a different tool), plus the single sample being assembled
   * between TOOL_POSITION/TOOL_AXES/TOOL_BUTTON and its TOOL_FRAME boundary. */
  GHashTable *tools;
  ClutterInputDeviceTool *current_tool;
  gboolean tool_in_proximity;
  gboolean sample_dirty;
  gboolean sample_has_motion;
  float sample_x;
  float sample_y;
  float sample_pressure;
  float sample_tilt_x;
  float sample_tilt_y;
  gboolean sample_tip;
  gboolean emitted_tip;
  uint32_t sample_button;
  gboolean sample_button_pressed;
  gboolean emitted_button_pressed;

  MetaAnlandInputRefreshFunc refresh_func;
  gpointer refresh_user_data;
};

static void
map_input_coordinates (MetaAnlandInput *input,
                       uint32_t         input_width,
                       uint32_t         input_height,
                       float            x,
                       float            y,
                       gboolean         is_delta,
                       float           *mapped_x,
                       float           *mapped_y)
{
  MetaMonitorManager *monitor_manager;
  MetaLogicalMonitor *logical_monitor;
  MtkRectangle layout;

  *mapped_x = x;
  *mapped_y = y;

  if (input_width == 0 || input_height == 0)
    return;

  monitor_manager = meta_backend_get_monitor_manager (input->backend);
  logical_monitor =
    meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (!logical_monitor)
    return;

  layout = meta_logical_monitor_get_layout (logical_monitor);

  /* Anland reports buffer pixels, while Clutter uses the logical layout. */
  *mapped_x = x * layout.width / input_width;
  *mapped_y = y * layout.height / input_height;

  if (!is_delta)
    {
      *mapped_x += layout.x;
      *mapped_y += layout.y;
    }
}

static gboolean
is_finite_pair (float x,
                float y)
{
  return isfinite (x) && isfinite (y);
}

static void
clear_touch_slot (MetaAnlandInput *input,
                  int              slot)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iter, input->touch_slots);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_UINT (value) == (guint) slot + 1)
        {
          g_hash_table_iter_remove (&iter);
          break;
        }
    }
  input->active_touch_slots[slot] = FALSE;
}

static void
cancel_touches (MetaAnlandInput *input)
{
  MetaVirtualTouchEvent events[CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS];
  guint n_events = 0;
  guint slot;

  if (!input->touchscreen)
    return;

  for (slot = 0; slot < G_N_ELEMENTS (input->active_touch_slots); slot++)
    {
      if (!input->active_touch_slots[slot])
        continue;

      events[n_events++] = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_CANCEL,
        .slot = slot,
      };
    }

  if (n_events > 0)
    meta_virtual_input_device_native_notify_touch_events (
      META_VIRTUAL_INPUT_DEVICE_NATIVE (input->touchscreen),
      g_get_monotonic_time (), events, n_events);

  if (input->touch_slots)
    g_hash_table_remove_all (input->touch_slots);
  memset (input->active_touch_slots, 0, sizeof (input->active_touch_slots));
}

static gboolean
touch_batch_timeout_cb (gpointer user_data)
{
  MetaAnlandInput *input = user_data;

  input->touch_batch_timeout_id = 0;
  if (input->touch_batch->len > 0)
    {
      g_warning ("Anland input dropped an incomplete touch frame");
      cancel_touches (input);
      g_array_set_size (input->touch_batch, 0);
    }

  return G_SOURCE_REMOVE;
}

static void
schedule_touch_batch_timeout (MetaAnlandInput *input)
{
  if (!input->touch_batch_timeout_id)
    input->touch_batch_timeout_id =
      g_timeout_add (ANLAND_TOUCH_BATCH_TIMEOUT_MS,
                     touch_batch_timeout_cb, input);
}

static int
allocate_touch_slot (MetaAnlandInput *input)
{
  guint slot;

  for (slot = 0; slot < G_N_ELEMENTS (input->active_touch_slots); slot++)
    {
      if (!input->active_touch_slots[slot])
        return slot;
    }

  return -1;
}

static void
flush_touch_batch (MetaAnlandInput *input)
{
  guint i;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }

  if (input->touch_batch->len == 0)
    return;

  meta_virtual_input_device_native_notify_touch_events (
    META_VIRTUAL_INPUT_DEVICE_NATIVE (input->touchscreen),
    g_get_monotonic_time (), (const MetaVirtualTouchEvent *) input->touch_batch->data,
    input->touch_batch->len);

  for (i = 0; i < input->touch_batch->len; i++)
    {
      const MetaVirtualTouchEvent *event =
        &g_array_index (input->touch_batch, MetaVirtualTouchEvent, i);

      if (event->type == META_VIRTUAL_TOUCH_EVENT_UP)
        clear_touch_slot (input, event->slot);
    }
  g_array_set_size (input->touch_batch, 0);
}

static void
handle_pointer_motion (MetaAnlandInput         *input,
                       const anland_device_input_t *event,
                       uint32_t                 input_width,
                       uint32_t                 input_height)
{
  float x, y, dx, dy;

  if (!is_finite_pair (event->pointer_motion.x, event->pointer_motion.y) ||
      !is_finite_pair (event->pointer_motion.dx, event->pointer_motion.dy))
    return;

  map_input_coordinates (input, input_width, input_height,
                         event->pointer_motion.x, event->pointer_motion.y,
                         FALSE, &x, &y);
  map_input_coordinates (input, input_width, input_height,
                         event->pointer_motion.dx, event->pointer_motion.dy,
                         TRUE, &dx, &dy);

  meta_virtual_input_device_native_notify_absolute_relative_motion (
    META_VIRTUAL_INPUT_DEVICE_NATIVE (input->pointer), g_get_monotonic_time (),
    x, y, dx, dy);
}

static void
handle_pointer_button (MetaAnlandInput         *input,
                       const anland_device_input_t *event)
{
  uint32_t button = event->pointer_button.button;
  gboolean pressed = event->pointer_button.pressed != 0;

  if (button >= KEY_CNT || button < BTN_LEFT || button > BTN_GEAR_UP ||
      input->pressed_buttons[button] == pressed)
    return;

  input->pressed_buttons[button] = pressed;
  clutter_virtual_input_device_notify_button (
    input->pointer, g_get_monotonic_time (), meta_evdev_button_to_clutter (button),
    pressed ? CLUTTER_BUTTON_STATE_PRESSED : CLUTTER_BUTTON_STATE_RELEASED);
}

static void
handle_pointer_axis (MetaAnlandInput         *input,
                     const anland_device_input_t *event)
{
  ClutterScrollDirection direction;
  double dx = 0.0;
  double dy = 0.0;
  int64_t discrete = event->pointer_axis.discrete;
  int steps;

  if (!isfinite (event->pointer_axis.value))
    return;

  switch (event->pointer_axis.axis)
    {
    case 0:
      dy = event->pointer_axis.value;
      direction = discrete >= 0 ? CLUTTER_SCROLL_DOWN : CLUTTER_SCROLL_UP;
      break;
    case 1:
      dx = event->pointer_axis.value;
      direction = discrete >= 0 ? CLUTTER_SCROLL_RIGHT : CLUTTER_SCROLL_LEFT;
      break;
    default:
      g_warning ("Anland input ignored invalid scroll axis %u",
                 event->pointer_axis.axis);
      return;
    }

  if (dx != 0.0 || dy != 0.0)
    clutter_virtual_input_device_notify_scroll_continuous (
      input->pointer, g_get_monotonic_time (), dx, dy,
      CLUTTER_SCROLL_SOURCE_WHEEL, CLUTTER_SCROLL_FINISHED_NONE);

  steps = MIN (llabs (discrete), 120);
  while (steps-- > 0)
    clutter_virtual_input_device_notify_discrete_scroll (
      input->pointer, g_get_monotonic_time (), direction,
      CLUTTER_SCROLL_SOURCE_WHEEL);
}

static void
handle_key (MetaAnlandInput         *input,
            const anland_device_input_t *event)
{
  uint32_t keycode = event->key.keycode;
  gboolean pressed;

  if (keycode >= KEY_CNT ||
      (event->key.action != ANLAND_DEVICE_ACTION_DOWN &&
       event->key.action != ANLAND_DEVICE_ACTION_UP))
    return;

  pressed = event->key.action == ANLAND_DEVICE_ACTION_DOWN;
  if (input->pressed_keys[keycode] == pressed)
    return;

  input->pressed_keys[keycode] = pressed;
  clutter_virtual_input_device_notify_key (
    input->keyboard, g_get_monotonic_time (), keycode,
    pressed ? CLUTTER_KEY_STATE_PRESSED : CLUTTER_KEY_STATE_RELEASED);
}

static void
handle_touch (MetaAnlandInput         *input,
              const anland_device_input_t *event,
              uint32_t                 input_width,
              uint32_t                 input_height)
{
  gpointer value;
  int slot;
  MetaVirtualTouchEvent touch_event;
  float x, y;

  if ((event->touch.action == ANLAND_DEVICE_ACTION_DOWN ||
       event->touch.action == ANLAND_DEVICE_ACTION_MOVE) &&
      !is_finite_pair (event->touch.x, event->touch.y))
    return;

  if (event->touch.action == ANLAND_DEVICE_ACTION_DOWN ||
      event->touch.action == ANLAND_DEVICE_ACTION_MOVE)
    map_input_coordinates (input, input_width, input_height,
                           event->touch.x, event->touch.y,
                           FALSE, &x, &y);

  value = g_hash_table_lookup (input->touch_slots,
                               GINT_TO_POINTER (event->touch.pointer_id));
  switch (event->touch.action)
    {
    case ANLAND_DEVICE_ACTION_DOWN:
      if (value)
        return;

      slot = allocate_touch_slot (input);
      if (slot < 0)
        {
          g_warning ("Anland input ran out of touch slots");
          return;
        }
      input->active_touch_slots[slot] = TRUE;
      g_hash_table_insert (input->touch_slots,
                           GINT_TO_POINTER (event->touch.pointer_id),
                           GUINT_TO_POINTER (slot + 1));
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_DOWN,
        .slot = slot,
        .x = x,
        .y = y,
      };
      break;
    case ANLAND_DEVICE_ACTION_MOVE:
      if (!value)
        return;
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_MOTION,
        .slot = GPOINTER_TO_UINT (value) - 1,
        .x = x,
        .y = y,
      };
      break;
    case ANLAND_DEVICE_ACTION_UP:
      if (!value)
        return;
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_UP,
        .slot = GPOINTER_TO_UINT (value) - 1,
      };
      break;
    default:
      return;
    }

  if (input->touch_batch->len >= ANLAND_MAX_TOUCH_BATCH_EVENTS)
    {
      g_warning ("Anland input dropped an oversized touch frame");
      cancel_touches (input);
      g_array_set_size (input->touch_batch, 0);
      return;
    }

  g_array_append_val (input->touch_batch, touch_event);
  schedule_touch_batch_timeout (input);
}

static ClutterInputDeviceTool *
get_or_create_tool (MetaAnlandInput *input,
                    uint32_t         tool_id,
                    uint32_t         tool_type,
                    uint32_t         caps)
{
  ClutterInputDeviceTool *tool;
  ClutterInputAxisFlags axes = 0;
  ClutterInputDeviceToolType type;

  tool = g_hash_table_lookup (input->tools, GUINT_TO_POINTER (tool_id));
  if (tool)
    return tool;

  if (caps & ANLAND_DEVICE_TOOL_CAP_PRESSURE)
    axes |= CLUTTER_INPUT_AXIS_FLAG_PRESSURE;
  if (caps & ANLAND_DEVICE_TOOL_CAP_TILT)
    axes |= CLUTTER_INPUT_AXIS_FLAG_XTILT | CLUTTER_INPUT_AXIS_FLAG_YTILT;
  if (caps & ANLAND_DEVICE_TOOL_CAP_DISTANCE)
    axes |= CLUTTER_INPUT_AXIS_FLAG_DISTANCE;
  if (caps & ANLAND_DEVICE_TOOL_CAP_ROTATION)
    axes |= CLUTTER_INPUT_AXIS_FLAG_ROTATION;

  type = tool_type == ANLAND_DEVICE_TOOL_TYPE_ERASER ?
    CLUTTER_INPUT_DEVICE_TOOL_ERASER : CLUTTER_INPUT_DEVICE_TOOL_PEN;

  tool = meta_input_device_tool_native_new_virtual (tool_id, type, axes);
  g_hash_table_insert (input->tools, GUINT_TO_POINTER (tool_id), tool);

  return tool;
}

static void
reset_tool_sample (MetaAnlandInput *input)
{
  input->sample_dirty = FALSE;
  input->sample_has_motion = FALSE;
  input->sample_pressure = 0.0f;
  input->sample_tilt_x = 0.0f;
  input->sample_tilt_y = 0.0f;
  input->sample_tip = FALSE;
  input->emitted_tip = FALSE;
  input->sample_button = 0;
  input->sample_button_pressed = FALSE;
  input->emitted_button_pressed = FALSE;
}

static void
tool_proximity_out (MetaAnlandInput *input)
{
  if (!input->tool_in_proximity || !input->current_tool || !input->tablet)
    return;

  /* Never leave the tip logically pressed when the tool leaves range. */
  if (input->emitted_tip)
    {
      clutter_virtual_input_device_notify_button (
        input->tablet, g_get_monotonic_time (),
        meta_evdev_tool_button_to_clutter (BTN_TOUCH),
        CLUTTER_BUTTON_STATE_RELEASED);
      input->emitted_tip = FALSE;
    }

  /* No Clutter proximity-out is emitted here. Android sends HOVER_EXIT
   * both when the pen leaves range and when the tip touches down, and the
   * app maps both to PROXIMITY_OUT, with an immediate PROXIMITY_IN in the
   * contact case. Emitting the out makes the wayland tablet tool clear its
   * focus (PROXIMITY_OUT -> set_focus NULL) right before the tip arrives,
   * and the entire stroke is lost: measured, every tip-down arrived
   * focusless while the hover motions around it were delivered. The IN
   * that follows re-establishes proximity, so nothing is emitted here.
   * shortcut: while the pen is physically away the compositor keeps the
   * last focus and hover cursor, until the app defers its HOVER_EXIT
   * leave with a timeout. */
  input->tool_in_proximity = FALSE;
  reset_tool_sample (input);
}

static void
handle_tool (MetaAnlandInput             *input,
             const anland_device_input_t *event)
{
  ClutterInputDeviceTool *tool;

  switch (event->tool.action)
    {
    case ANLAND_DEVICE_TOOL_ADDED:
      get_or_create_tool (input, event->tool.tool_id, event->tool.tool_type,
                          event->tool.caps);
      break;
    case ANLAND_DEVICE_TOOL_REMOVED:
      tool = g_hash_table_lookup (input->tools,
                                  GUINT_TO_POINTER (event->tool.tool_id));
      if (tool && tool == input->current_tool)
        tool_proximity_out (input);

      /* The tool object is deliberately KEPT. tablet-v2 wants one stable tool
       * per physical tool, and mutter's wayland layer keys its tools by this
       * pointer and never forgets them: recreating the object each hover
       * session leaves the compositor holding tools that never receive a focus
       * surface, whose events are then dropped (measured: 15297 events matched
       * a tool with no focus, 0 of them tip presses). Only the proximity state
       * is session-scoped, not the tool. */
      break;
    default:
      break;
    }
}

static void
handle_tool_position (MetaAnlandInput             *input,
                      const anland_device_input_t *event,
                      uint32_t                     input_width,
                      uint32_t                     input_height)
{
  ClutterInputDeviceTool *tool;

  tool = g_hash_table_lookup (input->tools,
                              GUINT_TO_POINTER (event->tool_position.tool_id));
  if (!tool)
    return;

  switch (event->tool_position.action)
    {
    case ANLAND_DEVICE_TOOL_PROXIMITY_IN:
      if (input->tool_in_proximity && input->current_tool == tool)
        break;

      tool_proximity_out (input);
      input->current_tool = tool;
      meta_virtual_input_device_native_notify_tool_proximity (
        META_VIRTUAL_INPUT_DEVICE_NATIVE (input->tablet),
        g_get_monotonic_time (), tool, TRUE);
      input->tool_in_proximity = TRUE;
      break;
    case ANLAND_DEVICE_TOOL_MOTION:
      if (!input->tool_in_proximity || input->current_tool != tool)
        break;
      if (!is_finite_pair (event->tool_position.x, event->tool_position.y))
        break;

      map_input_coordinates (input, input_width, input_height,
                             event->tool_position.x, event->tool_position.y,
                             FALSE, &input->sample_x, &input->sample_y);
      input->sample_has_motion = TRUE;
      input->sample_dirty = TRUE;
      break;
    case ANLAND_DEVICE_TOOL_PROXIMITY_OUT:
      if (input->current_tool == tool)
        tool_proximity_out (input);
      break;
    default:
      break;
    }
}

static void
handle_tool_axes (MetaAnlandInput             *input,
                  const anland_device_input_t *event)
{
  ClutterInputDeviceTool *tool;

  if (!input->tool_in_proximity)
    return;

  tool = g_hash_table_lookup (input->tools,
                              GUINT_TO_POINTER (event->tool_axes.tool_id));
  if (!tool || tool != input->current_tool)
    return;

  /* The consumer normalises pressure, but clamp again: the raw value reached
   * ~9.77 on the Pad 3 before normalisation, and a bad producer must never
   * push an out-of-range axis into the seat. */
  if (isfinite (event->tool_axes.pressure))
    input->sample_pressure = CLAMP (event->tool_axes.pressure, 0.0f, 1.0f);
  /* The app sends radians; Clutter's tablet axes (libinput convention) and
   * the tablet-v2 wire protocol are DEGREES (GDK maps the tilt event from
   * a -90..90 valuator range). Sending radians made every client read
   * tilt/180: GTK showed -0.009 for a -0.75 rad tilt. */
  if (isfinite (event->tool_axes.tilt_x))
    input->sample_tilt_x = event->tool_axes.tilt_x * (180.0 / G_PI);
  if (isfinite (event->tool_axes.tilt_y))
    input->sample_tilt_y = event->tool_axes.tilt_y * (180.0 / G_PI);

  input->sample_dirty = TRUE;
}

static void
handle_tool_button (MetaAnlandInput             *input,
                    const anland_device_input_t *event)
{
  ClutterInputDeviceTool *tool;

  if (!input->tool_in_proximity)
    return;

  tool = g_hash_table_lookup (input->tools,
                              GUINT_TO_POINTER (event->tool_button.tool_id));
  if (!tool || tool != input->current_tool)
    return;

  /* The Stylo 2 reports no barrel button, so in practice this is the tip
   * contact (BTN_TOUCH semantics) that drives zwp_tablet_tool_v2 down/up. */
  if (event->tool_button.button != 0)
    {
      input->sample_button = event->tool_button.button;
      input->sample_button_pressed = event->tool_button.pressed != 0;
    }
  else
    {
      input->sample_tip = event->tool_button.tip != 0;
    }

  input->sample_dirty = TRUE;
}

static void
flush_tool_sample (MetaAnlandInput *input)
{
  if (!input->sample_dirty || !input->tool_in_proximity || !input->current_tool)
    return;

  if (input->sample_has_motion)
    {
      meta_virtual_input_device_native_notify_tool_motion (
        META_VIRTUAL_INPUT_DEVICE_NATIVE (input->tablet),
        g_get_monotonic_time (),
        input->sample_x, input->sample_y,
        input->sample_pressure, input->sample_tilt_x, input->sample_tilt_y);
    }

  /* NOTE: clutter_virtual_input_device_notify_button() takes a CLUTTER button
   * number (the existing pointer path passes meta_evdev_button_to_clutter()),
   * and mutter turns it back into an evdev code before the seat sees it.
   * Handing it a raw evdev code instead makes the virtual-device button guard
   * reject it ("Unknown/invalid virtual device button 0x255 pressed") and the
   * tip is silently dropped. With the tool set, BTN_TOUCH -> PRIMARY then maps
   * back to BTN_TOUCH in the seat, which is exactly tablet-v2 tip semantics. */
  if (input->sample_tip != input->emitted_tip)
    {
      clutter_virtual_input_device_notify_button (
        input->tablet, g_get_monotonic_time (),
        meta_evdev_tool_button_to_clutter (BTN_TOUCH),
        input->sample_tip ? CLUTTER_BUTTON_STATE_PRESSED :
                            CLUTTER_BUTTON_STATE_RELEASED);
      input->emitted_tip = input->sample_tip;
    }

  if (input->sample_button != 0 &&
      input->sample_button_pressed != input->emitted_button_pressed)
    {
      clutter_virtual_input_device_notify_button (
        input->tablet, g_get_monotonic_time (),
        meta_evdev_tool_button_to_clutter (input->sample_button),
        input->sample_button_pressed ? CLUTTER_BUTTON_STATE_PRESSED :
                                       CLUTTER_BUTTON_STATE_RELEASED);
      input->emitted_button_pressed = input->sample_button_pressed;
    }

  input->sample_has_motion = FALSE;
  input->sample_dirty = FALSE;
}

MetaAnlandInput *
meta_anland_input_new (MetaBackend               *backend,
                       MetaAnlandInputRefreshFunc  refresh_func,
                       gpointer                    user_data,
                       GError                    **error)
{
  MetaAnlandInput *input;
  ClutterSeat *seat;

  seat = meta_backend_get_default_seat (backend);
  if (!seat)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland backend has no default input seat");
      return NULL;
    }

  input = g_new0 (MetaAnlandInput, 1);
  input->backend = backend;
  input->touch_slots = g_hash_table_new (g_direct_hash, g_direct_equal);
  input->touch_batch = g_array_new (FALSE, FALSE, sizeof (MetaVirtualTouchEvent));
  input->tools = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                        NULL, g_object_unref);
  input->pointer = clutter_seat_create_virtual_device (seat,
                                                        CLUTTER_POINTER_DEVICE);
  input->keyboard = clutter_seat_create_virtual_device (seat,
                                                         CLUTTER_KEYBOARD_DEVICE);
  input->touchscreen = clutter_seat_create_virtual_device (seat,
                                                            CLUTTER_TOUCHSCREEN_DEVICE);
  input->tablet = clutter_seat_create_virtual_device (seat,
                                                       CLUTTER_TABLET_DEVICE);
  if (!input->pointer || !input->keyboard || !input->touchscreen ||
      !input->tablet)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create Anland virtual input devices");
      meta_anland_input_free (input);
      return NULL;
    }

  input->refresh_func = refresh_func;
  input->refresh_user_data = user_data;
  return input;
}

void
meta_anland_input_free (MetaAnlandInput *input)
{
  if (!input)
    return;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }
  meta_anland_input_reset (input);
  g_clear_object (&input->pointer);
  g_clear_object (&input->keyboard);
  g_clear_object (&input->touchscreen);
  g_clear_object (&input->tablet);
  g_clear_pointer (&input->touch_slots, g_hash_table_unref);
  g_clear_pointer (&input->touch_batch, g_array_unref);
  g_clear_pointer (&input->tools, g_hash_table_unref);
  g_free (input);
}

MetaAnlandInputEventResult
meta_anland_input_handle_event (MetaAnlandInput             *input,
                                const anland_device_input_t *event,
                                uint32_t                     input_width,
                                uint32_t                     input_height)
{
  switch (event->type)
    {
    case ANLAND_DEVICE_IN_PTR_MOTION:
      handle_pointer_motion (input, event, input_width, input_height);
      break;
    case ANLAND_DEVICE_IN_PTR_BUTTON:
      handle_pointer_button (input, event);
      break;
    case ANLAND_DEVICE_IN_PTR_AXIS:
      handle_pointer_axis (input, event);
      break;
    case ANLAND_DEVICE_IN_KEY:
      handle_key (input, event);
      break;
    case ANLAND_DEVICE_IN_TOUCH:
      handle_touch (input, event, input_width, input_height);
      break;
    case ANLAND_DEVICE_IN_TOUCH_FRAME:
      flush_touch_batch (input);
      break;
    case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
      if (event->display.refresh_mhz >= 1000 &&
          event->display.refresh_mhz <= 1000000 && input->refresh_func)
        input->refresh_func (event->display.refresh_mhz,
                             input->refresh_user_data);
      break;
    case ANLAND_DEVICE_IN_TOOL:
      handle_tool (input, event);
      break;
    case ANLAND_DEVICE_IN_TOOL_POSITION:
      handle_tool_position (input, event, input_width, input_height);
      break;
    case ANLAND_DEVICE_IN_TOOL_AXES:
      handle_tool_axes (input, event);
      break;
    case ANLAND_DEVICE_IN_TOOL_BUTTON:
      handle_tool_button (input, event);
      break;
    case ANLAND_DEVICE_IN_TOOL_FRAME:
      flush_tool_sample (input);
      break;
    case ANLAND_DEVICE_IN_CLIPBOARD:
    case ANLAND_DEVICE_IN_TEXT_INPUT:
      return META_ANLAND_INPUT_EVENT_NEEDS_PAYLOAD;
    case ANLAND_DEVICE_IN_RESOURCE:
      return META_ANLAND_INPUT_EVENT_NEEDS_RESOURCE_FDS;
    case ANLAND_DEVICE_IN_ACTION:
      break;
    default:
      g_warning ("Anland input ignored unknown event type %u", event->type);
      break;
    }

  return META_ANLAND_INPUT_EVENT_HANDLED;
}

gboolean
meta_anland_input_inject_text (MetaAnlandInput *input,
                               const char      *text)
{
  const char *cursor = text;
  gboolean injected = FALSE;

  if (!input || !input->keyboard || !text)
    return FALSE;

  while (*cursor)
    {
      gunichar keyval;

      keyval = g_utf8_get_char_validated (cursor, -1);
      if (keyval == (gunichar) -1 || keyval == (gunichar) -2)
        return FALSE;

      switch (keyval)
        {
        case '\n':
        case '\r':
          keyval = CLUTTER_KEY_Return;
          break;
        case '\t':
          keyval = CLUTTER_KEY_Tab;
          break;
        case '\b':
          keyval = CLUTTER_KEY_BackSpace;
          break;
        default:
          break;
        }

      clutter_virtual_input_device_notify_keyval (
        input->keyboard, g_get_monotonic_time (), keyval,
        CLUTTER_KEY_STATE_PRESSED);
      clutter_virtual_input_device_notify_keyval (
        input->keyboard, g_get_monotonic_time (), keyval,
        CLUTTER_KEY_STATE_RELEASED);
      cursor = g_utf8_next_char (cursor);
      injected = TRUE;
    }

  return injected;
}

void
meta_anland_input_reset (MetaAnlandInput *input)
{
  guint code;

  if (!input)
    return;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }

  for (code = 0; code < G_N_ELEMENTS (input->pressed_keys); code++)
    {
      if (!input->pressed_keys[code])
        continue;

      clutter_virtual_input_device_notify_key (
        input->keyboard, g_get_monotonic_time (), code,
        CLUTTER_KEY_STATE_RELEASED);
      input->pressed_keys[code] = FALSE;
    }

  for (code = BTN_LEFT; code < G_N_ELEMENTS (input->pressed_buttons); code++)
    {
      if (!input->pressed_buttons[code])
        continue;

      clutter_virtual_input_device_notify_button (
        input->pointer, g_get_monotonic_time (),
        meta_evdev_button_to_clutter (code), CLUTTER_BUTTON_STATE_RELEASED);
      input->pressed_buttons[code] = FALSE;
    }

  cancel_touches (input);
  g_array_set_size (input->touch_batch, 0);

  tool_proximity_out (input);
  input->current_tool = NULL;
}