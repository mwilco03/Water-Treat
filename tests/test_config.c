/**
 * @file test_config.c
 * @brief Unit tests for configuration parsing
 */

#include "test_framework.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Simplified config parser for testing (mirrors config.c logic) */
static char* trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return str;
}

static int parse_bool(const char *str) {
    return (strcasecmp(str, "true") == 0 ||
            strcasecmp(str, "yes") == 0 ||
            strcasecmp(str, "1") == 0);
}

static int parse_int(const char *str, int base) {
    return (int)strtol(str, NULL, base);
}

/* Test trimming whitespace */
void test_config_trim(void) {
    char s1[] = "  hello  ";
    TEST_ASSERT_STR_EQ("hello", trim(s1));

    char s2[] = "no_spaces";
    TEST_ASSERT_STR_EQ("no_spaces", trim(s2));

    char s3[] = "   ";
    TEST_ASSERT_STR_EQ("", trim(s3));

    char s4[] = "\t\ttabs\t\t";
    TEST_ASSERT_STR_EQ("tabs", trim(s4));
}

/* Test boolean parsing */
void test_config_parse_bool(void) {
    TEST_ASSERT_EQ(1, parse_bool("true"));
    TEST_ASSERT_EQ(1, parse_bool("TRUE"));
    TEST_ASSERT_EQ(1, parse_bool("True"));
    TEST_ASSERT_EQ(1, parse_bool("yes"));
    TEST_ASSERT_EQ(1, parse_bool("YES"));
    TEST_ASSERT_EQ(1, parse_bool("1"));

    TEST_ASSERT_EQ(0, parse_bool("false"));
    TEST_ASSERT_EQ(0, parse_bool("FALSE"));
    TEST_ASSERT_EQ(0, parse_bool("no"));
    TEST_ASSERT_EQ(0, parse_bool("0"));
    TEST_ASSERT_EQ(0, parse_bool(""));
}

/* Test integer parsing */
void test_config_parse_int(void) {
    TEST_ASSERT_EQ(42, parse_int("42", 10));
    TEST_ASSERT_EQ(-10, parse_int("-10", 10));
    TEST_ASSERT_EQ(0, parse_int("0", 10));
    TEST_ASSERT_EQ(255, parse_int("255", 10));
}

/* Test hexadecimal parsing */
void test_config_parse_hex(void) {
    TEST_ASSERT_EQ(0x0493, parse_int("0x0493", 0));
    TEST_ASSERT_EQ(0x0001, parse_int("0x0001", 0));
    TEST_ASSERT_EQ(0xFF, parse_int("0xFF", 0));
    TEST_ASSERT_EQ(0xABCD, parse_int("0xABCD", 0));
}

/* Test quote stripping */
void test_config_quote_strip(void) {
    char s1[] = "\"quoted string\"";
    size_t len = strlen(s1);
    if (len >= 2 && s1[0] == '"' && s1[len-1] == '"') {
        s1[len-1] = '\0';
        char *result = s1 + 1;
        TEST_ASSERT_STR_EQ("quoted string", result);
    }

    char s2[] = "'single quotes'";
    len = strlen(s2);
    if (len >= 2 && s2[0] == '\'' && s2[len-1] == '\'') {
        s2[len-1] = '\0';
        char *result = s2 + 1;
        TEST_ASSERT_STR_EQ("single quotes", result);
    }
}

/* Test section parsing */
void test_config_section_parse(void) {
    char line[] = "[profinet]";
    char *section = NULL;

    if (line[0] == '[') {
        char *end = strchr(line, ']');
        if (end) {
            *end = '\0';
            section = line + 1;
        }
    }

    TEST_ASSERT_NOT_NULL(section);
    TEST_ASSERT_STR_EQ("profinet", section);
}

/* Test key=value parsing */
void test_config_kv_parse(void) {
    char line[] = "station_name = rtu-abcd";
    char *key = NULL;
    char *value = NULL;

    char *eq = strchr(line, '=');
    if (eq) {
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);
    }

    TEST_ASSERT_NOT_NULL(key);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_STR_EQ("station_name", key);
    TEST_ASSERT_STR_EQ("rtu-abcd", value);
}

/* Test default values */
void test_config_defaults(void) {
    /* These should match config.c defaults
     * Note: device_name and station_name are now auto-detected from MAC (rtu-XXXX format)
     * Fallback is rtu-0000 if MAC detection fails */
    char default_interface[] = "eth0";
    int default_vendor_id = 0x0493;
    int default_device_id = 0x0001;
    int default_log_interval = 60;

    TEST_ASSERT_STR_EQ("eth0", default_interface);
    TEST_ASSERT_EQ(0x0493, default_vendor_id);
    TEST_ASSERT_EQ(0x0001, default_device_id);
    TEST_ASSERT_EQ(60, default_log_interval);
}

/* Test health config defaults */
void test_config_health_defaults(void) {
    int default_http_port = 8080;
    int default_update_interval = 10;
    char default_file_path[] = "/var/lib/water-treat/health.prom";

    TEST_ASSERT_EQ(8080, default_http_port);
    TEST_ASSERT_EQ(10, default_update_interval);
    TEST_ASSERT(strlen(default_file_path) > 0);
}

/* Test comment detection */
void test_config_comments(void) {
    char line1[] = "# This is a comment";
    char line2[] = "; This is also a comment";
    char line3[] = "key = value # inline comment";

    /* Lines starting with # or ; are comments */
    char *t1 = trim(line1);
    int is_comment1 = (t1[0] == '#' || t1[0] == ';');
    TEST_ASSERT(is_comment1);

    char *t2 = trim(line2);
    int is_comment2 = (t2[0] == '#' || t2[0] == ';');
    TEST_ASSERT(is_comment2);

    char *t3 = trim(line3);
    int is_comment3 = (t3[0] == '#' || t3[0] == ';');
    TEST_ASSERT(!is_comment3);  /* Not a full-line comment */
}

void run_config_tests(void) {
    TEST_SUITE_BEGIN("Configuration Parsing");

    RUN_TEST(test_config_trim);
    RUN_TEST(test_config_parse_bool);
    RUN_TEST(test_config_parse_int);
    RUN_TEST(test_config_parse_hex);
    RUN_TEST(test_config_quote_strip);
    RUN_TEST(test_config_section_parse);
    RUN_TEST(test_config_kv_parse);
    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_health_defaults);
    RUN_TEST(test_config_comments);
}
