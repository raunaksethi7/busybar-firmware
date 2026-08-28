#include "http_api.h"

#include <desktop/desktop.h>

#define TAG "HttpSlots"

/**
 * Assigning apps to rotary-switch positions.
 *
 * The mapping used to be a static table compiled into the desktop service, so which app a
 * position launched could only be changed by rebuilding. These endpoints make it data, so
 * an app written for this device can claim a position without touching the firmware.
 *
 * GET  /api/slots            -> the current assignment for every position
 * PUT  /api/slots            -> {"slot": "custom", "app": "clock"}; app null/"" clears it
 */

typedef struct {
    HttpHandlersList_t handlers;
} ApiSlotsCtx;

#define API_SLOTS_JSON_LEN (512)

/** Wire names for the switch positions, matching the labels on the device. */
static const struct {
    const char* name;
    InputSwitchPosition pos;
} api_slot_names[] = {
    {"busy", InputSwitchPositionBusy},
    {"custom", InputSwitchPositionStatus},
    {"off", InputSwitchPositionOff},
    {"apps", InputSwitchPositionApps},
    {"settings", InputSwitchPositionSettings},
};

static bool api_slots_find(const char* name, InputSwitchPosition* out) {
    for(size_t i = 0; i < COUNT_OF(api_slot_names); ++i) {
        if(strcmp(name, api_slot_names[i].name) == 0) {
            *out = api_slot_names[i].pos;
            return true;
        }
    }
    return false;
}

static void api_slots_reply_all(struct mg_connection* conn) {
    Desktop* desktop = furi_record_open(RECORD_DESKTOP);

    FuriString* json = furi_string_alloc_set("{");
    for(size_t i = 0; i < COUNT_OF(api_slot_names); ++i) {
        const char* assigned = desktop_get_slot_app(desktop, api_slot_names[i].pos);
        furi_string_cat_printf(
            json, "%s\"%s\":", (i == 0) ? "" : ",", api_slot_names[i].name);
        if(assigned) {
            furi_string_cat_printf(json, "\"%s\"", assigned);
        } else {
            // null rather than "" so a caller can tell "built-in default" from "assigned
            // to an app whose id happens to be empty".
            furi_string_cat_str(json, "null");
        }
    }
    furi_string_cat_str(json, "}");

    furi_record_close(RECORD_DESKTOP);

    MG_REPLY_OK_BODY(conn, furi_string_get_cstr(json));
    furi_string_free(json);
}

static bool api_slots_get_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(msg);
    UNUSED(ctx);

    api_slots_reply_all(conn);
    return true;
}

static bool api_slots_set_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    UNUSED(path);
    UNUSED(method);
    UNUSED(ctx);

    char* slot_name = mg_json_get_str(msg->body, "$.slot");
    if(slot_name == NULL) {
        MG_REPLY_BAD_REQUEST(conn);
        return true;
    }

    InputSwitchPosition pos;
    const bool is_known = api_slots_find(slot_name, &pos);
    free(slot_name);

    if(!is_known) {
        MG_REPLY_BAD_REQUEST(conn);
        return true;
    }

    // A missing or null "app" clears the assignment, which is how a position is put back
    // to its built-in default.
    char* app_id = mg_json_get_str(msg->body, "$.app");

    Desktop* desktop = furi_record_open(RECORD_DESKTOP);
    const bool is_success = desktop_set_slot_app(desktop, pos, app_id);
    furi_record_close(RECORD_DESKTOP);

    if(app_id) free(app_id);

    if(is_success) {
        api_slots_reply_all(conn);
    } else {
        MG_REPLY_BAD_REQUEST(conn);
    }

    return true;
}

/**
 * One handler for both methods, branching inside.
 *
 * Two entries would not work: an empty URI prefix-matches every path, so whichever landed
 * first in the list would answer for both methods and reject the other with 405.
 */
static bool api_slots_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    if(method == HttpMethodPut) {
        return api_slots_set_callback(path, method, conn, msg, ctx);
    }
    return api_slots_get_callback(path, method, conn, msg, ctx);
}

static const HttpHandler handlers_slots[] = {
    {
        .uri = "",
        .method = HttpMethodGet | HttpMethodPut,
        .type = HttpHandlerCustom,
        .on_request = api_slots_callback,
    },
};

void* http_api_slots_alloc(void) {
    ApiSlotsCtx* context = malloc(sizeof(ApiSlotsCtx));

    HttpHandlersList_init(context->handlers);
    // Added in reverse, matching api_root.c: http_handler_add inserts at the front, so
    // this is what preserves the table's order.
    for(size_t i = COUNT_OF(handlers_slots); i > 0; --i) {
        http_handler_add(context->handlers, &handlers_slots[i - 1]);
    }

    return context;
}

void http_api_slots_free(void* ctx) {
    furi_assert(ctx);
    ApiSlotsCtx* context = ctx;

    HttpHandlersList_clear(context->handlers);
    free(context);
}

bool http_api_slots_callback(
    FuriString* path,
    HttpMethod method,
    struct mg_connection* conn,
    struct mg_http_message* msg,
    void* ctx) {
    ApiSlotsCtx* context = ctx;
    return http_handle_request(path, method, context->handlers, conn, msg);
}
