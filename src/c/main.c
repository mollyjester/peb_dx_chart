#include <pebble.h>

/* ---------------------------------------------------------------------------
 * Configuration constants
 * --------------------------------------------------------------------------- */
#define MAX_READINGS      36
#define CHART_START_X      30   /* Left margin for time labels */
#define CHART_START_Y      10   /* Top margin for value labels */
#define CHART_WIDTH       114   /* 144 - 30 */
#define CHART_HEIGHT      148   /* 168 - 20 (status bar) */
#define TIME_SPACING        4   /* Pixels between readings vertically */

/* Dotted-line pattern: draw DOT_ON pixels, skip DOT_OFF pixels */
#define DOT_ON              2
#define DOT_OFF             3
#define DOT_PERIOD          (DOT_ON + DOT_OFF)

/* Time-grid interval in readings (6 readings = 30 minutes) */
#define TIME_GRID_INTERVAL  6

/* Auto-scaling constants */
#define BG_PADDING         10   /* Padding in internal units (±1 mg/dL or ±0.1 mmol/L) */
#define BG_MIN_RANGE       30   /* Minimum visible range in internal units */

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

/** Map a BG value to an x-pixel coordinate within the chart area. */
static int bg_to_x(int bg_value, int min_bg, int bg_range) {
    return CHART_START_X + ((bg_value - min_bg) * CHART_WIDTH) / bg_range;
}

/** Map a reading index to a y-pixel coordinate (index 0 = bottom / newest). */
static int index_to_y(int index) {
    return CHART_START_Y + CHART_HEIGHT - (index * TIME_SPACING);
}

/** Clamp an x value to the visible chart area. */
static int clamp_x(int x) {
    if (x < CHART_START_X) return CHART_START_X;
    if (x > CHART_START_X + CHART_WIDTH) return CHART_START_X + CHART_WIDTH;
    return x;
}

/** Return true when x falls inside the visible chart area. */
static bool x_in_bounds(int x) {
    return x >= CHART_START_X && x <= CHART_START_X + CHART_WIDTH;
}

/**
 * Draw a dotted vertical line from y_start to y_end at the given x.
 * Pattern: DOT_ON pixels drawn, DOT_OFF pixels skipped, repeating.
 */
static void draw_dotted_vline(GContext *ctx, int x, int y_start, int y_end) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
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
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_line(ctx, GPoint(x, y_start), GPoint(x, y_end));
}

/**
 * Draw a dotted horizontal line from x_start to x_end at the given y.
 * Pattern: DOT_ON pixels drawn, DOT_OFF pixels skipped, repeating.
 */
static void draw_dotted_hline(GContext *ctx, int y, int x_start, int x_end) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    for (int x = x_start; x <= x_end; x++) {
        if ((x - x_start) % DOT_PERIOD < DOT_ON) {
            graphics_draw_pixel(ctx, GPoint(x, y));
        }
    }
}

/**
 * Choose a nice grid step that produces 1–3 lines within the given range.
 */
static int choose_grid_step(int bg_range) {
    if (s_is_mmol) {
        /* mmol/L internal values are ×10; steps represent 1.0, 2.0, 5.0 mmol/L */
        static const int steps[] = {10, 20, 50};
        for (int i = 0; i < 3; i++) {
            int count = bg_range / steps[i];
            if (count >= 1 && count <= 3) return steps[i];
        }
        return 50;
    } else {
        /* mg/dL mode */
        static const int steps[] = {10, 20, 25, 50, 100};
        for (int i = 0; i < 5; i++) {
            int count = bg_range / steps[i];
            if (count >= 1 && count <= 3) return steps[i];
        }
        return 100;
    }
}

/**
 * Draw a grid line label at the top of the chart.
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

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, label,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(x - 15, 0, 30, 14),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
}

/**
 * Draw the vertical value-reference grid lines with labels at the top.
 * Includes one fixed clinical threshold line (solid) and dynamic dotted lines.
 */
static void draw_value_grid(GContext *ctx, int min_bg, int bg_range) {
    int max_bg = min_bg + bg_range;

    /* Fixed clinical threshold: 4.0 mmol/L (72 mg/dL) if in range, else 3.0 (54) */
    int threshold;
    if (s_is_mmol) {
        threshold = (40 >= min_bg && 40 <= max_bg) ? 40 : 30;
    } else {
        threshold = (72 >= min_bg && 72 <= max_bg) ? 72 : 54;
    }

    /* Draw fixed threshold as solid line */
    {
        int x = bg_to_x(threshold, min_bg, bg_range);
        if (x_in_bounds(x)) {
            draw_solid_vline(ctx, x, CHART_START_Y, CHART_START_Y + CHART_HEIGHT);
            draw_grid_label(ctx, threshold, min_bg, bg_range);
        }
    }

    /* Dynamic grid lines */
    int step = choose_grid_step(bg_range);
    int first = ((min_bg / step) + 1) * step;
    int close_dist = bg_range / 20;  /* 5% of range */
    if (close_dist < 1) close_dist = 1;

    int drawn = 0;
    for (int bg = first; bg < max_bg && drawn < 3; bg += step) {
        /* Skip if too close to the fixed threshold */
        int diff = bg - threshold;
        if (diff < 0) diff = -diff;
        if (diff < close_dist) continue;

        int x = bg_to_x(bg, min_bg, bg_range);
        if (!x_in_bounds(x)) continue;

        draw_dotted_vline(ctx, x, CHART_START_Y, CHART_START_Y + CHART_HEIGHT);
        draw_grid_label(ctx, bg, min_bg, bg_range);
        drawn++;
    }
}

