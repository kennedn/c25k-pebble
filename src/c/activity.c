#include "activity.h"



#include "bitmap.h"
#include "programme.h"
#include "reward.h"

static const uint32_t MARKER_DEGREES = 2;
static const int16_t PHASE_HEIGHT = 36;
static const uint32_t TIMER_TIMEOUT_MS = 500;

#ifdef PBL_ROUND
static const int16_t PADDING_SIZE = -1;
static const int16_t RADIAL_WIDTH = 32;
static const int16_t MARKER_SIZE = 32;
#else
static const int16_t PADDING_SIZE = 5;
static const int16_t RADIAL_WIDTH = 16;
static const int16_t MARKER_SIZE = 16;
#endif


typedef enum {
  ACTIVITY_ACTIVE,
  ACTIVITY_PAUSED,
  ACTIVITY_COMPLETE,
} ActivityState;

struct ActivityWindow {
  time_t elapsed;
  time_t started_at;
  ActivityState state;
  AppTimer* timer;
  AppTimer *action_bar_timer;

  const Programme* programme;

  Window* window;
  Layer* gfx;
  TextLayer* phase;
  TextLayer* time_remaining;
  RewardWindow* reward;

#ifndef PBL_ROUND
  ActionBarLayer* action_bar;
#endif

  char phase_buffer[10];
  char time_remaining_buffer[24];

  ActivityCallbacks callbacks;
};

typedef struct {
  const GRect bounds;
  GContext* ctx;
  const time_t total_duration;
} StateIteratorData;

static GColor state_colour(ProgrammeState state) {
  switch (state) {
    case PROGRAMME_STATE_WARM_UP:
    case PROGRAMME_STATE_WARM_DOWN:
      return GColorLightGray;

    case PROGRAMME_STATE_WALK:
      return GColorWhite;

    case PROGRAMME_STATE_RUN:
      // The other colours in here are fine on the B&W Pebble screens, but the
      // green gets turned to white, which isn't really useful. We'll do what we
      // did with the number selector and instead use black to highlight.
      return COLOR_FALLBACK(GColorJaegerGreen, GColorBlack);

    default:
      LOG_ERROR("unexpected programme state: %d", (int)state);
      return GColorRed;
  }
}

static int32_t calculate_angle(time_t at, time_t total_duration) {
  return (TRIG_MAX_RATIO * at) / total_duration;
}

static void on_state(time_t at,
                     time_t phase_duration,
                     ProgrammeState state,
                     void* userdata) {
  StateIteratorData* data = (StateIteratorData*)userdata;

  graphics_context_set_fill_color(data->ctx, state_colour(state));
  graphics_fill_radial(
      data->ctx, data->bounds, GOvalScaleModeFitCircle, RADIAL_WIDTH,
      calculate_angle(at, data->total_duration),
      calculate_angle(at + phase_duration, data->total_duration));
}

static void on_update_proc(Layer* layer, GContext* ctx) {
  ActivityWindow* activity =
      (ActivityWindow*)window_get_user_data(layer_get_window(layer));
  GRect bounds = layer_get_bounds(layer);
  int16_t window_width = layer_get_unobstructed_bounds(layer).size.w;
  static const uint32_t marker_angle_delta = DEG_TO_TRIGANGLE(MARKER_DEGREES);
  StateIteratorData userdata = {
      .bounds = bounds,
      .ctx = ctx,
      .total_duration = programme_duration(activity->programme),
  };

  // We'll draw the circle that shows the activity in stages by iterating over
  // the phases in the programme. Each phase will draw an arc section, and if we
  // do the maths correctly, everything will just look like a fancy
  // multicoloured circle.
  programme_iterate_states(activity->programme, on_state, &userdata);

  bounds.size.w += MARKER_SIZE - RADIAL_WIDTH;
  bounds.size.h += MARKER_SIZE - RADIAL_WIDTH;
  bounds.origin.x -= (MARKER_SIZE - RADIAL_WIDTH) / 2;
  bounds.origin.y -= (MARKER_SIZE - RADIAL_WIDTH) / 2;

  graphics_context_set_fill_color(ctx, GColorJaegerGreen);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, MARKER_SIZE,
                       TRIG_MAX_ANGLE - marker_angle_delta, TRIG_MAX_ANGLE);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, MARKER_SIZE,
                       DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(2));

  // Calculate and draw the marker showing our actual progression to date.
  graphics_context_set_fill_color(ctx, GColorRed);
  uint32_t marker_angle = calculate_angle(activity->elapsed, userdata.total_duration);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, MARKER_SIZE,
                        0,
                        marker_angle + marker_angle_delta / 2);
  graphics_context_set_stroke_color(ctx, GColorFromHEX(0x1c7d7a));
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, grect_center_point(&bounds), window_width / 2 - RADIAL_WIDTH - 1);
  graphics_draw_circle(ctx, grect_center_point(&bounds), window_width / 2 - 1);
}

