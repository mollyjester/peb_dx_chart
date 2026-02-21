#include <pebble.h>

/* ---------------------------------------------------------------------------
 * Configuration constants
 * --------------------------------------------------------------------------- */
#define MAX_READINGS      36
#define CHART_START_X      30   /* Left margin for time labels */
#define CHART_START_Y      10   /* Top margin for value labels */
#define CHART_WIDTH       114   /* 144 - 30 */
#define CHART_HEIGHT      134   /* 148 - 14; leaves 14 px at bottom for glucose-axis labels */
#define TIME_SPACING        4   /* Pixels between readings vertically */

/* Dotted-line pattern: draw DOT_ON pixels, skip DOT_OFF pixels */
#define DOT_ON              2
#define DOT_OFF             3
#define DOT_PERIOD          (DOT_ON + DOT_OFF)

/* Time-grid interval in readings (6 readings = 30 minutes) */
#define TIME_GRID_INTERVAL  6

/* Maximum gap between consecutive readings (in seconds) before
   breaking the glucose line.  Two missed 5-minute readings → 10 min. */
#define MAX_GAP_SECONDS   600

/* Padding inside the chart area so edge data points are not clipped */
#define GRID_PADDING        2

/* Message buffer sizes */
#define APPMESSAGE_INBOX  2048
#define APPMESSAGE_OUTBOX  128

/* Bytes per reading in bulk transfer */
#define BYTES_PER_READING   6

/* ---------------------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------------------- */
static Window    *s_main_window;
static Layer     *s_chart_layer;
static TextLayer *s_status_layer;

typedef struct {
    int16_t value;      /* BG value x10 for mmol/L precision (e.g. 123 mg/dL = 1230) */
    time_t  timestamp;
} GlucoseReading;

static GlucoseReading s_readings[MAX_READINGS];
static int  s_reading_count   = 0;
static int  s_expected_count  = 0;
static int  s_received_count  = 0;
static bool s_receiving_data  = false;
static bool s_is_mmol         = false;
static char s_bg_units[10]    = "mg/dL";
static char s_status_text[32] = "Loading...";

/* Forward declarations */
static void update_chart(void);
static void request_data(void);

/* ---------------------------------------------------------------------------
 * Chart-drawing helpers
 * --------------------------------------------------------------------------- */

/** Map a BG value to an x-pixel coordinate within the padded chart area. */
static int bg_to_x(int bg_value, int min_bg, int bg_range) {
    int usable = CHART_WIDTH - 2 * GRID_PADDING;
    return CHART_START_X + GRID_PADDING +
           ((bg_value - min_bg) * usable) / bg_range;
}

/** Map a timestamp to a y-pixel coordinate (now = bottom, older = higher). */
static int timestamp_to_y(time_t ts, time_t now) {
    int seconds_ago = (int)(now - ts);
    /* 5 minutes (300 s) = TIME_SPACING pixels */
    int pixel_offset = (seconds_ago * TIME_SPACING) / 300;
    return CHART_START_Y + CHART_HEIGHT - GRID_PADDING - pixel_offset;
}

/** Clamp an x value to the padded chart area. */
static int clamp_x(int x) {
    int lo = CHART_START_X + GRID_PADDING;
    int hi = CHART_START_X + CHART_WIDTH - GRID_PADDING;
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/** Return true when x falls inside the padded chart area. */
static bool x_in_bounds(int x) {
    return x >= CHART_START_X + GRID_PADDING &&
           x <= CHART_START_X + CHART_WIDTH - GRID_PADDING;
}

/**
 * Draw a dotted vertical line from y_start to y_end at the given x.
 * Pattern: DOT_ON pixels drawn, DOT_OFF pixels skipped, repeating.
 */
static void draw_dotted_vline(GContext *ctx, int x, int y_start, int y_end) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    for (int y = y_start; y <= y_end; y++) {
        if ((y - y_start) % DOT_PERIOD < DOT_ON) {
            graphics_draw_pixel(ctx, GPoint(x, y));
        }
    }
}

/**
 * Draw a solid vertical line from y_start to y_end at the given x.
 */
static void draw_solid_vline(GContext *ctx, int x, int y_start, int y_end) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(x, y_start), GPoint(x, y_end));
}

/**
 * Draw a dotted horizontal line from x_start to x_end at the given y.
 * Pattern: DOT_ON pixels drawn, DOT_OFF pixels skipped, repeating.
 */
