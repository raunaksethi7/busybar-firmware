#include "settings.h"

#include <storage/storage.h>

#include <string.h>

#define DESKTOP_SETTINGS_FILE_PATH APP_DATA_PATH("settings.json")
#define DESKTOP_SETTINGS_VERSION   1
#define DESKTOP_SETTINGS_ROOT      desktop_v1_settings_root

bool desktop_settings_reset(DesktopSettings* settings) {
    SettingProvider* provider =
        setting_provider_alloc(DESKTOP_SETTINGS_FILE_PATH, DESKTOP_SETTINGS_VERSION, NULL, 0);
    bool is_successful = setting_provider_reset(provider, &DESKTOP_SETTINGS_ROOT, settings);
    setting_provider_free(provider);

    return is_successful;
}

bool desktop_settings_load(DesktopSettings* settings) {
    furi_check(settings);

    SettingProvider* provider =
        setting_provider_alloc(DESKTOP_SETTINGS_FILE_PATH, DESKTOP_SETTINGS_VERSION, NULL, 0);
    bool is_successful = setting_provider_load(provider, &DESKTOP_SETTINGS_ROOT, settings);
    setting_provider_free(provider);

    return is_successful;
}

bool desktop_settings_save(const DesktopSettings* settings) {
    furi_check(settings);

    SettingProvider* provider =
        setting_provider_alloc(DESKTOP_SETTINGS_FILE_PATH, DESKTOP_SETTINGS_VERSION, NULL, 0);
    bool is_successful = setting_provider_save(provider, &DESKTOP_SETTINGS_ROOT, settings);
    setting_provider_free(provider);

    return is_successful;
}

/** Maps a switch position onto its slot field. One place to keep the two in step. */
static char* desktop_settings_slot_field(DesktopSettings* settings, InputSwitchPosition pos) {
    switch(pos) {
    case InputSwitchPositionBusy:
        return settings->slot_busy;
    case InputSwitchPositionStatus:
        return settings->slot_status;
    case InputSwitchPositionOff:
        return settings->slot_off;
    case InputSwitchPositionApps:
        return settings->slot_apps;
    case InputSwitchPositionSettings:
        return settings->slot_settings;
    default:
        return NULL;
    }
}

const char* desktop_settings_get_slot(const DesktopSettings* settings, InputSwitchPosition pos) {
    furi_check(settings);

    // Cast away const only to reuse the field mapping; nothing is written here.
    const char* value = desktop_settings_slot_field((DesktopSettings*)settings, pos);
    if((value == NULL) || (value[0] == '\0')) return NULL;

    return value;
}

bool desktop_settings_set_slot(
    DesktopSettings* settings,
    InputSwitchPosition pos,
    const char* app_id) {
    furi_check(settings);

    char* field = desktop_settings_slot_field(settings, pos);
    if(field == NULL) return false;

    if((app_id == NULL) || (app_id[0] == '\0')) {
        field[0] = '\0';
        return true;
    }

    if(strlen(app_id) >= DESKTOP_SLOT_APP_MAX_SIZE) return false;

    strncpy(field, app_id, DESKTOP_SLOT_APP_MAX_SIZE - 1);
    field[DESKTOP_SLOT_APP_MAX_SIZE - 1] = '\0';

    return true;
}
