/**
 * @file rtu_registration.c
 * @brief RTU registration and enrollment implementation
 *
 * Implements registration protocol with SCADA controller including:
 * - HTTP-based registration (POST /api/v1/rtu/register)
 * - PROFINET-based enrollment confirmation (index 0xF845)
 * - Background retry with exponential backoff
 * - Enrollment token persistence
 */

#include "rtu_registration.h"
#include "controller_discovery.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>

#if defined(HAVE_CURL) && defined(HAVE_CJSON)
#include <curl/curl.h>
#include <cjson/cJSON.h>
#define REGISTRATION_HTTP_ENABLED 1
#else
#define REGISTRATION_HTTP_ENABLED 0
#endif

/* ============================================================================
 * CRC16-CCITT (same as user_sync.c)
 * ============================================================================ */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ============================================================================
 * Module State
 * ============================================================================ */

static struct {
    /* Configuration */
    rtu_device_info_t device_info;
    const rtu_registration_nv_ops_t *nv_ops;

    /* State */
    rtu_registration_state_t state;
    char enrollment_token[RTU_ENROLL_TOKEN_LEN];
    char controller_ip[16];
    char controller_name[MAX_NAME_LEN];
    uint64_t registered_at;
    uint64_t last_attempt;
    uint32_t attempt_count;
    uint32_t error_count;
    int last_http_status;
    char last_error[64];

    /* Thread control */
    pthread_t reg_thread;
    pthread_mutex_t mutex;
    volatile bool running;
    volatile bool stop_requested;

    /* Callback */
    rtu_registration_callback_t callback;
    void *callback_ctx;

    bool initialized;
} g_reg = {0};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void set_state(rtu_registration_state_t new_state) {
    g_reg.state = new_state;

    if (g_reg.callback) {
        rtu_registration_status_t status;
        rtu_registration_get_status(&status);
        g_reg.callback(new_state, &status, g_reg.callback_ctx);
    }
}

#if REGISTRATION_HTTP_ENABLED
static void set_error(const char *msg) {
    SAFE_STRNCPY(g_reg.last_error, msg, sizeof(g_reg.last_error));
    g_reg.error_count++;
    LOG_ERROR("Registration error: %s", msg);
}
#endif

/**
 * Get MAC address of primary network interface
 */
static result_t get_mac_address(char *mac_buf, size_t buf_size) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return RESULT_ERROR;
    }

    /* Try common interface names */
    const char *interfaces[] = {"eth0", "end0", "enp0s3", "ens33", NULL};

    for (int i = 0; interfaces[i] != NULL; i++) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        SAFE_STRNCPY(ifr.ifr_name, interfaces[i], IFNAMSIZ);

        if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
            unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
            snprintf(mac_buf, buf_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            close(fd);
            return RESULT_OK;
        }
    }

    close(fd);
    SAFE_STRNCPY(mac_buf, "00:00:00:00:00:00", buf_size);
    return RESULT_NOT_FOUND;
}

/**
 * Validate enrollment token format
 */