static void draw_dotted_hline(GContext *ctx, int y, int x_start, int x_end) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    for (int x = x_start; x <= x_end; x++) {
        if ((x - x_start) % DOT_PERIOD < DOT_ON) {
            graphics_draw_pixel(ctx, GPoint(x, y));
        }
    }
}

/**
 * Draw a grid line label at the bottom of the chart.
 */
static void draw_grid_label(GContext *ctx, int bg, int min_bg, int bg_range) {
    int x = bg_to_x(bg, min_bg, bg_range);
    if (!x_in_bounds(x)) return;

    static char label[8];
    if (s_is_mmol) {
        snprintf(label, sizeof(label), "%d.%d", bg / 10, bg % 10);
    } else {
        snprintf(label, sizeof(label), "%d", bg);
    }

    int label_y = CHART_START_Y + CHART_HEIGHT;
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, label,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(x - 15, label_y, 30, 14),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
}

/**
 * Draw the vertical value-reference grid lines with labels at the bottom.
 * Uses 5 fixed values: 0, 4, 10, 15, 20 (mmol/L) or 0, 72, 180, 270, 360 (mg/dL).
 * The clinical thresholds at 4.0 mmol/L (72 mg/dL) and 10.0 mmol/L (180 mg/dL)
 * are drawn as solid lines.
 */
static void draw_value_grid(GContext *ctx, int min_bg, int bg_range) {
    /* Fixed grid values */
    static const int mmol_grid[] = {0, 40, 100, 150, 200};
    static const int mgdl_grid[] = {0, 72, 180, 270, 360};
    const int *grid = s_is_mmol ? mmol_grid : mgdl_grid;
    int threshold_lo = s_is_mmol ?  40 :  72;  /* 4.0 mmol/L or  72 mg/dL */
    int threshold_hi = s_is_mmol ? 100 : 180;  /* 10.0 mmol/L or 180 mg/dL */

    for (int i = 0; i < 5; i++) {
        int x = bg_to_x(grid[i], min_bg, bg_range);
        if (!x_in_bounds(x)) continue;

        if (grid[i] == threshold_lo || grid[i] == threshold_hi) {
            draw_solid_vline(ctx, x, CHART_START_Y + GRID_PADDING,
                             CHART_START_Y + CHART_HEIGHT - GRID_PADDING);
        } else {
            draw_dotted_vline(ctx, x, CHART_START_Y + GRID_PADDING,
                              CHART_START_Y + CHART_HEIGHT - GRID_PADDING);
        }
        draw_grid_label(ctx, grid[i], min_bg, bg_range);
    }
}

/**
 * Draw the horizontal time-grid lines with labels on the left.
 * Lines are drawn at fixed 30-minute intervals from "now".
 */
static void draw_time_grid(GContext *ctx, time_t now) {
    for (int slot = 0; slot <= MAX_READINGS; slot += TIME_GRID_INTERVAL) {
        int minutes_ago = slot * 5;
        int y = timestamp_to_y(now - minutes_ago * 60, now);
        if (y < CHART_START_Y || y > CHART_START_Y + CHART_HEIGHT) continue;

        /* Dotted horizontal grid line */
        draw_dotted_hline(ctx, y, CHART_START_X + GRID_PADDING,
                          CHART_START_X + CHART_WIDTH - GRID_PADDING);

        /* Time label – skip index 0 because the glucose-axis "0" already
           occupies the bottom-left corner (origin of both axes). */
        if (minutes_ago == 0) continue;
        static char time_label[8];
        if (minutes_ago == 30) {
            snprintf(time_label, sizeof(time_label), "30m");
        } else if (minutes_ago % 60 == 0) {
            snprintf(time_label, sizeof(time_label), "%dh", minutes_ago / 60);
        } else {
            snprintf(time_label, sizeof(time_label), "%d.5h", minutes_ago / 60);
        }
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, time_label,
                           fonts_get_system_font(FONT_KEY_GOTHIC_14),
                           GRect(0, y - 7, 28, 14),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
    }
}

/**
 * Draw the glucose line graph (line segments + data-point dots).
 * Uses real timestamps for vertical positioning so gaps in readings
 * (beginning, middle, or end) are rendered correctly.
 * Line segments are omitted across gaps larger than MAX_GAP_SECONDS.
 */
