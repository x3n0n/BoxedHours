#include <pebble.h>

// draws a clock in the style of TTMMIT (12 boxes in a square, filled to indicate the current hour).

static const uint8_t HOUR_BOX_LOC[] = { // y,x grid locations, each as a nibble|nibble byte
  0x02, 0x03, 0x13, 0x23, 0x33, 0x32, 0x31, 0x30, 0x20, 0x10, 0x00, 0x01
};
static inline uint8_t hour_row(uint8_t i) { return HOUR_BOX_LOC[i] >> 4; }
static inline uint8_t hour_col(uint8_t i) { return HOUR_BOX_LOC[i] & 0xF; }

enum { VIBE_START_HOUR = 7, VIBE_LAST_HOUR = 21 };
static inline bool allow_vibration(const struct tm *now) {
  return VIBE_START_HOUR <= now->tm_hour && now->tm_hour <= VIBE_LAST_HOUR;
}

static const char
  *MINUTES_FONT_NAME = FONT_KEY_LECO_42_NUMBERS,
  *DATE_FONT_NAME = FONT_KEY_GOTHIC_28_BOLD,
  *HOUR_FONT_NAME = FONT_KEY_LECO_20_BOLD_NUMBERS;

enum {
  HOURS = ARRAY_LENGTH(HOUR_BOX_LOC),
  COLS = 4,
  HOUR_DOT_COLS = 4,
  
  DOUBLE_TAP_MS = 500
};

static GColor s_bg, s_fg;

static Window *s_window;

static uint8_t s_dot_wh;

static Layer *s_hour_dots[HOURS]; // fill bounds with bg, then fill dots with fg

static bool s_show_hour_names;
static char s_hour_name[HOURS][3]; // update on the fly
static TextLayer *s_hour[HOURS]; // hidden when not "active"

static char s_minutes_text[] = "00";
static TextLayer *s_minutes; // clock minutes

static const char *s_bt_text = "\U0001F494"; // broken heart
static TextLayer *s_bt;

static const char *DAY_NAME[] = { "Su", "M ", "T ", "W ", "R ", "F ", "Sa" };
static char s_date_text[] = "Su30";
static TextLayer *s_date; // 2-char weekday followed by 0-padded month-day

#if PBL_HEALTH
static bool s_do_heart_rate;
static char s_health_text[] = "199\U0001F49F 00000"; // heart decoration
static TextLayer *s_health; // heart rate (if avail), followed by steps
#endif

static time_t s_last_tap_s;
static uint16_t s_last_tap_ms;
static uint8_t s_current_taps;


static void render_dots(Layer *l, GContext *ctx) {
  const GRect b = layer_get_bounds(l);
  
  graphics_context_set_fill_color(ctx, s_bg);
  graphics_fill_rect(ctx, b, 0, 0);
  
  graphics_context_set_fill_color(ctx, s_fg);
  for (uint16_t x = b.origin.x; x < b.origin.x + b.size.w; x += 2 * s_dot_wh)
    for (uint16_t y = b.origin.x; y < b.origin.y + b.size.h; y += 2 * s_dot_wh)
      graphics_fill_rect(ctx, GRect(x, y, s_dot_wh, s_dot_wh), 0, 0);
}

