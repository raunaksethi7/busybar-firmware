#include "../busy_i.h"

#include <gui/modules/label.h>
#include <font_registry/fonts.h>
#include <stopwatch/stopwatch.h>
#include <toolbox/color.h>

#include "../helpers/animate.h"

#include <inttypes.h>

/**
 * The face for the CUSTOM slot: a plain count-up readout, LaMetric-style — white pixel
 * digits on bare black, no theme art behind them.
 *
 * The scene owns none of the timing. That lives in the Stopwatch service, which outlives
 * this scene, so leaving CUSTOM, opening settings, or putting the display to sleep does
 * not touch the count. Entering the scene reads whatever the service has been doing.
 *
 * The readout grows as the count does, and the growth is animated: the minutes and
 * seconds slide across to open a gap, then the new hours digit lands in it.
 */

#define STOPWATCH_TEXT_LEN (8)

/**
 * Seconds in an "hour" for display purposes.
 *
 * A test seam: the readout only grows once an hour has passed, which makes the growth
 * animation impractical to check by hand. Building with a small value here — e.g.
 * `./fbt CFLAGS_APP="-DSTOPWATCH_HOUR_S=20"` — puts both transitions seconds apart.
 */
#ifndef STOPWATCH_HOUR_S
#define STOPWATCH_HOUR_S (3600UL)
#endif

/** Front panel is 72 x 16; everything here is laid out against that. */
#define STOPWATCH_PANEL_WIDTH  (72)
#define STOPWATCH_PANEL_HEIGHT (16)

/** Long enough to read as a deliberate shift, short enough not to hide a second. */
#define STOPWATCH_SLIDE_MS (340)

/**
 * Measured on hardware at the 14px face: a digit is ~11px and a colon ~6px, so MM:SS is
 * 50px and H:MM:SS is 66px — both fit the 72px panel. HH:MM:SS would be 78px.
 *
 * Rather than shrink the whole readout past ten hours, only the hours prefix steps down
 * to the 10px face: "HH:" is then ~16px and the minutes and seconds stay at full size,
 * for ~66px total. Minutes and seconds are what a stopwatch is actually read for, so
 * they never shrink.
 */
#define STOPWATCH_FONT_WIDE   FONT_BUSY_REGULAR_14
#define STOPWATCH_FONT_NARROW FONT_BUSY_BOLD_10

typedef enum {
    /** MM:SS */
    StopwatchFormatMinutes = 0,
    /** H:MM:SS */
    StopwatchFormatOneHourDigit = 1,
    /** HH:MM:SS */
    StopwatchFormatTwoHourDigits = 2,
} StopwatchFormat;

typedef struct {
    /** The hours and their colon: "" while under an hour, else "H:" or "HH:". */
    Label* head;
    /** Always present: "MM:SS". */
    Label* tail;
    /** Fires once at the end of a slide to drop the new hours digit into the gap. */
    FuriEventLoopTimer* reveal_timer;

    FuriPubSub* stopwatch_pubsub;
    FuriPubSubSubscription* stopwatch_sub;
    StopwatchState state;

    StopwatchFormat format;
    bool is_head_pending;
    /** Widget has no position getter, so the slide's start point is tracked here. */
    int32_t tail_x;
} BusySceneStopwatch;

// MARK: - Formatting

static StopwatchFormat busy_scene_stopwatch_format_for(uint32_t elapsed_ms) {
    const uint32_t hours = (elapsed_ms / 1000UL) / STOPWATCH_HOUR_S;

    if(hours == 0) return StopwatchFormatMinutes;
    return (hours < 10) ? StopwatchFormatOneHourDigit : StopwatchFormatTwoHourDigits;
}

/** The hours prefix shrinks only when it needs a second digit. */
static const char* busy_scene_stopwatch_head_font(StopwatchFormat format) {
    return (format == StopwatchFormatTwoHourDigits) ? STOPWATCH_FONT_NARROW :
                                                      STOPWATCH_FONT_WIDE;
}

static void busy_scene_stopwatch_texts(
    uint32_t elapsed_ms,
    char* head,
    size_t head_len,
    char* tail,
    size_t tail_len) {
    const uint32_t total_s = elapsed_ms / 1000;
    const uint32_t hours = total_s / STOPWATCH_HOUR_S;
    const uint32_t minutes = (total_s % STOPWATCH_HOUR_S) / 60;
    const uint32_t seconds = total_s % 60;

    if(hours > 0) {
        snprintf(head, head_len, "%" PRIu32 ":", hours);
    } else {
        head[0] = '\0';
    }

    snprintf(tail, tail_len, "%02" PRIu32 ":%02" PRIu32, minutes, seconds);
}