static void draw_glucose_line(GContext *ctx, int min_bg, int bg_range,
                              time_t now) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);

    for (int i = 0; i < s_reading_count; i++) {
        int x = clamp_x(bg_to_x(s_readings[i].value, min_bg, bg_range));
        int y = timestamp_to_y(s_readings[i].timestamp, now);

        /* Draw line segment to the next (older) reading unless there is a
           gap larger than MAX_GAP_SECONDS between them. */
        if (i < s_reading_count - 1) {
            int gap = (int)(s_readings[i].timestamp -
                            s_readings[i + 1].timestamp);
            if (gap <= MAX_GAP_SECONDS) {
                int x2 = clamp_x(bg_to_x(s_readings[i + 1].value,
                                          min_bg, bg_range));
                int y2 = timestamp_to_y(s_readings[i + 1].timestamp, now);
                graphics_draw_line(ctx, GPoint(x, y), GPoint(x2, y2));
            }
        }

        /* Draw data-point dot (only if inside the visible chart area) */
        if (y >= CHART_START_Y && y <= CHART_START_Y + CHART_HEIGHT) {
            graphics_context_set_fill_color(ctx, GColorBlack);
            graphics_fill_circle(ctx, GPoint(x, y), 2);
        }
    }
}

/**
 * Draw numerical labels at the extremum (min / max) glucose points.
 *
 * Labels are placed on the "empty" side of the chart relative to the data
 * point: the min label toward lower values, the max label toward higher
 * values.
 * When the two labels are close together vertically they are pushed apart.
 */
static void draw_extremum_labels(GContext *ctx, int min_bg, int bg_range,
                                 time_t now) {
    if (s_reading_count < 1) return;

    int min_val = s_readings[0].value;
    int max_val = s_readings[0].value;
    int min_idx = 0;
    int max_idx = 0;

    for (int i = 1; i < s_reading_count; i++) {
        if (s_readings[i].value < min_val) {
            min_val = s_readings[i].value;
            min_idx = i;
        }
        if (s_readings[i].value > max_val) {
            max_val = s_readings[i].value;
            max_idx = i;
        }
    }

    /* Format value string – mmol/L uses one decimal place */
    static char min_label[8];
    static char max_label[8];

    if (s_is_mmol) {
        snprintf(min_label, sizeof(min_label), "%d.%d", min_val / 10, min_val % 10);
        snprintf(max_label, sizeof(max_label), "%d.%d", max_val / 10, max_val % 10);
    } else {
        snprintf(min_label, sizeof(min_label), "%d", min_val);
        snprintf(max_label, sizeof(max_label), "%d", max_val);
    }

    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    int label_w = 30;
    int label_h = 16;
    int right_edge = CHART_START_X + CHART_WIDTH;

    /* --- helper: compute label x so label sits on the empty side --- */
    /* For the minimum value the empty space is toward lower BG values;
       for the maximum it is toward higher BG values.
       Lower BG maps to left, higher to right. */
    #define OFFSET 4  /* gap between data point and label edge */

    /* --- minimum label position --- */
    int min_px = clamp_x(bg_to_x(min_val, min_bg, bg_range));
    int min_py = timestamp_to_y(s_readings[min_idx].timestamp, now);
    int min_lx, min_ly;

    /* Place min label toward lower-value side (left) */
    min_lx = min_px - label_w - OFFSET;
    if (min_lx < CHART_START_X) min_lx = min_px + OFFSET;
    if (min_lx + label_w > right_edge) min_lx = right_edge - label_w;
    min_ly = min_py - label_h / 2;

    /* --- maximum label position --- */
    int max_px = clamp_x(bg_to_x(max_val, min_bg, bg_range));
    int max_py = timestamp_to_y(s_readings[max_idx].timestamp, now);
    int max_lx, max_ly;

    /* Place max label toward higher-value side (right) */
    max_lx = max_px + OFFSET;
    if (max_lx + label_w > right_edge) max_lx = max_px - label_w - OFFSET;
    if (max_lx < CHART_START_X) max_lx = CHART_START_X;
    max_ly = max_py - label_h / 2;

    #undef OFFSET

    /* --- push labels apart when they overlap vertically --- */
    {
        int min_top = min_ly;
        int min_bot = min_ly + label_h;
        int max_top = max_ly;
        int max_bot = max_ly + label_h;

        /* Check if the label rectangles overlap vertically */
        if (min_top < max_bot && max_top < min_bot) {
            int lower_top = (min_top > max_top) ? min_top : max_top;
            int upper_bot = (min_bot < max_bot) ? min_bot : max_bot;
            int overlap  = upper_bot - lower_top;
            int half = (overlap + 1) / 2;

            /* Push the label whose data point is higher on screen upward,
               and the other one downward */
            if (min_py < max_py) {
                min_ly -= half;
                max_ly += half;
            } else {
                max_ly -= half;
                min_ly += half;
            }
        }
    }

    /* Clamp both labels to the visible chart area (below grid labels) */
    int top_limit = CHART_START_Y;  /* avoid overlapping grid labels at top */
    int bot_limit = CHART_START_Y + CHART_HEIGHT - label_h;

    if (min_ly < top_limit) min_ly = top_limit;
    if (min_ly > bot_limit) min_ly = bot_limit;
    if (max_ly < top_limit) max_ly = top_limit;
    if (max_ly > bot_limit) max_ly = bot_limit;

    /* --- draw hollow dots at extremum points --- */
    /* Draw hollow extremum dot: black fill radius 6, white fill radius 2 */
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(min_px, min_py), 6);
    graphics_fill_circle(ctx, GPoint(max_px, max_py), 6);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(min_px, min_py), 2);
    graphics_fill_circle(ctx, GPoint(max_px, max_py), 2);

    /* --- draw minimum label --- */
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, min_label, font,
                       GRect(min_lx, min_ly, label_w, label_h),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);

    /* --- draw maximum label --- */
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, max_label, font,
                       GRect(max_lx, max_ly, label_w, label_h),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
}

