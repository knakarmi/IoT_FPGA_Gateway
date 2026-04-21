/* =============================================================================
 * IoT Gateway - Week 10
 * File   : functional_test.h
 * Purpose: Functional test suite for the IoT gateway AES-256-GCM pipeline
 * =============================================================================
 */
#ifndef FUNCTIONAL_TEST_H
#define FUNCTIONAL_TEST_H

#include "xil_types.h"

/* Test result codes */
#define TEST_PASS       0
#define TEST_FAIL       1

/* Number of repeated transfers for stability test */
#define STABILITY_TEST_COUNT    20

/* ---------------------------------------------------------------------------
 * run_functional_tests
 * Execute the full Week 10 functional test suite.
 * Prints results to UART and returns TEST_PASS if all tests pass.
 * --------------------------------------------------------------------------- */
int run_functional_tests(void);

#endif /* FUNCTIONAL_TEST_H */
