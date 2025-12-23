#include "config.h"
#include "utils/logger.h"
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>

/* ============================================================================
 * Table-Driven Config Loading
 * ============================================================================
 * What: Replace 37 repetitive if-blocks with a single data-driven loop
 * Why: Reduces cyclomatic complexity from 37 to ~5, eliminates copy-paste errors
 * How: Define field metadata (section, key, type, offset) and iterate
 */

typedef enum {
    CFG_TYPE_STRING,
    CFG_TYPE_INT,
    CFG_TYPE_BOOL,
    CFG_TYPE_UINT16,
    CFG_TYPE_UINT32
} config_field_type_t;

typedef struct {
    const char *section;
    const char *key;
    config_field_type_t type;
    size_t offset;
    size_t size;  /* For strings: buffer size. For others: 0 */
} config_field_t;

/* Field descriptor table - all 37 config entries */
static const config_field_t config_fields[] = {
    /* System section */
    { "system", "device_name", CFG_TYPE_STRING,
      offsetof(app_config_t, system.device_name),
      sizeof(((app_config_t*)0)->system.device_name) },
    { "system", "log_level", CFG_TYPE_STRING,
      offsetof(app_config_t, system.log_level),
      sizeof(((app_config_t*)0)->system.log_level) },
    { "system", "log_file", CFG_TYPE_STRING,
      offsetof(app_config_t, system.log_file),
      sizeof(((app_config_t*)0)->system.log_file) },
    { "system", "daemon_mode", CFG_TYPE_BOOL,
      offsetof(app_config_t, system.daemon_mode), 0 },

    /* Network section */
    { "network", "interface", CFG_TYPE_STRING,
      offsetof(app_config_t, network.interface),
      sizeof(((app_config_t*)0)->network.interface) },
    { "network", "dhcp_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, network.dhcp_enabled), 0 },
    { "network", "ip_address", CFG_TYPE_STRING,
      offsetof(app_config_t, network.ip_address),
      sizeof(((app_config_t*)0)->network.ip_address) },
    { "network", "netmask", CFG_TYPE_STRING,
      offsetof(app_config_t, network.netmask),
      sizeof(((app_config_t*)0)->network.netmask) },
    { "network", "gateway", CFG_TYPE_STRING,
      offsetof(app_config_t, network.gateway),
      sizeof(((app_config_t*)0)->network.gateway) },

    /* PROFINET section */
    { "profinet", "station_name", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.station_name),
      sizeof(((app_config_t*)0)->profinet.station_name) },
    { "profinet", "vendor_id", CFG_TYPE_UINT16,
      offsetof(app_config_t, profinet.vendor_id), 0 },
    { "profinet", "device_id", CFG_TYPE_UINT16,
      offsetof(app_config_t, profinet.device_id), 0 },
    { "profinet", "product_name", CFG_TYPE_STRING,
      offsetof(app_config_t, profinet.product_name),
      sizeof(((app_config_t*)0)->profinet.product_name) },
    { "profinet", "min_device_interval", CFG_TYPE_UINT32,
      offsetof(app_config_t, profinet.min_device_interval), 0 },
    { "profinet", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, profinet.enabled), 0 },

    /* Database section */
    { "database", "path", CFG_TYPE_STRING,
      offsetof(app_config_t, database.path),
      sizeof(((app_config_t*)0)->database.path) },
    { "database", "create_if_missing", CFG_TYPE_BOOL,
      offsetof(app_config_t, database.create_if_missing), 0 },
    { "database", "busy_timeout_ms", CFG_TYPE_INT,
      offsetof(app_config_t, database.busy_timeout_ms), 0 },

    /* Logging section */
    { "logging", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, logging.enabled), 0 },
    { "logging", "interval_seconds", CFG_TYPE_INT,
      offsetof(app_config_t, logging.interval_seconds), 0 },
    { "logging", "retention_days", CFG_TYPE_INT,
      offsetof(app_config_t, logging.retention_days), 0 },
    { "logging", "destination", CFG_TYPE_INT,
      offsetof(app_config_t, logging.destination), 0 },
    { "logging", "remote_url", CFG_TYPE_STRING,
      offsetof(app_config_t, logging.remote_url),
      sizeof(((app_config_t*)0)->logging.remote_url) },
    { "logging", "remote_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, logging.remote_enabled), 0 },

    /* Health section */
    { "health", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, health.enabled), 0 },
    { "health", "http_enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, health.http_enabled), 0 },
    { "health", "http_port", CFG_TYPE_UINT16,
      offsetof(app_config_t, health.http_port), 0 },
    { "health", "file_path", CFG_TYPE_STRING,
      offsetof(app_config_t, health.file_path),
      sizeof(((app_config_t*)0)->health.file_path) },
    { "health", "update_interval_seconds", CFG_TYPE_INT,
      offsetof(app_config_t, health.update_interval_seconds), 0 },

    /* LED section */
    { "led", "enabled", CFG_TYPE_BOOL,
      offsetof(app_config_t, led.enabled), 0 },
    { "led", "led_count", CFG_TYPE_INT,
      offsetof(app_config_t, led.led_count), 0 },
    { "led", "brightness", CFG_TYPE_INT,
      offsetof(app_config_t, led.brightness), 0 },
    { "led", "backend", CFG_TYPE_STRING,
      offsetof(app_config_t, led.backend),
      sizeof(((app_config_t*)0)->led.backend) },
    { "led", "spi_device", CFG_TYPE_STRING,
      offsetof(app_config_t, led.spi_device),
      sizeof(((app_config_t*)0)->led.spi_device) },
    { "led", "spi_speed_hz", CFG_TYPE_UINT32,
      offsetof(app_config_t, led.spi_speed_hz), 0 },
    { "led", "gpio_pin", CFG_TYPE_INT,
      offsetof(app_config_t, led.gpio_pin), 0 },
    { "led", "dma_channel", CFG_TYPE_INT,
      offsetof(app_config_t, led.dma_channel), 0 },
};

