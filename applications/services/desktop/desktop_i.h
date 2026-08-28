#pragma once

#include "desktop.h"
#include "settings/settings.h"

/** An app to launch for a switch position, with an optional launch argument. */
typedef struct {
    const char* name;
    const char* args;
} DesktopDefaultApp;

#include <furi.h>

#include <input/input.h>
#include <loader/loader.h>

#include "desktop_overlay.h"
#include "desktop_start_request.h"

// Time to wait for the rotary switch steady state.
#define SWITCH_DELAY_MS   (100)
// Maximum and initial counts for synchronisation primitives
#define INPUT_QUEUE_COUNT (8)
#define START_QUEUE_COUNT (3)
#define EXIT_SEMAPH_COUNT (1)
#define EXIT_SEMAPH_INIT  (1)

#define DESKTOP_STARTUP_APP_NAME "Power On"

struct Desktop {
    FuriEventLoop* event_loop;
    FuriSemaphore* exit_semaphore;
    FuriMessageQueue* input_queue;
    FuriMessageQueue* start_queue;
    FuriEventLoopTimer* switch_timer;
    FuriEventLoopTimer* start_timer;
    FuriString* error_message;
    Loader* loader;
    DesktopOverlay* overlay;
    DesktopStartRequest* current_request;
    InputSwitchPosition switch_pos;
    /** Assignments of apps to switch positions; empty entries fall back to defaults. */
    DesktopSettings settings;
    /** Filled from the settings when an assignment is in force for the current position. */
    DesktopDefaultApp slot_app;
    DesktopSwitchDirection switch_direction;
    bool pin_current_app;
};