static bool validate_token(const char *token) {
    if (!token || strlen(token) != RTU_ENROLL_TOKEN_LEN - 1) {
        return false;
    }

    /* Check prefix */
    if (strncmp(token, RTU_ENROLL_TOKEN_PREFIX, strlen(RTU_ENROLL_TOKEN_PREFIX)) != 0) {
        return false;
    }

    /* Check hex chars after prefix */
    const char *hex = token + strlen(RTU_ENROLL_TOKEN_PREFIX);
    for (int i = 0; i < 32; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

#if REGISTRATION_HTTP_ENABLED

/* ============================================================================
 * HTTP Registration
 * ============================================================================ */

struct memory_chunk {
    char *data;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory_chunk *mem = (struct memory_chunk *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        LOG_ERROR("Out of memory in HTTP response");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

/**
 * Build JSON registration payload
 */
static char* build_registration_json(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "station_name", g_reg.device_info.station_name);
    cJSON_AddStringToObject(root, "serial", g_reg.device_info.serial_number);
    cJSON_AddNumberToObject(root, "vendor_id", g_reg.device_info.vendor_id);
    cJSON_AddNumberToObject(root, "device_id", g_reg.device_info.device_id);
    cJSON_AddStringToObject(root, "mac_address", g_reg.device_info.mac_address);
    cJSON_AddStringToObject(root, "capabilities", g_reg.device_info.capabilities);
    cJSON_AddNumberToObject(root, "sensor_count", g_reg.device_info.sensor_count);
    cJSON_AddNumberToObject(root, "actuator_count", g_reg.device_info.actuator_count);
    cJSON_AddStringToObject(root, "firmware_version", g_reg.device_info.firmware_version);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json;
}

/**
 * Parse registration response
 */
static result_t parse_registration_response(const char *json, char *token_out) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        LOG_ERROR("Failed to parse registration response");
        return RESULT_PARSE_ERROR;
    }

    /* Check for error */
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error && cJSON_IsString(error)) {
        set_error(cJSON_GetStringValue(error));
        cJSON_Delete(root);
        return RESULT_ERROR;
    }

    /* Get enrollment token */
    cJSON *token = cJSON_GetObjectItem(root, "enrollment_token");
    if (!token || !cJSON_IsString(token)) {
        set_error("Missing enrollment_token in response");
        cJSON_Delete(root);
        return RESULT_ERROR;
    }

    const char *token_str = cJSON_GetStringValue(token);
    if (!validate_token(token_str)) {
        set_error("Invalid enrollment_token format");
        cJSON_Delete(root);
        return RESULT_ERROR;
    }

    SAFE_STRNCPY(token_out, token_str, RTU_ENROLL_TOKEN_LEN);

    /* Get optional controller name */
    cJSON *name = cJSON_GetObjectItem(root, "controller_name");
    if (name && cJSON_IsString(name)) {
        SAFE_STRNCPY(g_reg.controller_name, cJSON_GetStringValue(name),
                     sizeof(g_reg.controller_name));
    }

    cJSON_Delete(root);
    return RESULT_OK;
}

/**
 * Perform HTTP registration request
 */
static result_t do_http_registration(const char *controller_ip) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        set_error("Failed to init curl");
        return RESULT_ERROR;
    }

    /* Build URL - Controller REST API on port 8000 */
    char url[128];
    snprintf(url, sizeof(url), "http://%s:8000/api/v1/rtu/register", controller_ip);

    /* Build JSON payload */
    char *json = build_registration_json();
    if (!json) {
        curl_easy_cleanup(curl);
        set_error("Failed to build JSON");
        return RESULT_ERROR;
    }

    LOG_INFO("Registering with controller at %s", url);
    LOG_DEBUG("Registration payload: %s", json);

    /* Setup request */
    struct memory_chunk response = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, RTU_REGISTRATION_TIMEOUT_MS / 1000);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    /* Perform request */
    CURLcode res = curl_easy_perform(curl);

    /* Get HTTP status */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    g_reg.last_http_status = (int)http_code;

    curl_slist_free_all(headers);
    free(json);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        set_error(curl_easy_strerror(res));
        free(response.data);
        return RESULT_ERROR;
    }

    if (http_code != 200 && http_code != 201) {
        char err[64];
        snprintf(err, sizeof(err), "HTTP %ld", http_code);
        set_error(err);
        free(response.data);
        return RESULT_ERROR;
    }

    /* Parse response */
    char token[RTU_ENROLL_TOKEN_LEN];
    result_t result = parse_registration_response(response.data, token);
    free(response.data);

    if (result != RESULT_OK) {
        return result;
    }

    /* Store token */
    pthread_mutex_lock(&g_reg.mutex);
    SAFE_STRNCPY(g_reg.enrollment_token, token, sizeof(g_reg.enrollment_token));
    SAFE_STRNCPY(g_reg.controller_ip, controller_ip, sizeof(g_reg.controller_ip));
    g_reg.registered_at = get_time_ms();
    pthread_mutex_unlock(&g_reg.mutex);

    /* Persist to NV if available */
    if (g_reg.nv_ops && g_reg.nv_ops->save_token) {
        if (g_reg.nv_ops->save_token(token) != 0) {
            LOG_WARNING("Failed to persist enrollment token to NV storage");
        }
    }

    LOG_INFO("Registration successful, token: %s...",
             g_reg.enrollment_token + strlen(RTU_ENROLL_TOKEN_PREFIX));

    return RESULT_OK;
}