// MARK: - Layout

/**
 * Centre the head and tail as one group.
 *
 * @param head_width   width to reserve for the hours, measured while it was visible.
 * @param slide_from_x when >= 0, the tail starts there and animates to its new home —
 *                     this is the "numbers slide over" half of a growth transition.
 */
static void busy_scene_stopwatch_layout(
    BusySceneStopwatch* data,
    int32_t head_width,
    int32_t slide_from_x) {
    Widget* head_base = label_get_base(data->head);
    Widget* tail_base = label_get_base(data->tail);

    const int32_t tail_width = widget_get_width(tail_base);

    // head_width is passed in rather than measured here: during a growth the head is
    // hidden until the slide finishes, and a hidden widget cannot be relied on to report
    // its text width. The gap must be reserved anyway, or the tail slides to the wrong
    // place and jumps again when the digit appears.
    const int32_t origin_x = (STOPWATCH_PANEL_WIDTH - (head_width + tail_width)) / 2;

    widget_set_pos_x(head_base, origin_x);

    const int32_t tail_x = origin_x + head_width;
    if(slide_from_x >= 0 && slide_from_x != tail_x) {
        animate_pos_x(tail_base, slide_from_x, tail_x, STOPWATCH_SLIDE_MS);
    } else {
        widget_set_pos_x(tail_base, tail_x);
    }
    data->tail_x = tail_x;
}

static void busy_scene_stopwatch_reveal_timer_callback(void* context) {
    furi_assert(context);
    const BusyApp* instance = context;

    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    with_gui(instance->gui, {
        data->is_head_pending = false;
        widget_set_visible(label_get_base(data->head), true);
    });
}

static void busy_scene_stopwatch_render(BusyApp* instance) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    char head_text[STOPWATCH_TEXT_LEN];
    char tail_text[STOPWATCH_TEXT_LEN];
    busy_scene_stopwatch_texts(
        data->state.elapsed_ms, head_text, sizeof(head_text), tail_text, sizeof(tail_text));

    const StopwatchFormat format = busy_scene_stopwatch_format_for(data->state.elapsed_ms);
    const bool is_transition = (format != data->format);

    with_gui(instance->gui, {
        const int32_t previous_tail_x = data->tail_x;

        if(is_transition) {
            // Only the head ever changes face; the tail stays at the wide one for life.
            label_set_font(data->head, busy_scene_stopwatch_head_font(format));
        }

        label_set_text(data->head, head_text);
        label_set_text(data->tail, tail_text);

        // Measure now, before any hiding below.
        const int32_t head_width =
            (head_text[0] != '\0') ? widget_get_width(label_get_base(data->head)) : 0;

        if(is_transition) {
            // Growing: hold the hours back so the slide reads as making room for it.
            // Shrinking (only at the 24h rollover) has nothing to reveal, so it just snaps.
            const bool is_growing = (format > data->format);

            data->is_head_pending = is_growing;
            widget_set_visible(label_get_base(data->head), !is_growing && head_text[0] != '\0');

            busy_scene_stopwatch_layout(data, head_width, previous_tail_x);

            if(is_growing) {
                furi_event_loop_timer_start(data->reveal_timer, STOPWATCH_SLIDE_MS);
            }

            data->format = format;

        } else {
            busy_scene_stopwatch_layout(data, head_width, -1);
        }

        mirror_card_set_header_text(
            instance->timer_card, data->state.is_running ? "RUNNING" : "PAUSED");
        mirror_card_set_footer_primary_text(
            instance->timer_card, head_text[0] != '\0' ? head_text : tail_text);
    });
}

// MARK: - Events

static void busy_scene_stopwatch_pubsub_callback(const void* msg, void* context) {
    furi_assert(msg);
    furi_assert(context);

    const StopwatchEvent* event = msg;
    BusyApp* instance = context;

    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);
    data->state = event->state;

    busy_send_custom_event(instance, BusyCustomEventStopwatchUpdated);
}

static bool busy_scene_stopwatch_input_callback(const InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    BusyApp* instance = context;

    bool consumed = false;
    BusyCustomEvent custom_event;

    if(event->key == InputKeyStart) {
        if(event->type == InputTypeShort) {
            custom_event = BusyCustomEventStopwatchToggle;
            consumed = true;
        } else if(event->type == InputTypeLong) {
            // Hold to zero it, the way a physical stopwatch works. Back is left alone so
            // leaving the screen can never destroy a running measurement.
            custom_event = BusyCustomEventStopwatchReset;
            consumed = true;
        }
    }

    if(consumed) {
        busy_send_custom_event(instance, custom_event);
    }

    return consumed;
}

