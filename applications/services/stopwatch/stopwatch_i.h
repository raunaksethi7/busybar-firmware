#pragma once

#include "stopwatch.h"

#include <furi.h>

#include <toolbox/api_lock.h>

#define TAG "Stopwatch"

#define STOPWATCH_API_QUEUE_SIZE (4)

/**
 * Poll fast enough that a start or pause feels immediate and that the seconds digit
 * turns over close to the right moment, but slow enough to stay cheap while the
 * display is asleep and nobody is watching.
 */
#define STOPWATCH_POLL_PERIOD_MS (100)

typedef enum {
    StopwatchApiMessageTypeStart,
    StopwatchApiMessageTypePause,
    StopwatchApiMessageTypeToggle,
    StopwatchApiMessageTypeReset,
    StopwatchApiMessageTypeAdjust,
    StopwatchApiMessageTypeGetState,
    StopwatchApiMessageTypeMax,
} StopwatchApiMessageType;

typedef struct {
    StopwatchState* state;
} StopwatchApiMessageGetState;

typedef struct {
    int32_t delta_ms;
} StopwatchApiMessageAdjust;

typedef struct {
    StopwatchApiMessageType type;
    union {
        StopwatchApiMessageGetState get_state;
        StopwatchApiMessageAdjust adjust;
    } data;
    FuriApiLock lock;
} StopwatchApiMessage;

struct Stopwatch {
    FuriEventLoop* event_loop;
    FuriEventLoopTimer* poll_timer;
    FuriMessageQueue* api_queue;
    FuriPubSub* event_pubsub;

    uint32_t elapsed_ms;
    /** Device clock at the last accumulation, in milliseconds. Only valid while running. */
    uint32_t prev_tick_timestamp_ms;
    /** Whole seconds last published, so a tick fires only when the digits change. */
    uint32_t published_second;
    bool is_running;
    /** Not yet started since the last reset, so the elapsed time may still be dialled. */
    bool can_adjust;
};
