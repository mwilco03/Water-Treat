/**
 * @file config_sync.c
 * @brief Configuration sync from SCADA controller implementation
 *
 * Processes configuration packets from controller and applies to RTU.
 * Config sync triggered on PROFINET AR_STATE_RUN (automatic on connect).
 */

#include "config_sync.h"
#include "utils/logger.h"
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>

/* ============================================================================
 * CRC16-CCITT (same as user_sync.c, rtu_registration.c)
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
    pthread_mutex_t mutex;
    bool initialized;

    /* Current configuration */
    authority_mode_t authority;
    uint32_t watchdog_ms;
    char station_name[CONFIG_SYNC_STATION_LEN];

    /* Configured items */
    config_sync_sensor_entry_t sensors[CONFIG_SYNC_MAX_SENSORS];
    uint8_t sensor_count;

    config_sync_actuator_entry_t actuators[CONFIG_SYNC_MAX_ACTUATORS];
    uint8_t actuator_count;

    /* Statistics */
    uint32_t device_config_count;
    uint32_t sensor_config_count;
    uint32_t actuator_config_count;
    uint32_t last_device_timestamp;
} g_cfg = {0};

/* ============================================================================
 * Device Configuration (0xF841)
 * ============================================================================ */

result_t config_sync_process_device(const uint8_t *data, uint16_t length) {
    CHECK_NULL(data);

    if (length < sizeof(config_sync_device_packet_t)) {
        LOG_ERROR("Device config packet too short: %u < %zu",
                  length, sizeof(config_sync_device_packet_t));
        return RESULT_INVALID_PARAM;
    }

    const config_sync_device_packet_t *pkt = (const config_sync_device_packet_t *)data;

    /* Verify version */
    if (pkt->version != CONFIG_SYNC_VERSION) {
        LOG_ERROR("Unsupported device config version: %u", pkt->version);
        return RESULT_INVALID_PARAM;
    }

    /* Verify CRC (over packet after CRC field) */
    size_t crc_offset = offsetof(config_sync_device_packet_t, crc16) + sizeof(uint16_t);
    size_t payload_len = length - crc_offset;
    uint16_t calc_crc = crc16_ccitt(data + crc_offset, payload_len);
    uint16_t recv_crc = ntohs(pkt->crc16);

    if (calc_crc != recv_crc) {
        LOG_ERROR("Device config CRC mismatch: 0x%04X != 0x%04X", calc_crc, recv_crc);
        return RESULT_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_cfg.mutex);

    /* Apply configuration */
    g_cfg.authority = (authority_mode_t)pkt->authority_mode;
    g_cfg.watchdog_ms = ntohl(pkt->watchdog_ms);
    g_cfg.last_device_timestamp = ntohl(pkt->timestamp);

    /* Copy station name (ensure null termination) */
    memcpy(g_cfg.station_name, pkt->station_name, CONFIG_SYNC_STATION_LEN - 1);
    g_cfg.station_name[CONFIG_SYNC_STATION_LEN - 1] = '\0';

    g_cfg.device_config_count++;

    LOG_INFO("Device config applied: station=%s, authority=%s, watchdog=%ums",
             g_cfg.station_name,
             g_cfg.authority == AUTHORITY_SUPERVISED ? "SUPERVISED" : "AUTONOMOUS",
             g_cfg.watchdog_ms);

    pthread_mutex_unlock(&g_cfg.mutex);

    return RESULT_OK;
}

/* ============================================================================
 * Sensor Configuration (0xF842)
 * ============================================================================ */

