#include <pebble.h>

// Configuration
#define MAX_READINGS 36
#define CHART_START_X 30    // Leave space for time labels
#define CHART_START_Y 10    // Leave space for value labels at top
#define CHART_WIDTH 114     // 144 - 30
#define CHART_HEIGHT 148    // 168 - 20 (status bar)
#define MIN_VALUE 40        // Minimum BG value to display (mg/dL)
#define MAX_VALUE 400       // Maximum BG value to display (mg/dL)
#define TIME_SPACING 4      // Pixels between readings vertically

// Message buffer sizes
#define APPMESSAGE_INBOX 256
#define APPMESSAGE_OUTBOX 128

// Global state
static Window *s_main_window;
static Layer *s_chart_layer;
static TextLayer *s_status_layer;

// Glucose data storage
typedef struct {
    int16_t value;      // BG value * 10 (e.g., 123 mg/dL = 1230)
    time_t timestamp;
} GlucoseReading;

static GlucoseReading s_readings[MAX_READINGS];
static int s_reading_count = 0;
static int s_expected_count = 0;
static int s_received_count = 0;
static bool s_is_mmol = false;
static char s_bg_units[10] = "mg/dL";
static char s_status_text[32] = "Loading...";

// Forward declarations
static void update_chart(void);
static void request_data(void);

/**
 * Draw the glucose chart
 */
static void chart_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_reading_count == 0) {
        // Draw "No data" message
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, "No data\nOpen settings\non phone", 
                          fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                          GRect(0, 50, 144, 70),
                          GTextOverflowModeWordWrap,
                          GTextAlignmentCenter,
                          NULL);
        return;
    }
    
    GRect bounds = layer_get_bounds(layer);
    
    // Calculate scaling using cached unit type
    int min_bg = s_is_mmol ? 20 : MIN_VALUE; // 2.0 mmol/L or 40 mg/dL
    int max_bg = s_is_mmol ? 220 : MAX_VALUE; // 22.0 mmol/L or 400 mg/dL
    int bg_range = max_bg - min_bg;
    
    // Draw background grid
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    
    // Vertical grid lines (every 50 mg/dL or 3 mmol/L)
    int grid_step = s_is_mmol ? 30 : 50; // 3.0 mmol/L or 50 mg/dL
    for (int bg = min_bg; bg <= max_bg; bg += grid_step) {
        int x = CHART_START_X + ((bg - min_bg) * CHART_WIDTH) / bg_range;
        graphics_draw_line(ctx, GPoint(x, CHART_START_Y), GPoint(x, CHART_START_Y + CHART_HEIGHT));
        
        // Draw value labels at top
        static char label[8];
        if (s_is_mmol) {
            snprintf(label, sizeof(label), "%.0f", bg / 10.0);
        } else {
            snprintf(label, sizeof(label), "%d", bg / 10);
        }
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, label,
                          fonts_get_system_font(FONT_KEY_GOTHIC_14),
                          GRect(x - 10, 0, 20, 14),
                          GTextOverflowModeTrailingEllipsis,
                          GTextAlignmentCenter,
                          NULL);
    }
    
    // Horizontal grid lines (every 6 readings = 30 minutes)
    // Draw from bottom (most recent) to top (oldest)
    for (int i = 0; i <= s_reading_count; i += 6) {
        // Calculate y from bottom: most recent (i=0) at bottom, oldest at top
        int y = CHART_START_Y + CHART_HEIGHT - (i * TIME_SPACING);
        if (y >= CHART_START_Y && y <= CHART_START_Y + CHART_HEIGHT) {
            graphics_draw_line(ctx, GPoint(CHART_START_X, y), GPoint(CHART_START_X + CHART_WIDTH, y));
            
            // Draw time labels (minutes ago from most recent)
            int minutes_ago = i * 5; // Each reading is 5 minutes
            static char time_label[8];
            snprintf(time_label, sizeof(time_label), "-%dm", minutes_ago);
            graphics_context_set_text_color(ctx, GColorWhite);
            graphics_draw_text(ctx, time_label,
                              fonts_get_system_font(FONT_KEY_GOTHIC_14),
                              GRect(0, y - 7, 28, 14),
                              GTextOverflowModeTrailingEllipsis,
                              GTextAlignmentRight,
                              NULL);
        }
    }
    
    // Draw threshold lines
    graphics_context_set_stroke_color(ctx, GColorRed);
    int low_threshold = s_is_mmol ? 40 : 70;   // 4.0 mmol/L or 70 mg/dL
    int high_threshold = s_is_mmol ? 100 : 180; // 10.0 mmol/L or 180 mg/dL
    
    int low_x = CHART_START_X + ((low_threshold - min_bg) * CHART_WIDTH) / bg_range;
    int high_x = CHART_START_X + ((high_threshold - min_bg) * CHART_WIDTH) / bg_range;
    
    if (low_x >= CHART_START_X && low_x <= CHART_START_X + CHART_WIDTH) {
        graphics_draw_line(ctx, GPoint(low_x, CHART_START_Y), 
                          GPoint(low_x, CHART_START_Y + CHART_HEIGHT));
    }
    if (high_x >= CHART_START_X && high_x <= CHART_START_X + CHART_WIDTH) {
        graphics_draw_line(ctx, GPoint(high_x, CHART_START_Y), 
                          GPoint(high_x, CHART_START_Y + CHART_HEIGHT));
    }
    
    // Draw glucose readings as a line graph
    // Most recent (index 0) at bottom, oldest at top
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    
    for (int i = 0; i < s_reading_count - 1; i++) {
        int value1 = s_readings[i].value;
        int value2 = s_readings[i + 1].value;
        
        // Calculate x positions based on values
        int x1 = CHART_START_X + ((value1 - min_bg) * CHART_WIDTH) / bg_range;
        int x2 = CHART_START_X + ((value2 - min_bg) * CHART_WIDTH) / bg_range;
        
        // Calculate y positions based on time (index)
        // Most recent (i=0) at bottom, oldest at top
        int y1 = CHART_START_Y + CHART_HEIGHT - (i * TIME_SPACING);
        int y2 = CHART_START_Y + CHART_HEIGHT - ((i + 1) * TIME_SPACING);
        
        // Clamp to chart bounds
        if (x1 < CHART_START_X) x1 = CHART_START_X;
        if (x1 > CHART_START_X + CHART_WIDTH) x1 = CHART_START_X + CHART_WIDTH;
        if (x2 < CHART_START_X) x2 = CHART_START_X;
        if (x2 > CHART_START_X + CHART_WIDTH) x2 = CHART_START_X + CHART_WIDTH;
        
        // Draw line connecting points
        graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
        
        // Draw point
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, GPoint(x1, y1), 2);
    }
    
    // Draw last point (oldest reading at top)
    if (s_reading_count > 0) {
        int last_value = s_readings[s_reading_count - 1].value;
        int last_x = CHART_START_X + ((last_value - min_bg) * CHART_WIDTH) / bg_range;
        int last_y = CHART_START_Y + CHART_HEIGHT - ((s_reading_count - 1) * TIME_SPACING);
        
        if (last_x >= CHART_START_X && last_x <= CHART_START_X + CHART_WIDTH) {
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_circle(ctx, GPoint(last_x, last_y), 2);
        }
    }
}

