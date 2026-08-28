#pragma once

#include "interface_v1.h"

#include <input/input.h>

typedef DesktopSettingsV1 DesktopSettings;

bool desktop_settings_reset(DesktopSettings* settings);
bool desktop_settings_load(DesktopSettings* settings);
bool desktop_settings_save(const DesktopSettings* settings);

/**
 * The app id assigned to a switch position, or NULL when none is.
 *
 * NULL means the caller should fall back to the built-in default for that position.
 */
const char* desktop_settings_get_slot(const DesktopSettings* settings, InputSwitchPosition pos);

/**
 * Assign an app to a switch position. Pass NULL or "" to clear the assignment and go
 * back to the built-in default.
 *
 * @returns false if the position is out of range or the id is too long to store
 */
bool desktop_settings_set_slot(
    DesktopSettings* settings,
    InputSwitchPosition pos,
    const char* app_id);
