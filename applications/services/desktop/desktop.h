/**
 * @file desktop.h
 *
 * @brief Functions for controlling the currently running application.
 *
 * Normally, only one application can be run and visible on the displays (except for services, which
 * may or may not draw something on the display(s)). Desktop service responds to the rotary switch position,
 * additionally, it allows overriding the current app programmatically (e.g. starting from the Apps menu).
 */
#pragma once

#include <input/input.h>

#include <stdbool.h>

/**
 * @brief Record key to get the Desktop instance.
 */
#define RECORD_DESKTOP "desktop"

/**
 * @brief Opaque declaration for the Desktop type.
 */
typedef struct Desktop Desktop;

/**
 * @brief Direction of rotary switch movement.
 */
typedef enum {
    DesktopSwitchDirectionUp, /**< Switch moved up */
    DesktopSwitchDirectionDown, /**< Switch moved down */

    DesktopSwitchDirectionsCount,
} DesktopSwitchDirection;

/**
 * @brief Request the Desktop service to replace the currently running app.
 *
 * Calling this function will merely schedule the request, not actually start the
 * application. It may be overridden at any point in time by the rotary switch or
 * further calls to this function.
 *
 * @note Both application name and arguments are copied, so they may be deleted
 *       immediately after calling this function.
 *
 * @param[in,out] instance pointer to the Desktop instance
 * @param[in] name app name or ID to replace the current app with
 * @param[in,out] args pointer to a zero-terminated string containing application arguments
 * @returns true if the request has been scheduled, false otherwise
 */
bool desktop_replace_current_app(Desktop* instance, const char* name, const char* args);

/**
 * @brief Pin or unpin the currently running app.
 *
 * Calling this function will prevent the Desktop from changing the currently
 * running app in response to flicking the rotary switch, or, conversely,
 * enable this behaviour, depending on the pin parameter value.
 *
 * @warning This function is intended for testing purposes only.
 *
 * @param[in,out] instance pointer to the Desktop instance
 * @param[in] pin pin current app if true, do not pin if false
 */
void desktop_pin_current_app(Desktop* instance, bool pin);

/**
 * @brief Get the direction of the last rotary switch movement.
 *
 * Returns the direction the rotary switch was last moved in, which is used
 * The direction is calculated based on comparing the previous switch position
 * with the new position.
 *
 * @param[in] instance pointer to the Desktop instance
 * @returns DesktopSwitchDirectionUp if switch was last moved up,
 *          DesktopSwitchDirectionDown if switch was last moved down
 */
DesktopSwitchDirection desktop_get_switch_direction(Desktop* instance);

/**
 * @brief Get the app assigned to a rotary switch position.
 *
 * @param[in] instance pointer to the Desktop instance
 * @param[in] pos switch position to query
 * @returns the assigned app id, or NULL when the position uses its built-in default
 */
const char* desktop_get_slot_app(Desktop* instance, InputSwitchPosition pos);

/**
 * @brief Assign an app to a rotary switch position.
 *
 * The assignment is stored and survives a restart. Pass NULL or an empty string to clear
 * it and go back to the built-in default. Takes effect the next time the switch lands on
 * that position, so moving the switch away and back applies it.
 *
 * @param[in,out] instance pointer to the Desktop instance
 * @param[in] pos switch position to assign
 * @param[in] app_id id of the app to launch there, or NULL to clear
 * @returns false if the position is invalid, the id is too long, or saving failed
 */
bool desktop_set_slot_app(Desktop* instance, InputSwitchPosition pos, const char* app_id);
