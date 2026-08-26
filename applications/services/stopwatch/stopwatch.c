#include "stopwatch_i.h"

#include <furi_hal_rtc.h>

/**
 * Seed the count at boot. A demo/debug seam so a long elapsed time can be put on the
 * hardware without waiting for it; 0 in any real build.
 */
#ifndef STOPWATCH_DEBUG_SEED_MS
#define STOPWATCH_DEBUG_SEED_MS (0UL)
#endif

// MARK: - State

static void stopwatch_notify(const Stopwatch* instance, StopwatchEventType type) {
    StopwatchEvent event = {
        .type = type,
        .state =
            {
                .elapsed_ms = instance->elapsed_ms,
                .is_running = instance->is_running,
                .can_adjust = instance->can_adjust,
            },
    };

    furi_pubsub_publish(instance->event_pubsub, &event);
}

/**
 * Fold the time since the last poll into the total.
 *
 * Accumulates from device-clock deltas rather than counting poll callbacks, so a late
 * or dropped tick — which happens whenever something heavier is running — does not
 * lose time. The clock can also jump backwards when the system time is corrected from
 * the network; treat that as a zero-length interval instead of subtracting.
 */
static void stopwatch_accumulate(Stopwatch* instance) {
    const uint32_t now_ms = furi_hal_rtc_get_timestamp_ms();

    if(now_ms < instance->prev_tick_timestamp_ms) {
        instance->prev_tick_timestamp_ms = now_ms;
        return;
    }

    const uint32_t delta_ms = now_ms - instance->prev_tick_timestamp_ms;
    instance->prev_tick_timestamp_ms = now_ms;

    instance->elapsed_ms += delta_ms;

    if(instance->elapsed_ms >= STOPWATCH_ROLLOVER_MS) {
        instance->elapsed_ms %= STOPWATCH_ROLLOVER_MS;
        // A rollover changes the reading by a whole day, so make sure the next
        // comparison sees a difference and republishes even if the second matches.
        instance->published_second = UINT32_MAX;
    }
}

static void stopwatch_poll_timer_callback(void* context) {
    furi_assert(context);
    Stopwatch* instance = context;

    if(!instance->is_running) return;

    stopwatch_accumulate(instance);

    const uint32_t second = instance->elapsed_ms / 1000;
    if(second != instance->published_second) {
        instance->published_second = second;
        stopwatch_notify(instance, StopwatchEventTypeTick);
    }
}

// MARK: - Commands

static void stopwatch_handle_start(Stopwatch* instance) {
    if(instance->is_running) return;

    instance->is_running = true;
    // Starting fixes the number: from here a stray turn of the wheel must not move it.
    instance->can_adjust = false;
    instance->prev_tick_timestamp_ms = furi_hal_rtc_get_timestamp_ms();
    furi_event_loop_timer_start(instance->poll_timer, STOPWATCH_POLL_PERIOD_MS);

    stopwatch_notify(instance, StopwatchEventTypeStateChanged);
}

static void stopwatch_handle_pause(Stopwatch* instance) {
    if(!instance->is_running) return;

    // Fold in the final partial interval before stopping, or it is lost.
    stopwatch_accumulate(instance);

    instance->is_running = false;
    furi_event_loop_timer_stop(instance->poll_timer);

    stopwatch_notify(instance, StopwatchEventTypeStateChanged);
}

static void stopwatch_handle_toggle(Stopwatch* instance) {
    if(instance->is_running) {
        stopwatch_handle_pause(instance);
    } else {
        stopwatch_handle_start(instance);
    }
}

static void stopwatch_handle_reset(Stopwatch* instance) {
    instance->elapsed_ms = 0;
    instance->published_second = 0;

    // Back to the power-on state: zero *and* stopped. Leaving it running would start
    // counting again the instant it was cleared, which is not what reset means on any
    // stopwatch — the next press is what starts it.
    if(instance->is_running) {
        instance->is_running = false;
        furi_event_loop_timer_stop(instance->poll_timer);
    }

    // Back to a dialable count, so a measurement can be picked up where it left off.
    instance->can_adjust = true;

    stopwatch_notify(instance, StopwatchEventTypeStateChanged);
}