/**
 * Update the chart display
 */
static void update_chart(void) {
    if (s_chart_layer) {
        layer_mark_dirty(s_chart_layer);
    }
    
    // Update status text
    if (s_reading_count > 0) {
        time_t now = time(NULL);
        int minutes_ago = (now - s_readings[0].timestamp) / 60;
        snprintf(s_status_text, sizeof(s_status_text), "%d readings, %dm ago", 
                s_reading_count, minutes_ago);
    } else {
        snprintf(s_status_text, sizeof(s_status_text), "No data");
    }
    
    text_layer_set_text(s_status_layer, s_status_text);
}

/**
 * Request data from phone
 */
static void request_data(void) {
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    
    if (iter) {
        // Send empty message to trigger data fetch
        dict_write_uint8(iter, MESSAGE_KEY_BG_DATA, 0);
        app_message_outbox_send();
    }
}

/**
 * Handle incoming messages from phone
 */
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_BG_COUNT);
    Tuple *units_tuple = dict_find(iterator, MESSAGE_KEY_BG_UNITS);
    Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_BG_INDEX);
    Tuple *value_tuple = dict_find(iterator, MESSAGE_KEY_BG_VALUE);
    Tuple *timestamp_tuple = dict_find(iterator, MESSAGE_KEY_BG_TIMESTAMP);
    
    // Handle units
    if (units_tuple) {
        snprintf(s_bg_units, sizeof(s_bg_units), "%s", units_tuple->value->cstring);
        s_is_mmol = (strcmp(s_bg_units, "mmol/L") == 0);
    }
    
    // Handle count (start of new data)
    if (count_tuple) {
        int count = count_tuple->value->int32;
        if (count > MAX_READINGS) count = MAX_READINGS;
        s_reading_count = count;
        s_expected_count = count;
        s_received_count = 0;
        
        // Reset readings array
        memset(s_readings, 0, sizeof(s_readings));
        return;
    }
    
    // Handle individual reading
    if (index_tuple && value_tuple && timestamp_tuple) {
        int index = index_tuple->value->int32;
        
        if (index >= 0 && index < MAX_READINGS) {
            s_readings[index].value = value_tuple->value->int16;
            s_readings[index].timestamp = timestamp_tuple->value->int32;
            s_received_count++;
            
            // Only update chart after all readings are received
            if (s_received_count >= s_expected_count) {
                update_chart();
            }
        }
    }
}

/**
 * Handle message send failures
 */
static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

/**
 * Handle outbox send failure
 */
static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message send failed: %d", reason);
}

/**
 * Handle tick events (every 5 minutes for battery efficiency)
 */
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    if (tick_time->tm_min % 5 == 0) {
        update_chart();
        request_data();
    }
}

/**
 * Window load handler
 */
static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);
    
    // Create chart layer (full screen except status bar at bottom)
    s_chart_layer = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 20));
    layer_set_update_proc(s_chart_layer, chart_layer_update_proc);
    layer_add_child(window_layer, s_chart_layer);
    
    // Create status text layer at bottom
    s_status_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
    text_layer_set_background_color(s_status_layer, GColorBlack);
    text_layer_set_text_color(s_status_layer, GColorWhite);
    text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_text(s_status_layer, s_status_text);
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));
}

/**
 * Window unload handler
 */
static void main_window_unload(Window *window) {
    layer_destroy(s_chart_layer);
    text_layer_destroy(s_status_layer);
}

/**
 * App initialization
 */
static void init(void) {
    // Create main window
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload
    });
    window_stack_push(s_main_window, true);
    
    // Register AppMessage callbacks
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    
    // Open AppMessage
    app_message_open(APPMESSAGE_INBOX, APPMESSAGE_OUTBOX);
    
    // Register tick timer (every 5 minutes for battery efficiency)
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    
    // Request initial data
    request_data();
}

/**
 * App deinitialization
 */
static void deinit(void) {
    window_destroy(s_main_window);
}

/**
 * Main entry point
 */
int main(void) {
    init();
    app_event_loop();
    deinit();
}
