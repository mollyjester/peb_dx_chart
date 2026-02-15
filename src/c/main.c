#include <pebble.h>

// Configuration
#define MAX_READINGS 36
#define CHART_START_X 0
#define CHART_START_Y 0
#define CHART_WIDTH 144
#define CHART_HEIGHT 168
#define VALUE_SCALE 2  // pixels per mg/dL (or per 0.1 mmol/L)
#define TIME_SCALE 4   // pixels per 5-minute interval

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
        return;
    }
    
    GRect bounds = layer_get_bounds(layer);
    
    // Draw grid lines
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    
    // Horizontal grid lines (every 50 mg/dL or ~3 mmol/L)
    for (int i = 0; i <= 400; i += 50) {
        int x = i * VALUE_SCALE;
        if (x < CHART_WIDTH) {
            graphics_draw_line(ctx, GPoint(x, 0), GPoint(x, CHART_HEIGHT));
        }
    }
    
    // Vertical grid lines (every 30 minutes = 6 readings)
    for (int i = 0; i <= 36; i += 6) {
        int y = i * TIME_SCALE;
        if (y < CHART_HEIGHT) {
            graphics_draw_line(ctx, GPoint(0, y), GPoint(CHART_WIDTH, y));
        }
    }
    
    // Draw threshold lines (70 and 180 mg/dL or ~4 and 10 mmol/L)
    graphics_context_set_stroke_color(ctx, GColorRed);
    int low_threshold = 70;
    int high_threshold = 180;
    
    // Convert thresholds if using mmol/L
    if (strcmp(s_bg_units, "mmol/L") == 0) {
        low_threshold = 40;  // ~4.0 mmol/L * 10
        high_threshold = 100; // ~10.0 mmol/L * 10
    }
    
    int low_x = (low_threshold * VALUE_SCALE) / 10;
    int high_x = (high_threshold * VALUE_SCALE) / 10;
    
    if (low_x < CHART_WIDTH) {
        graphics_draw_line(ctx, GPoint(low_x, 0), GPoint(low_x, CHART_HEIGHT));
    }
    if (high_x < CHART_WIDTH) {
        graphics_draw_line(ctx, GPoint(high_x, 0), GPoint(high_x, CHART_HEIGHT));
    }
    
    // Draw glucose readings as a line
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    
    for (int i = 0; i < s_reading_count - 1; i++) {
        // Calculate positions
        int value1 = s_readings[i].value / 10; // Convert back to actual value
        int value2 = s_readings[i + 1].value / 10;
        
        int x1 = value1 * VALUE_SCALE;
        int x2 = value2 * VALUE_SCALE;
        int y1 = i * TIME_SCALE;
        int y2 = (i + 1) * TIME_SCALE;
        
        // Clamp to chart bounds
        if (x1 < 0) x1 = 0;
        if (x1 >= CHART_WIDTH) x1 = CHART_WIDTH - 1;
        if (x2 < 0) x2 = 0;
        if (x2 >= CHART_WIDTH) x2 = CHART_WIDTH - 1;
        if (y1 < 0) y1 = 0;
        if (y1 >= CHART_HEIGHT) y1 = CHART_HEIGHT - 1;
        if (y2 < 0) y2 = 0;
        if (y2 >= CHART_HEIGHT) y2 = CHART_HEIGHT - 1;
        
        graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
        
        // Draw point
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, GPoint(x1, y1), 2);
    }
    
    // Draw last point
    if (s_reading_count > 0) {
        int last_value = s_readings[s_reading_count - 1].value / 10;
        int last_x = last_value * VALUE_SCALE;
        int last_y = (s_reading_count - 1) * TIME_SCALE;
        
        if (last_x >= 0 && last_x < CHART_WIDTH && last_y >= 0 && last_y < CHART_HEIGHT) {
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_circle(ctx, GPoint(last_x, last_y), 2);
        }
    }
    
    // Draw labels on the side
    graphics_context_set_text_color(ctx, GColorWhite);
    
    // Current value label
    if (s_reading_count > 0) {
        static char value_text[16];
        int current_value = s_readings[0].value;
        
        if (strcmp(s_bg_units, "mmol/L") == 0) {
            snprintf(value_text, sizeof(value_text), "%.1f", current_value / 10.0);
        } else {
            snprintf(value_text, sizeof(value_text), "%d", current_value / 10);
        }
        
        graphics_draw_text(ctx, value_text, 
                          fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                          GRect(2, 2, 40, 20),
                          GTextOverflowModeTrailingEllipsis,
                          GTextAlignmentLeft,
                          NULL);
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
        APP_LOG(APP_LOG_LEVEL_INFO, "Requested data from phone");
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
        APP_LOG(APP_LOG_LEVEL_INFO, "Units: %s", s_bg_units);
    }
    
    // Handle count (start of new data)
    if (count_tuple) {
        int count = count_tuple->value->int32;
        if (count > MAX_READINGS) count = MAX_READINGS;
        s_reading_count = count;
        APP_LOG(APP_LOG_LEVEL_INFO, "Expecting %d readings", s_reading_count);
        
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
            
            APP_LOG(APP_LOG_LEVEL_INFO, "Reading %d: value=%d, time=%d", 
                   index, s_readings[index].value, (int)s_readings[index].timestamp);
            
            // Update chart after receiving each reading
            update_chart();
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
 * Handle outbox send success
 */
static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Message sent successfully");
}

/**
 * Handle outbox send failure
 */
static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message send failed: %d", reason);
}

/**
 * Handle tick events (every minute)
 */
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    // Update chart to show elapsed time
    update_chart();
    
    // Request new data every 5 minutes
    if (tick_time->tm_min % 5 == 0) {
        request_data();
    }
}

/**
 * Window load handler
 */
static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);
    
    // Create chart layer
    s_chart_layer = layer_create(GRect(CHART_START_X, CHART_START_Y, CHART_WIDTH, CHART_HEIGHT - 20));
    layer_set_update_proc(s_chart_layer, chart_layer_update_proc);
    layer_add_child(window_layer, s_chart_layer);
    
    // Create status text layer at bottom
    s_status_layer = text_layer_create(GRect(0, CHART_HEIGHT - 20, CHART_WIDTH, 20));
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
    app_message_register_outbox_sent(outbox_sent_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    
    // Open AppMessage
    app_message_open(APPMESSAGE_INBOX, APPMESSAGE_OUTBOX);
    
    // Register tick timer (every minute)
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
