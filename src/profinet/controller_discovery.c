/**
 * @file controller_discovery.c
 * @brief Controller discovery implementation
 *
 * Implements bidirectional controller discovery allowing RTUs to know
 * which controller they're connected to or should connect to.
 */

#include "controller_discovery.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

/* Optional Avahi support for mDNS discovery */
#ifdef HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>
#endif

/* mDNS service type for PROFINET controllers */
#define MDNS_SERVICE_TYPE "_profinet-controller._tcp"

/* State */
static struct {
    discovered_controller_t controller;
    pthread_mutex_t mutex;
    bool initialized;

    /* Callback */
    controller_discovered_cb_t callback;
    void *callback_ctx;

    /* mDNS state */
#ifdef HAVE_AVAHI
    AvahiSimplePoll *avahi_poll;
    AvahiClient *avahi_client;
    AvahiServiceBrowser *avahi_browser;
    pthread_t mdns_thread;
    volatile bool mdns_running;
#endif
} g_disc = {0};

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

static void notify_callback(void) {
    if (g_disc.callback) {
        g_disc.callback(&g_disc.controller, g_disc.callback_ctx);
    }
}

/**
 * Try to get peer IP from network connections
 *
 * PROFINET uses Ethernet frames directly, but we can check the ARP/neighbor
 * cache for recently communicated peers on the PROFINET interface.
 */
static result_t resolve_controller_ip_from_network(char *ip_buf, size_t ip_buf_size) {
    /* Read ARP cache to find potential controller IP */
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) {
        return RESULT_IO_ERROR;
    }

    char line[256];
    char found_ip[16] = {0};

    /* Skip header line */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return RESULT_NOT_FOUND;
    }

    /*
     * ARP cache format:
     * IP address       HW type     Flags       HW address            Mask     Device
     * 192.168.1.1      0x1         0x2         aa:bb:cc:dd:ee:ff     *        eth0
     *
     * We look for entries on eth0/end0 that are not the gateway.
     * In a PROFINET setup, the controller should be in the ARP cache.
     */
    while (fgets(line, sizeof(line), f)) {
        char ip[16], hw[20], dev[16];
        int hw_type, flags;

        if (sscanf(line, "%15s 0x%x 0x%x %19s %*s %15s",
                   ip, &hw_type, &flags, hw, dev) >= 5) {
            /* Look for entries on typical PROFINET interfaces */
            if ((strncmp(dev, "eth", 3) == 0 ||
                 strncmp(dev, "end", 3) == 0 ||
                 strncmp(dev, "enp", 3) == 0) &&
                flags & 0x2) {  /* Complete entry */

                /* Skip common gateway addresses */
                if (strstr(ip, ".1") == ip + strlen(ip) - 2) {
                    continue;  /* Likely gateway (x.x.x.1) */
                }

                SAFE_STRNCPY(found_ip, ip, sizeof(found_ip));
                /* Keep looking - last entry is often most recent */
            }
        }
    }

    fclose(f);

    if (found_ip[0] != '\0') {
        SAFE_STRNCPY(ip_buf, found_ip, ip_buf_size);
        return RESULT_OK;
    }

    return RESULT_NOT_FOUND;
}

#ifdef HAVE_AVAHI
/* ============================================================================
 * Avahi/mDNS Discovery
 * ========================================================================== */

static void avahi_resolve_callback(
    AvahiServiceResolver *r,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char *host_name,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags flags,
    void *userdata)
{
    UNUSED(interface); UNUSED(protocol); UNUSED(type);
    UNUSED(domain); UNUSED(port); UNUSED(txt);
    UNUSED(flags); UNUSED(userdata);

    if (event == AVAHI_RESOLVER_FOUND) {
        char ip[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(ip, sizeof(ip), address);

        pthread_mutex_lock(&g_disc.mutex);

        /* Update controller info */
        SAFE_STRNCPY(g_disc.controller.ip, ip, sizeof(g_disc.controller.ip));
        if (name) {
            SAFE_STRNCPY(g_disc.controller.name, name, sizeof(g_disc.controller.name));
        } else if (host_name) {
            SAFE_STRNCPY(g_disc.controller.name, host_name, sizeof(g_disc.controller.name));
        }
        g_disc.controller.source = DISCOVERY_SOURCE_MDNS;
        g_disc.controller.discovered_at = get_time_ms();
        g_disc.controller.last_seen = g_disc.controller.discovered_at;

        LOG_INFO("mDNS: Discovered controller '%s' at %s",
                 g_disc.controller.name, g_disc.controller.ip);

        notify_callback();

        pthread_mutex_unlock(&g_disc.mutex);
    }

    avahi_service_resolver_free(r);
}

static void avahi_browse_callback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AvahiLookupResultFlags flags,
    void *userdata)
{
    UNUSED(b); UNUSED(flags); UNUSED(userdata);

    switch (event) {
        case AVAHI_BROWSER_NEW:
            LOG_DEBUG("mDNS: Found service '%s' type '%s' domain '%s'", name, type, domain);
            avahi_service_resolver_new(g_disc.avahi_client, interface, protocol,
                                       name, type, domain, AVAHI_PROTO_UNSPEC, 0,
                                       avahi_resolve_callback, NULL);
            break;

        case AVAHI_BROWSER_REMOVE:
            LOG_DEBUG("mDNS: Service '%s' removed", name);
            break;

        case AVAHI_BROWSER_FAILURE:
            LOG_ERROR("mDNS browser failure: %s",
                     avahi_strerror(avahi_client_errno(g_disc.avahi_client)));
            break;

        default:
            break;
    }
}

