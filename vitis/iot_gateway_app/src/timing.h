/* =============================================================================
 * IoT Gateway - Week 10
 * File   : timing.h
 * Purpose: Cycle-accurate latency measurement using the Cortex-A9 PMU
 *
 * The ARM Cortex-A9 Performance Monitor Unit (PMU) contains a 32-bit cycle
 * counter that increments every CPU clock cycle. On the ZC706 the CPU runs
 * at 666.666 MHz, so each tick is approximately 1.5 nanoseconds.
 *
 * The counter wraps around after 2^32 cycles (~6.4 seconds at 667 MHz).
 * For measuring individual packet operations (microseconds to milliseconds)
 * this is more than sufficient.
 *
 * Usage:
 *   timing_init();                    // call once at startup
 *   u32 t0 = timing_now();            // record start
 *   ... operation ...
 *   u32 t1 = timing_now();            // record end
 *   u32 us = timing_cycles_to_us(t1 - t0);  // convert to microseconds
 * =============================================================================
 */
#ifndef TIMING_H
#define TIMING_H

#include "xil_types.h"

/* CPU clock frequency on ZC706 (Hz) */
#define CPU_FREQ_HZ     666666667U

/* ---------------------------------------------------------------------------
 * timing_init
 * Enable the PMU cycle counter. Must be called once before timing_now().
 * --------------------------------------------------------------------------- */
void timing_init(void);

/* ---------------------------------------------------------------------------
 * timing_now
 * Read the current 32-bit cycle counter value.
 * --------------------------------------------------------------------------- */
u32 timing_now(void);

/* ---------------------------------------------------------------------------
 * timing_cycles_to_us
 * Convert a cycle count difference to microseconds.
 * Uses integer arithmetic to avoid floating point.
 * --------------------------------------------------------------------------- */
u32 timing_cycles_to_us(u32 cycles);

/* ---------------------------------------------------------------------------
 * timing_cycles_to_ns
 * Convert a cycle count difference to nanoseconds.
 * --------------------------------------------------------------------------- */
u32 timing_cycles_to_ns(u32 cycles);

#endif /* TIMING_H */
