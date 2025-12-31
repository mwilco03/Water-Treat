/**
 * @file config_defaults.h
 * @brief Central configuration defaults for Water-Treat RTU
 *
 * All configurable defaults are defined here. Override via:
 *   1. Command-line arguments (highest precedence)
 *   2. Environment variables (WT_* prefix)
 *   3. Configuration file
 *   4. Compiled defaults (this file)
 *
 * See: docs/decisions/DR-001-port-allocation.md for port allocation rationale
 */

#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

/* ============================================================================
 * HTTP Server Configuration
 * ============================================================================
 * Port allocation scheme:
 *   8xxx = Controller plane (Water-Controller on SBC #1)
 *   9xxx = RTU plane (Water-Treat on SBC #2)
 *
 * This prevents port conflicts when running both on a single device.
 */
#ifndef WT_HTTP_PORT_DEFAULT
#define WT_HTTP_PORT_DEFAULT    9081
#endif

#define WT_HTTP_PORT_MIN        1
#define WT_HTTP_PORT_MAX        65535

/* Environment variable names - all use WT_ prefix for consistency */
#define WT_HTTP_PORT_ENV        "WT_HTTP_PORT"

/* ============================================================================
 * Future Configuration Options
 * ============================================================================
 * Add other WT_ prefixed defaults here as the project grows.
 * Follow the same pattern: DEFAULT, MIN, MAX, ENV name.
 *
 * Examples for future use:
 * #define WT_LOG_LEVEL_ENV      "WT_LOG_LEVEL"
 * #define WT_INTERFACE_ENV      "WT_INTERFACE"
 * #define WT_CYCLE_TIME_ENV     "WT_CYCLE_TIME"
 */

#endif /* CONFIG_DEFAULTS_H */