static void avahi_client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    UNUSED(userdata);

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            LOG_INFO("mDNS client running, starting browser");
            g_disc.avahi_browser = avahi_service_browser_new(
                c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                MDNS_SERVICE_TYPE, NULL, 0,
                avahi_browse_callback, NULL);
            if (!g_disc.avahi_browser) {
                LOG_ERROR("Failed to create mDNS browser: %s",
                         avahi_strerror(avahi_client_errno(c)));
            }
            break;

        case AVAHI_CLIENT_FAILURE:
            LOG_ERROR("mDNS client failure: %s", avahi_strerror(avahi_client_errno(c)));
            avahi_simple_poll_quit(g_disc.avahi_poll);
            break;

        default:
            break;
    }
}

static void* mdns_thread_func(void *arg) {
    UNUSED(arg);

    LOG_INFO("mDNS discovery thread started");

    while (g_disc.mdns_running) {
        avahi_simple_poll_iterate(g_disc.avahi_poll, 1000);
    }

    LOG_INFO("mDNS discovery thread stopped");
    return NULL;
}
#endif /* HAVE_AVAHI */

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t controller_discovery_init(const profinet_config_t *config) {
    if (g_disc.initialized) {
        return RESULT_OK;
    }

    memset(&g_disc, 0, sizeof(g_disc));
    pthread_mutex_init(&g_disc.mutex, NULL);

    /* Check for pre-configured controller in config */
    if (config && config->controller_ip[0] != '\0') {
        SAFE_STRNCPY(g_disc.controller.ip, config->controller_ip,
                     sizeof(g_disc.controller.ip));
        if (config->controller_name[0] != '\0') {
            SAFE_STRNCPY(g_disc.controller.name, config->controller_name,
                         sizeof(g_disc.controller.name));
        }
        g_disc.controller.source = DISCOVERY_SOURCE_CONFIG;
        g_disc.controller.discovered_at = get_time_ms();

        LOG_INFO("Controller pre-configured: %s (%s)",
                 g_disc.controller.ip,
                 g_disc.controller.name[0] ? g_disc.controller.name : "unnamed");
    }

    g_disc.initialized = true;
    LOG_INFO("Controller discovery initialized");

    return RESULT_OK;
}

void controller_discovery_shutdown(void) {
    if (!g_disc.initialized) return;

    controller_discovery_stop_mdns();
    pthread_mutex_destroy(&g_disc.mutex);
    g_disc.initialized = false;

    LOG_INFO("Controller discovery shutdown");
}

