#include "stopwatch_i.h"

/**
 * Commands cross from the caller's thread into the service's event loop rather than
 * touching state directly, so all mutation and every pubsub notification happen on one
 * thread. Same arrangement as BusyTimer's API.
 */
static void stopwatch_api_asynchronous_request(
    const Stopwatch* instance,
    StopwatchApiMessage* message) {
    message->lock = NULL;
    furi_check(
        furi_message_queue_put(instance->api_queue, message, FuriWaitForever) == FuriStatusOk);
}

static void stopwatch_api_blocking_request(
    const Stopwatch* instance,
    StopwatchApiMessage* message) {
    message->lock = api_lock_alloc_locked();
    furi_check(
        furi_message_queue_put(instance->api_queue, message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message->lock);
}

FuriPubSub* stopwatch_get_pubsub(const Stopwatch* instance) {
    furi_check(instance);
    return instance->event_pubsub;
}

void stopwatch_start(Stopwatch* instance) {
    furi_check(instance);

    StopwatchApiMessage message = {.type = StopwatchApiMessageTypeStart};
    stopwatch_api_asynchronous_request(instance, &message);
}

void stopwatch_pause(Stopwatch* instance) {
    furi_check(instance);

    StopwatchApiMessage message = {.type = StopwatchApiMessageTypePause};
    stopwatch_api_asynchronous_request(instance, &message);
}

void stopwatch_toggle(Stopwatch* instance) {
    furi_check(instance);

    StopwatchApiMessage message = {.type = StopwatchApiMessageTypeToggle};
    stopwatch_api_asynchronous_request(instance, &message);
}

void stopwatch_reset(Stopwatch* instance) {
    furi_check(instance);

    StopwatchApiMessage message = {.type = StopwatchApiMessageTypeReset};
    stopwatch_api_asynchronous_request(instance, &message);
}

void stopwatch_get_state(const Stopwatch* instance, StopwatchState* state) {
    furi_check(instance);
    furi_check(state);

    StopwatchApiMessage message = {
        .type = StopwatchApiMessageTypeGetState,
        .data.get_state = {.state = state},
    };
    stopwatch_api_blocking_request(instance, &message);
}
