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
//
// Layout is proportional to the screen so the same code fits every Pebble:
// 144x168 (aplite, basalt, diorite, flint), 200x228 (emery), and the round
// 180x180 (chalk) and 260x260 (gabbro).

// Colors, with fallbacks for black-and-white watches.
#define COLOR_MUTED     COLOR_FALLBACK(GColorLightGray, GColorWhite)
#define COLOR_DISABLED  COLOR_FALLBACK(GColorDarkGray, GColorWhite)
#define COLOR_VALUE     COLOR_FALLBACK(GColorPictonBlue, GColorWhite)
#define COLOR_BAR_BG    COLOR_FALLBACK(GColorDarkGray, GColorWhite)
#define COLOR_ICON      PBL_IF_ROUND_ELSE(GColorWhite, COLOR_FALLBACK(GColorWhite, GColorBlack))
#define COLOR_TRACK     COLOR_FALLBACK(GColorDarkGray, GColorBlack)

static const char *const BITS_FONTS[] = {
  FONT_KEY_LECO_36_BOLD_NUMBERS, FONT_KEY_LECO_28_LIGHT_NUMBERS,
  FONT_KEY_LECO_20_BOLD_NUMBERS, FONT_KEY_GOTHIC_18_BOLD,
};
static const char *const CLOCK_FONTS[] = {
  FONT_KEY_LECO_42_NUMBERS, FONT_KEY_LECO_36_BOLD_NUMBERS, FONT_KEY_LECO_28_LIGHT_NUMBERS,
  FONT_KEY_LECO_20_BOLD_NUMBERS,
};
static const char *const LARGE_FONTS[] = {
  FONT_KEY_GOTHIC_28_BOLD, FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_18_BOLD,
};
static const char *const LABEL_FONTS[] = {
  FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_18_BOLD,
};
static const char *const SMALL_FONTS[] = {
  FONT_KEY_GOTHIC_18, FONT_KEY_GOTHIC_14,
};

