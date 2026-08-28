#include "interface_v1.h"

#define SLOT_DEFAULT ""

#define SLOT_SETTING(idx, key, member)                                            \
    [idx] = {                                                                     \
        .name = key,                                                              \
        .interface =                                                              \
            &(const SettingProviderStringInterface){                              \
                .default_value = SLOT_DEFAULT,                                    \
                .max_size = SIZEOF_MEMBER(DesktopSettingsV1, member),             \
            },                                                                    \
        .field_offset = offsetof(DesktopSettingsV1, member),                      \
        .type = SettingProviderSettingTypeString,                                 \
    }

const SettingProviderSetting desktop_v1_settings[] = {
    SLOT_SETTING(DesktopSettingV1IdxSlotBusy, "slot_busy", slot_busy),
    SLOT_SETTING(DesktopSettingV1IdxSlotStatus, "slot_status", slot_status),
    SLOT_SETTING(DesktopSettingV1IdxSlotOff, "slot_off", slot_off),
    SLOT_SETTING(DesktopSettingV1IdxSlotApps, "slot_apps", slot_apps),
    SLOT_SETTING(DesktopSettingV1IdxSlotSettings, "slot_settings", slot_settings),
};

const SettingProviderSetting desktop_v1_settings_root = {
    .name = NULL,
    .interface =
        &(const SettingProviderStructInterface){
            .inner_settings = desktop_v1_settings,
            .inner_settings_count = COUNT_OF(desktop_v1_settings),
        },
    .field_offset = 0,
    .type = SettingProviderSettingTypeStruct,
};

static_assert(COUNT_OF(desktop_v1_settings) == DesktopSettingV1IdxsCount);
