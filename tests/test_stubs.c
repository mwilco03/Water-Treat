/**
 * @file test_stubs.c
 * @brief Stub implementations for symbols needed by test dependencies
 *
 * The test binary links logger.c which references TUI functions.
 * Since tests run without a TUI, these stubs return safe defaults.
 */

#include <stdbool.h>

bool tui_is_active(void) {
    return false;  /* TUI never active during tests */
}

void tui_log_message(int level, const char *message) {
    (void)level;
    (void)message;
}
