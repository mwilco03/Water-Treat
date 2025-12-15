#ifndef DRIVER_WEB_POLL_H
#define DRIVER_WEB_POLL_H

#include "common.h"

#ifdef HAVE_CURL
#include <curl/curl.h>
#else
typedef void CURL;
#endif

typedef struct {
    char url[256];
    char method[16];
    char headers[256];
    char json_path[128];
    char post_body[512];
    CURL *curl;
    float last_value;
    time_t last_fetch;
    bool cache_on_error;
} web_poll_device_t;

result_t web_poll_init(web_poll_device_t *dev, const char *url, const char *method);
void web_poll_destroy(web_poll_device_t *dev);
result_t web_poll_set_headers(web_poll_device_t *dev, const char *headers);
result_t web_poll_set_json_path(web_poll_device_t *dev, const char *json_path);
result_t web_poll_fetch(web_poll_device_t *dev, float *value);

#endif
