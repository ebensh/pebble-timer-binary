// Binary Timer: enter the timer length in minutes as a binary number.
//
// Entry:   UP = 1, DOWN = 0, SELECT = start, long SELECT = clear,
//          BACK = erase last bit (exits when empty).
// Running: SELECT = pause/resume, BACK = cancel back to entry.
// Done:    vibrates; SELECT or BACK returns to entry.
#include <pebble.h>

#define MAX_BITS 10              // up to 1023 minutes
#define ALARM_REPEATS 10
#define ALARM_INTERVAL_MS 3000

#define KEY_STATE 1
#define KEY_BITS 2
#define KEY_END_TIME 3
#define KEY_REMAINING 4
#define KEY_TOTAL 5

typedef enum {
  STATE_ENTRY = 0,
  STATE_RUNNING,
  STATE_PAUSED,
  STATE_DONE,
} TimerState;

static Window *s_window;
static Layer *s_canvas;

static TimerState s_state = STATE_ENTRY;
static char s_bits[MAX_BITS + 1];
static int s_num_bits;
static time_t s_end_time;       // when RUNNING
static int s_remaining;         // seconds, when PAUSED (and display cache)
static int s_total;             // seconds, for the progress bar

static AppTimer *s_alarm_timer;
static int s_alarm_count;

static const GPathInfo PLAY_INFO = {
  .num_points = 3,
  .points = (GPoint[]) {{-6, -9}, {-6, 9}, {9, 0}},
};
static GPath *s_play_path;

// ---------------------------------------------------------------- helpers

static int bits_value(void) {
  int v = 0;
  for (int i = 0; i < s_num_bits; i++) {
    v = (v << 1) | (s_bits[i] == '1');
  }
  return v;
}

static int seconds_left(void) {
  if (s_state == STATE_RUNNING) {
    int left = (int)(s_end_time - time(NULL));
    return left > 0 ? left : 0;
  }
  return s_remaining;
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed);

static void set_ticking(bool on) {
  if (on) {
    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  } else {
    tick_timer_service_unsubscribe();
  }
}

static void stop_alarm(void) {
  if (s_alarm_timer) {
    app_timer_cancel(s_alarm_timer);
    s_alarm_timer = NULL;
  }
  vibes_cancel();
}

static void alarm_pulse(void *data) {
  s_alarm_timer = NULL;
  static const uint32_t segments[] = {300, 150, 300, 150, 600};
  vibes_enqueue_custom_pattern((VibePattern) {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  });
  light_enable_interaction();
  if (++s_alarm_count < ALARM_REPEATS) {
    s_alarm_timer = app_timer_register(ALARM_INTERVAL_MS, alarm_pulse, NULL);
  }
}

// ---------------------------------------------------------- state changes

static void go_entry(void) {
  stop_alarm();
  set_ticking(false);
  s_state = STATE_ENTRY;
  layer_mark_dirty(s_canvas);
}

static void go_done(void) {
  set_ticking(false);
  s_state = STATE_DONE;
  s_remaining = 0;
  s_alarm_count = 0;
  alarm_pulse(NULL);
  layer_mark_dirty(s_canvas);
}

static void start_timer(int seconds) {
  s_total = seconds;
  s_end_time = time(NULL) + seconds;
  s_state = STATE_RUNNING;
  set_ticking(true);
  layer_mark_dirty(s_canvas);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (s_state == STATE_RUNNING && seconds_left() == 0) {
    go_done();
  } else {
    layer_mark_dirty(s_canvas);
  }
}

// ---------------------------------------------------------------- drawing