result_t config_sync_process_sensors(const uint8_t *data, uint16_t length) {
    CHECK_NULL(data);

    if (length < sizeof(config_sync_sensor_packet_t)) {
        LOG_ERROR("Sensor config packet too short: %u < %zu",
                  length, sizeof(config_sync_sensor_packet_t));
        return RESULT_INVALID_PARAM;
    }

    const config_sync_sensor_packet_t *pkt = (const config_sync_sensor_packet_t *)data;

    /* Verify version */
    if (pkt->version != CONFIG_SYNC_VERSION) {
        LOG_ERROR("Unsupported sensor config version: %u", pkt->version);
        return RESULT_INVALID_PARAM;
    }

    /* Check entry count */
    if (pkt->count > CONFIG_SYNC_MAX_SENSORS) {
        LOG_ERROR("Too many sensors in config: %u > %u", pkt->count, CONFIG_SYNC_MAX_SENSORS);
        return RESULT_INVALID_PARAM;
    }

    /* Verify packet length */
    size_t expected_len = sizeof(config_sync_sensor_packet_t) +
                          pkt->count * sizeof(config_sync_sensor_entry_t);
    if (length < expected_len) {
        LOG_ERROR("Sensor config packet truncated: %u < %zu", length, expected_len);
        return RESULT_INVALID_PARAM;
    }

    /* Verify CRC (over entries) */
    const uint8_t *entries_data = data + sizeof(config_sync_sensor_packet_t);
    size_t entries_len = pkt->count * sizeof(config_sync_sensor_entry_t);
    uint16_t calc_crc = crc16_ccitt(entries_data, entries_len);
    uint16_t recv_crc = ntohs(pkt->crc16);

    if (calc_crc != recv_crc) {
        LOG_ERROR("Sensor config CRC mismatch: 0x%04X != 0x%04X", calc_crc, recv_crc);
        return RESULT_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_cfg.mutex);

    /* Clear existing sensors */
    memset(g_cfg.sensors, 0, sizeof(g_cfg.sensors));
    g_cfg.sensor_count = 0;

    /* Copy sensor entries */
    const config_sync_sensor_entry_t *entries =
        (const config_sync_sensor_entry_t *)entries_data;

    for (uint8_t i = 0; i < pkt->count; i++) {
        /* Validate slot range (1-8) */
        if (entries[i].slot < 1 || entries[i].slot > 8) {
            LOG_WARNING("Invalid sensor slot %u, skipping", entries[i].slot);
            continue;
        }

        memcpy(&g_cfg.sensors[g_cfg.sensor_count], &entries[i],
               sizeof(config_sync_sensor_entry_t));

        /* Ensure null termination */
        g_cfg.sensors[g_cfg.sensor_count].name[CONFIG_SYNC_NAME_LEN - 1] = '\0';
        g_cfg.sensors[g_cfg.sensor_count].unit[CONFIG_SYNC_UNIT_LEN - 1] = '\0';

        LOG_DEBUG("Sensor slot %u: %s [%s] range=%.1f-%.1f",
                  entries[i].slot, g_cfg.sensors[g_cfg.sensor_count].name,
                  g_cfg.sensors[g_cfg.sensor_count].unit,
                  entries[i].scale_min, entries[i].scale_max);

        g_cfg.sensor_count++;
    }

    g_cfg.sensor_config_count++;

    LOG_INFO("Sensor config applied: %u sensors configured", g_cfg.sensor_count);

    pthread_mutex_unlock(&g_cfg.mutex);

    return RESULT_OK;
}

/* ============================================================================
 * Actuator Configuration (0xF843)
 * ============================================================================ */