result_t controller_discovery_start_mdns(void) {
#ifdef HAVE_AVAHI
    if (g_disc.mdns_running) {
        return RESULT_OK;
    }

    int error;

    g_disc.avahi_poll = avahi_simple_poll_new();
    if (!g_disc.avahi_poll) {
        LOG_ERROR("Failed to create Avahi poll");
        return RESULT_ERROR;
    }

    g_disc.avahi_client = avahi_client_new(
        avahi_simple_poll_get(g_disc.avahi_poll),
        0, avahi_client_callback, NULL, &error);

    if (!g_disc.avahi_client) {
        LOG_ERROR("Failed to create Avahi client: %s", avahi_strerror(error));
        avahi_simple_poll_free(g_disc.avahi_poll);
        g_disc.avahi_poll = NULL;
        return RESULT_ERROR;
    }

    g_disc.mdns_running = true;
    if (pthread_create(&g_disc.mdns_thread, NULL, mdns_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create mDNS thread");
        avahi_client_free(g_disc.avahi_client);
        avahi_simple_poll_free(g_disc.avahi_poll);
        g_disc.avahi_client = NULL;
        g_disc.avahi_poll = NULL;
        g_disc.mdns_running = false;
        return RESULT_ERROR;
    }

    LOG_INFO("mDNS discovery started, looking for '%s' services", MDNS_SERVICE_TYPE);
    return RESULT_OK;
#else
    LOG_WARNING("mDNS discovery not available (compiled without Avahi support)");
    return RESULT_NOT_SUPPORTED;
#endif
}

void controller_discovery_stop_mdns(void) {
#ifdef HAVE_AVAHI
    if (!g_disc.mdns_running) return;

    g_disc.mdns_running = false;

    if (g_disc.avahi_poll) {
        avahi_simple_poll_quit(g_disc.avahi_poll);
    }

    pthread_join(g_disc.mdns_thread, NULL);

    if (g_disc.avahi_browser) {
        avahi_service_browser_free(g_disc.avahi_browser);
        g_disc.avahi_browser = NULL;
    }
    if (g_disc.avahi_client) {
        avahi_client_free(g_disc.avahi_client);
        g_disc.avahi_client = NULL;
    }
    if (g_disc.avahi_poll) {
        avahi_simple_poll_free(g_disc.avahi_poll);
        g_disc.avahi_poll = NULL;
    }

    LOG_INFO("mDNS discovery stopped");
#endif
}

result_t controller_discovery_on_connect(uint32_t arep) {
    pthread_mutex_lock(&g_disc.mutex);

    g_disc.controller.connected = true;
    g_disc.controller.arep = arep;
    g_disc.controller.last_seen = get_time_ms();

    /* Try to resolve controller IP if not already known */
    if (g_disc.controller.ip[0] == '\0') {
        char ip[16];
        if (resolve_controller_ip_from_network(ip, sizeof(ip)) == RESULT_OK) {
            SAFE_STRNCPY(g_disc.controller.ip, ip, sizeof(g_disc.controller.ip));
            g_disc.controller.source = DISCOVERY_SOURCE_PROFINET;
            g_disc.controller.discovered_at = get_time_ms();

            LOG_INFO("Discovered controller IP from PROFINET connection: %s", ip);
        }
    } else {
        /* Update source if we connected to pre-configured controller */
        if (g_disc.controller.source == DISCOVERY_SOURCE_CONFIG) {
            LOG_INFO("Connected to configured controller: %s", g_disc.controller.ip);
        }
    }

    notify_callback();

    pthread_mutex_unlock(&g_disc.mutex);

    return RESULT_OK;
}

void controller_discovery_on_disconnect(uint32_t arep) {
    pthread_mutex_lock(&g_disc.mutex);

    if (g_disc.controller.arep == arep) {
        g_disc.controller.connected = false;
        g_disc.controller.arep = 0;

        LOG_INFO("Controller disconnected: %s",
                 g_disc.controller.ip[0] ? g_disc.controller.ip : "(unknown)");

        notify_callback();
    }

    pthread_mutex_unlock(&g_disc.mutex);
}

result_t controller_discovery_get(discovered_controller_t *controller) {
    CHECK_NULL(controller);

    pthread_mutex_lock(&g_disc.mutex);

    if (g_disc.controller.source == DISCOVERY_SOURCE_NONE &&
        g_disc.controller.ip[0] == '\0') {
        pthread_mutex_unlock(&g_disc.mutex);
        return RESULT_NOT_FOUND;
    }

    memcpy(controller, &g_disc.controller, sizeof(*controller));

    pthread_mutex_unlock(&g_disc.mutex);
    return RESULT_OK;
}

result_t controller_discovery_set(const char *ip, const char *name) {
    CHECK_NULL(ip);

    pthread_mutex_lock(&g_disc.mutex);

    SAFE_STRNCPY(g_disc.controller.ip, ip, sizeof(g_disc.controller.ip));
    if (name) {
        SAFE_STRNCPY(g_disc.controller.name, name, sizeof(g_disc.controller.name));
    }
    g_disc.controller.source = DISCOVERY_SOURCE_MANUAL;
    g_disc.controller.discovered_at = get_time_ms();

    LOG_INFO("Controller manually set: %s (%s)",
             g_disc.controller.ip,
             g_disc.controller.name[0] ? g_disc.controller.name : "unnamed");

    notify_callback();

    pthread_mutex_unlock(&g_disc.mutex);
    return RESULT_OK;
}

void controller_discovery_set_callback(controller_discovered_cb_t callback, void *ctx) {
    pthread_mutex_lock(&g_disc.mutex);
    g_disc.callback = callback;
    g_disc.callback_ctx = ctx;
    pthread_mutex_unlock(&g_disc.mutex);
}

const char* discovery_source_to_string(discovery_source_t source) {
    switch (source) {
        case DISCOVERY_SOURCE_NONE:     return "Not discovered";
        case DISCOVERY_SOURCE_CONFIG:   return "Config file";
        case DISCOVERY_SOURCE_PROFINET: return "PROFINET connection";
        case DISCOVERY_SOURCE_MDNS:     return "mDNS/Avahi";
        case DISCOVERY_SOURCE_MANUAL:   return "Manual";
        default:                        return "Unknown";
    }
}