static void update_text_labels(ActivityWindow* activity) {
  if (activity->state == ACTIVITY_ACTIVE) {
    time_t phase_remaining =
        programme_phase_remaining_at(activity->programme, activity->elapsed);

    if (phase_remaining / 60 > 9) {
      snprintf(activity->time_remaining_buffer,
               sizeof(activity->time_remaining_buffer), "%2ld:%02ld",
               phase_remaining / 60, phase_remaining % 60);
    } else {
      snprintf(activity->time_remaining_buffer,
               sizeof(activity->time_remaining_buffer), "%1ld:%02ld",
               phase_remaining / 60, phase_remaining % 60);

      if (phase_remaining == 0) {
        vibes_double_pulse();
      }
    }
    activity
        ->time_remaining_buffer[sizeof(activity->time_remaining_buffer) - 1] =
        '\0';

    strncpy(activity->phase_buffer,
            programme_state_string(
                programme_state_at(activity->programme, activity->elapsed)),
            sizeof(activity->phase_buffer));
    activity->phase_buffer[sizeof(activity->phase_buffer) - 1] = '\0';
  }
}

static void on_reward_back(void* userdata) {
  ActivityWindow* activity = (ActivityWindow*)userdata;

  window_stack_pop(true);
  (activity->callbacks.on_back)(activity->callbacks.userdata);
}

static void activity_complete(ActivityWindow* activity) {
  activity->reward = reward_window_create((RewardCallbacks){
      .on_back = on_reward_back,
      .userdata = activity,
  });

  window_stack_push(reward_window_get_window(activity->reward), true);
}

static void on_tick(void* userdata) {
  ActivityWindow* activity = (ActivityWindow*)userdata;
  activity->timer = NULL;
  activity->elapsed = time(NULL) - activity->started_at;
  if (activity->elapsed >= programme_duration(activity->programme)) {
    activity_window_set_active(activity, false);
    activity_complete(activity);
  } else {
    // We have to chain timers because the TimerService doesn't allow for
    // userdata to be provided. This is going to use more battery, but provides
    // more encapsulated code.
    activity->timer = app_timer_register(TIMER_TIMEOUT_MS, on_tick, activity);
  }

  layer_mark_dirty(activity->gfx);
  update_text_labels(activity);
}

static void on_appear(Window* window) {
  ActivityWindow* activity = (ActivityWindow*)window_get_user_data(window);

  activity->elapsed = 0;
  activity_window_set_active(activity, true);
  update_text_labels(activity);
}

static void on_disappear(Window* window) {
  ActivityWindow* activity = (ActivityWindow*)window_get_user_data(window);

  activity_window_set_active(activity, false);
}

static void on_button_back(ClickRecognizerRef ref, void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;

  (activity->callbacks.on_back)(activity->callbacks.userdata);
}

static void on_button_select(ClickRecognizerRef ref, void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;

  activity_window_set_active(activity,
                             activity->state == ACTIVITY_PAUSED ? true : false);
  layer_mark_dirty(activity->gfx);
  update_text_labels(activity);
}

static void on_button_up(ClickRecognizerRef ref, void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;

  if (activity->state == ACTIVITY_ACTIVE) {
    time_t phase_remaining =
        programme_phase_remaining_at(activity->programme, activity->elapsed);

    if (activity->elapsed + phase_remaining >=
        programme_duration(activity->programme)) {
      activity_complete(activity);
      return;
    }

    activity->started_at -= phase_remaining;
  }
}

