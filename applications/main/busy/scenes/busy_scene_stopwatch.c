#include "../busy_i.h"

#include <gui/modules/label.h>
#include <font_registry/fonts.h>
#include <stopwatch/stopwatch.h>
#include <brightness_control/brightness_control.h>
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
 * How long Start/Pause must be held to zero the count.
 *
 * The system's own long-press fires after INPUT_LONG_PRESS_COUNTS * INPUT_PRESS_TICKS,
 * which is 300 ms — short enough that something leaning on the button inside a bag will
 * wipe a running measurement. Repeat events arrive every INPUT_PRESS_TICKS (150 ms)
 * after that, so counting them turns the gesture into a deliberate hold.
 *
 * The readout switches to a warning partway through, so the reset is never a surprise
 * and releasing early cancels it.
 */
#define STOPWATCH_RESET_ARM_REPEATS (6) /* ~1.05 s hold to arm */

/** How long the armed state waits for confirmation before assuming a false press. */
#define STOPWATCH_RESET_ARM_TIMEOUT_MS (4000)

/** Brightness step per detent of the wheel. */
#define STOPWATCH_BRIGHTNESS_STEP (5)

/**
 * Dialling the starting time: minutes per detent, and the same again once the wheel is
 * being spun rather than nudged.
 *
 * A single rate cannot serve both jobs. Restoring several hours at one minute a detent
 * is hundreds of clicks, while a coarse-only rate cannot land on a specific minute. So
 * detents arriving in quick succession switch to the coarse step, and pausing goes back
 * to fine.
 */
#define STOPWATCH_ADJUST_FINE_MN   (1)
#define STOPWATCH_ADJUST_COARSE_MN (15)
#define STOPWATCH_ADJUST_RAMP_AFTER (5)
#define STOPWATCH_ADJUST_RAMP_GAP_MS (400)

/**
 * Every digit is the same size at every count. Measured on hardware at this face a digit
 * is ~11px and a colon ~6px, so MM:SS is 50px and H:MM:SS is 66px against a 72px panel.
 * HH:MM:SS would be ~77px.
 *
 * Those last few pixels are taken out of the gaps between characters rather than out of
 * the glyphs: past ten hours the tracking tightens by a pixel, which is invisible next to
 * a readout where half the digits had shrunk.
 */
#define STOPWATCH_FONT (FONT_BUSY_REGULAR_14)

/**
 * Tracking applied only in the eight-character band, to claw back ~7px.
 *
 * -1 is the floor. Measured on hardware, -2 pulls the glyphs into each other badly
 * enough that the colons stop reading as separators and the whole thing turns to mush.
 * At -1 the readout is 71px of the 72 available: flush to the left edge, but complete.
 */
#define STOPWATCH_TIGHT_TRACKING (-1)

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
    /** Last text pushed to the mirror card, so identical writes are skipped. */
    char card_header[8];
    char card_time[STOPWATCH_TEXT_LEN * 2];
    /** Widget has no position getter, so the slide's start point is tracked here. */
    int32_t tail_x;
    /** Repeat events seen while Start/Pause is held; 0 when not holding. */
    uint32_t reset_hold;
    /** Showing RESET and waiting for a confirming tap. */
    bool is_reset_armed;
    /** Disarms when confirmation does not arrive. */
    FuriEventLoopTimer* arm_timer;

    /** Wheel is retasked to brightness until clicked again. */
    bool is_brightness_mode;
    BrightnessControl* brightness;
    uint8_t brightness_value;

    /** Consecutive quick detents, for ramping the dial rate. */
    uint32_t adjust_run;
    uint32_t adjust_last_ms;
} BusySceneStopwatch;

// MARK: - Formatting

static StopwatchFormat busy_scene_stopwatch_format_for(uint32_t elapsed_ms) {
    const uint32_t hours = (elapsed_ms / 1000UL) / STOPWATCH_HOUR_S;

    if(hours == 0) return StopwatchFormatMinutes;
    return (hours < 10) ? StopwatchFormatOneHourDigit : StopwatchFormatTwoHourDigits;
}