// MARK: - Standard SceneManager handlers

static void busy_scene_stopwatch_on_enter(void* context) {
    furi_assert(context);
    BusyApp* instance = context;

    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    busy_set_front_display_blanking(instance, false);

    // Seed from the service before first paint: the count has very likely been running
    // while this scene did not exist.
    stopwatch_get_state(instance->stopwatch, &data->state);
    data->format = busy_scene_stopwatch_format_for(data->state.elapsed_ms);
    data->is_head_pending = false;
    data->tail_x = 0;

    with_gui(instance->gui, {
        GuiLayer* layer = gui_get_layer(instance->gui, GuiLayerIdMain);
        gui_layer_add_input_callback(layer, busy_scene_stopwatch_input_callback, instance);

        const Color white = (Color)COLOR_MAKE_HEX(0xFFFFFF);

        // AlignLeftMid makes LVGL centre each label vertically for us and turns pos_x
        // into an offset from the left edge. Positioning y by hand instead put the 14px
        // glyph box below the panel's 16 rows and clipped the bottom of every digit.
        data->head = label_alloc(instance->front_window);
        label_set_text_color(data->head, white);
        label_set_font(data->head, busy_scene_stopwatch_head_font(data->format));
        widget_set_align(label_get_base(data->head), AlignLeftMid);

        data->tail = label_alloc(instance->front_window);
        label_set_text_color(data->tail, white);
        label_set_font(data->tail, STOPWATCH_FONT_WIDE);
        widget_set_align(label_get_base(data->tail), AlignLeftMid);

        widget_set_visible(mirror_card_get_base(instance->timer_card), true);
        widget_set_visible(nav_bar_get_base(instance->nav_bar), false);
        mirror_card_set_show_header(instance->timer_card, true);
        mirror_card_set_show_footer(instance->timer_card, true);
        mirror_card_set_footer_secondary_text(instance->timer_card, "ELAPSED");
    });

    data->reveal_timer = furi_event_loop_timer_alloc(
        instance->event_loop,
        busy_scene_stopwatch_reveal_timer_callback,
        FuriEventLoopTimerTypeOnce,
        instance);

    data->stopwatch_pubsub = stopwatch_get_pubsub(instance->stopwatch);
    data->stopwatch_sub = furi_pubsub_subscribe(
        data->stopwatch_pubsub, busy_scene_stopwatch_pubsub_callback, instance);

    busy_scene_stopwatch_render(instance);
    busy_start_transition(instance);
}

static void busy_scene_stopwatch_on_exit(void* context) {
    furi_assert(context);
    BusyApp* instance = context;

    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    furi_pubsub_unsubscribe(data->stopwatch_pubsub, data->stopwatch_sub);
    furi_event_loop_timer_free(data->reveal_timer);

    with_gui(instance->gui, {
        GuiLayer* layer = gui_get_layer(instance->gui, GuiLayerIdMain);
        gui_layer_remove_input_callback(layer, busy_scene_stopwatch_input_callback);

        mirror_card_set_show_header(instance->timer_card, false);
        mirror_card_set_show_footer(instance->timer_card, false);

        label_free(data->head);
        label_free(data->tail);
    });

    // The Stopwatch service is deliberately NOT stopped here. That is the whole feature.
}

static bool busy_scene_stopwatch_on_event(const SceneManagerEvent* event, void* context) {
    furi_assert(context);
    BusyApp* instance = context;

    bool consumed = false;

    if(event->type == SceneManagerEventTypeCustom) {
        if(event->event == BusyCustomEventStopwatchUpdated) {
            busy_scene_stopwatch_render(instance);

        } else if(event->event == BusyCustomEventStopwatchToggle) {
            stopwatch_toggle(instance->stopwatch);

        } else if(event->event == BusyCustomEventStopwatchReset) {
            stopwatch_reset(instance->stopwatch);
        }

        consumed = true;

    } else if(event->type == SceneManagerEventTypeBack) {
        busy_set_front_display_blanking(instance, false);
        busy_exit(instance);
        consumed = true;
    }

    return consumed;
}

const Scene busy_scene_stopwatch = {
    .enter_callback = busy_scene_stopwatch_on_enter,
    .exit_callback = busy_scene_stopwatch_on_exit,
    .event_callback = busy_scene_stopwatch_on_event,
    .data_size = sizeof(BusySceneStopwatch),
};