static void on_button_down(ClickRecognizerRef ref, void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;

  if (activity->state == ACTIVITY_ACTIVE) {
    time_t phase_elapsed =
        programme_phase_elapsed_at(activity->programme, activity->elapsed);

    // If we're less than five seconds into the phase, we want to go back to the
    // start of the _previous_ phase.
    if (phase_elapsed < 5) {
      // Special case: just go back to the start if this is the first phase.
      if (activity->elapsed - activity->started_at < 5) {
        activity->started_at = time(NULL);
        return;
      }

      activity->started_at += programme_phase_elapsed_at(
                                  activity->programme, activity->elapsed - 5) +
                              5;
    }

    activity->started_at += phase_elapsed + 1;
  }
}

#ifndef PBL_ROUND
static void action_bar_hide(void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;
  activity->action_bar_timer = NULL;
  layer_set_hidden(action_bar_layer_get_layer(activity->action_bar), true);
}
#endif

static void on_button(ClickRecognizerRef ref, void* ctx) {
  ActivityWindow* activity = (ActivityWindow*)ctx;

#ifndef PBL_ROUND
  layer_set_hidden(action_bar_layer_get_layer(activity->action_bar), false);
  if (activity->action_bar_timer) {
    app_timer_reschedule(activity->action_bar_timer, 1600);
  } else {
    activity->action_bar_timer = app_timer_register(1600, action_bar_hide, (void*)activity);
  }
#endif

  switch(click_recognizer_get_button_id(ref)) {
    case BUTTON_ID_BACK:
      on_button_back(ref, ctx);
      break;

    case BUTTON_ID_SELECT:
      on_button_select(ref, ctx);
      break;

    case BUTTON_ID_UP:
      on_button_up(ref, ctx);
      break;

    case BUTTON_ID_DOWN:
      on_button_down(ref, ctx);
      break;
    case NUM_BUTTONS:
      break;
  }

}

static void click_config_provider(void* ctx) {
  window_single_click_subscribe(BUTTON_ID_BACK, on_button);
  window_single_click_subscribe(BUTTON_ID_SELECT, on_button);
  window_single_click_subscribe(BUTTON_ID_UP, on_button);
  window_single_click_subscribe(BUTTON_ID_DOWN, on_button);
}

static void on_load(Window* window) {
  ActivityWindow* activity = (ActivityWindow*)window_get_user_data(window);
  Layer* root = window_get_root_layer(window);
  window_set_background_color(window, GColorJaegerGreen);

#ifdef PBL_ROUND
  // To centre a circle within the round watch screen, we have to ignore the
  // status and action bars, and instead use extra padding to avoid overlaps.
  GRect bounds = layer_get_bounds(root);
#else
  GRect bounds = layer_get_unobstructed_bounds(root);
#endif

  // By working from the outside in, we can incrementally reduce the size of the
  // bounds. First up, we'll just move in the padding size for the graphics
  // layer on which we'll draw the circular view of the activity.
  bounds.size.w -= PADDING_SIZE * 2;
  bounds.size.h -= PADDING_SIZE * 2;
  bounds.origin.x += PADDING_SIZE;
  bounds.origin.y += PADDING_SIZE;
  activity->gfx = layer_create(bounds);
  layer_set_update_proc(activity->gfx, on_update_proc);

  // Now we want to constrain the text layers to the inside of the circle. If we
  // were doing this properly, there'd be a square root involved somewhere. But
  // since not all Pebbles have a FPU and I don't care for pixel precision,
  // we'll just put some fudge in and it'll all be fine. Mmmm. Fudge.
  const uint16_t circle_radius = bounds.size.w - RADIAL_WIDTH * 2.5;
  bounds.origin.x += bounds.size.w / 2 - circle_radius / 2;
  bounds.origin.y += bounds.size.h / 2 - circle_radius / 2 + 20;
  bounds.size.w = circle_radius;
  bounds.size.h = 32;
  activity->time_remaining = text_layer_create(bounds);
  text_layer_set_text_alignment(activity->time_remaining, GTextAlignmentCenter);
  text_layer_set_background_color(activity->time_remaining, GColorJaegerGreen);
  text_layer_set_text_color(activity->time_remaining, GColorWhite);
  text_layer_set_overflow_mode(activity->time_remaining, GTextOverflowModeTrailingEllipsis);
  text_layer_set_font(activity->time_remaining,
                      fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS));
  text_layer_set_text(activity->time_remaining,
                      activity->time_remaining_buffer);
  layer_add_child(root, text_layer_get_layer(activity->time_remaining));

  // Finally, put the phase label below the time remaining.
  bounds.origin.y += bounds.size.h + 4;
  bounds.size.h = 22;
  activity->phase = text_layer_create(bounds);
  text_layer_set_text_alignment(activity->phase, GTextAlignmentCenter);
  text_layer_set_background_color(activity->phase, GColorJaegerGreen);
  text_layer_set_text_color(activity->phase, GColorWhite);
  text_layer_set_font(activity->phase,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(activity->phase, activity->phase_buffer);
  layer_add_child(root, text_layer_get_layer(activity->phase));

  layer_add_child(root, activity->gfx);
#ifndef PBL_ROUND
  activity->action_bar = action_bar_layer_create();
  action_bar_layer_set_context(activity->action_bar, activity);
  action_bar_layer_set_click_config_provider(activity->action_bar,
                                             click_config_provider);
  action_bar_layer_set_background_color(activity->action_bar, GColorWhite);
  action_bar_layer_add_to_window(activity->action_bar, activity->window);
  action_bar_hide((void*)activity);
#else
  window_set_click_config_provider_with_context(
      activity->window, click_config_provider, activity);
#endif


}

