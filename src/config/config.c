#include "config.h"
#include "utils/logger.h"
#include <stdio.h>
#include <ctype.h>

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
    fprintf(f,"# PROFINET Monitor Configuration\n\n"); char cs[MAX_NAME_LEN]="";
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
    SAFE_STRNCPY(c->system.device_name,"profinet-sensor-hub",sizeof(c->system.device_name));
    SAFE_STRNCPY(c->system.log_level,"info",sizeof(c->system.log_level));
    SAFE_STRNCPY(c->network.interface,"eth0",sizeof(c->network.interface)); c->network.dhcp_enabled=true;
    SAFE_STRNCPY(c->profinet.station_name,"rpi-sensor-hub",sizeof(c->profinet.station_name));
    c->profinet.vendor_id=0x0493; c->profinet.device_id=0x0001; c->profinet.enabled=true;
    SAFE_STRNCPY(c->database.path,"/var/lib/profinet-monitor/data.db",sizeof(c->database.path)); c->database.create_if_missing=true;
    c->logging.enabled=true; c->logging.interval_seconds=60; c->logging.retention_days=30;
    /* Health check defaults */
    c->health.enabled=true; c->health.http_enabled=true; c->health.http_port=8080;
    SAFE_STRNCPY(c->health.file_path,"/var/lib/profinet-monitor/health.prom",sizeof(c->health.file_path));
    c->health.update_interval_seconds=10;
}

result_t config_load_app_config(config_manager_t *m, app_config_t *c) {
    CHECK_NULL(m); CHECK_NULL(c); config_get_defaults(c);
    char v[MAX_CONFIG_VALUE_LEN]; int iv; bool bv;
    if(config_get_string(m,"system","device_name",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->system.device_name,v,sizeof(c->system.device_name));
    if(config_get_string(m,"system","log_level",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->system.log_level,v,sizeof(c->system.log_level));
    if(config_get_string(m,"network","interface",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->network.interface,v,sizeof(c->network.interface));
    if(config_get_bool(m,"network","dhcp_enabled",&bv)==RESULT_OK) c->network.dhcp_enabled=bv;
    if(config_get_string(m,"profinet","station_name",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->profinet.station_name,v,sizeof(c->profinet.station_name));
    if(config_get_int(m,"profinet","vendor_id",&iv)==RESULT_OK) c->profinet.vendor_id=(uint16_t)iv;
    if(config_get_int(m,"profinet","device_id",&iv)==RESULT_OK) c->profinet.device_id=(uint16_t)iv;
    if(config_get_bool(m,"profinet","enabled",&bv)==RESULT_OK) c->profinet.enabled=bv;
    if(config_get_string(m,"database","path",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->database.path,v,sizeof(c->database.path));
    if(config_get_bool(m,"logging","enabled",&bv)==RESULT_OK) c->logging.enabled=bv;
    if(config_get_int(m,"logging","interval_seconds",&iv)==RESULT_OK) c->logging.interval_seconds=iv;
    /* Health check configuration */
    if(config_get_bool(m,"health","enabled",&bv)==RESULT_OK) c->health.enabled=bv;
    if(config_get_bool(m,"health","http_enabled",&bv)==RESULT_OK) c->health.http_enabled=bv;
    if(config_get_int(m,"health","http_port",&iv)==RESULT_OK) c->health.http_port=(uint16_t)iv;
    if(config_get_string(m,"health","file_path",v,sizeof(v))==RESULT_OK) SAFE_STRNCPY(c->health.file_path,v,sizeof(c->health.file_path));
    if(config_get_int(m,"health","update_interval_seconds",&iv)==RESULT_OK) c->health.update_interval_seconds=iv;
    return RESULT_OK;
}