/** Only the widest band needs tightening; the others have room to spare. */
static int32_t busy_scene_stopwatch_tracking(StopwatchFormat format) {
    return (format == StopwatchFormatTwoHourDigits) ? STOPWATCH_TIGHT_TRACKING : 0;
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

    if(data->is_brightness_mode) {
        char brightness_text[STOPWATCH_TEXT_LEN];
        snprintf(brightness_text, sizeof(brightness_text), "%u%%", data->brightness_value);

        with_gui(instance->gui, {
            label_set_text(data->head, "");
            label_set_text(data->tail, brightness_text);
            widget_set_visible(label_get_base(data->head), false);
            widget_update_layout(label_get_base(data->tail));
            busy_scene_stopwatch_layout(data, 0, -1);

            const char* header = "BRIGHT";
            if(strcmp(data->card_header, header) != 0) {
                strncpy(data->card_header, header, sizeof(data->card_header) - 1);
                mirror_card_set_header_text(instance->timer_card, data->card_header);
            }
        });
        return;
    }

    if(data->is_reset_armed) {
        // Say what is about to happen rather than letting the count vanish silently.
        with_gui(instance->gui, {
            label_set_text(data->head, "");
            label_set_text(data->tail, "RESET");
            widget_set_visible(label_get_base(data->head), false);
            widget_update_layout(label_get_base(data->tail));
            busy_scene_stopwatch_layout(data, 0, -1);
        });
        return;
    }

    const StopwatchFormat format = busy_scene_stopwatch_format_for(data->state.elapsed_ms);
    const bool is_transition = (format != data->format);

    with_gui(instance->gui, {
        const int32_t previous_tail_x = data->tail_x;

        if(is_transition) {
            // The face never changes — only how tightly the characters are packed.
            const int32_t tracking = busy_scene_stopwatch_tracking(format);
            label_set_letter_spacing(data->head, tracking);
            label_set_letter_spacing(data->tail, tracking);
        }

        label_set_text(data->head, head_text);
        label_set_text(data->tail, tail_text);

        // Flush geometry before measuring. Straight after a text change the labels still
        // report their previous size, and on the scene's very first paint that is zero —
        // which centred a zero-width group and pushed the digits off the right edge until
        // the next tick corrected it.
        widget_update_layout(label_get_base(data->head));
        widget_update_layout(label_get_base(data->tail));

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
            // Restore the hours whenever the readout is not mid-growth. The brightness
            // and RESET screens hide the head, and without this it stayed hidden — while
            // its width went on being reserved, so the time also sat visibly off-centre.
            if(!data->is_head_pending) {
                widget_set_visible(label_get_base(data->head), head_text[0] != '\0');
            }
            busy_scene_stopwatch_layout(data, head_width, -1);
        }

        // Keep these short and only write them when they change. The mirror card's text
        // boxes are narrow; a string that overflows switches the label into its scrolling
        // long-content mode, and re-setting the text every second restarts that animation,
        // which on a 16-grey panel reads as smeared, doubled glyphs rather than motion.
        const char* header = data->state.is_running ? "ACTIVE" : "PAUSED";
        if(strcmp(data->card_header, header) != 0) {
            strncpy(data->card_header, header, sizeof(data->card_header) - 1);
            mirror_card_set_header_text(instance->timer_card, data->card_header);
        }

        char card_time[sizeof(data->card_time)];
        snprintf(card_time, sizeof(card_time), "%s%s", head_text, tail_text);
        if(strcmp(data->card_time, card_time) != 0) {
            strncpy(data->card_time, card_time, sizeof(data->card_time) - 1);
            mirror_card_set_footer_primary_text(instance->timer_card, data->card_time);
        }
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

// MARK: - Wheel

static uint8_t busy_scene_stopwatch_read_brightness(BusySceneStopwatch* data) {
    FuriState* fstate = brightness_control_get_state(data->brightness);
    BrightnessControlState state;
    furi_state_get(fstate, &state);
    return state.brightness_setting;
}

/** Nudge the display brightness and keep the on-screen number in step. */
static void busy_scene_stopwatch_step_brightness(BusyApp* instance, int32_t direction) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    int32_t next = (int32_t)data->brightness_value + direction * STOPWATCH_BRIGHTNESS_STEP;
    if(next < BRIGHTNESS_MIN) next = BRIGHTNESS_MIN;
    if(next > BRIGHTNESS_MAX) next = BRIGHTNESS_MAX;

    data->brightness_value = (uint8_t)next;
    brightness_control_set_manual_brightness(data->brightness, data->brightness_value);

    busy_scene_stopwatch_render(instance);
}

/**
 * Dial the starting time, faster while the wheel keeps moving.
 *
 * The service refuses this once the count has been started, so there is no need to
 * check here — a turn mid-measurement simply does nothing.
 */
static void busy_scene_stopwatch_step_offset(BusyApp* instance, int32_t direction) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    if(!data->state.can_adjust) return;

    const uint32_t now_ms = furi_get_tick();
    if(now_ms - data->adjust_last_ms > STOPWATCH_ADJUST_RAMP_GAP_MS) {
        data->adjust_run = 0;
    }
    data->adjust_last_ms = now_ms;
    data->adjust_run++;

    const int32_t minutes = (data->adjust_run > STOPWATCH_ADJUST_RAMP_AFTER) ?
                                STOPWATCH_ADJUST_COARSE_MN :
                                STOPWATCH_ADJUST_FINE_MN;

    stopwatch_adjust(instance->stopwatch, direction * minutes * 60 * 1000);
    // Read back rather than waiting for the tick: the count is not running, so no tick
    // is coming and the readout would sit stale until the next turn.
    stopwatch_get_state(instance->stopwatch, &data->state);
    busy_scene_stopwatch_render(instance);
}