static void load_main_window(Window *window) {
  window_set_background_color(window, s_bg);

  Layer *wl = window_get_root_layer(window);
  const GRect wb = layer_get_bounds(wl);
  const uint16_t w = wb.size.w;
  
  // The empty clock is a field of (COLS * HOUR_DOT_COLS) dots in each direction,
  // centered horizontally and top-aligned (with the same minimal margin).
  // The space between adjacent dots (and between "HOUR" boxes) is the same width as a dot.
  uint16_t clock_center_xy = w / 2;
  const uint8_t margin = PBL_IF_ROUND_ELSE(w * 20 / 200, 0); // so the square fits
  const uint16_t total_dot_cols = COLS * (HOUR_DOT_COLS * 2) - 1;
  
  s_dot_wh = (w - (2 * margin)) / total_dot_cols;
  const uint8_t hour_wh = s_dot_wh * (HOUR_DOT_COLS * 2 - 1);
  
  uint16_t clock_wh = COLS * hour_wh + (COLS - 1) * s_dot_wh;
  uint16_t clock_xy = clock_center_xy - clock_wh / 2;
  
  GFont hour_font = fonts_get_system_font(HOUR_FONT_NAME);
  for (int i = 0; i < HOURS; ++i) {
    uint16_t col = hour_col(i);
    
    uint16_t x = clock_xy + col * (hour_wh + s_dot_wh);
    uint16_t y = clock_xy + hour_row(i) * (hour_wh + s_dot_wh);
    
    const GRect b = GRect(x, y, hour_wh, hour_wh);

    Layer *dots = layer_create(b);
    layer_set_update_proc(dots, render_dots);
    layer_add_child(wl, dots);
    s_hour_dots[i] = dots;
    
    TextLayer *hour = text_layer_create(b);
    text_layer_set_background_color(hour, s_fg);
    text_layer_set_text_color(hour, s_bg);
    text_layer_set_font(hour, hour_font);
    text_layer_set_text_alignment(
      hour, col == 0 ? GTextAlignmentLeft : col == COLS - 1 ? GTextAlignmentRight : GTextAlignmentCenter);
    text_layer_set_text(hour, s_hour_name[i]);
    layer_add_child(wl, text_layer_get_layer(hour));
    layer_set_hidden(text_layer_get_layer(hour), true);
    s_hour[i] = hour;
  }

  GFont minutes_font = fonts_get_system_font(MINUTES_FONT_NAME);
  uint16_t minutes_h = graphics_text_layout_get_content_size(
    s_minutes_text, minutes_font, wb, GTextOverflowModeWordWrap, GTextAlignmentCenter).h;
  s_minutes = text_layer_create(GRect(0, clock_center_xy - minutes_h * 2 / 3, w, minutes_h));
  text_layer_set_background_color(s_minutes, GColorClear);
  text_layer_set_text_color(s_minutes, s_fg);
  text_layer_set_text(s_minutes, s_minutes_text);
  text_layer_set_font(s_minutes, minutes_font);
  text_layer_set_text_alignment(s_minutes, GTextAlignmentCenter);
  layer_add_child(wl, text_layer_get_layer(s_minutes));

  // bluetooth disconnected
  s_bt = text_layer_create(GRect(clock_center_xy - hour_wh, clock_center_xy - hour_wh - 7, hour_wh, hour_wh));
  text_layer_set_background_color(s_bt, GColorClear);
  text_layer_set_text_color(s_bt, s_fg);
  text_layer_set_text(s_bt, s_bt_text);
  if (w > 180)
    text_layer_set_font(s_bt, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_set_hidden(text_layer_get_layer(s_bt), true);
  layer_add_child(wl, text_layer_get_layer(s_bt));

  // date setup
  GFont date_font = fonts_get_system_font(DATE_FONT_NAME);
  uint16_t date_h = graphics_text_layout_get_content_size(
    s_date_text, date_font, wb, GTextOverflowModeWordWrap, GTextAlignmentCenter).h;

  s_date = text_layer_create(GRect(0, wb.size.h - date_h - PBL_IF_ROUND_ELSE(5, 0), w, date_h));
  text_layer_set_background_color(s_date, GColorClear);
  text_layer_set_text_color(s_date, s_fg);
  text_layer_set_text(s_date, s_date_text);
  text_layer_set_font(s_date, date_font);
  text_layer_set_text_alignment(s_date, PBL_IF_RECT_ELSE(GTextAlignmentLeft, GTextAlignmentCenter));
  layer_add_child(wl, text_layer_get_layer(s_date));

#if PBL_HEALTH
  s_do_heart_rate = health_service_metric_accessible(HealthMetricHeartRateBPM, time(NULL), time(NULL));

  const char *health_font_name =
    PBL_IF_ROUND_ELSE(true, wb.size.w <= 144) ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_28_BOLD;
  GFont health_font = fonts_get_system_font(health_font_name);
  uint16_t health_h = graphics_text_layout_get_content_size(
    s_health_text, health_font, wb, GTextOverflowModeWordWrap, GTextAlignmentCenter).h;

  s_health = text_layer_create(GRect(0, PBL_IF_ROUND_ELSE(0, wb.size.h - health_h), w, health_h));

  text_layer_set_background_color(s_health, GColorClear);
  text_layer_set_text_color(s_health, s_fg);
  text_layer_set_text(s_health, s_health_text);
  text_layer_set_font(s_health, health_font);
  text_layer_set_text_alignment(s_health, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentRight));

  layer_add_child(wl, text_layer_get_layer(s_health));
#endif
}