static void draw_centered(GContext *ctx, const char *text, const char *font_key,
                          int x, int y, int w, int h) {
  graphics_draw_text(ctx, text, fonts_get_system_font(font_key),
                     GRect(x, y, w, h), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

static void draw_action_bar(GContext *ctx, GRect bar) {
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bar, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, GColorWhite);

  const int cx = bar.origin.x + bar.size.w / 2;
  const int up_y = 30, mid_y = bar.size.h / 2, down_y = bar.size.h - 30;

  switch (s_state) {
    case STATE_ENTRY:
      draw_centered(ctx, "1", FONT_KEY_GOTHIC_28_BOLD, bar.origin.x, up_y - 19, bar.size.w, 34);
      draw_centered(ctx, "0", FONT_KEY_GOTHIC_28_BOLD, bar.origin.x, down_y - 19, bar.size.w, 34);
      if (s_num_bits > 0 && bits_value() > 0) {
        gpath_move_to(s_play_path, GPoint(cx, mid_y));
        gpath_draw_filled(ctx, s_play_path);
      }
      break;
    case STATE_RUNNING:
      graphics_fill_rect(ctx, GRect(cx - 7, mid_y - 9, 5, 18), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx + 2, mid_y - 9, 5, 18), 0, GCornerNone);
      break;
    case STATE_PAUSED:
      gpath_move_to(s_play_path, GPoint(cx, mid_y));
      gpath_draw_filled(ctx, s_play_path);
      break;
    case STATE_DONE:
      draw_centered(ctx, "OK", FONT_KEY_GOTHIC_18_BOLD, bar.origin.x, mid_y - 12, bar.size.w, 24);
      break;
  }
}

static void draw_entry(GContext *ctx, int w, int h) {
  graphics_context_set_text_color(ctx, GColorLightGray);
  draw_centered(ctx, "BINARY TIMER", FONT_KEY_GOTHIC_18_BOLD, 0, 6, w, 22);

  // Bits, in the largest number font that fits.
  graphics_context_set_text_color(ctx, GColorWhite);
  if (s_num_bits == 0) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    draw_centered(ctx, "_", FONT_KEY_LECO_42_NUMBERS, 0, 54, w, 50);
  } else {
    const char *font_key = s_num_bits <= 6 ? FONT_KEY_LECO_36_BOLD_NUMBERS
                                           : FONT_KEY_LECO_20_BOLD_NUMBERS;
    const int y = s_num_bits <= 6 ? 58 : 70;
    draw_centered(ctx, s_bits, font_key, 0, y, w, 50);
  }

  // Decimal equivalent.
  static char value_buf[16];
  int minutes = bits_value();
  snprintf(value_buf, sizeof(value_buf), "= %d min", minutes);
  graphics_context_set_text_color(ctx, minutes > 0 ? GColorPictonBlue : GColorDarkGray);
  draw_centered(ctx, value_buf, FONT_KEY_GOTHIC_28_BOLD, 0, 116, w, 34);

  graphics_context_set_text_color(ctx, GColorLightGray);
  draw_centered(ctx, s_num_bits == 0 ? "UP adds 1\nDOWN adds 0"
                                     : "BACK erases\nhold SELECT clears",
                FONT_KEY_GOTHIC_18, 0, h - 52, w, 46);
}

static void draw_countdown(GContext *ctx, int w, int h) {
  int left = seconds_left();
  static char buf[16];
  int hours = left / 3600, mins = (left / 60) % 60, secs = left % 60;
  if (hours > 0) {
    snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, mins, secs);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
  }

  static const char *labels[] = {
    [STATE_RUNNING] = "RUNNING", [STATE_PAUSED] = "PAUSED", [STATE_DONE] = "TIME'S UP!",
  };
  GColor label_color = s_state == STATE_DONE   ? GColorRed
                     : s_state == STATE_PAUSED ? GColorChromeYellow
                                               : GColorGreen;
  graphics_context_set_text_color(ctx, label_color);
  draw_centered(ctx, labels[s_state], FONT_KEY_GOTHIC_24_BOLD, 0, 44, w, 30);

  graphics_context_set_text_color(ctx, GColorWhite);
  draw_centered(ctx, buf, hours > 0 ? FONT_KEY_LECO_28_LIGHT_NUMBERS : FONT_KEY_LECO_42_NUMBERS,
                0, hours > 0 ? 92 : 84, w, 50);

  // Progress bar (fraction remaining).
  const int bar_x = 12, bar_w = w - 24, bar_y = 148, bar_h = 8;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h), 4, GCornersAll);
  if (s_total > 0 && left > 0) {
    graphics_context_set_fill_color(ctx, label_color);
    graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w * left / s_total, bar_h), 4, GCornersAll);
  }

  static char total_buf[32];
  snprintf(total_buf, sizeof(total_buf), "of %d min (%s)", s_total / 60, s_bits);
  graphics_context_set_text_color(ctx, GColorLightGray);
  draw_centered(ctx, total_buf, FONT_KEY_GOTHIC_18, 0, 162, w, 22);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const int content_w = bounds.size.w - ACTION_BAR_WIDTH;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_state == STATE_ENTRY) {
    draw_entry(ctx, content_w, bounds.size.h);
  } else {
    draw_countdown(ctx, content_w, bounds.size.h);
  }
  draw_action_bar(ctx, GRect(content_w, 0, ACTION_BAR_WIDTH, bounds.size.h));
}