#define CONFIG_FIELD_COUNT (sizeof(config_fields) / sizeof(config_fields[0]))

/* Generic field loader - handles all types via the descriptor */
static void config_load_field(
    config_manager_t *m,
    app_config_t *c,
    const config_field_t *field
) {
    void *target = (char*)c + field->offset;
    char str_buf[MAX_CONFIG_VALUE_LEN];
    int int_val;
    bool bool_val;

    switch (field->type) {
        case CFG_TYPE_STRING:
            if (config_get_string(m, field->section, field->key,
                                  str_buf, sizeof(str_buf)) == RESULT_OK) {
                SAFE_STRNCPY((char*)target, str_buf, field->size);
            }
            break;

        case CFG_TYPE_INT:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(int*)target = int_val;
            }
            break;

        case CFG_TYPE_BOOL:
            if (config_get_bool(m, field->section, field->key,
                                &bool_val) == RESULT_OK) {
                *(bool*)target = bool_val;
            }
            break;

        case CFG_TYPE_UINT16:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(uint16_t*)target = (uint16_t)int_val;
            }
            break;

        case CFG_TYPE_UINT32:
            if (config_get_int(m, field->section, field->key,
                               &int_val) == RESULT_OK) {
                *(uint32_t*)target = (uint32_t)int_val;
            }
            break;
    }
}

/* ============================================================================
 * Station ID Detection
 * ============================================================================
 * What: Generate unique station ID from hardware MAC address
 * Why: Multiple RTUs need unique identifiers tied to physical hardware
 * Format: rtu-XXXX where XXXX is last 4 hex chars of primary MAC
 * Note: "rtu" aligns with control station / RTU architecture naming
 */
static void detect_station_id(char *station_name, size_t size) {
    DIR *dir;
    struct dirent *entry;
    char mac_path[256];
    char mac_addr[18] = {0};
    FILE *f;

    /* Default fallback */
    SAFE_STRNCPY(station_name, "rtu-0000", size);

    dir = opendir("/sys/class/net");
    if (!dir) return;

    /* Find first physical network interface (prefer eth or enp over wlan/lo) */
    char *best_iface = NULL;
    int best_priority = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, "lo") == 0) continue;

        /* Priority: eth* > enp* > ens* > wlan* > others */
        int priority = 1;
        if (strncmp(entry->d_name, "eth", 3) == 0) priority = 5;
        else if (strncmp(entry->d_name, "enp", 3) == 0) priority = 4;
        else if (strncmp(entry->d_name, "ens", 3) == 0) priority = 3;
        else if (strncmp(entry->d_name, "wlan", 4) == 0) priority = 2;

        if (priority > best_priority) {
            best_priority = priority;
            free(best_iface);
            best_iface = strdup(entry->d_name);
        }
    }
    closedir(dir);

    if (!best_iface) return;

    /* Read MAC address */
    snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", best_iface);
    free(best_iface);

    f = fopen(mac_path, "r");
    if (!f) return;

    if (fgets(mac_addr, sizeof(mac_addr), f)) {
        /* MAC format: aa:bb:cc:dd:ee:ff - extract last 4 hex chars */
        size_t len = strlen(mac_addr);
        if (len >= 17) {
            /* Remove newline if present */
            if (mac_addr[len-1] == '\n') mac_addr[len-1] = '\0';

            /* Skip all-zeros MAC */
            if (strcmp(mac_addr, "00:00:00:00:00:00") != 0) {
                /* Extract last 4 hex digits (positions 12,13 and 15,16) */
                char suffix[5];
                suffix[0] = mac_addr[12];
                suffix[1] = mac_addr[13];
                suffix[2] = mac_addr[15];
                suffix[3] = mac_addr[16];
                suffix[4] = '\0';

                /* Convert to lowercase */
                for (int i = 0; i < 4; i++) {
                    if (suffix[i] >= 'A' && suffix[i] <= 'F') {
                        suffix[i] = suffix[i] - 'A' + 'a';
                    }
                }

                snprintf(station_name, size, "rtu-%s", suffix);
            }
        }
    }
    fclose(f);
}

