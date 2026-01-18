/**
 * @file controller_discovery.h
 * @brief Controller discovery for bidirectional RTU-Controller awareness
 *
 * Implements multiple discovery mechanisms:
 * 1. Passive: Capture controller IP when PROFINET connection established
 * 2. mDNS/Avahi: Discover controllers advertising on network
 * 3. Manual: Use configured controller_ip from config file
 *
 * This allows RTUs to know which controller they belong to, complementing
 * the controller's ability to register RTUs.
 */

#ifndef CONTROLLER_DISCOVERY_H
#define CONTROLLER_DISCOVERY_H

#include "common.h"
#include "config/config.h"

#define CONTROLLER_IP_LEN 16
#define CONTROLLER_NAME_LEN MAX_NAME_LEN

/**
 * Discovery source - how controller was found
 */
typedef enum {
    DISCOVERY_SOURCE_NONE = 0,      /* Not discovered */
    DISCOVERY_SOURCE_CONFIG,        /* From config file */
    DISCOVERY_SOURCE_PROFINET,      /* From active PROFINET connection */
    DISCOVERY_SOURCE_MDNS,          /* From mDNS/Avahi discovery */
    DISCOVERY_SOURCE_MANUAL         /* Manually set via API */
} discovery_source_t;

/**
 * Discovered controller information
 */
typedef struct {
    char ip[CONTROLLER_IP_LEN];           /* Controller IP address */
    char name[CONTROLLER_NAME_LEN];       /* Controller name/station name */
    discovery_source_t source;            /* How it was discovered */
    uint64_t discovered_at;               /* Timestamp of discovery (ms) */
    uint64_t last_seen;                   /* Last communication timestamp */
    bool connected;                       /* Currently connected */
    uint32_t arep;                        /* PROFINET AREP when connected */
} discovered_controller_t;

/**
 * Callback when controller is discovered or changes
 */
typedef void (*controller_discovered_cb_t)(const discovered_controller_t *controller, void *ctx);

/**
 * Initialize controller discovery
 *
 * @param config   PROFINET configuration (may contain pre-configured controller)
 * @return RESULT_OK on success
 */
result_t controller_discovery_init(const profinet_config_t *config);

/**
 * Shutdown controller discovery
 */
void controller_discovery_shutdown(void);

/**
 * Start mDNS discovery (non-blocking)
 *
 * Starts background thread to discover controllers via mDNS.
 * Results are delivered via callback set by controller_discovery_set_callback().
 *
 * @return RESULT_OK if started, RESULT_NOT_SUPPORTED if Avahi unavailable
 */
result_t controller_discovery_start_mdns(void);

/**
 * Stop mDNS discovery
 */
void controller_discovery_stop_mdns(void);

/**
 * Notify that a PROFINET controller connected
 *
 * Called by profinet_manager when connection is established.
 * Will attempt to resolve controller IP from AREP/network layer.
 *
 * @param arep   Application Relationship Endpoint from connection
 * @return RESULT_OK if controller info captured
 */
result_t controller_discovery_on_connect(uint32_t arep);

/**
 * Notify that PROFINET controller disconnected
 *
 * @param arep   AREP of disconnected controller
 */
void controller_discovery_on_disconnect(uint32_t arep);

/**
 * Get currently discovered/configured controller
 *
 * @param controller   Output structure (copied)
 * @return RESULT_OK if controller known, RESULT_NOT_FOUND if none
 */
result_t controller_discovery_get(discovered_controller_t *controller);

/**
 * Manually set controller information
 *
 * @param ip     Controller IP address
 * @param name   Controller name (optional, can be NULL)
 * @return RESULT_OK on success
 */
result_t controller_discovery_set(const char *ip, const char *name);

/**
 * Register callback for discovery events
 *
 * @param callback   Function to call when controller discovered/changed
 * @param ctx        User context passed to callback
 */
void controller_discovery_set_callback(controller_discovered_cb_t callback, void *ctx);

/**
 * Get source name as string for display
 */
const char* discovery_source_to_string(discovery_source_t source);

#endif /* CONTROLLER_DISCOVERY_H */