// Draws text centered horizontally in [x, x + w) and vertically on y_center,
// in the first of `keys` that fits the width.
static void draw_fitted(GContext *ctx, const char *text, const char *const *keys, int n,
                        int x, int w, int y_center) {
  GFont font = NULL;
  GSize size = GSizeZero;
  for (int i = 0; i < n; i++) {
    font = fonts_get_system_font(keys[i]);
    size = graphics_text_layout_get_content_size(text, font, GRect(0, 0, 1000, 1000),
                                                 GTextOverflowModeWordWrap,
                                                 GTextAlignmentCenter);
    if (size.w <= w) break;
  }
  graphics_draw_text(ctx, text, font, GRect(x, y_center - size.h / 2 - 4, w, size.h + 8),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

#define DRAW_FITTED(ctx, text, fonts, x, w, y) \
  draw_fitted(ctx, text, fonts, ARRAY_LENGTH(fonts), x, w, y)

static void draw_label(GContext *ctx, const char *text, GPoint center) {
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(center.x - 15, center.y - 20, 30, 34), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

static void draw_play(GContext *ctx, GPoint center) {
  gpath_move_to(s_play_path, center);
  gpath_draw_filled(ctx, s_play_path);
}

// Button hints next to UP, SELECT and DOWN: a strip on rectangular watches,
// free-floating labels just inside the edge on round ones.
static void draw_button_hints(GContext *ctx, GRect bounds) {
  const int w = bounds.size.w, h = bounds.size.h;
#if defined(PBL_ROUND)
  const int r = w / 2;
  const GPoint up = GPoint(w / 2 + r * 60 / 100, h / 2 - r * 50 / 100);
  const GPoint select = GPoint(w / 2 + r * 80 / 100, h / 2);
  const GPoint down = GPoint(up.x, h - up.y);
#else
  graphics_context_set_fill_color(ctx, COLOR_BAR_BG);
  graphics_fill_rect(ctx, GRect(w - ACTION_BAR_WIDTH, 0, ACTION_BAR_WIDTH, h), 0, GCornerNone);
  const int cx = w - ACTION_BAR_WIDTH / 2;
  const GPoint up = GPoint(cx, h * 13 / 100);
  const GPoint select = GPoint(cx, h / 2);
  const GPoint down = GPoint(cx, h - up.y);
#endif
  graphics_context_set_text_color(ctx, COLOR_ICON);
  graphics_context_set_fill_color(ctx, COLOR_ICON);

  switch (s_state) {
    case STATE_ENTRY:
      draw_label(ctx, "1", up);
      draw_label(ctx, "0", down);
      if (bits_value() > 0) draw_play(ctx, select);
      break;
    case STATE_RUNNING:
      graphics_fill_rect(ctx, GRect(select.x - 7, select.y - 9, 5, 18), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(select.x + 2, select.y - 9, 5, 18), 0, GCornerNone);
      break;
    case STATE_PAUSED:
      draw_play(ctx, select);
      break;
    case STATE_DONE:
      graphics_draw_text(ctx, "OK", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                         GRect(select.x - 15, select.y - 12, 30, 24), GTextOverflowModeWordWrap,
                         GTextAlignmentCenter, NULL);
      break;
  }
}

static void draw_entry(GContext *ctx, int x, int w, int h, int screen_w) {
  graphics_context_set_text_color(ctx, COLOR_MUTED);
#if defined(PBL_ROUND)
  // Full width and above the "1" hint, which sits at the column's top right.
  graphics_draw_text(ctx, "BINARY TIMER", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, h * 10 / 100, screen_w, 18), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
#else
  DRAW_FITTED(ctx, "BINARY TIMER", SMALL_FONTS, x, w, h * 7 / 100);
#endif

  if (s_num_bits == 0) {
    graphics_context_set_text_color(ctx, COLOR_DISABLED);
    DRAW_FITTED(ctx, "_", BITS_FONTS, x, w, h * 36 / 100);
  } else {
    graphics_context_set_text_color(ctx, GColorWhite);
    DRAW_FITTED(ctx, s_bits, BITS_FONTS, x, w, h * 36 / 100);
  }

  static char value_buf[16];
  int minutes = bits_value();
  snprintf(value_buf, sizeof(value_buf), "= %d min", minutes);
  graphics_context_set_text_color(ctx, minutes > 0 ? COLOR_VALUE : COLOR_DISABLED);
  DRAW_FITTED(ctx, value_buf, LARGE_FONTS, x, w, h * 56 / 100);

  graphics_context_set_text_color(ctx, COLOR_MUTED);
  const char *hint = s_num_bits == 0 ? "UP adds 1\nDOWN adds 0" : "BACK erases\nhold SELECT clears";
  graphics_draw_text(ctx, hint,
                     fonts_get_system_font(PBL_IF_ROUND_ELSE(FONT_KEY_GOTHIC_14, FONT_KEY_GOTHIC_18)),
                     GRect(x, h * PBL_IF_ROUND_ELSE(70, 72) / 100, w, h / 4),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_countdown(GContext *ctx, int x, int w, int h) {
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
  GColor label_color = s_state == STATE_DONE   ? COLOR_FALLBACK(GColorRed, GColorWhite)
                     : s_state == STATE_PAUSED ? COLOR_FALLBACK(GColorChromeYellow, GColorWhite)
                                               : COLOR_FALLBACK(GColorGreen, GColorWhite);
  graphics_context_set_text_color(ctx, label_color);
  DRAW_FITTED(ctx, labels[s_state], LABEL_FONTS, x, w, h * 25 / 100);

  graphics_context_set_text_color(ctx, GColorWhite);
  DRAW_FITTED(ctx, buf, CLOCK_FONTS, x, w, h * 45 / 100);

  // Progress bar (fraction remaining).
  const GRect track = GRect(x + 12, h * 64 / 100, w - 24, 8);
  graphics_context_set_fill_color(ctx, COLOR_TRACK);
  graphics_fill_rect(ctx, track, 4, GCornersAll);
#if !defined(PBL_COLOR)
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_round_rect(ctx, track, 4);
#endif
  if (s_total > 0 && left > 0) {
    graphics_context_set_fill_color(ctx, label_color);
    graphics_fill_rect(ctx, GRect(track.origin.x, track.origin.y, track.size.w * left / s_total,
                                  track.size.h), 4, GCornersAll);
  }

  static char total_buf[32];
  snprintf(total_buf, sizeof(total_buf), "of %d min (%s)", s_total / 60, s_bits);
  graphics_context_set_text_color(ctx, COLOR_MUTED);
  DRAW_FITTED(ctx, total_buf, SMALL_FONTS, x, w, h * 75 / 100);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const int h = bounds.size.h;
  // Content column, clear of the button hints.
#if defined(PBL_ROUND)
  const int x = bounds.size.w / 8, w = bounds.size.w * 68 / 100;
#else
  const int x = 0, w = bounds.size.w - ACTION_BAR_WIDTH;
#endif

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_state == STATE_ENTRY) {
    draw_entry(ctx, x, w, h, bounds.size.w);
  } else {
    draw_countdown(ctx, x, w, h);
  }
  draw_button_hints(ctx, bounds);
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
