#include "http_api.h"

#include <stopwatch/stopwatch.h>

#define TAG "HttpStopwatch"

/**
 * The stopwatch is not a BusyTimer session, so it does not appear anywhere under
 * /api/busy. Without these endpoints a client polling the timer sees "nothing running"
 * while the bar is visibly counting.
 */

typedef struct {
    HttpHandlersList_t handlers;
} ApiStopwatchCtx;

#define API_STOPWATCH_JSON_LEN (96)

static void api_stopwatch_reply_state(struct mg_connection* conn) {
    Stopwatch* stopwatch = furi_record_open(RECORD_STOPWATCH);
    StopwatchState state;
    stopwatch_get_state(stopwatch, &state);
    furi_record_close(RECORD_STOPWATCH);

    char json[API_STOPWATCH_JSON_LEN];
    snprintf(
        json,
        sizeof(json),
        "{\"elapsed_ms\":%lu,\"is_running\":%s}",
        (unsigned long)state.elapsed_ms,
        state.is_running ? "true" : "false");

    MG_REPLY_OK_BODY(conn, json);
}

static bool api_stopwatch_state_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(msg);
    UNUSED(ctx);

    api_stopwatch_reply_state(conn);
    return true;
}

/** Commands are asynchronous, so settle before reading back the state we report. */
static void api_stopwatch_reply_after_command(struct mg_connection* conn) {
    furi_delay_ms(20);
    api_stopwatch_reply_state(conn);
}

static bool api_stopwatch_start_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(msg);
    UNUSED(ctx);

    Stopwatch* stopwatch = furi_record_open(RECORD_STOPWATCH);
    stopwatch_start(stopwatch);
    furi_record_close(RECORD_STOPWATCH);

    api_stopwatch_reply_after_command(conn);
    return true;
}

static bool api_stopwatch_pause_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(msg);
    UNUSED(ctx);

    Stopwatch* stopwatch = furi_record_open(RECORD_STOPWATCH);
    stopwatch_pause(stopwatch);
    furi_record_close(RECORD_STOPWATCH);

    api_stopwatch_reply_after_command(conn);
    return true;
}

static bool api_stopwatch_reset_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(msg);
    UNUSED(ctx);

    Stopwatch* stopwatch = furi_record_open(RECORD_STOPWATCH);
    stopwatch_reset(stopwatch);
    furi_record_close(RECORD_STOPWATCH);

    api_stopwatch_reply_after_command(conn);
    return true;
}

static const HttpHandler handlers_stopwatch[] = {
    {
        .uri = "start",
        .method = HttpMethodPost,
        .type = HttpHandlerCustom,
        .on_request = api_stopwatch_start_callback,
    },
    {
        .uri = "pause",
        .method = HttpMethodPost,
        .type = HttpHandlerCustom,
        .on_request = api_stopwatch_pause_callback,
    },
    {
        .uri = "reset",
        .method = HttpMethodPost,
        .type = HttpHandlerCustom,
        .on_request = api_stopwatch_reset_callback,
    },
    {
        .uri = "",
        .method = HttpMethodGet,
        .type = HttpHandlerCustom,
        .on_request = api_stopwatch_state_callback,
    },
};

void* http_api_stopwatch_alloc(void) {
    ApiStopwatchCtx* context = malloc(sizeof(ApiStopwatchCtx));

    HttpHandlersList_init(context->handlers);
    // Added in reverse: http_handler_add inserts at the front of the list, so this is
    // what preserves the table's order. It matters here because the last entry has an
    // empty URI and matches any path — added forward it would land first and swallow
    // every request, answering 405 to the command routes below it.
    for(size_t i = COUNT_OF(handlers_stopwatch); i > 0; --i) {
        http_handler_add(context->handlers, &handlers_stopwatch[i - 1]);
    }

    return context;
}

void http_api_stopwatch_free(void* ctx) {
    furi_assert(ctx);
    ApiStopwatchCtx* context = ctx;

    HttpHandlersList_clear(context->handlers);
    free(context);
}

bool http_api_stopwatch_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    ApiStopwatchCtx* context = ctx;
    return http_handle_request(path, method, context->handlers, conn, msg);
}