static void unload_main_window(Window *w) {
#if PBL_HEALTH
  text_layer_destroy(s_health);
#endif
  text_layer_destroy(s_date);
  text_layer_destroy(s_bt);
  text_layer_destroy(s_minutes);
  for (int i = 0; i < HOURS; ++i) {
    text_layer_destroy(s_hour[i]);
    layer_destroy(s_hour_dots[i]);
  }
}

static void update(struct tm *now, TimeUnits units_changed) {
  if (units_changed & DAY_UNIT) {
    snprintf(s_date_text, sizeof(s_date_text), "%s%02d", DAY_NAME[now->tm_wday], now->tm_mday);
    text_layer_set_text(s_date, s_date_text);
  }

  char hour_2_first_digit = s_hour_name[1][0];
  bool showing_hour_names = hour_2_first_digit != '\0';
  
  if (s_show_hour_names != showing_hour_names || (units_changed & HOUR_UNIT)) {
    bool morning = (1 <= now->tm_hour && now->tm_hour <= HOURS);
    char expected_hour_2_first_digit = !s_show_hour_names ? '\0' : morning ? '2' : '1'; // "14"
    
    if (expected_hour_2_first_digit != hour_2_first_digit) {
      for (int i = 0; i < HOURS; ++i) {
        if (s_show_hour_names)
          snprintf(s_hour_name[i], sizeof(s_hour_name[i]), "%d", ((HOURS * !morning) + i + 1) % 24);
        else
          s_hour_name[i][0] = '\0';
        text_layer_set_text(s_hour[i], s_hour_name[i]);
      }
    }

    int8_t last_on_hour = (now->tm_hour + (HOURS - 1)) % HOURS; // 0 -> 11, 1 -> 0, etc
    for (int i = 0; i < HOURS; ++i) {
      bool hide = i > last_on_hour;
      Layer *l = text_layer_get_layer(s_hour[i]);
      if (layer_get_hidden(l) != hide)
        layer_set_hidden(l, hide);
    }
  }
  
  if (units_changed & MINUTE_UNIT) {
    strftime(s_minutes_text, sizeof(s_minutes_text), "%M", now);
    text_layer_set_text(s_minutes, s_minutes_text);
  }

  bool connected = connection_service_peek_pebble_app_connection();
  Layer *bt = text_layer_get_layer(s_bt);
  if (connected != layer_get_hidden(bt))
    layer_set_hidden(bt, connected);

#if PBL_HEALTH
  if (s_health != NULL) {
    int n = 0;
    if (s_do_heart_rate)
      n = snprintf(s_health_text, sizeof(s_health_text), "%ld\U0001F49F ",
                   (long)health_service_peek_current_value(HealthMetricHeartRateBPM));
    snprintf(s_health_text + n, sizeof(s_health_text) - n, "%ld",
             (long)health_service_sum_today(HealthMetricStepCount));
    text_layer_set_text(s_health, s_health_text);
  }
#endif
}

static time_t update_now(TimeUnits units_to_check) {
  time_t now = time(NULL);
  update(localtime(&now), units_to_check);
  return now;
}

static void tick(struct tm *now, TimeUnits changed_units) {
  update(now, changed_units);

  if ((changed_units & HOUR_UNIT) && allow_vibration(now))
    vibes_short_pulse();
}

static void tap(AccelAxisType axis, int32_t direction) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  
  bool expired = 1000 * (s - s_last_tap_s) + (ms - s_last_tap_ms) > DOUBLE_TAP_MS;
  
  if (!expired) {
    ++s_current_taps;
    
    if (s_current_taps > 2) {
      s_show_hour_names ^= 1;
      expired = true;
    }
  }
  
  if (expired) {
    s_last_tap_s = s;
    s_last_tap_ms = ms;
    
    s_current_taps = 1;
  }
  
  update_now(0);
}

static void connection_changed(bool connected) {
  time_t now = update_now(0);
  
  if (!connected && allow_vibration(localtime(&now)))
    vibes_long_pulse();
}

static void init() {
  s_bg = PBL_IF_BW_ELSE(GColorBlack, GColorDarkCandyAppleRed);
  s_fg = GColorWhite;

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = load_main_window,
    .unload = unload_main_window,
  });
  window_stack_push(s_window, true);

  update_now(DAY_UNIT | HOUR_UNIT | MINUTE_UNIT);

  tick_timer_service_subscribe(MINUTE_UNIT, tick);
  accel_tap_service_subscribe(tap);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_changed,
  });
}

static void deinit() {
  connection_service_unsubscribe();
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();

  window_destroy(s_window);
}

int main() {
  init();
  app_event_loop();
  deinit();
}