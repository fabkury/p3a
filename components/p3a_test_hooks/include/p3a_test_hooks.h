// SPDX-License-Identifier: Apache-2.0
// Test hooks for automated device tests. Exists only in test builds
// (CONFIG_P3A_TEST_HOOKS); every symbol here starts with p3a_test_ so that a
// release build can be checked for their absence.
//
// The UART console protocol, one line each way:
//
//   host   -> device   !<command>[ <arguments>]\n
//   device -> host     TEST|ok|<json object>\n      the command succeeded
//                      TEST|err|<message>\n         it did not
//                      TEST|ready|<json object>\n   once, when the console starts
//
// Input is not echoed, so secrets sent as arguments do not come back on the
// wire. Lines that do not start with '!' are ignored.
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_P3A_TEST_HOOKS

// Starts the UART test console. Call once from app_main.
void p3a_test_console_start(void);

// Registers the test hooks' own source ("test"). Call before the scheduler
// starts.
void p3a_test_sources_register(void);

#endif

#ifdef __cplusplus
}
#endif
