/**
 * @file config_resolver.c
 * @brief Runtime configuration resolution implementation
 */

#include "config_resolver.h"
#include "utils/logger.h"
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

const char* config_source_name(config_source_t source)
{
    switch (source) {
        case CONFIG_SOURCE_CLI:     return "CLI";
        case CONFIG_SOURCE_ENV:     return "ENV";
        case CONFIG_SOURCE_FILE:    return "FILE";
        case CONFIG_SOURCE_DEFAULT: return "DEFAULT";
        default:                    return "UNKNOWN";
    }
}

int config_resolve_http_port(int cli_port)
{
    /* CLI takes highest precedence */
    if (cli_port >= WT_HTTP_PORT_MIN && cli_port <= WT_HTTP_PORT_MAX) {
        LOG_INFO("HTTP port: %d (from %s)", cli_port,
                 config_source_name(CONFIG_SOURCE_CLI));
        return cli_port;
    }

    /* Check environment variable */
    const char *env_val = getenv(WT_HTTP_PORT_ENV);
    if (env_val != NULL && env_val[0] != '\0') {
        char *endptr;
        errno = 0;
        long port = strtol(env_val, &endptr, 10);

        if (errno == 0 && *endptr == '\0' &&
            port >= WT_HTTP_PORT_MIN && port <= WT_HTTP_PORT_MAX) {
            LOG_INFO("HTTP port: %ld (from %s=%s)",
                     port, WT_HTTP_PORT_ENV, env_val);
            return (int)port;
        }

        LOG_WARNING("Invalid %s value '%s' (must be %d-%d), using default",
                    WT_HTTP_PORT_ENV, env_val,
                    WT_HTTP_PORT_MIN, WT_HTTP_PORT_MAX);
    }

    /* Fall back to compiled default */
    LOG_INFO("HTTP port: %d (from %s)", WT_HTTP_PORT_DEFAULT,
             config_source_name(CONFIG_SOURCE_DEFAULT));
    return WT_HTTP_PORT_DEFAULT;
}
