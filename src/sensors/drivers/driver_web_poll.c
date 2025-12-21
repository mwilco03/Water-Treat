#include "driver_web_poll.h"
#include "utils/logger.h"
#include <string.h>
#include <stdlib.h>

#if defined(HAVE_CURL) && defined(HAVE_CJSON)
#include <curl/curl.h>
#include <cjson/cJSON.h>
#define WEB_POLL_ENABLED 1
#else
#define WEB_POLL_ENABLED 0
#endif

#if WEB_POLL_ENABLED

// Callback for curl to write response data
struct memory_struct {
    char *memory;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory_struct *mem = (struct memory_struct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        LOG_ERROR("Out of memory");
        return 0;
    }
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

result_t web_poll_init(web_poll_device_t *dev, const char *url, const char *method) {
    memset(dev, 0, sizeof(*dev));
    
    SAFE_STRNCPY(dev->url, url, sizeof(dev->url));
    SAFE_STRNCPY(dev->method, method, sizeof(dev->method));
    
    dev->curl = curl_easy_init();
    if (!dev->curl) {
        LOG_ERROR("Failed to initialize curl");
        return RESULT_ERROR;
    }
    
    dev->cache_on_error = true;
    
    LOG_INFO("Initialized web poll sensor: %s", url);
    return RESULT_OK;
}

void web_poll_destroy(web_poll_device_t *dev) {
    if (dev->curl) {
        curl_easy_cleanup(dev->curl);
        dev->curl = NULL;
    }
}

result_t web_poll_set_headers(web_poll_device_t *dev, const char *headers) {
    SAFE_STRNCPY(dev->headers, headers, sizeof(dev->headers));
    return RESULT_OK;
}

result_t web_poll_set_json_path(web_poll_device_t *dev, const char *json_path) {
    SAFE_STRNCPY(dev->json_path, json_path, sizeof(dev->json_path));
    return RESULT_OK;
}

// Simple JSONPath resolver (supports $.field and $.field.subfield)
static result_t parse_json_value(const char *json_str, const char *json_path, float *value) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        LOG_ERROR("Failed to parse JSON response");
        return RESULT_ERROR;
    }
    
    // Simple path parsing for $.field or $.field.subfield
    const char *path = json_path;
    if (strncmp(path, "$.", 2) == 0) {
        path += 2;
    }
    
    cJSON *current = root;
    char path_copy[128];
    SAFE_STRNCPY(path_copy, path, sizeof(path_copy));
    
    char *token = strtok(path_copy, ".");
    while (token != NULL) {
        current = cJSON_GetObjectItem(current, token);
        if (!current) {
            LOG_ERROR("JSON path not found: %s", token);
            cJSON_Delete(root);
            return RESULT_NOT_FOUND;
        }
        token = strtok(NULL, ".");
    }
    
    if (cJSON_IsNumber(current)) {
        *value = (float)cJSON_GetNumberValue(current);
    } else if (cJSON_IsString(current)) {
        *value = atof(cJSON_GetStringValue(current));
    } else {
        LOG_ERROR("JSON value is not a number");
        cJSON_Delete(root);
        return RESULT_ERROR;
    }
    
    cJSON_Delete(root);
    return RESULT_OK;
}

result_t web_poll_fetch(web_poll_device_t *dev, float *value) {
    struct memory_struct chunk = {NULL, 0};
    
    // Set URL
    curl_easy_setopt(dev->curl, CURLOPT_URL, dev->url);
    
    // Set method
    if (strcmp(dev->method, "POST") == 0) {
        curl_easy_setopt(dev->curl, CURLOPT_POST, 1L);
        if (dev->post_body[0] != '\0') {
            curl_easy_setopt(dev->curl, CURLOPT_POSTFIELDS, dev->post_body);
        }
    }
    
    // Set headers if provided
    struct curl_slist *headers = NULL;
    if (dev->headers[0] != '\0') {
        // Parse JSON headers object
        cJSON *headers_json = cJSON_Parse(dev->headers);
        if (headers_json) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, headers_json) {
                char header_line[256];
                SAFE_SNPRINTF(header_line, sizeof(header_line), "%s: %s", 
                             item->string, cJSON_GetStringValue(item));
                headers = curl_slist_append(headers, header_line);
            }
            cJSON_Delete(headers_json);
        }
        curl_easy_setopt(dev->curl, CURLOPT_HTTPHEADER, headers);
    }
    
    // Set write callback
    curl_easy_setopt(dev->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(dev->curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // Set timeout
    curl_easy_setopt(dev->curl, CURLOPT_TIMEOUT, 10L);
    
    // Perform request
    CURLcode res = curl_easy_perform(dev->curl);
    
    if (headers) {
        curl_slist_free_all(headers);
    }
    
    if (res != CURLE_OK) {
        LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        
        if (dev->cache_on_error) {
            *value = dev->last_value;
            return RESULT_OK;
        }
        
        return RESULT_ERROR;
    }
    
    // Parse JSON response
    result_t result = parse_json_value(chunk.memory, dev->json_path, value);
    
    free(chunk.memory);
    
    if (result == RESULT_OK) {
        dev->last_value = *value;
        dev->last_fetch = time(NULL);
    } else if (dev->cache_on_error) {
        *value = dev->last_value;
        result = RESULT_OK;
    }
    
    return result;
}

#else /* WEB_POLL_ENABLED == 0 */

/* Stub implementations when CURL/cJSON are not available */

result_t web_poll_init(web_poll_device_t *dev, const char *url, const char *method) {
    (void)dev; (void)url; (void)method;
    LOG_WARNING("Web poll not available: CURL and/or cJSON not installed");
    return RESULT_NOT_SUPPORTED;
}

void web_poll_destroy(web_poll_device_t *dev) {
    (void)dev;
}

result_t web_poll_set_headers(web_poll_device_t *dev, const char *headers) {
    (void)dev; (void)headers;
    return RESULT_NOT_SUPPORTED;
}

result_t web_poll_set_json_path(web_poll_device_t *dev, const char *json_path) {
    (void)dev; (void)json_path;
    return RESULT_NOT_SUPPORTED;
}

result_t web_poll_fetch(web_poll_device_t *dev, float *value) {
    (void)dev; (void)value;
    return RESULT_NOT_SUPPORTED;
}

#endif /* WEB_POLL_ENABLED */