// ----------------------------------------------------------------- clicks

static void add_bit(char bit) {
  if (s_state != STATE_ENTRY || s_num_bits >= MAX_BITS) {
    if (s_state == STATE_ENTRY) vibes_short_pulse();  // full
    return;
  }
  s_bits[s_num_bits++] = bit;
  s_bits[s_num_bits] = '\0';
  layer_mark_dirty(s_canvas);
}

static void up_click(ClickRecognizerRef r, void *ctx) { add_bit('1'); }
static void down_click(ClickRecognizerRef r, void *ctx) { add_bit('0'); }

static void select_click(ClickRecognizerRef r, void *ctx) {
  switch (s_state) {
    case STATE_ENTRY:
      if (bits_value() > 0) start_timer(bits_value() * 60);
      break;
    case STATE_RUNNING:
      s_remaining = seconds_left();
      s_state = STATE_PAUSED;
      set_ticking(false);
      layer_mark_dirty(s_canvas);
      break;
    case STATE_PAUSED:
      s_end_time = time(NULL) + s_remaining;
      s_state = STATE_RUNNING;
      set_ticking(true);
      layer_mark_dirty(s_canvas);
      break;
    case STATE_DONE:
      go_entry();
      break;
  }
}

static void select_long_click(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_ENTRY && s_num_bits > 0) {
    s_num_bits = 0;
    s_bits[0] = '\0';
    vibes_short_pulse();
    layer_mark_dirty(s_canvas);
  }
}

static void back_click(ClickRecognizerRef r, void *ctx) {
  if (s_state != STATE_ENTRY) {
    go_entry();
  } else if (s_num_bits > 0) {
    s_bits[--s_num_bits] = '\0';
    layer_mark_dirty(s_canvas);
  } else {
    window_stack_pop(true);
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// ------------------------------------------------------------- lifecycle

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  s_play_path = gpath_create(&PLAY_INFO);
}

static void window_unload(Window *window) {
  gpath_destroy(s_play_path);
  layer_destroy(s_canvas);
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  // Fired while the app is open; the tick handler normally beats it to it.
  if (s_state == STATE_RUNNING) go_done();
}

static void restore_state(void) {
  if (persist_exists(KEY_BITS)) {
    persist_read_string(KEY_BITS, s_bits, sizeof(s_bits));
    s_num_bits = strlen(s_bits);
  }
  TimerState saved = persist_read_int(KEY_STATE);
  s_total = persist_read_int(KEY_TOTAL);
  if (saved == STATE_RUNNING) {
    s_end_time = persist_read_int(KEY_END_TIME);
    if (s_end_time > time(NULL)) {
      s_state = STATE_RUNNING;
      set_ticking(true);
    } else {
      go_done();
    }
  } else if (saved == STATE_PAUSED) {
    s_remaining = persist_read_int(KEY_REMAINING);
    s_state = STATE_PAUSED;
  }
}

static void save_state(void) {
  persist_write_string(KEY_BITS, s_bits);
  persist_write_int(KEY_STATE, s_state == STATE_DONE ? STATE_ENTRY : s_state);
  persist_write_int(KEY_END_TIME, (int)s_end_time);
  persist_write_int(KEY_REMAINING, s_remaining);
  persist_write_int(KEY_TOTAL, s_total);

  // Leaving while running: have the system relaunch us when time is up.
  if (s_state == STATE_RUNNING) {
    wakeup_schedule(s_end_time, 0, true);
  }
}

static void init(void) {
  wakeup_cancel_all();
  wakeup_service_subscribe(wakeup_handler);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  restore_state();
}

static void deinit(void) {
  stop_alarm();
  save_state();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