/**
 * Show a centred status message when no data is available.
 */
static void draw_no_data_message(GContext *ctx) {
    graphics_context_set_text_color(ctx, GColorBlack);
    if (s_receiving_data) {
        graphics_draw_text(ctx, "Loading...",
                           fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                           GRect(0, 60, 144, 30),
                           GTextOverflowModeWordWrap,
                           GTextAlignmentCenter, NULL);
    } else {
        graphics_draw_text(ctx, "No data\nOpen settings\non phone",
                           fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                           GRect(0, 50, 144, 70),
                           GTextOverflowModeWordWrap,
                           GTextAlignmentCenter, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Main chart update callback
 * --------------------------------------------------------------------------- */

static void chart_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_reading_count == 0) {
        draw_no_data_message(ctx);
        return;
    }

    /* Fixed scale: 0–20 mmol/L (internal 0–200) or 0–360 mg/dL */
    int min_bg, bg_range;
    if (s_is_mmol) {
        min_bg   = 0;    /* 0.0 mmol/L */
        bg_range = 200;  /* up to 20.0 mmol/L (internal ×10) */
    } else {
        min_bg   = 0;    /* 0 mg/dL */
        bg_range = 360;  /* up to 360 mg/dL */
    }

    draw_value_grid(ctx, min_bg, bg_range);

    time_t now = time(NULL);
    draw_time_grid(ctx, now);
    draw_glucose_line(ctx, min_bg, bg_range, now);
    draw_extremum_labels(ctx, min_bg, bg_range, now);
}

/* ---------------------------------------------------------------------------
 * Chart / status refresh
 * --------------------------------------------------------------------------- */

/** Mark the chart layer dirty and refresh the status bar text. */
static void update_chart(void) {
    if (s_chart_layer) {
        layer_mark_dirty(s_chart_layer);
    }

    if (s_reading_count > 0) {
        time_t now = time(NULL);
        int minutes_ago = (int)((now - s_readings[0].timestamp) / 60);
        snprintf(s_status_text, sizeof(s_status_text),
                 "%d readings, %dm ago", s_reading_count, minutes_ago);
    } else {
        snprintf(s_status_text, sizeof(s_status_text), "No data");
    }

    text_layer_set_text(s_status_layer, s_status_text);
}

/* ---------------------------------------------------------------------------
 * AppMessage helpers
 * --------------------------------------------------------------------------- */

/** Send an empty message to the phone to trigger a data fetch. */
static void request_data(void) {
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    if (iter) {
        dict_write_uint8(iter, MESSAGE_KEY_BG_DATA, 0);
        app_message_outbox_send();
    }
}

/** Process an incoming AppMessage (units, count header, chunk, or reading). */
static void inbox_received_callback(DictionaryIterator *iterator,
                                     void *context) {
    Tuple *count_tuple     = dict_find(iterator, MESSAGE_KEY_BG_COUNT);
    Tuple *units_tuple     = dict_find(iterator, MESSAGE_KEY_BG_UNITS);
    Tuple *index_tuple     = dict_find(iterator, MESSAGE_KEY_BG_INDEX);
    Tuple *chunk_tuple     = dict_find(iterator, MESSAGE_KEY_BG_CHUNK);
    Tuple *value_tuple     = dict_find(iterator, MESSAGE_KEY_BG_VALUE);
    Tuple *timestamp_tuple = dict_find(iterator, MESSAGE_KEY_BG_TIMESTAMP);

    if (units_tuple) {
        snprintf(s_bg_units, sizeof(s_bg_units), "%s",
                 units_tuple->value->cstring);
        s_is_mmol = (strcmp(s_bg_units, "mmol/L") == 0);
    }

    if (count_tuple) {
        int count = count_tuple->value->int32;
        if (count > MAX_READINGS) count = MAX_READINGS;
        s_expected_count = count;
        s_received_count = 0;
        s_receiving_data = true;
        memset(s_readings, 0, sizeof(s_readings));
        return;
    }

    /* Bulk chunk path */
    if (chunk_tuple && index_tuple) {
        uint8_t *data = chunk_tuple->value->data;
        int byte_len = chunk_tuple->length;
        int start_index = index_tuple->value->int32;
        int readings_in_chunk = byte_len / BYTES_PER_READING;

        for (int i = 0; i < readings_in_chunk; i++) {
            int idx = start_index + i;
            if (idx >= MAX_READINGS) break;
            int offset = i * BYTES_PER_READING;
            s_readings[idx].value = (int16_t)(data[offset] | (data[offset + 1] << 8));
            s_readings[idx].timestamp = (time_t)((uint32_t)data[offset + 2] |
                                                  ((uint32_t)data[offset + 3] << 8) |
                                                  ((uint32_t)data[offset + 4] << 16) |
                                                  ((uint32_t)data[offset + 5] << 24));
            s_received_count++;
        }

        if (s_received_count >= s_expected_count) {
            s_reading_count = s_expected_count;
            s_receiving_data = false;
            update_chart();
        }
        return;
    }

    /* Legacy per-reading path */
    if (index_tuple && value_tuple && timestamp_tuple) {
        int index = index_tuple->value->int32;
        if (index >= 0 && index < MAX_READINGS) {
            s_readings[index].value     = value_tuple->value->int16;
            s_readings[index].timestamp = timestamp_tuple->value->int32;
            s_received_count++;

            if (s_received_count >= s_expected_count) {
                s_reading_count = s_expected_count;
                s_receiving_data = false;
                update_chart();
            }
        }
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator,
                                    AppMessageResult reason,
                                    void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message send failed: %d", reason);
}

/* ---------------------------------------------------------------------------
 * Timer
 * --------------------------------------------------------------------------- */

/** Tick handler – refresh every 5 minutes for battery efficiency. */
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    if (tick_time->tm_min % 5 == 0) {
        update_chart();
        request_data();
    }
}

/* ---------------------------------------------------------------------------
 * Window lifecycle
 * --------------------------------------------------------------------------- */

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_chart_layer = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 20));
    layer_set_update_proc(s_chart_layer, chart_layer_update_proc);
    layer_add_child(window_layer, s_chart_layer);

    s_status_layer = text_layer_create(
        GRect(0, bounds.size.h - 20, bounds.size.w, 20));
    text_layer_set_background_color(s_status_layer, GColorWhite);
    text_layer_set_text_color(s_status_layer, GColorBlack);
    text_layer_set_font(s_status_layer,
                        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_text(s_status_layer, s_status_text);
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));
}

static void main_window_unload(Window *window) {
    layer_destroy(s_chart_layer);
    text_layer_destroy(s_status_layer);
}

/* ---------------------------------------------------------------------------
 * App init / deinit / main
 * --------------------------------------------------------------------------- */

static void init(void) {
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorWhite);
    window_set_window_handlers(s_main_window, (WindowHandlers){
        .load   = main_window_load,
        .unload = main_window_unload
    });
    window_stack_push(s_main_window, true);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_open(APPMESSAGE_INBOX, APPMESSAGE_OUTBOX);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    request_data();
}

static void deinit(void) {
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
