#pragma once

#include <setting_provider.h>

/** Matches the appid length the loader accepts, plus a terminator. */
#define DESKTOP_SLOT_APP_MAX_SIZE (32 + 1)

/**
 * An empty override means "use the built-in default for this position".
 *
 * Stored per rotary-switch position, so any app on the device can claim a position
 * without the mapping being recompiled. Indices line up with ``InputSwitchPosition``.
 */
typedef enum {
    DesktopSettingV1IdxSlotBusy,
    DesktopSettingV1IdxSlotStatus,
    DesktopSettingV1IdxSlotOff,
    DesktopSettingV1IdxSlotApps,
    DesktopSettingV1IdxSlotSettings,

    DesktopSettingV1IdxsCount,
} DesktopSettingV1Idx;

typedef struct {
    char slot_busy[DESKTOP_SLOT_APP_MAX_SIZE];
    char slot_status[DESKTOP_SLOT_APP_MAX_SIZE];
    char slot_off[DESKTOP_SLOT_APP_MAX_SIZE];
    char slot_apps[DESKTOP_SLOT_APP_MAX_SIZE];
    char slot_settings[DESKTOP_SLOT_APP_MAX_SIZE];
} DesktopSettingsV1;

extern const SettingProviderSetting desktop_v1_settings[];
extern const SettingProviderSetting desktop_v1_settings_root;