static void busy_scene_stopwatch_toggle_brightness_mode(BusyApp* instance) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    data->is_brightness_mode = !data->is_brightness_mode;
    if(data->is_brightness_mode) {
        data->brightness_value = busy_scene_stopwatch_read_brightness(data);
    }

    busy_scene_stopwatch_render(instance);
}

/** One place deciding what a turn of the wheel currently means. */
static void busy_scene_stopwatch_handle_wheel(BusyApp* instance, int32_t direction) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    if(data->is_brightness_mode) {
        busy_scene_stopwatch_step_brightness(instance, direction);
    } else {
        busy_scene_stopwatch_step_offset(instance, direction);
    }
}

/** Put the time back and stop waiting for a confirmation. */
static void busy_scene_stopwatch_disarm(BusyApp* instance, bool repaint) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    if(!data->is_reset_armed) return;

    data->is_reset_armed = false;
    furi_event_loop_timer_stop(data->arm_timer);

    if(repaint) busy_scene_stopwatch_render(instance);
}

static void busy_scene_stopwatch_arm_timer_callback(void* context) {
    furi_assert(context);
    // No confirmation arrived, so the hold was almost certainly not deliberate.
    busy_scene_stopwatch_disarm(context, true);
}

/** Count a held Start/Pause towards arming, which only ever offers the reset. */
static void busy_scene_stopwatch_advance_hold(BusyApp* instance) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    // Keeping the button down past the arming point must do nothing further. The whole
    // point is that the confirming gesture has to be a separate one, and a steady press
    // can only ever produce the first.
    if(data->is_reset_armed) return;

    data->reset_hold++;

    if(data->reset_hold == STOPWATCH_RESET_ARM_REPEATS) {
        data->is_reset_armed = true;
        furi_event_loop_timer_start(data->arm_timer, STOPWATCH_RESET_ARM_TIMEOUT_MS);
        busy_scene_stopwatch_render(instance);
    }
}

/** Releasing only ends the hold; an armed reset stays up waiting to be confirmed. */
static void busy_scene_stopwatch_release_hold(BusyApp* instance) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);
    data->reset_hold = 0;
}

/** A tap confirms when armed, and starts or pauses the rest of the time. */
static void busy_scene_stopwatch_handle_tap(BusyApp* instance) {
    BusySceneStopwatch* data =
        scene_manager_get_scene_data(instance->scene_manager, BusyAppSceneIdStopwatch);

    if(data->is_reset_armed) {
        stopwatch_reset(instance->stopwatch);
        // Commands queue in order, so this blocking read returns the state *after* the
        // reset. Without it the repaint below would draw the stale pre-reset time for a
        // tick before the event arrived.
        stopwatch_get_state(instance->stopwatch, &data->state);
        busy_scene_stopwatch_disarm(instance, true);
    } else {
        stopwatch_toggle(instance->stopwatch);
    }
}