#endif /* REGISTRATION_HTTP_ENABLED */

/* ============================================================================
 * Registration Thread
 * ============================================================================ */

static void* registration_thread(void *arg) {
    UNUSED(arg);

    LOG_INFO("Registration thread started");

    uint32_t retry_delay_ms = RTU_REGISTRATION_RETRY_MS;
#if REGISTRATION_HTTP_ENABLED
    const uint32_t max_retry_delay_ms = 300000; /* 5 minutes max */
#endif

    while (!g_reg.stop_requested) {
        /* Check if already registered */
        pthread_mutex_lock(&g_reg.mutex);
        bool is_registered = (g_reg.state == RTU_REG_STATE_REGISTERED);
        pthread_mutex_unlock(&g_reg.mutex);

        if (is_registered) {
            /* Sleep and check periodically */
            for (int i = 0; i < 60 && !g_reg.stop_requested; i++) {
                usleep(1000000); /* 1 second */
            }
            continue;
        }

        /* Try to discover controller */
        set_state(RTU_REG_STATE_DISCOVERING);

        discovered_controller_t controller;
        result_t disc_result = controller_discovery_get(&controller);

        if (disc_result != RESULT_OK || controller.ip[0] == '\0') {
            LOG_DEBUG("No controller discovered, retrying in %u ms", retry_delay_ms);
            usleep(retry_delay_ms * 1000);
            continue;
        }

#if REGISTRATION_HTTP_ENABLED
        /* Attempt registration */
        set_state(RTU_REG_STATE_REGISTERING);
        g_reg.last_attempt = get_time_ms();
        g_reg.attempt_count++;

        result_t reg_result = do_http_registration(controller.ip);

        if (reg_result == RESULT_OK) {
            set_state(RTU_REG_STATE_AWAITING_CONFIRM);
            LOG_INFO("Registration request accepted, awaiting PROFINET binding");

            /* Reset retry delay on success */
            retry_delay_ms = RTU_REGISTRATION_RETRY_MS;

            /* Wait for PROFINET confirmation or timeout */
            uint64_t wait_start = get_time_ms();
            while (!g_reg.stop_requested) {
                pthread_mutex_lock(&g_reg.mutex);
                bool confirmed = (g_reg.state == RTU_REG_STATE_REGISTERED);
                pthread_mutex_unlock(&g_reg.mutex);

                if (confirmed) {
                    break;
                }

                uint64_t elapsed = get_time_ms() - wait_start;
                if (elapsed > 60000) { /* 60 second timeout */
                    LOG_WARNING("PROFINET binding timeout, registration still valid");
                    set_state(RTU_REG_STATE_REGISTERED);
                    break;
                }

                usleep(1000000); /* 1 second */
            }
        } else {
            set_state(RTU_REG_STATE_ERROR);

            /* Exponential backoff */
            retry_delay_ms = MIN(retry_delay_ms * 2, max_retry_delay_ms);
            LOG_WARNING("Registration failed, retrying in %u ms", retry_delay_ms);
            usleep(retry_delay_ms * 1000);
        }
#else
        LOG_WARNING("HTTP registration not available (CURL/cJSON not built)");
        set_state(RTU_REG_STATE_ERROR);
        usleep(retry_delay_ms * 1000);
#endif
    }

    LOG_INFO("Registration thread stopped");
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

result_t rtu_registration_init(const app_config_t *config) {
    if (g_reg.initialized) {
        return RESULT_OK;
    }

    memset(&g_reg, 0, sizeof(g_reg));
    pthread_mutex_init(&g_reg.mutex, NULL);

    /* Populate device info from config */
    if (config) {
        SAFE_STRNCPY(g_reg.device_info.station_name, config->profinet.station_name,
                     sizeof(g_reg.device_info.station_name));
        g_reg.device_info.vendor_id = config->profinet.vendor_id;
        g_reg.device_info.device_id = config->profinet.device_id;
        SAFE_STRNCPY(g_reg.device_info.firmware_version, VERSION_STRING,
                     sizeof(g_reg.device_info.firmware_version));
    }

    /* Get MAC address */
    get_mac_address(g_reg.device_info.mac_address, sizeof(g_reg.device_info.mac_address));

    /* Set capabilities */
    SAFE_STRNCPY(g_reg.device_info.capabilities, "sensors,actuators,user_sync",
                 sizeof(g_reg.device_info.capabilities));

    /* Try to load existing token from NV */
    if (g_reg.nv_ops && g_reg.nv_ops->load_token) {
        char token[RTU_ENROLL_TOKEN_LEN];
        if (g_reg.nv_ops->load_token(token) == 0 && validate_token(token)) {
            SAFE_STRNCPY(g_reg.enrollment_token, token, sizeof(g_reg.enrollment_token));
            g_reg.state = RTU_REG_STATE_REGISTERED;
            LOG_INFO("Loaded enrollment token from NV storage");
        }
    }

    g_reg.initialized = true;
    LOG_INFO("RTU registration initialized, station: %s", g_reg.device_info.station_name);

    return RESULT_OK;
}

void rtu_registration_shutdown(void) {
    if (!g_reg.initialized) return;

    rtu_registration_stop();
    pthread_mutex_destroy(&g_reg.mutex);
    g_reg.initialized = false;

    LOG_INFO("RTU registration shutdown");
}

result_t rtu_registration_start(void) {
    if (!g_reg.initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (g_reg.running) {
        return RESULT_OK;
    }

    g_reg.stop_requested = false;
    g_reg.running = true;

    if (pthread_create(&g_reg.reg_thread, NULL, registration_thread, NULL) != 0) {
        g_reg.running = false;
        LOG_ERROR("Failed to create registration thread");
        return RESULT_ERROR;
    }

    LOG_INFO("RTU registration started");
    return RESULT_OK;
}

void rtu_registration_stop(void) {
    if (!g_reg.running) return;

    g_reg.stop_requested = true;
    pthread_join(g_reg.reg_thread, NULL);
    g_reg.running = false;

    LOG_INFO("RTU registration stopped");
}

result_t rtu_registration_trigger(const char *controller_ip) {
#if REGISTRATION_HTTP_ENABLED
    const char *ip = controller_ip;

    if (!ip) {
        discovered_controller_t controller;
        if (controller_discovery_get(&controller) != RESULT_OK) {
            return RESULT_NOT_FOUND;
        }
        ip = controller.ip;
    }

    pthread_mutex_lock(&g_reg.mutex);
    set_state(RTU_REG_STATE_REGISTERING);
    g_reg.last_attempt = get_time_ms();
    g_reg.attempt_count++;
    pthread_mutex_unlock(&g_reg.mutex);

    result_t result = do_http_registration(ip);

    if (result == RESULT_OK) {
        pthread_mutex_lock(&g_reg.mutex);
        set_state(RTU_REG_STATE_REGISTERED);
        pthread_mutex_unlock(&g_reg.mutex);
    } else {
        pthread_mutex_lock(&g_reg.mutex);
        set_state(RTU_REG_STATE_ERROR);
        pthread_mutex_unlock(&g_reg.mutex);
    }

    return result;
#else
    UNUSED(controller_ip);
    return RESULT_NOT_SUPPORTED;
#endif
}

result_t rtu_registration_process_enrollment(const uint8_t *data, uint16_t length) {
    CHECK_NULL(data);

    if (length < sizeof(rtu_enroll_packet_t)) {
        LOG_ERROR("Enrollment packet too short: %u < %zu", length, sizeof(rtu_enroll_packet_t));
        return RESULT_INVALID_PARAM;
    }

    const rtu_enroll_packet_t *packet = (const rtu_enroll_packet_t *)data;

    /* Verify magic */
    uint32_t magic = ntohl(packet->magic);
    if (magic != RTU_ENROLL_MAGIC) {
        LOG_ERROR("Invalid enrollment magic: 0x%08X (expected 0x%08X)", magic, RTU_ENROLL_MAGIC);
        return RESULT_INVALID_PARAM;
    }

    /* Verify version */
    if (packet->version != RTU_ENROLL_VERSION) {
        LOG_ERROR("Unsupported enrollment version: %u", packet->version);
        return RESULT_INVALID_PARAM;
    }

    /* Verify checksum (over packet minus checksum field itself) */
    size_t check_len = offsetof(rtu_enroll_packet_t, checksum);
    uint16_t expected_crc = crc16_ccitt(data, check_len);
    uint16_t received_crc = ntohs(packet->checksum);
    if (expected_crc != received_crc) {
        LOG_ERROR("Enrollment checksum mismatch: 0x%04X != 0x%04X", expected_crc, received_crc);
        return RESULT_INVALID_PARAM;
    }

    rtu_enroll_operation_t op = (rtu_enroll_operation_t)packet->operation;
    LOG_INFO("Processing enrollment operation: %s", rtu_enroll_op_to_string(op));

    pthread_mutex_lock(&g_reg.mutex);

    switch (op) {
        case RTU_ENROLL_OP_BIND:
        case RTU_ENROLL_OP_REBIND:
            /* Validate and store token */
            if (!validate_token(packet->enrollment_token)) {
                pthread_mutex_unlock(&g_reg.mutex);
                LOG_ERROR("Invalid enrollment token format");
                return RESULT_INVALID_PARAM;
            }

            SAFE_STRNCPY(g_reg.enrollment_token, packet->enrollment_token,
                         sizeof(g_reg.enrollment_token));
            g_reg.registered_at = get_time_ms();
            set_state(RTU_REG_STATE_REGISTERED);

            /* Persist to NV */
            if (g_reg.nv_ops && g_reg.nv_ops->save_token) {
                g_reg.nv_ops->save_token(g_reg.enrollment_token);
            }

            LOG_INFO("Enrollment binding confirmed, token stored");
            break;

        case RTU_ENROLL_OP_UNBIND:
            /* Clear enrollment */
            memset(g_reg.enrollment_token, 0, sizeof(g_reg.enrollment_token));
            memset(g_reg.controller_ip, 0, sizeof(g_reg.controller_ip));
            g_reg.registered_at = 0;
            set_state(RTU_REG_STATE_UNREGISTERED);

            /* Clear from NV */
            if (g_reg.nv_ops && g_reg.nv_ops->clear_token) {
                g_reg.nv_ops->clear_token();
            }

            LOG_INFO("Enrollment cleared (unbind)");
            break;

        case RTU_ENROLL_OP_STATUS:
            /* Just log status query */
            LOG_INFO("Enrollment status query: %s",
                     rtu_registration_state_to_string(g_reg.state));
            break;

        default:
            pthread_mutex_unlock(&g_reg.mutex);
            LOG_ERROR("Unknown enrollment operation: %u", op);
            return RESULT_INVALID_PARAM;
    }

    pthread_mutex_unlock(&g_reg.mutex);
    return RESULT_OK;
}

result_t rtu_registration_get_status(rtu_registration_status_t *status) {
    CHECK_NULL(status);

    pthread_mutex_lock(&g_reg.mutex);

    status->state = g_reg.state;
    SAFE_STRNCPY(status->controller_ip, g_reg.controller_ip, sizeof(status->controller_ip));
    SAFE_STRNCPY(status->controller_name, g_reg.controller_name, sizeof(status->controller_name));
    SAFE_STRNCPY(status->enrollment_token, g_reg.enrollment_token,
                 sizeof(status->enrollment_token));
    status->registered_at = g_reg.registered_at;
    status->last_attempt = g_reg.last_attempt;
    status->attempt_count = g_reg.attempt_count;
    status->error_count = g_reg.error_count;
    status->last_http_status = g_reg.last_http_status;
    SAFE_STRNCPY(status->last_error, g_reg.last_error, sizeof(status->last_error));

    pthread_mutex_unlock(&g_reg.mutex);
    return RESULT_OK;
}

bool rtu_registration_is_registered(void) {
    pthread_mutex_lock(&g_reg.mutex);
    bool result = (g_reg.state == RTU_REG_STATE_REGISTERED &&
                   g_reg.enrollment_token[0] != '\0');
    pthread_mutex_unlock(&g_reg.mutex);
    return result;
}

result_t rtu_registration_get_token(char *token) {
    CHECK_NULL(token);

    pthread_mutex_lock(&g_reg.mutex);

    if (g_reg.enrollment_token[0] == '\0') {
        pthread_mutex_unlock(&g_reg.mutex);
        return RESULT_NOT_FOUND;
    }

    SAFE_STRNCPY(token, g_reg.enrollment_token, RTU_ENROLL_TOKEN_LEN);

    pthread_mutex_unlock(&g_reg.mutex);
    return RESULT_OK;
}

result_t rtu_registration_clear(void) {
    pthread_mutex_lock(&g_reg.mutex);

    memset(g_reg.enrollment_token, 0, sizeof(g_reg.enrollment_token));
    memset(g_reg.controller_ip, 0, sizeof(g_reg.controller_ip));
    memset(g_reg.controller_name, 0, sizeof(g_reg.controller_name));
    g_reg.registered_at = 0;
    g_reg.state = RTU_REG_STATE_UNREGISTERED;

    /* Clear from NV */
    if (g_reg.nv_ops && g_reg.nv_ops->clear_token) {
        g_reg.nv_ops->clear_token();
    }

    pthread_mutex_unlock(&g_reg.mutex);

    LOG_INFO("Registration cleared");
    return RESULT_OK;
}

void rtu_registration_set_callback(rtu_registration_callback_t callback, void *ctx) {
    pthread_mutex_lock(&g_reg.mutex);
    g_reg.callback = callback;
    g_reg.callback_ctx = ctx;
    pthread_mutex_unlock(&g_reg.mutex);
}

result_t rtu_registration_get_device_info(rtu_device_info_t *info) {
    CHECK_NULL(info);

    pthread_mutex_lock(&g_reg.mutex);
    memcpy(info, &g_reg.device_info, sizeof(*info));
    pthread_mutex_unlock(&g_reg.mutex);

    return RESULT_OK;
}

result_t rtu_registration_set_nv_backend(const rtu_registration_nv_ops_t *ops) {
    CHECK_NULL(ops);

    pthread_mutex_lock(&g_reg.mutex);
    g_reg.nv_ops = ops;
    pthread_mutex_unlock(&g_reg.mutex);

    return RESULT_OK;
}

/* ============================================================================
 * String Helpers
 * ============================================================================ */

const char* rtu_registration_state_to_string(rtu_registration_state_t state) {
    switch (state) {
        case RTU_REG_STATE_UNREGISTERED:    return "Unregistered";
        case RTU_REG_STATE_DISCOVERING:     return "Discovering";
        case RTU_REG_STATE_REGISTERING:     return "Registering";
        case RTU_REG_STATE_AWAITING_CONFIRM: return "Awaiting Confirm";
        case RTU_REG_STATE_REGISTERED:      return "Registered";
        case RTU_REG_STATE_ERROR:           return "Error";
        default:                            return "Unknown";
    }
}

const char* rtu_enroll_op_to_string(rtu_enroll_operation_t op) {
    switch (op) {
        case RTU_ENROLL_OP_BIND:     return "Bind";
        case RTU_ENROLL_OP_UNBIND:   return "Unbind";
        case RTU_ENROLL_OP_REBIND:   return "Rebind";
        case RTU_ENROLL_OP_STATUS:   return "Status";
        default:                     return "Unknown";
    }
}
