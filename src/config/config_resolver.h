/**
 * @file config_resolver.h
 * @brief Runtime configuration resolution with precedence handling
 *
 * Resolves configuration values using the following precedence:
 *   1. CLI arguments (highest)
 *   2. Environment variables
 *   3. Configuration file
 *   4. Compiled defaults (lowest)
 *
 * See: docs/decisions/DR-001-port-allocation.md
 */

#ifndef CONFIG_RESOLVER_H
#define CONFIG_RESOLVER_H

#include "config_defaults.h"

/**
 * Resolve HTTP port using precedence: CLI > ENV > DEFAULT
 *
 * @param cli_port Port specified via CLI (-1 if not specified)
 * @return Valid port number (1-65535) or -1 on error
 *
 * Resolution order:
 *   1. If cli_port is valid (1-65535), use it
 *   2. Else check WT_HTTP_PORT environment variable
 *   3. Else use WT_HTTP_PORT_DEFAULT (9081)
 *
 * Logs the source of the resolved value (CLI/ENV/DEFAULT)
 */
int config_resolve_http_port(int cli_port);

/**
 * Configuration source tracking for diagnostics
 */
typedef enum {
    CONFIG_SOURCE_DEFAULT = 0,
    CONFIG_SOURCE_FILE,
    CONFIG_SOURCE_ENV,
    CONFIG_SOURCE_CLI
} config_source_t;

/**
 * Get human-readable name for configuration source
 */
const char* config_source_name(config_source_t source);

#endif /* CONFIG_RESOLVER_H */