static bool busy_scene_stopwatch_input_callback(const InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    BusyApp* instance = context;

    bool consumed = false;
    BusyCustomEvent custom_event;

    if(event->key == InputKeyOk) {
        // Click the wheel to retask it to brightness, click again to put it back.
        if(event->type == InputTypeShort) {
            busy_send_custom_event(instance, BusyCustomEventStopwatchBrightnessMode);
            return true;
        }
        return false;
    }

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
            busy_send_custom_event(
                instance,
                (event->key == InputKeyUp) ? BusyCustomEventStopwatchWheelUp :
                                             BusyCustomEventStopwatchWheelDown);
            return true;
        }
        return false;
    }

    if(event->key != InputKeyStart) {
        // Reaching for another control is a good sign the hold was not meant as a reset.
        if(event->type == InputTypeShort || event->type == InputTypeLong) {
            busy_send_custom_event(instance, BusyCustomEventStopwatchHoldCancel);
        }
        return false;
    }

    {
        if(event->type == InputTypeShort) {
            custom_event = BusyCustomEventStopwatchToggle;
            consumed = true;

        } else if(event->type == InputTypeLong || event->type == InputTypeRepeat) {
            // Hold to zero it, the way a physical stopwatch works. Back is deliberately
            // left alone so leaving the screen can never destroy a measurement.
            custom_event = BusyCustomEventStopwatchHoldAdvance;
            consumed = true;

        } else if(event->type == InputTypeRelease) {
            custom_event = BusyCustomEventStopwatchHoldRelease;
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
    data->card_header[0] = '\0';
    data->card_time[0] = '\0';
    data->reset_hold = 0;
    data->is_reset_armed = false;
    data->is_brightness_mode = false;
    data->adjust_run = 0;
    data->adjust_last_ms = 0;
    data->brightness = furi_record_open(RECORD_BRIGHTNESS_CONTROL);
    data->brightness_value = busy_scene_stopwatch_read_brightness(data);

    with_gui(instance->gui, {
        GuiLayer* layer = gui_get_layer(instance->gui, GuiLayerIdMain);
        gui_layer_add_input_callback(layer, busy_scene_stopwatch_input_callback, instance);

        const Color white = (Color)COLOR_MAKE_HEX(0xFFFFFF);

        // AlignLeftMid makes LVGL centre each label vertically for us and turns pos_x
        // into an offset from the left edge. Positioning y by hand instead put the 14px
        // glyph box below the panel's 16 rows and clipped the bottom of every digit.
        data->head = label_alloc(instance->front_window);
        label_set_text_color(data->head, white);
        label_set_font(data->head, STOPWATCH_FONT);
        label_set_letter_spacing(data->head, busy_scene_stopwatch_tracking(data->format));
        widget_set_align(label_get_base(data->head), AlignLeftMid);

        data->tail = label_alloc(instance->front_window);
        label_set_text_color(data->tail, white);
        label_set_font(data->tail, STOPWATCH_FONT);
        label_set_letter_spacing(data->tail, busy_scene_stopwatch_tracking(data->format));
        widget_set_align(label_get_base(data->tail), AlignLeftMid);

        widget_set_visible(mirror_card_get_base(instance->timer_card), true);
        widget_set_visible(nav_bar_get_base(instance->nav_bar), false);
        mirror_card_set_show_header(instance->timer_card, true);
        mirror_card_set_show_footer(instance->timer_card, true);
        mirror_card_set_footer_secondary_text(instance->timer_card, "TOTAL");
    });

    data->reveal_timer = furi_event_loop_timer_alloc(
        instance->event_loop,
        busy_scene_stopwatch_reveal_timer_callback,
        FuriEventLoopTimerTypeOnce,
        instance);

    data->arm_timer = furi_event_loop_timer_alloc(
        instance->event_loop,
        busy_scene_stopwatch_arm_timer_callback,
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
    furi_event_loop_timer_free(data->arm_timer);
    furi_record_close(RECORD_BRIGHTNESS_CONTROL);

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
            busy_scene_stopwatch_handle_tap(instance);

        } else if(event->event == BusyCustomEventStopwatchHoldAdvance) {
            busy_scene_stopwatch_advance_hold(instance);

        } else if(event->event == BusyCustomEventStopwatchHoldRelease) {
            busy_scene_stopwatch_release_hold(instance);

        } else if(event->event == BusyCustomEventStopwatchHoldCancel) {
            busy_scene_stopwatch_disarm(instance, true);

        } else if(event->event == BusyCustomEventStopwatchBrightnessMode) {
            busy_scene_stopwatch_disarm(instance, false);
            busy_scene_stopwatch_toggle_brightness_mode(instance);

        } else if(event->event == BusyCustomEventStopwatchWheelUp) {
            busy_scene_stopwatch_handle_wheel(instance, +1);

        } else if(event->event == BusyCustomEventStopwatchWheelDown) {
            busy_scene_stopwatch_handle_wheel(instance, -1);
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
