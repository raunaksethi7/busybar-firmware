/**
 * @file stopwatch.h
 * @brief A free-running count-up stopwatch.
 *
 * Deliberately independent of BusyTimer. The BUSY timer's lifetime is owned by the
 * busy app — `busy_free()` stops it — so anything built on it dies the moment the mode
 * selector moves away. This service owns its own event loop, starts with the system,
 * and is never stopped by a UI, so the count survives switching modes, entering
 * settings, opening apps, and the display being asleep. Only losing power stops it.
 *
 * State lives in RAM by design: a reboot is a power loss, so the count starts at zero.
 */
#pragma once

#include <core/pubsub.h>

#include <stdint.h>
#include <stdbool.h>

#define RECORD_STOPWATCH "stopwatch"

/** Wraps back to zero after a full day. */
#define STOPWATCH_ROLLOVER_MS (24UL * 60UL * 60UL * 1000UL)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Stopwatch Stopwatch;

typedef enum {
    /** The displayed second changed. Not emitted while paused. */
    StopwatchEventTypeTick,
    /** Started, paused, or reset. */
    StopwatchEventTypeStateChanged,
    StopwatchEventTypeMax,
} StopwatchEventType;

typedef struct {
    uint32_t elapsed_ms;
    bool is_running;
} StopwatchState;

typedef struct {
    StopwatchEventType type;
    StopwatchState state;
} StopwatchEvent;

/** Subscribe for ``StopwatchEvent`` updates. */
FuriPubSub* stopwatch_get_pubsub(const Stopwatch* instance);

/** Begin counting. No effect if already running. */
void stopwatch_start(Stopwatch* instance);

/** Stop counting, keeping the elapsed time. No effect if already paused. */
void stopwatch_pause(Stopwatch* instance);

/** Start if paused, pause if running. */
void stopwatch_toggle(Stopwatch* instance);

/** Return to zero and stop. The next ``stopwatch_start`` is what resumes counting. */
void stopwatch_reset(Stopwatch* instance);

/** Read the current state. Blocks briefly on the service thread. */
void stopwatch_get_state(const Stopwatch* instance, StopwatchState* state);

#ifdef __cplusplus
}
#endif