static char* trim(char *str) { char *e; while(isspace((unsigned char)*str)) str++; if(*str==0) return str; e=str+strlen(str)-1; while(e>str && isspace((unsigned char)*e)) e--; *(e+1)='\0'; return str; }
static config_entry_t* find_entry(config_manager_t *m, const char *s, const char *k) { for(int i=0;i<m->entry_count;i++) if(strcasecmp(m->entries[i].section,s)==0 && strcasecmp(m->entries[i].key,k)==0) return &m->entries[i]; return NULL; }

result_t config_manager_init(config_manager_t *m) { CHECK_NULL(m); memset(m,0,sizeof(*m)); return RESULT_OK; }
void config_manager_destroy(config_manager_t *m) { if(m) memset(m,0,sizeof(*m)); }

result_t config_load_file(config_manager_t *m, const char *p) {
    CHECK_NULL(m); CHECK_NULL(p);
    FILE *f = fopen(p,"r"); if(!f) { LOG_ERROR("Cannot open: %s",p); return RESULT_IO_ERROR; }
    SAFE_STRNCPY(m->config_path,p,sizeof(m->config_path)); m->entry_count=0;
    char line[1024], sec[MAX_NAME_LEN]="default";
    while(fgets(line,sizeof(line),f)) {
        char *t = trim(line); if(*t=='\0' || *t=='#' || *t==';') continue;
        if(*t=='[') { char *e=strchr(t,']'); if(e) { *e='\0'; SAFE_STRNCPY(sec,t+1,sizeof(sec)); } continue; }
        char *eq=strchr(t,'=');
        if(eq && m->entry_count < MAX_CONFIG_ENTRIES) {
            *eq='\0'; char *k=trim(t), *v=trim(eq+1);
            size_t vl=strlen(v); if(vl>=2 && ((v[0]=='"' && v[vl-1]=='"') || (v[0]=='\'' && v[vl-1]=='\''))) { v[vl-1]='\0'; v++; }
            config_entry_t *e = &m->entries[m->entry_count++];
            SAFE_STRNCPY(e->section,sec,sizeof(e->section)); SAFE_STRNCPY(e->key,k,sizeof(e->key)); SAFE_STRNCPY(e->value,v,sizeof(e->value));
        }
    }
    fclose(f); LOG_INFO("Loaded %d entries from %s",m->entry_count,p); return RESULT_OK;
}

result_t config_save_file(config_manager_t *m, const char *p) {
    CHECK_NULL(m); const char *sp = p ? p : m->config_path; if(!sp || strlen(sp)==0) return RESULT_INVALID_PARAM;
    FILE *f = fopen(sp,"w"); if(!f) return RESULT_IO_ERROR;
    fprintf(f,"# Water-Treat RTU Configuration\n\n"); char cs[MAX_NAME_LEN]="";
    for(int i=0;i<m->entry_count;i++) {
        if(strcmp(cs,m->entries[i].section)!=0) { if(cs[0]!='\0') fprintf(f,"\n"); fprintf(f,"[%s]\n",m->entries[i].section); SAFE_STRNCPY(cs,m->entries[i].section,sizeof(cs)); }
        fprintf(f,"%s = %s\n",m->entries[i].key,m->entries[i].value);
    }
    fclose(f); m->modified=false; return RESULT_OK;
}