result_t config_sync_process_actuators(const uint8_t *data, uint16_t length) {
    CHECK_NULL(data);

    if (length < sizeof(config_sync_actuator_packet_t)) {
        LOG_ERROR("Actuator config packet too short: %u < %zu",
                  length, sizeof(config_sync_actuator_packet_t));
        return RESULT_INVALID_PARAM;
    }

    const config_sync_actuator_packet_t *pkt = (const config_sync_actuator_packet_t *)data;

    /* Verify version */
    if (pkt->version != CONFIG_SYNC_VERSION) {
        LOG_ERROR("Unsupported actuator config version: %u", pkt->version);
        return RESULT_INVALID_PARAM;
    }

    /* Check entry count */
    if (pkt->count > CONFIG_SYNC_MAX_ACTUATORS) {
        LOG_ERROR("Too many actuators in config: %u > %u", pkt->count, CONFIG_SYNC_MAX_ACTUATORS);
        return RESULT_INVALID_PARAM;
    }

    /* Verify packet length */
    size_t expected_len = sizeof(config_sync_actuator_packet_t) +
                          pkt->count * sizeof(config_sync_actuator_entry_t);
    if (length < expected_len) {
        LOG_ERROR("Actuator config packet truncated: %u < %zu", length, expected_len);
        return RESULT_INVALID_PARAM;
    }

    /* Verify CRC (over entries) */
    const uint8_t *entries_data = data + sizeof(config_sync_actuator_packet_t);
    size_t entries_len = pkt->count * sizeof(config_sync_actuator_entry_t);
    uint16_t calc_crc = crc16_ccitt(entries_data, entries_len);
    uint16_t recv_crc = ntohs(pkt->crc16);

    if (calc_crc != recv_crc) {
        LOG_ERROR("Actuator config CRC mismatch: 0x%04X != 0x%04X", calc_crc, recv_crc);
        return RESULT_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_cfg.mutex);

    /* Clear existing actuators */
    memset(g_cfg.actuators, 0, sizeof(g_cfg.actuators));
    g_cfg.actuator_count = 0;

    /* Copy actuator entries */
    const config_sync_actuator_entry_t *entries =
        (const config_sync_actuator_entry_t *)entries_data;

    for (uint8_t i = 0; i < pkt->count; i++) {
        /* Validate slot range (9-15 per controller team) */
        if (entries[i].slot < 9 || entries[i].slot > 15) {
            LOG_WARNING("Invalid actuator slot %u (must be 9-15), skipping", entries[i].slot);
            continue;
        }

        memcpy(&g_cfg.actuators[g_cfg.actuator_count], &entries[i],
               sizeof(config_sync_actuator_entry_t));

        /* Ensure null termination */
        g_cfg.actuators[g_cfg.actuator_count].name[CONFIG_SYNC_NAME_LEN - 1] = '\0';

        LOG_DEBUG("Actuator slot %u: %s default=%u interlock=0x%04X",
                  entries[i].slot, g_cfg.actuators[g_cfg.actuator_count].name,
                  entries[i].default_state, entries[i].interlock_mask);

        g_cfg.actuator_count++;
    }

    g_cfg.actuator_config_count++;

    LOG_INFO("Actuator config applied: %u actuators configured", g_cfg.actuator_count);

    pthread_mutex_unlock(&g_cfg.mutex);

    return RESULT_OK;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

result_t config_sync_init(void) {
    if (g_cfg.initialized) {
        return RESULT_OK;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    pthread_mutex_init(&g_cfg.mutex, NULL);

    g_cfg.authority = AUTHORITY_AUTONOMOUS;  /* Default to autonomous */
    g_cfg.watchdog_ms = 5000;                /* 5 second default */

    g_cfg.initialized = true;
    LOG_INFO("Config sync initialized");

    return RESULT_OK;
}

void config_sync_shutdown(void) {
    if (!g_cfg.initialized) return;

    pthread_mutex_destroy(&g_cfg.mutex);
    g_cfg.initialized = false;

    LOG_INFO("Config sync shutdown");
}

result_t config_sync_get_status(config_sync_status_t *status) {
    CHECK_NULL(status);

    pthread_mutex_lock(&g_cfg.mutex);

    status->device_config_count = g_cfg.device_config_count;
    status->sensor_config_count = g_cfg.sensor_config_count;
    status->actuator_config_count = g_cfg.actuator_config_count;
    status->last_device_timestamp = g_cfg.last_device_timestamp;
    status->current_authority = (uint8_t)g_cfg.authority;
    status->sensors_configured = g_cfg.sensor_count;
    status->actuators_configured = g_cfg.actuator_count;

    pthread_mutex_unlock(&g_cfg.mutex);

    return RESULT_OK;
}

authority_mode_t config_sync_get_authority(void) {
    pthread_mutex_lock(&g_cfg.mutex);
    authority_mode_t auth = g_cfg.authority;
    pthread_mutex_unlock(&g_cfg.mutex);
    return auth;
}