static void stopwatch_handle_adjust(Stopwatch* instance, int32_t delta_ms) {
    if(!instance->can_adjust) return;

    // Clamp rather than wrap. Turning down past zero should stop at zero, not jump to
    // the far end of the day.
    int64_t next = (int64_t)instance->elapsed_ms + delta_ms;
    if(next < 0) next = 0;
    if(next >= (int64_t)STOPWATCH_ROLLOVER_MS) next = (int64_t)STOPWATCH_ROLLOVER_MS - 1000;

    instance->elapsed_ms = (uint32_t)next;
    instance->published_second = instance->elapsed_ms / 1000;

    stopwatch_notify(instance, StopwatchEventTypeStateChanged);
}

static void stopwatch_handle_get_state(Stopwatch* instance, StopwatchState* state) {
    // Include time accrued since the last poll so a caller never reads a stale value.
    if(instance->is_running) {
        stopwatch_accumulate(instance);
    }

    state->elapsed_ms = instance->elapsed_ms;
    state->is_running = instance->is_running;
    state->can_adjust = instance->can_adjust;
}

static void stopwatch_message_queue_callback(FuriEventLoopObject* object, void* context) {
    UNUSED(object);
    furi_assert(context);

    Stopwatch* instance = context;
    StopwatchApiMessage message;

    furi_check(
        furi_message_queue_get(instance->api_queue, &message, 0) == FuriStatusOk);

    switch(message.type) {
    case StopwatchApiMessageTypeStart:
        stopwatch_handle_start(instance);
        break;
    case StopwatchApiMessageTypePause:
        stopwatch_handle_pause(instance);
        break;
    case StopwatchApiMessageTypeToggle:
        stopwatch_handle_toggle(instance);
        break;
    case StopwatchApiMessageTypeReset:
        stopwatch_handle_reset(instance);
        break;
    case StopwatchApiMessageTypeAdjust:
        stopwatch_handle_adjust(instance, message.data.adjust.delta_ms);
        break;
    case StopwatchApiMessageTypeGetState:
        stopwatch_handle_get_state(instance, message.data.get_state.state);
        break;
    default:
        furi_crash("Unknown Stopwatch API message");
    }

    if(message.lock) {
        api_lock_unlock(message.lock);
    }
}

// MARK: - Service

static Stopwatch* stopwatch_alloc(void) {
    Stopwatch* instance = malloc(sizeof(Stopwatch));

    instance->event_loop = furi_event_loop_alloc();
    instance->poll_timer = furi_event_loop_timer_alloc(
        instance->event_loop,
        stopwatch_poll_timer_callback,
        FuriEventLoopTimerTypePeriodic,
        instance);
    instance->api_queue =
        furi_message_queue_alloc(STOPWATCH_API_QUEUE_SIZE, sizeof(StopwatchApiMessage));
    instance->event_pubsub = furi_pubsub_alloc();

    instance->elapsed_ms = STOPWATCH_DEBUG_SEED_MS;
    instance->prev_tick_timestamp_ms = 0;
    instance->published_second = instance->elapsed_ms / 1000;
    instance->is_running = false;
    instance->can_adjust = true;

    furi_event_loop_subscribe_message_queue(
        instance->event_loop,
        instance->api_queue,
        FuriEventLoopEventIn,
        stopwatch_message_queue_callback,
        instance);

    furi_record_create(RECORD_STOPWATCH, instance);

    return instance;
}

int stopwatch_srv(void* arg) {
    UNUSED(arg);

    Stopwatch* instance = stopwatch_alloc();
    furi_event_loop_run(instance->event_loop);

    return 0;
}