result_t config_get_string(config_manager_t *m, const char *s, const char *k, char *v, size_t sz) { CHECK_NULL(m); CHECK_NULL(s); CHECK_NULL(k); CHECK_NULL(v); config_entry_t *e=find_entry(m,s,k); if(!e) return RESULT_NOT_FOUND; SAFE_STRNCPY(v,e->value,sz); return RESULT_OK; }
result_t config_set_string(config_manager_t *m, const char *s, const char *k, const char *v) { CHECK_NULL(m); CHECK_NULL(s); CHECK_NULL(k); CHECK_NULL(v); config_entry_t *e=find_entry(m,s,k); if(e) { SAFE_STRNCPY(e->value,v,sizeof(e->value)); } else { if(m->entry_count>=MAX_CONFIG_ENTRIES) return RESULT_NO_MEMORY; e=&m->entries[m->entry_count++]; SAFE_STRNCPY(e->section,s,sizeof(e->section)); SAFE_STRNCPY(e->key,k,sizeof(e->key)); SAFE_STRNCPY(e->value,v,sizeof(e->value)); } m->modified=true; return RESULT_OK; }
result_t config_get_int(config_manager_t *m, const char *s, const char *k, int *v) { char str[MAX_CONFIG_VALUE_LEN]; result_t r=config_get_string(m,s,k,str,sizeof(str)); if(r!=RESULT_OK) return r; *v=(int)strtol(str,NULL,0); return RESULT_OK; }
result_t config_get_bool(config_manager_t *m, const char *s, const char *k, bool *v) { char str[MAX_CONFIG_VALUE_LEN]; result_t r=config_get_string(m,s,k,str,sizeof(str)); if(r!=RESULT_OK) return r; *v=(strcasecmp(str,"true")==0||strcasecmp(str,"yes")==0||strcasecmp(str,"1")==0); return RESULT_OK; }

void config_get_defaults(app_config_t *c) {
    memset(c,0,sizeof(*c));

    /* System defaults */
    detect_station_id(c->system.device_name, sizeof(c->system.device_name));
    SAFE_STRNCPY(c->system.log_level,"info",sizeof(c->system.log_level));
    SAFE_STRNCPY(c->system.log_file,"/var/log/water-treat/monitor.log",sizeof(c->system.log_file));
    c->system.daemon_mode=false;

    /* Network defaults */
    SAFE_STRNCPY(c->network.interface,"eth0",sizeof(c->network.interface));
    c->network.dhcp_enabled=true;
    /* ip_address, netmask, gateway left empty (DHCP) */

    /* PROFINET defaults - station name derived from MAC */
    detect_station_id(c->profinet.station_name, sizeof(c->profinet.station_name));
    SAFE_STRNCPY(c->profinet.product_name,"Water Treatment RTU",sizeof(c->profinet.product_name));
    c->profinet.vendor_id=0x0493;
    c->profinet.device_id=0x0001;
    c->profinet.min_device_interval=32;
    c->profinet.enabled=true;

    /* Database defaults */
    SAFE_STRNCPY(c->database.path,"/var/lib/water-treat/data.db",sizeof(c->database.path));
    c->database.create_if_missing=true;
    c->database.busy_timeout_ms=5000;

    /* Logging defaults */
    c->logging.enabled=true;
    c->logging.interval_seconds=60;
    c->logging.retention_days=30;
    c->logging.destination=1; /* Local */
    c->logging.remote_enabled=false;

    /* Health check defaults */
    c->health.enabled=true;
    c->health.http_enabled=true;
    c->health.http_port=8080;
    SAFE_STRNCPY(c->health.file_path,"/var/lib/water-treat/health.prom",sizeof(c->health.file_path));
    c->health.update_interval_seconds=10;

    /* LED indicator defaults (disabled by default) */
    c->led.enabled=false;
    c->led.led_count=8;
    c->led.brightness=64;  /* 25% - safe default */
    SAFE_STRNCPY(c->led.backend,"auto",sizeof(c->led.backend));
    SAFE_STRNCPY(c->led.spi_device,"/dev/spidev0.0",sizeof(c->led.spi_device));
    c->led.spi_speed_hz=2400000;
    c->led.gpio_pin=18;
    c->led.dma_channel=10;
}

result_t config_load_app_config(config_manager_t *m, app_config_t *c) {
    CHECK_NULL(m);
    CHECK_NULL(c);

    /* Set defaults first - missing config entries use these */
    config_get_defaults(c);

    /* Load all fields from table - errors are non-fatal (keeps default) */
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
        config_load_field(m, c, &config_fields[i]);
    }

    return RESULT_OK;
}