static void on_unload(Window* window) {
  ActivityWindow* activity = (ActivityWindow*)window_get_user_data(window);

#ifndef PBL_ROUND
  action_bar_layer_destroy(activity->action_bar);
#endif

  layer_destroy(activity->gfx);
  text_layer_destroy(activity->phase);
  text_layer_destroy(activity->time_remaining);
}

ActivityWindow* activity_window_create(ActivityCallbacks callbacks) {
  ActivityWindow* activity = malloc(sizeof(ActivityWindow));

  activity->elapsed = 0;
  activity->state = ACTIVITY_PAUSED;
  activity->programme = NULL;
  activity->callbacks = callbacks;
  activity->reward = NULL;

  activity->window = window_create();
  window_set_user_data(activity->window, activity);

  window_set_window_handlers(activity->window, (WindowHandlers){
                                                   .load = on_load,
                                                   .unload = on_unload,
                                                   .appear = on_appear,
                                                   .disappear = on_disappear,
                                               });

  return activity;
}

void activity_window_destroy(ActivityWindow* activity) {
  if (activity->reward) {
    reward_window_destroy(activity->reward);
  }
  window_destroy(activity->window);
  free(activity);
}

Window* activity_window_get_window(ActivityWindow* activity) {
  return activity->window;
}

void activity_window_set_active(ActivityWindow* activity, bool active) {
  if (active) {
#ifndef PBL_ROUND
    action_bar_layer_set_icon(activity->action_bar, BUTTON_ID_SELECT,
                              image_pause);
    action_bar_layer_set_icon(activity->action_bar, BUTTON_ID_UP,
                              image_skip_forward);
    action_bar_layer_set_icon(activity->action_bar, BUTTON_ID_DOWN,
                              image_skip_backward);
#endif

    activity->state = ACTIVITY_ACTIVE;
    activity->started_at = time(NULL) - activity->elapsed;
    activity->timer = app_timer_register(TIMER_TIMEOUT_MS, on_tick, activity);
  } else {
#ifndef PBL_ROUND
    action_bar_layer_set_icon(activity->action_bar, BUTTON_ID_SELECT,
                              image_play);
    action_bar_layer_clear_icon(activity->action_bar, BUTTON_ID_UP);
    action_bar_layer_clear_icon(activity->action_bar, BUTTON_ID_DOWN);
#endif

    activity->state = ACTIVITY_PAUSED;
    if(activity->timer) {
      app_timer_cancel(activity->timer);
      activity->timer = NULL;
    }
    if(activity->action_bar_timer) {
      app_timer_cancel(activity->action_bar_timer);
      activity->action_bar_timer = NULL;
    }
  }
}

void activity_window_set_programme(ActivityWindow* activity,
                                   const Programme* programme) {
  activity->programme = programme;
}