/**
 * Draw the horizontal time-grid lines with labels on the left.
 */
static void draw_time_grid(GContext *ctx) {
    for (int i = 0; i <= s_reading_count; i += TIME_GRID_INTERVAL) {
        int y = index_to_y(i);
        if (y < CHART_START_Y || y > CHART_START_Y + CHART_HEIGHT) continue;

        /* Dotted horizontal grid line */
        draw_dotted_hline(ctx, y, CHART_START_X, CHART_START_X + CHART_WIDTH);

        /* Time label */
        int minutes_ago = i * 5;
        static char time_label[8];
        if (minutes_ago == 0) {
            snprintf(time_label, sizeof(time_label), "now");
        } else if (minutes_ago == 30) {
            snprintf(time_label, sizeof(time_label), "30m");
        } else if (minutes_ago % 60 == 0) {
            snprintf(time_label, sizeof(time_label), "%dh", minutes_ago / 60);
        } else {
            snprintf(time_label, sizeof(time_label), "%d.5h", minutes_ago / 60);
        }
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, time_label,
                           fonts_get_system_font(FONT_KEY_GOTHIC_14),
                           GRect(0, y - 7, 28, 14),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
    }
}

/**
 * Draw the glucose line graph (line segments + data-point dots).
 */
static void draw_glucose_line(GContext *ctx, int min_bg, int bg_range) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);

    for (int i = 0; i < s_reading_count - 1; i++) {
        int x1 = clamp_x(bg_to_x(s_readings[i].value,     min_bg, bg_range));
        int x2 = clamp_x(bg_to_x(s_readings[i + 1].value, min_bg, bg_range));
        int y1 = index_to_y(i);
        int y2 = index_to_y(i + 1);

        graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));

        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, GPoint(x1, y1), 2);
    }

    /* Draw the last (oldest) data point */
    if (s_reading_count > 0) {
        int last_idx = s_reading_count - 1;
        int lx = bg_to_x(s_readings[last_idx].value, min_bg, bg_range);
        int ly = index_to_y(last_idx);

        if (x_in_bounds(lx)) {
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_circle(ctx, GPoint(lx, ly), 2);
        }
    }
}

/**
 * Draw numerical labels at the extremum (min / max) glucose points.
 *
 * A small label showing the BG value is placed next to the data point.
 * Labels are offset horizontally so they don't overlap the line.
 * Both min and max are always labeled, even if they are at the same index.
 */
static void draw_extremum_labels(GContext *ctx, int min_bg, int bg_range) {
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
    graphics_context_set_text_color(ctx, GColorWhite);

    int label_w = 30;
    int label_h = 16;

    /* --- minimum --- */
    {
        int px = clamp_x(bg_to_x(min_val, min_bg, bg_range));
        int py = index_to_y(min_idx);
        int lx = (px + label_w + 4 > CHART_START_X + CHART_WIDTH)
                     ? px - label_w - 4
                     : px + 4;
        int ly = py - label_h / 2;
        /* Clamp vertically */
        if (ly < 0) ly = 0;
        if (ly + label_h > CHART_START_Y + CHART_HEIGHT) ly = CHART_START_Y + CHART_HEIGHT - label_h;
        graphics_draw_text(ctx, min_label, font,
                           GRect(lx, ly, label_w, label_h),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft, NULL);
    }

    /* --- maximum (always draw, offset if same index as minimum) --- */
    {
        int px = clamp_x(bg_to_x(max_val, min_bg, bg_range));
        int py = index_to_y(max_idx);
        int lx = (px + label_w + 4 > CHART_START_X + CHART_WIDTH)
                     ? px - label_w - 4
                     : px + 4;
        int ly = py - label_h / 2;
        /* If same index as min, offset the max label upward to avoid overlap */
        if (max_idx == min_idx) {
            ly = py - label_h - 2;
        }
        /* Clamp vertically */
        if (ly < 0) ly = 0;
        if (ly + label_h > CHART_START_Y + CHART_HEIGHT) ly = CHART_START_Y + CHART_HEIGHT - label_h;
        graphics_draw_text(ctx, max_label, font,
                           GRect(lx, ly, label_w, label_h),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft, NULL);
    }
}

/**
 * Show a centred status message when no data is available.
 */
static void draw_no_data_message(GContext *ctx) {
    graphics_context_set_text_color(ctx, GColorWhite);
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

    /* Auto-scale: find min/max BG values from readings */
    int data_min = s_readings[0].value;
    int data_max = s_readings[0].value;
    for (int i = 1; i < s_reading_count; i++) {
        if (s_readings[i].value < data_min) data_min = s_readings[i].value;
        if (s_readings[i].value > data_max) data_max = s_readings[i].value;
    }

    /* Add padding */
    int min_bg = data_min - BG_PADDING;
    int max_bg = data_max + BG_PADDING;

    /* Enforce minimum visible range */
    int bg_range = max_bg - min_bg;
    if (bg_range < BG_MIN_RANGE) {
        int center = (min_bg + max_bg) / 2;
        min_bg = center - BG_MIN_RANGE / 2;
        max_bg = center + BG_MIN_RANGE / 2;
        bg_range = max_bg - min_bg;
    }

    draw_value_grid(ctx, min_bg, bg_range);
    draw_time_grid(ctx);
    draw_glucose_line(ctx, min_bg, bg_range);
    draw_extremum_labels(ctx, min_bg, bg_range);
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
    text_layer_set_background_color(s_status_layer, GColorBlack);
    text_layer_set_text_color(s_status_layer, GColorWhite);
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
    window_set_background_color(s_main_window, GColorBlack);
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
